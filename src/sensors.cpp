// CylinderIQ Hub V2 — sensors.cpp (ESP32-S3)
// DS18B20 OneWire driver with automatic ROM discovery and
// temperature-based role auto-mapping.
//
// Auto-mapping algorithm (on first boot or forced re-scan):
//   1. Scan OneWire bus using Search ROM command (0xF0)
//   2. Broadcast Convert T, wait 750 ms for 12-bit conversion
//   3. Read temperature from each discovered sensor
//   4. Sort by temperature descending and assign roles:
//      [0] Hottest  → hot_outlet     (hot pipe from cylinder top)
//      [1] 2nd      → tundish        (PRV discharge, room ambient ~20-25°C)
//      [2] 3rd      → cylinder_inlet (post-NRV, slightly warmed by cylinder)
//      [3] Coldest  → mains_supply   (incoming ground water ~10-18°C)
//   5. Persist mapping to NVS — survives reboots without re-scanning
//   6. On subsequent boots, load mapping from NVS directly
//
// If auto-mapping assigns roles incorrectly, use sensors_swap_roles()
// via POST /api/sensor_swap to swap two roles and re-save to NVS.

#include "sensors.h"
#include "nvs_store.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"   // ets_delay_us()
#include <string.h>

static const char *TAG = "sensors";

// Role names for logging
static const char *ROLE_NAMES[SENSOR_COUNT] = {
    "hot_outlet",
    "cylinder_inlet",
    "mains_supply",
    "tundish",
};

// ──────────────────────────────────────────────────────────────
// Active ROM addresses — populated by auto-discovery or NVS load.
// Indexed by role (SENSOR_IDX_*).
// ──────────────────────────────────────────────────────────────
static uint8_t s_roms[SENSOR_COUNT][8];
static bool    s_mapped = false;
static bool    s_from_nvs = false;
static int     s_bus_count = 0;
static volatile bool s_rescan_requested = false;

// Dynamic GPIO pins (persisted in NVS)
static gpio_num_t s_sensors_gpio = DEFAULT_SENSORS_GPIO; // GPIO 7 default
static gpio_num_t s_leak_gpio    = DEFAULT_LEAK_GPIO;    // GPIO 6 default

// Candidate pins to scan if sensors are not found on the default pin
static const gpio_num_t CANDIDATE_PINS[] = {
    GPIO_NUM_7,
    GPIO_NUM_6,
    GPIO_NUM_8,
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_3,
    GPIO_NUM_9,
    GPIO_NUM_10,
    GPIO_NUM_11,
    GPIO_NUM_12,
    GPIO_NUM_13,
    GPIO_NUM_14,
};

// Per-role live state
static float s_role_temps[SENSOR_COUNT] = {0};
static bool  s_role_present[SENSOR_COUNT] = {false};

// ──────────────────────────────────────────────────────────────
// Shared sensor readings — protected by mutex
// ──────────────────────────────────────────────────────────────
static sensor_readings_t s_readings = {0.0f, 0.0f, 0.0f, 0.0f, false, false};
static SemaphoreHandle_t s_mutex;

// ──────────────────────────────────────────────────────────────
// OneWire — GPIO open-drain bit-bang
// ──────────────────────────────────────────────────────────────

static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void ow_release(void)
{
    gpio_set_level(s_sensors_gpio, 1);
}

static inline void ow_drive_low(void)
{
    gpio_set_level(s_sensors_gpio, 0);
}

static void configure_ow_pin(gpio_num_t pin)
{
    gpio_config_t ow_cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,         // internal pullup enabled
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&ow_cfg);
    s_sensors_gpio = pin;
    ow_release();
    ets_delay_us(100);
}

static void configure_leak_pin(gpio_num_t pin)
{
    gpio_config_t leak_cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&leak_cfg);
    s_leak_gpio = pin;
}

