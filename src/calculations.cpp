// CylinderIQ Hub V2 — calculations.cpp (ESP32-S3)
// All hot water derived values. Formulas verbatim from handoff spec.

#include "calculations.h"
#include "nvs_store.h"
#include "sensors.h"

#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "calc";

static calc_result_t  s_result;
static SemaphoreHandle_t s_mutex;

// ── Mains reference gate ──────────────────────────────────────────────────────
// s_mains_ref updates only when outlet, inlet, AND mains are all quiet.
//   • Hot draw     — outlet spikes (> 0.5°C/s)  → gate closes ✓
//   • Cold draw    — mains drops   (> 0.05°C/s) → gate closes ✓
//   • Overnight drift — all sensors < threshold  → gate open, ref tracks slowly ✓
static float s_mains_ref   = 14.0f;
static float s_prev_outlet = 0.0f;
static float s_prev_inlet  = 0.0f;
static float s_prev_mains  = 14.0f;

// Called once from main_s3.cpp before any task uses calc_get()
static bool s_init_done = false;

static void ensure_init(void)
{
    if (!s_init_done) {
        s_mutex = xSemaphoreCreateMutex();
        configASSERT(s_mutex);
        s_init_done = true;
    }
}

// ──────────────────────────────────────────────────────────────
// Calculation engine
// ──────────────────────────────────────────────────────────────

void calc_run(void)
{
    ensure_init();

    sensor_readings_t s = sensors_get();
    if (!s.valid) {
        // Sensors haven't produced a reading yet — nothing to compute
        return;
    }

    // ── NVS config snapshot ───────────────────────────────────
    float bl_cold    = nvs_get_bl_cold();       // mains at calibration
    float bl_hot     = nvs_get_bl_hot();        // max hot at calibration
    int   tank_size  = nvs_get_tank_size();     // litres
    int   heat_type  = nvs_get_heat_type();     // 1=immersion 2=boiler
    float imm_kw     = nvs_get_imm_kw();
    int   imm_count  = nvs_get_imm_count();
    float on_peak    = nvs_get_on_peak_ppm();   // pence per kWh
    int   active_pid = (int)nvs_get_active_profile();

    float shower_l   = nvs_get_preset(active_pid, "shower");
    float bath_l     = nvs_get_preset(active_pid, "bath");

    // ── Mains reference update ────────────────────────────────
    // Gate: only update ref when outlet, inlet AND mains are all quiet.
    // Thresholds per 1s cycle: outlet/inlet 0.5°C, mains 0.05°C.
    // 0.05°C/s catches cold-water events (~0.05°C/s) while allowing
    // seasonal overnight drift (~0.0001°C/s).
    {
        float d_outlet = fabsf(s.hot_outlet     - s_prev_outlet);
        float d_inlet  = fabsf(s.cylinder_inlet - s_prev_inlet);
        float d_mains  = fabsf(s.mains_supply   - s_prev_mains);

        if (d_outlet < 0.5f && d_inlet < 0.5f && d_mains < 0.05f) {
            s_mains_ref = s.mains_supply;
        }

        s_prev_outlet = s.hot_outlet;
        s_prev_inlet  = s.cylinder_inlet;
        s_prev_mains  = s.mains_supply;
    }

    // ── Usable hot water ──────────────────────────────────────
    // Water below 40.0°C is lukewarm / unusable for showers and domestic hot water.
    float ratio = 0.0f;
    float denom = bl_hot - 40.0f;
    if (denom < 5.0f) denom = 20.0f; // guard against degenerate calibration
    if (s.hot_outlet >= 40.0f) {
        ratio = (s.hot_outlet - 40.0f) / denom;
    }
    // Clamp 0..1
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    float usable  = ratio * (float)tank_size;
    float hot_pct = ratio * 100.0f;

    // ── Showers / baths remaining ─────────────────────────────
    float showers = (shower_l > 0.0f) ? floorf(usable / shower_l) : 0.0f;
    float baths   = (bath_l   > 0.0f) ? floorf(usable / bath_l)   : 0.0f;

    // ── Recovery time (minutes) ───────────────────────────────
    // Energy to heat full cold tank to baseline_hot:
    //   cold_mass_kg = tank_size (1 kg/L)
    //   delta_T      = baseline_hot - T_mains
    //   energy_kWh   = mass * 4.186 * delta_T / 3600
    float cold_mass  = (float)tank_size;
    float delta_T    = bl_hot - s.mains_supply;
    if (delta_T < 0.0f) delta_T = 0.0f;

    float energy_kwh = cold_mass * 4.186f * delta_T / 3600.0f;

    float heater_kw = imm_kw * (float)imm_count;
    if (heat_type == 2) {
        // Indirect / boiler — default 24 kW
        heater_kw = 24.0f;
    }

    float recovery_min = 0.0f;
    if (heater_kw > 0.1f) {
        recovery_min = (energy_kwh / heater_kw) * 60.0f;
    }
    // Cap display at 999 min
    if (recovery_min > 999.0f) recovery_min = 999.0f;

    // ── Cost to recover (pence) ───────────────────────────────
    // cost_pence = energy_kWh * on_peak_ppm * 100
    float cost_pence = energy_kwh * on_peak * 100.0f;

    // ── Publish ───────────────────────────────────────────────
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_result.hot_outlet      = s.hot_outlet;
    s_result.cylinder_inlet  = s.cylinder_inlet;
    s_result.mains_supply    = s.mains_supply;
    s_result.tundish         = s.tundish;
    s_result.leak_wet        = s.leak_wet;
    s_result.usable_litres   = usable;
    s_result.hot_pct         = hot_pct;
    s_result.showers_remaining = showers;
    s_result.baths_remaining   = baths;
    s_result.recovery_min      = recovery_min;
    s_result.cost_pence        = cost_pence;
    s_result.valid             = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG,
             "usable=%.0fL (%.0f%%) showers=%.0f baths=%.0f "
             "recovery=%.0fmin cost=%.2fp",
             usable, hot_pct, showers, baths, recovery_min, cost_pence);
}

calc_result_t calc_get(void)
{
    ensure_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    calc_result_t copy = s_result;
    xSemaphoreGive(s_mutex);
    return copy;
}
