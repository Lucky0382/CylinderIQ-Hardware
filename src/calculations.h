#pragma once
#include <stdbool.h>
#include "sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — Calculations module (ESP32-S3)
//
// All five derived values computed from live sensor readings
// and NVS-stored calibration/config. Formulas from handoff spec.
//
// calc_run()  — recomputes all values; call after each sensor read.
// calc_get()  — thread-safe copy of last computed result.
// ──────────────────────────────────────────────────────────────

typedef struct {
    // Raw temperatures (mirror from sensors)
    float hot_outlet;
    float cylinder_inlet;
    float mains_supply;
    float tundish;           // PRV discharge pipe temperature

    // Safety
    bool  leak_wet;          // true = leak rope detected moisture

    // Derived
    float usable_litres;     // litres of usable hot water now
    float hot_pct;           // percentage (0–100)
    float showers_remaining; // floor(usable / shower preset)
    float baths_remaining;   // floor(usable / bath preset)
    float recovery_min;      // minutes to full recovery (capped at 999)
    float cost_pence;        // pence to recover at on-peak rate

    bool  valid;             // false until first successful compute
} calc_result_t;

// Recompute all derived values from current sensor readings + NVS config.
// Reads sensors_get() and NVS internally. Safe to call from any task.
void calc_run(void);

// Thread-safe snapshot of the last computed result.
calc_result_t calc_get(void);

#ifdef __cplusplus
}
#endif
