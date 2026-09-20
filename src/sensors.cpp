// CylinderIQ Hub V2 — sensors.cpp (ESP32-S3)
// DS18B20 OneWire driver. GPIO bit-bang with open-drain mode.
// All 4 sensors addressed by confirmed ROM addresses; no discovery.
// Leak detection rope on GPIO7 (internal pullup, active-low).

#include "sensors.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"   // ets_delay_us()

static const char *TAG = "sensors";

// ──────────────────────────────────────────────────────────────
// ROM addresses — confirmed hardware, DO NOT CHANGE
// Stored as byte arrays, byte[0] = family code (0x28).
// Derived from handoff uint64 values (little-endian):
//   Hot Outlet     0x19000000BE29E828 → {0x28,0xE8,0x29,0xBE,0x00,0x00,0x00,0x19}
//   Cylinder Inlet 0x5400000048EDA428 → {0x28,0xA4,0xED,0x48,0x00,0x00,0x00,0x54}
//   Mains Supply   0x2B0000005413EB28 → {0x28,0xEB,0x13,0x54,0x00,0x00,0x00,0x2B}
//   Tundish        *** REPLACE 0xFF bytes after running ROM scan sketch ***
// ──────────────────────────────────────────────────────────────
static const uint8_t ROM_HOT_OUTLET[8]     = {0x28, 0xE8, 0x29, 0xBE, 0x00, 0x00, 0x00, 0x19};
static const uint8_t ROM_CYLINDER_INLET[8] = {0x28, 0xA4, 0xED, 0x48, 0x00, 0x00, 0x00, 0x54};
static const uint8_t ROM_MAINS_SUPPLY[8]   = {0x28, 0xEB, 0x13, 0x54, 0x00, 0x00, 0x00, 0x2B};
static const uint8_t ROM_TUNDISH[8]        = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // TODO: replace after ROM scan

static const uint8_t *ROMS[SENSOR_COUNT] = {
    ROM_HOT_OUTLET,
    ROM_CYLINDER_INLET,
    ROM_MAINS_SUPPLY,
    ROM_TUNDISH,
};

static const char *SENSOR_NAMES[SENSOR_COUNT] = {
    "hot_outlet",
    "cylinder_inlet",
    "mains_supply",
    "tundish",
};

// ──────────────────────────────────────────────────────────────
// Shared state — protected by mutex
// ──────────────────────────────────────────────────────────────
static sensor_readings_t s_readings = {0.0f, 0.0f, 0.0f, 0.0f, false, false};
static SemaphoreHandle_t s_mutex;

// ──────────────────────────────────────────────────────────────
// OneWire — GPIO open-drain bit-bang
//
// Hardware: GPIO6 configured as OUTPUT_OD (open-drain).
// External 4.7 kΩ pullup to 3.3 V required on the bus.
//
// Drive low  → gpio_set_level(pin, 0)   (transistor sinks current)
// Release    → gpio_set_level(pin, 1)   (transistor off, pullup pulls high)
// Read level → gpio_get_level(pin)      (works in OUTPUT_OD mode on ESP32)
//
// Timing uses ets_delay_us() inside portMUX critical sections so that
// task preemption or ISR jitter does not corrupt DS18B20 bit windows.
// Each critical section is ≤ 70 µs (one bit slot) — safe for esp-idf SMP.
// ──────────────────────────────────────────────────────────────

static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void ow_release(void)
{
    gpio_set_level(SENSORS_GPIO, 1);
}

static inline void ow_drive_low(void)
{
    gpio_set_level(SENSORS_GPIO, 0);
}

// Reset pulse. Returns true if at least one device pulls the presence pulse.
// Not in a critical section — 480 µs tolerances are wide.
static bool ow_reset(void)
{
    ow_drive_low();
    ets_delay_us(480);
    ow_release();
    ets_delay_us(70);
    bool presence = (gpio_get_level(SENSORS_GPIO) == 0);
    ets_delay_us(410);
    return presence;
}

// Write a single bit. Critical section keeps timing deterministic.
static void ow_write_bit(uint8_t bit)
{
    portENTER_CRITICAL(&s_ow_mux);
    ow_drive_low();
    if (bit) {
        ets_delay_us(5);
        ow_release();
        ets_delay_us(55);
    } else {
        ets_delay_us(60);
        ow_release();
        ets_delay_us(5);
    }
    portEXIT_CRITICAL(&s_ow_mux);
}

// Read a single bit.
static uint8_t ow_read_bit(void)
{
    portENTER_CRITICAL(&s_ow_mux);
    ow_drive_low();
    ets_delay_us(3);
    ow_release();
    ets_delay_us(10);
    uint8_t bit = (uint8_t)gpio_get_level(SENSORS_GPIO);
    ets_delay_us(47);
    portEXIT_CRITICAL(&s_ow_mux);
    return bit;
}

static void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte |= (ow_read_bit() << i);
    }
    return byte;
}

// ──────────────────────────────────────────────────────────────
// DS18B20 protocol
// ──────────────────────────────────────────────────────────────

#define CMD_SKIP_ROM        0xCC
#define CMD_MATCH_ROM       0x55
#define CMD_CONVERT_T       0x44
#define CMD_READ_SCRATCHPAD 0xBE

// CRC-8 Dallas/Maxim (used to validate scratchpad)
static uint8_t ds18b20_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (uint8_t b = 0; b < 8; b++) {
            uint8_t mix = (crc ^ byte) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}

