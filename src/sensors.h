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
// ROM addresses are auto-discovered at boot via OneWire Search ROM,
// then assigned to roles by temperature ranking:
//   Hottest  → hot_outlet     (hot water pipe from cylinder top)
//   2nd      → tundish        (PRV discharge pipe, room ambient)
//   3rd      → cylinder_inlet (post-NRV, slightly warmed)
//   Coldest  → mains_supply   (incoming ground water)
// Mapping is persisted in NVS and survives reboots.
//
// Leak detection rope on GPIO7 (internal pullup — HIGH=dry, LOW=wet).
//
// Public API:
//   sensors_init()       — call once from app_main before starting task
//   sensors_task()       — FreeRTOS task, pass NULL, never returns
//   sensors_get()        — thread-safe copy of latest readings
//   sensors_get_map()    — current ROM-to-role mapping status
//   sensors_rescan()     — force re-discovery on next task iteration
//   sensors_swap_roles() — swap two role assignments and save to NVS
// ──────────────────────────────────────────────────────────────

#define SENSORS_GPIO        GPIO_NUM_6   // OneWire data line
#define LEAK_ROPE_GPIO      GPIO_NUM_7   // Leak detection rope — HIGH=dry, LOW=wet

#define SENSOR_COUNT        4
#define MAX_BUS_SENSORS     8   // Max sensors tracked during discovery

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

// Auto-discovery mapping status (returned by sensors_get_map)
typedef struct {
    bool    mapped;                             // true if all 4 roles assigned
    int     bus_count;                          // sensors discovered on bus
    bool    from_nvs;                           // mapping was loaded from NVS
    uint8_t roms[SENSOR_COUNT][8];              // ROM per role (indexed by SENSOR_IDX_*)
    float   temps[SENSOR_COUNT];                // last temperature per role
    bool    present[SENSOR_COUNT];              // sensor responding on bus
} sensor_map_t;

// Initialise GPIO and OneWire. Call before sensors_task().
void sensors_init(void);

// FreeRTOS task entry point. Pass as pvTaskCode to xTaskCreate().
void sensors_task(void *arg);

// Thread-safe snapshot of the latest sensor readings.
sensor_readings_t sensors_get(void);

// Thread-safe snapshot of the current sensor mapping.
sensor_map_t sensors_get_map(void);

// Request a re-scan of the OneWire bus on next task iteration.
void sensors_rescan(void);

// Swap two role assignments (by SENSOR_IDX_*) and persist to NVS.
bool sensors_swap_roles(int role_a, int role_b);

#ifdef __cplusplus
}
#endif