// Reset pulse. Returns true if at least one device pulls the presence pulse.
static bool ow_reset(void)
{
    ow_release();
    ets_delay_us(10);
    if (gpio_get_level(s_sensors_gpio) == 0) {
        ESP_LOGW(TAG, "OneWire pin %d stuck LOW before reset — check pullup or short to GND", (int)s_sensors_gpio);
        return false;
    }

    ow_drive_low();
    ets_delay_us(480);
    ow_release();
    ets_delay_us(70);
    bool presence = (gpio_get_level(s_sensors_gpio) == 0);
    ets_delay_us(410);

    if (gpio_get_level(s_sensors_gpio) == 0) {
        ESP_LOGW(TAG, "OneWire pin %d stuck LOW after reset — held down", (int)s_sensors_gpio);
        return false;
    }

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
    uint8_t bit = (uint8_t)gpio_get_level(s_sensors_gpio);
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

#define CMD_SEARCH_ROM      0xF0
#define CMD_SKIP_ROM        0xCC
#define CMD_MATCH_ROM       0x55
#define CMD_CONVERT_T       0x44
#define CMD_READ_SCRATCHPAD 0xBE

// CRC-8 Dallas/Maxim
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

// Format ROM address as hex string for logging. Uses rotating buffers to allow multiple calls in one log.
static const char *rom_to_str(const uint8_t *rom)
{
    static char bufs[4][20];
    static int idx = 0;
    char *buf = bufs[idx++ % 4];
    snprintf(buf, 20, "%02X%02X%02X%02X%02X%02X%02X%02X",
             rom[0], rom[1], rom[2], rom[3], rom[4], rom[5], rom[6], rom[7]);
    return buf;
}

// ──────────────────────────────────────────────────────────────
// OneWire Search ROM — discovers all devices on the bus
//
// Standard 1-Wire Search algorithm. Each call to ow_search_next()
// discovers one device. Call ow_search_first() to reset, then
// ow_search_next() repeatedly until it returns false.
// ──────────────────────────────────────────────────────────────

static uint8_t s_search_rom[8];
static int     s_search_last_discrepancy;
static bool    s_search_last_device;

static bool ow_search_next(uint8_t *rom_out)
{
    if (s_search_last_device) return false;
    if (!ow_reset()) return false;

    ow_write_byte(CMD_SEARCH_ROM);

    int last_zero = -1;

    for (int bit_pos = 0; bit_pos < 64; bit_pos++) {
        int byte_idx = bit_pos / 8;
        int bit_mask = 1 << (bit_pos % 8);

        // Read bit and its complement from the bus
        uint8_t id_bit  = ow_read_bit();
        uint8_t cmp_bit = ow_read_bit();

        if (id_bit == 1 && cmp_bit == 1) {
            // No devices responding — bus error
            return false;
        }

        uint8_t direction;
        if (id_bit != cmp_bit) {
            // All remaining devices agree on this bit
            direction = id_bit;
        } else {
            // Discrepancy — multiple devices differ at this bit position
            if (bit_pos == s_search_last_discrepancy) {
                direction = 1;  // Take the 1-branch this time
            } else if (bit_pos > s_search_last_discrepancy) {
                direction = 0;  // New territory — take 0-branch first
            } else {
                // Repeat the direction from the previous search
                direction = (s_search_rom[byte_idx] & bit_mask) ? 1 : 0;
            }
            if (direction == 0) {
                last_zero = bit_pos;
            }
        }

        // Record the chosen bit
        if (direction) {
            s_search_rom[byte_idx] |= (uint8_t)bit_mask;
        } else {
            s_search_rom[byte_idx] &= (uint8_t)~bit_mask;
        }

        // Send the chosen direction to select matching devices
        ow_write_bit(direction);
    }

    s_search_last_discrepancy = last_zero;
    if (last_zero == -1) {
        s_search_last_device = true;
    }

    memcpy(rom_out, s_search_rom, 8);

    // Validate CRC (byte[7] is the CRC of bytes[0..6])
    if (ds18b20_crc8(rom_out, 7) != rom_out[7]) {
        ESP_LOGW(TAG, "Search ROM CRC mismatch: %s", rom_to_str(rom_out));
        return false;
    }

    // Verify DS18B20 family code
    if (rom_out[0] != 0x28) {
        ESP_LOGW(TAG, "Non-DS18B20 device found: family=0x%02X", rom_out[0]);
        return false;
    }

    return true;
}

static bool ow_search_first(uint8_t *rom_out)
{
    s_search_last_discrepancy = -1;
    s_search_last_device = false;
    memset(s_search_rom, 0, 8);
    return ow_search_next(rom_out);
}

// ──────────────────────────────────────────────────────────────
// DS18B20 temperature reading
// ──────────────────────────────────────────────────────────────

// Issue Convert T to ALL sensors simultaneously (Skip ROM broadcast).
static bool ds18b20_start_all_conversions(void)
{
    if (!ow_reset()) {
        ESP_LOGW(TAG, "No presence pulse during broadcast convert");
        return false;
    }
    ow_write_byte(CMD_SKIP_ROM);
    ow_write_byte(CMD_CONVERT_T);
    ow_release();  // Pull bus high for parasitic power
    return true;
}

// Read scratchpad from one sensor addressed by its ROM.
static bool ds18b20_read_temp(const uint8_t *rom, float *temp_c)
{
    if (!ow_reset()) return false;

    ow_write_byte(CMD_MATCH_ROM);
    for (int i = 0; i < 8; i++) {
        ow_write_byte(rom[i]);
    }

    ow_write_byte(CMD_READ_SCRATCHPAD);
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_read_byte();
    }

    if (ds18b20_crc8(sp, 8) != sp[8]) {
        return false;
    }

    // Check for power-on-reset value (0x0550 = 85.0°C) or all-zeros
    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    if (raw == 0x0550 || (sp[0] == 0 && sp[1] == 0 && sp[2] == 0 && sp[3] == 0)) {
        // Sensor hasn't completed a real conversion yet — skip
        return false;
    }

    *temp_c = raw * 0.0625f;
    return true;
}