// Issue Convert T to ALL sensors simultaneously (Skip ROM broadcast).
// Call this once, then wait 750 ms before reading.
static bool ds18b20_start_all_conversions(void)
{
    if (!ow_reset()) {
        ESP_LOGW(TAG, "No presence pulse during broadcast convert");
        return false;
    }
    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_CONVERT_T);
    // Pull bus high to supply parasitic power during conversion.
    ow_release();
    return true;
}

// Read scratchpad from one sensor addressed by its ROM.
// Returns true and sets *temp_c on success.
static bool ds18b20_read_temp(const uint8_t *rom, float *temp_c)
{
    if (!ow_reset()) {
        ESP_LOGW(TAG, "No presence pulse during read (ROM %02X...)", rom[0]);
        return false;
    }

    // Address specific sensor
    ow_write_byte(CMD_MATCH_ROM);
    for (int i = 0; i < 8; i++) {
        ow_write_byte(rom[i]);
    }

    // Read all 9 scratchpad bytes
    ow_write_byte(CMD_READ_SCRATCHPAD);
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_read_byte();
    }

    // Validate CRC
    if (ds18b20_crc8(sp, 8) != sp[8]) {
        ESP_LOGW(TAG, "CRC mismatch reading ROM %02X%02X%02X%02X%02X%02X%02X%02X",
                 rom[0], rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
        return false;
    }

    // Convert raw 16-bit signed value: 12-bit resolution = 0.0625 °C per LSB
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    *temp_c = raw * 0.0625f;
    return true;
}

// ──────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────

void sensors_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    // ── OneWire bus (GPIO6, open-drain) ──────────────────────
    gpio_config_t ow_cfg = {
        .pin_bit_mask = (1ULL << SENSORS_GPIO),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,  // open-drain — pullup on PCB
        .pull_up_en   = GPIO_PULLUP_DISABLE,        // external 4.7 kΩ used
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&ow_cfg);
    ow_release();  // idle high

    // ── Leak rope (GPIO7, input with internal pullup) ─────────
    // Rope is open-circuit when dry (GPIO reads HIGH via pullup).
    // Rope conducts when wet, pulling GPIO to GND (reads LOW → alert).
    gpio_config_t leak_cfg = {
        .pin_bit_mask = (1ULL << LEAK_ROPE_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&leak_cfg);

    ESP_LOGI(TAG, "OneWire GPIO%d initialised (open-drain)", SENSORS_GPIO);
    ESP_LOGI(TAG, "Leak rope GPIO%d initialised (input, pullup)", LEAK_ROPE_GPIO);
}

void sensors_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Sensor task started");

    for (;;) {
        // ── 1. Broadcast Convert T ──────────────────────────
        if (!ds18b20_start_all_conversions()) {
            ESP_LOGE(TAG, "Bus error — no devices responding");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // ── 2. Wait 750 ms for 12-bit conversion ─────────────
        vTaskDelay(pdMS_TO_TICKS(750));

        // ── 3. Read each DS18B20 sensor by ROM address ────────
        float temps[SENSOR_COUNT];
        bool  ok[SENSOR_COUNT];

        for (int i = 0; i < SENSOR_COUNT; i++) {
            ok[i] = ds18b20_read_temp(ROMS[i], &temps[i]);
            if (ok[i]) {
                ESP_LOGD(TAG, "%-20s = %.2f °C", SENSOR_NAMES[i], temps[i]);
            } else {
                ESP_LOGW(TAG, "%-20s = READ FAILED", SENSOR_NAMES[i]);
            }
        }

        // ── 4. Read leak rope (GPIO7, active-low) ────────────
        // HIGH = dry (pullup dominant), LOW = wet (rope conducts to GND)
        bool leak_wet = (gpio_get_level(LEAK_ROPE_GPIO) == 0);
        if (leak_wet) {
            ESP_LOGW(TAG, "LEAK DETECTED — rope GPIO%d LOW", LEAK_ROPE_GPIO);
        }

        // ── 5. Update shared readings ─────────────────────────
        // Require the first 3 confirmed sensors (hot, inlet, mains) to be valid.
        // Tundish failure is non-critical — retain last value if read fails.
        if (ok[SENSOR_IDX_HOT_OUTLET] &&
            ok[SENSOR_IDX_CYLINDER_INLET] &&
            ok[SENSOR_IDX_MAINS_SUPPLY]) {

            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_readings.hot_outlet     = temps[SENSOR_IDX_HOT_OUTLET];
            s_readings.cylinder_inlet = temps[SENSOR_IDX_CYLINDER_INLET];
            s_readings.mains_supply   = temps[SENSOR_IDX_MAINS_SUPPLY];
            if (ok[SENSOR_IDX_TUNDISH]) {
                s_readings.tundish = temps[SENSOR_IDX_TUNDISH];
            }
            s_readings.leak_wet = leak_wet;
            s_readings.valid    = true;
            xSemaphoreGive(s_mutex);

            ESP_LOGI(TAG, "Hot=%.2f  Inlet=%.2f  Mains=%.2f  Tundish=%s  Leak=%s",
                     temps[SENSOR_IDX_HOT_OUTLET],
                     temps[SENSOR_IDX_CYLINDER_INLET],
                     temps[SENSOR_IDX_MAINS_SUPPLY],
                     ok[SENSOR_IDX_TUNDISH] ? "ok" : "err",
                     leak_wet ? "WET!" : "dry");
        } else {
            ESP_LOGW(TAG, "Partial read failure — retaining previous values");
        }

        // ── 6. Wait remainder of 1 s interval ─────────────────
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

sensor_readings_t sensors_get(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sensor_readings_t copy = s_readings;
    xSemaphoreGive(s_mutex);
    return copy;
}
