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

    // Safety & Alerts
    bool  leak_wet;          // true = leak rope detected moisture
    bool  leak_alert;        // latched leak alert
    bool  tundish_alert;     // latched PRV / rate-of-rise thermal surge alert
    float tundish_surge_delta; // delta T recorded during surge (e.g. 5.4C)

    // Draw / Flow state
    bool  flow_active;       // true while software water-use simulation is active
    float flow_rate_lpm;     // simulation rate in L/min (default 9.0)
    float snapshot_usable;   // actual usable litres captured when simulation started
    float actual_usable_litres; // live thermal-model result, never modified by simulation
    float simulated_draw_litres; // litres subtracted by the test simulation
    float simulation_target_litres; // 0 = run until stopped

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

// Start a software-only water-use simulation from the current actual usable volume.
// litres=0 means run until stopped. rate_lpm defaults to 9 L/min when <= 0.
void calc_start_simulation(float litres, float rate_lpm);

// Stop the software simulation. The thermal model remains untouched.
void calc_stop_simulation(void);

// Reset / silence latched alerts and stop active flow simulation
void calc_reset_alerts(void);

#ifdef __cplusplus
}
#endif