// ──────────────────────────────────────────────────────────────
// Auto-Discovery & Temperature-Based Role Mapping
// ──────────────────────────────────────────────────────────────

// Discover all DS18B20 sensors on the bus. Returns count found.
static int discover_all_roms(uint8_t roms[][8], int max_count)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  OneWire ROM Discovery — scanning bus on GPIO%d", (int)s_sensors_gpio);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    int count = 0;
    uint8_t rom[8];

    bool found = ow_search_first(rom);
    while (found && count < max_count) {
        memcpy(roms[count], rom, 8);
        ESP_LOGI(TAG, "  Found sensor %d: ROM=%s", count, rom_to_str(rom));
        count++;
        found = ow_search_next(rom);
    }

    if (count == 0) {
        ESP_LOGE(TAG, "  *** NO DS18B20 SENSORS FOUND ON GPIO%d ***", (int)s_sensors_gpio);
        ESP_LOGE(TAG, "  Check wiring: GPIO%d → DS18B20 data, 4.7kΩ pullup to 3.3V", (int)s_sensors_gpio);
    } else {
        ESP_LOGI(TAG, "  Total sensors found: %d", count);
    }

    return count;
}

// Verify that a ROM address is physically present on the bus
static bool verify_rom_present(const uint8_t *rom)
{
    if (!ow_reset()) return false;
    ow_write_byte(CMD_MATCH_ROM);
    for (int i = 0; i < 8; i++) {
        ow_write_byte(rom[i]);
    }
    // Try to read scratchpad — if it responds, it's present
    ow_write_byte(CMD_READ_SCRATCHPAD);
    uint8_t sp[9];
    for (int i = 0; i < 9; i++) {
        sp[i] = ow_read_byte();
    }
    return (ds18b20_crc8(sp, 8) == sp[8]);
}

// Sort helper struct
typedef struct {
    int     idx;    // Original index in discovery array
    float   temp;   // Temperature reading
} temp_sort_t;

