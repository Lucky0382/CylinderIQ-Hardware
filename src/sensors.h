#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — Sensor module (ESP32-S3)
//
// Reads 4x DS18B20 on OneWire bus (GPIO6, open-drain, 4.7kΩ pullup).
// ROM addresses are confirmed hardware — do NOT change or re-scan.
// Leak detection rope on GPIO7 (internal pullup — HIGH=dry, LOW=wet).
//
// Public API:
//   sensors_init()   — call once from app_main before starting task
//   sensors_task()   — FreeRTOS task, pass NULL, never returns
//   sensors_get()    — thread-safe copy of latest readings
// ──────────────────────────────────────────────────────────────

#define SENSORS_GPIO        GPIO_NUM_6   // OneWire data line
#define LEAK_ROPE_GPIO      GPIO_NUM_7   // Leak detection rope — HIGH=dry, LOW=wet

// Confirmed DS18B20 ROM addresses (uint64, little-endian byte order).
// Byte[0] = 0x28 (DS18B20 family code), Byte[7] = CRC.
// Hot Outlet:     0x19000000BE29E828
// Cylinder Inlet: 0x5400000048EDA428
// Mains Supply:   0x2B0000005413EB28
// Tundish:        *** RUN ROM SCAN — replace 0xFF placeholders with real address ***
#define SENSOR_COUNT        4

#define SENSOR_IDX_HOT_OUTLET       0
#define SENSOR_IDX_CYLINDER_INLET   1
#define SENSOR_IDX_MAINS_SUPPLY     2
#define SENSOR_IDX_TUNDISH          3

typedef struct {
    float hot_outlet;       // degrees C
    float cylinder_inlet;   // degrees C
    float mains_supply;     // degrees C
    float tundish;          // degrees C — PRV discharge pipe temperature
    bool  leak_wet;         // true = leak detected (rope conducting)
    bool  valid;            // false until first successful read
} sensor_readings_t;

// Initialise GPIO and OneWire. Call before sensors_task().
void sensors_init(void);

// FreeRTOS task entry point. Pass as pvTaskCode to xTaskCreate().
void sensors_task(void *arg);

// Thread-safe snapshot of the latest sensor readings.
sensor_readings_t sensors_get(void);

#ifdef __cplusplus
}
#endif