// Auto-map discovered sensors to roles by temperature ranking.
// Sorts descending: hottest → hot_outlet, coldest → mains_supply.
static bool auto_map_by_temperature(uint8_t disc_roms[][8], int disc_count)
{
    if (disc_count < SENSOR_COUNT) {
        ESP_LOGW(TAG, "Only %d sensors found, need %d — partial mapping", disc_count, SENSOR_COUNT);
    }

    int map_count = (disc_count < SENSOR_COUNT) ? disc_count : SENSOR_COUNT;

    // ── 1. Broadcast Convert T ──────────────────────────────
    ESP_LOGI(TAG, "  Reading temperatures for auto-mapping...");
    if (!ds18b20_start_all_conversions()) {
        ESP_LOGE(TAG, "  Failed to start conversion");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(800));  // 750ms for 12-bit + margin

    // ── 2. Read each discovered sensor ──────────────────────
    temp_sort_t sorted[MAX_BUS_SENSORS];
    int valid_count = 0;

    for (int i = 0; i < disc_count && i < MAX_BUS_SENSORS; i++) {
        float temp = 0.0f;
        bool ok = ds18b20_read_temp(disc_roms[i], &temp);
        if (ok) {
            sorted[valid_count].idx  = i;
            sorted[valid_count].temp = temp;
            valid_count++;
            ESP_LOGI(TAG, "  Sensor %d: ROM=%s  Temp=%.2f°C",
                     i, rom_to_str(disc_roms[i]), temp);
        } else {
            ESP_LOGW(TAG, "  Sensor %d: ROM=%s  Temp=READ FAILED (skipping)",
                     i, rom_to_str(disc_roms[i]));
        }
    }

    if (valid_count < SENSOR_COUNT) {
        // Second attempt — some sensors may need another conversion
        ESP_LOGW(TAG, "  Only %d valid reads, retrying...", valid_count);
        if (ds18b20_start_all_conversions()) {
            vTaskDelay(pdMS_TO_TICKS(800));
            valid_count = 0;
            for (int i = 0; i < disc_count && i < MAX_BUS_SENSORS; i++) {
                float temp = 0.0f;
                bool ok = ds18b20_read_temp(disc_roms[i], &temp);
                if (ok) {
                    sorted[valid_count].idx  = i;
                    sorted[valid_count].temp = temp;
                    valid_count++;
                    ESP_LOGI(TAG, "  Sensor %d (retry): ROM=%s  Temp=%.2f°C",
                             i, rom_to_str(disc_roms[i]), temp);
                }
            }
        }
    }

    if (valid_count < 1) {
        ESP_LOGE(TAG, "  No valid temperature readings — cannot auto-map");
        return false;
    }

    // ── 3. Sort by temperature descending (bubble sort, ≤8 items) ──
    for (int i = 0; i < valid_count - 1; i++) {
        for (int j = 0; j < valid_count - i - 1; j++) {
            if (sorted[j].temp < sorted[j + 1].temp) {
                temp_sort_t tmp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = tmp;
            }
        }
    }

    // ── 4. Assign roles ─────────────────────────────────────
    // Descending order: hot_outlet, tundish, cylinder_inlet, mains_supply
    const int role_order[SENSOR_COUNT] = {
        SENSOR_IDX_HOT_OUTLET,      // Hottest
        SENSOR_IDX_TUNDISH,         // 2nd (warm ambient ~20-25°C)
        SENSOR_IDX_CYLINDER_INLET,  // 3rd (slightly warmed by cylinder)
        SENSOR_IDX_MAINS_SUPPLY,    // Coldest (ground water)
    };

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  AUTO-MAPPED SENSOR ROLES (by temperature)");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    memset(s_roms, 0xFF, sizeof(s_roms));  // Clear all
    bool all_mapped = true;

    for (int rank = 0; rank < SENSOR_COUNT; rank++) {
        if (rank < valid_count) {
            int disc_idx = sorted[rank].idx;
            int role     = role_order[rank];
            memcpy(s_roms[role], disc_roms[disc_idx], 8);
            s_role_temps[role]   = sorted[rank].temp;
            s_role_present[role] = true;

            const char *label;
            switch (rank) {
                case 0: label = "HOTTEST";  break;
                case 1: label = "2nd";      break;
                case 2: label = "3rd";      break;
                case 3: label = "COLDEST";  break;
                default: label = "";        break;
            }
            ESP_LOGI(TAG, "  %-16s → ROM=%s  %.2f°C  (%s)",
                     ROLE_NAMES[role], rom_to_str(s_roms[role]),
                     sorted[rank].temp, label);
        } else {
            int role = role_order[rank];
            s_role_present[role] = false;
            all_mapped = false;
            ESP_LOGW(TAG, "  %-16s → NOT MAPPED (insufficient sensors)", ROLE_NAMES[role]);
        }
    }

    // Warn if middle two are very close (mapping might be ambiguous)
    if (valid_count >= 4) {
        float diff_middle = sorted[1].temp - sorted[2].temp;
        if (diff_middle < 1.5f && diff_middle > -1.5f) {
            ESP_LOGW(TAG, "");
            ESP_LOGW(TAG, "  ⚠ tundish and cylinder_inlet are very close (%.1f°C apart)", diff_middle);
            ESP_LOGW(TAG, "  ⚠ If wrong, use POST /api/sensor_swap {\"a\":1,\"b\":3} to swap them");
        }
    }

    ESP_LOGI(TAG, "═══════════════════════════════════════════════════");

    return all_mapped;
}

// Main discovery + mapping entry point
static void discover_and_map(void)
{
    // ── Try loading from NVS first ──────────────────────────
    if (nvs_get_sensor_roms(s_roms)) {
        ESP_LOGI(TAG, "Loaded sensor ROMs from NVS — verifying on bus...");

        // Verify all 4 stored ROMs are present
        bool all_present = true;
        for (int i = 0; i < SENSOR_COUNT; i++) {
            bool present = verify_rom_present(s_roms[i]);
            s_role_present[i] = present;
            if (present) {
                ESP_LOGI(TAG, "  %-16s ROM=%s  PRESENT ✓", ROLE_NAMES[i], rom_to_str(s_roms[i]));
            } else {
                ESP_LOGW(TAG, "  %-16s ROM=%s  MISSING ✗", ROLE_NAMES[i], rom_to_str(s_roms[i]));
                all_present = false;
            }
        }

        if (all_present) {
            ESP_LOGI(TAG, "All 4 sensors verified from NVS — using stored mapping");
            s_mapped = true;
            s_from_nvs = true;
            s_bus_count = SENSOR_COUNT;
            return;
        }

        ESP_LOGW(TAG, "Some NVS sensors missing — running fresh discovery");
        nvs_clear_sensor_roms();
    } else {
        ESP_LOGI(TAG, "No sensor ROM mapping in NVS — running first-time discovery");
    }

    // ── Discover all sensors on the bus ──────────────────────
    uint8_t disc_roms[MAX_BUS_SENSORS][8];
    int disc_count = discover_all_roms(disc_roms, MAX_BUS_SENSORS);

    // If no sensors found on current pin, probe all candidate pins!
    if (disc_count == 0) {
        ESP_LOGW(TAG, "No sensors on GPIO%d — probing candidate pins for DS18B20...", (int)s_sensors_gpio);

        for (size_t i = 0; i < sizeof(CANDIDATE_PINS)/sizeof(CANDIDATE_PINS[0]); i++) {
            gpio_num_t cand = CANDIDATE_PINS[i];
            if (cand == s_sensors_gpio) continue;

            ESP_LOGI(TAG, "Probing GPIO%d for OneWire...", (int)cand);
            configure_ow_pin(cand);
            vTaskDelay(pdMS_TO_TICKS(15));

            if (!ow_reset()) {
                continue;
            }

            // Presence pulse detected! Try discovering ROMs
            disc_count = discover_all_roms(disc_roms, MAX_BUS_SENSORS);
            if (disc_count > 0) {
                ESP_LOGI(TAG, "🎯 FOUND %d DS18B20 SENSORS ON GPIO%d! Auto-configuring OneWire=GPIO%d",
                         disc_count, (int)cand, (int)cand);
                s_sensors_gpio = cand;
                s_leak_gpio = (s_sensors_gpio == GPIO_NUM_7) ? GPIO_NUM_6 : GPIO_NUM_7;
                configure_leak_pin(s_leak_gpio);
                nvs_set_ow_gpio((int32_t)s_sensors_gpio);
                break;
            }
        }
    }

    s_bus_count = disc_count;

    if (disc_count == 0) {
        ESP_LOGE(TAG, "No sensors found on any probed pin — will retry next cycle");
        s_mapped = false;
        return;
    }

    // ── Auto-map by temperature ─────────────────────────────
    s_mapped = auto_map_by_temperature(disc_roms, disc_count);
    s_from_nvs = false;

    if (s_mapped) {
        // Save to NVS for persistence
        nvs_set_sensor_roms(s_roms);
        ESP_LOGI(TAG, "Sensor mapping saved to NVS ✓");
    }
}

// ──────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────

void sensors_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    // Load stored OneWire GPIO from NVS (defaults to GPIO 7 per user hardware)
    int32_t saved_pin = nvs_get_ow_gpio();
    s_sensors_gpio = (gpio_num_t)saved_pin;
    s_leak_gpio    = (s_sensors_gpio == GPIO_NUM_7) ? GPIO_NUM_6 : GPIO_NUM_7;

    configure_ow_pin(s_sensors_gpio);
    configure_leak_pin(s_leak_gpio);

    int idle_level = gpio_get_level(s_sensors_gpio);

    // Initialise ROM array to 0xFF (unmapped)
    memset(s_roms, 0xFF, sizeof(s_roms));

    ESP_LOGI(TAG, "OneWire GPIO%d initialised (open-drain, internal pullup ENABLED) — Idle line: %s",
             (int)s_sensors_gpio, idle_level ? "HIGH (3.3V) ✓" : "LOW (0V — STUCK OR SHORTED) ✗");
    ESP_LOGI(TAG, "Leak rope GPIO%d initialised (input, pullup)", (int)s_leak_gpio);
}

void sensors_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Sensor task started");

    // ── Initial discovery + mapping ─────────────────────────
    // Give the bus a moment to settle after power-on
    vTaskDelay(pdMS_TO_TICKS(500));
    discover_and_map();

    // ── Main read loop ──────────────────────────────────────
    for (;;) {
        // Check for re-scan request
        if (s_rescan_requested) {
            s_rescan_requested = false;
            ESP_LOGI(TAG, "Re-scan requested — running discovery...");
            nvs_clear_sensor_roms();
            s_mapped = false;
            discover_and_map();
        }

        // If not mapped yet, keep trying
        if (!s_mapped) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            discover_and_map();
            continue;
        }

        // ── 1. Broadcast Convert T ──────────────────────────
        if (!ds18b20_start_all_conversions()) {
            ESP_LOGE(TAG, "Bus error — no devices responding");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // ── 2. Wait 750 ms for 12-bit conversion ─────────────
        vTaskDelay(pdMS_TO_TICKS(750));

        // ── 3. Read each sensor by its mapped ROM address ────
        float temps[SENSOR_COUNT];
        bool  ok[SENSOR_COUNT];

        for (int i = 0; i < SENSOR_COUNT; i++) {
            // Skip unmapped roles
            if (s_roms[i][0] == 0xFF) {
                ok[i] = false;
                continue;
            }
            ok[i] = ds18b20_read_temp(s_roms[i], &temps[i]);
            if (ok[i]) {
                ESP_LOGD(TAG, "%-20s = %.2f °C", ROLE_NAMES[i], temps[i]);
            } else {
                ESP_LOGW(TAG, "%-20s = READ FAILED", ROLE_NAMES[i]);
            }
        }

        // ── 4. Read leak rope (active-low) ──────────────────
        bool leak_wet = (gpio_get_level(s_leak_gpio) == 0);
        if (leak_wet) {
            ESP_LOGW(TAG, "LEAK DETECTED — rope GPIO%d LOW", (int)s_leak_gpio);
        }

        // ── 5. Update shared readings ─────────────────────────
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

            // Update per-role tracking
            for (int i = 0; i < SENSOR_COUNT; i++) {
                s_role_present[i] = ok[i];
                if (ok[i]) s_role_temps[i] = temps[i];
            }
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

sensor_map_t sensors_get_map(void)
{
    sensor_map_t map;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    map.mapped      = s_mapped;
    map.bus_count   = s_bus_count;
    map.from_nvs    = s_from_nvs;
    map.active_gpio = (int)s_sensors_gpio;
    map.leak_gpio   = (int)s_leak_gpio;
    memcpy(map.roms, s_roms, sizeof(s_roms));
    memcpy(map.temps, s_role_temps, sizeof(s_role_temps));
    memcpy(map.present, s_role_present, sizeof(s_role_present));
    xSemaphoreGive(s_mutex);
    return map;
}

int sensors_get_gpio(void)
{
    return (int)s_sensors_gpio;
}

void sensors_set_gpio(int pin)
{
    if (pin < 0 || pin > 48) return;
    s_sensors_gpio = (gpio_num_t)pin;
    s_leak_gpio = (s_sensors_gpio == GPIO_NUM_7) ? GPIO_NUM_6 : GPIO_NUM_7;
    configure_ow_pin(s_sensors_gpio);
    configure_leak_pin(s_leak_gpio);
    nvs_set_ow_gpio((int32_t)s_sensors_gpio);
    sensors_rescan();
}

void sensors_rescan(void)
{
    ESP_LOGI(TAG, "Re-scan requested via API");
    s_rescan_requested = true;
}

bool sensors_swap_roles(int role_a, int role_b)
{
    if (role_a < 0 || role_a >= SENSOR_COUNT ||
        role_b < 0 || role_b >= SENSOR_COUNT ||
        role_a == role_b) {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Swap ROMs
    uint8_t tmp_rom[8];
    memcpy(tmp_rom, s_roms[role_a], 8);
    memcpy(s_roms[role_a], s_roms[role_b], 8);
    memcpy(s_roms[role_b], tmp_rom, 8);

    // Swap temps
    float tmp_temp = s_role_temps[role_a];
    s_role_temps[role_a] = s_role_temps[role_b];
    s_role_temps[role_b] = tmp_temp;

    // Swap present
    bool tmp_present = s_role_present[role_a];
    s_role_present[role_a] = s_role_present[role_b];
    s_role_present[role_b] = tmp_present;

    xSemaphoreGive(s_mutex);

    // Persist new mapping to NVS
    nvs_set_sensor_roms(s_roms);

    ESP_LOGI(TAG, "Swapped roles: %s ↔ %s (saved to NVS)",
             ROLE_NAMES[role_a], ROLE_NAMES[role_b]);

    return true;
}
