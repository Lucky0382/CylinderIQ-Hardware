#include "calculations.h"
#include "nvs_store.h"
#include "sensors.h"
#include "uart_bridge.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
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
static float s_prev_tundish= 0.0f;

// ── Safety & PRV / Draw tracking state ─────────────────────────────────────────
static bool    s_leak_alert          = false;
static int64_t s_leak_snooze_until_ms = 0; // 5-minute silence snooze after cancel
static bool    s_tundish_alert       = false;
static float   s_tundish_surge_delta = 0.0f;
static bool    s_flow_active         = false;
static float   s_snapshot_usable     = 0.0f;
static int64_t s_flow_start_us       = 0;
static float   s_flow_rate_lpm       = 4.0f; // 4.0 L/min = 1L every 15s (ideal for demo capture)

// Tundish rate-of-rise circular buffer (tracks samples over past 3 seconds)
#define TUNDISH_HIST_MAX 8
typedef struct {
    int64_t time_ms;
    float   temp;
} tundish_sample_t;

static tundish_sample_t s_tundish_hist[TUNDISH_HIST_MAX];
static int s_tundish_hist_count = 0;
static int s_tundish_hist_head  = 0;

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

// ── Autonomous Hardware Safety Interlock (Trips relays & valves via C6) ───────
static void trigger_autonomous_safety_shutoff(void)
{
    ESP_LOGW(TAG, "EMERGENCY SAFETY INTERLOCK TRIPPED! De-energizing relays & closing valves...");
    char *resp = NULL;
    int status = 0;
    // Trip all immersion switches immediately
    uart_bridge_request_c6("POST", "/switch/top/off", "", &status, &resp);
    if (resp) { free(resp); resp = NULL; }
    uart_bridge_request_c6("POST", "/switch/bottom/off", "", &status, &resp);
    if (resp) { free(resp); resp = NULL; }
    uart_bridge_request_c6("POST", "/switch/all/off", "", &status, &resp);
    if (resp) { free(resp); resp = NULL; }
    // Close automated valves
    uart_bridge_request_c6("POST", "/valve/close", "", &status, &resp);
    if (resp) { free(resp); resp = NULL; }
    uart_bridge_request_c6("POST", "/shutoff/close", "", &status, &resp);
    if (resp) { free(resp); resp = NULL; }
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

    int64_t now_ms = esp_timer_get_time() / 1000;

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

    // ── Tundish Rate-of-Rise Tracking (>= 5°C in <= 2s) ───────
    s_tundish_hist[s_tundish_hist_head].time_ms = now_ms;
    s_tundish_hist[s_tundish_hist_head].temp    = s.tundish;
    s_tundish_hist_head = (s_tundish_hist_head + 1) % TUNDISH_HIST_MAX;
    if (s_tundish_hist_count < TUNDISH_HIST_MAX) s_tundish_hist_count++;

    float max_tundish_rise = 0.0f;
    for (int i = 0; i < s_tundish_hist_count; i++) {
        int64_t dt_ms = now_ms - s_tundish_hist[i].time_ms;
        if (dt_ms >= 800 && dt_ms <= 2600) {
            float diff = s.tundish - s_tundish_hist[i].temp;
            if (diff > max_tundish_rise) {
                max_tundish_rise = diff;
            }
        }
    }

    // ── Top Temperature Shoot-Up Detection ────────────────────
    float d_outlet = s.hot_outlet - s_prev_outlet;
    bool top_temp_shoot_up = (d_outlet >= 0.4f && s.hot_outlet >= 38.0f);

    // ── PRV Drop / Thermal Surge Trigger ──────────────────────
    bool tundish_surge = (max_tundish_rise >= 4.5f) || (s.tundish >= 28.5f && s_prev_tundish < 28.5f);

    // ── Moisture Leak Rope Trigger (with 5-minute silence snooze) ────
    bool leak_snoozed = (now_ms < s_leak_snooze_until_ms);

    // If cable has physically dried out, auto-clear snooze timer so next wet event alerts immediately
    if (!s.leak_wet && s_leak_snooze_until_ms > 0) {
        s_leak_snooze_until_ms = 0;
        leak_snoozed = false;
        ESP_LOGI(TAG, "Leak cable dry — snooze timer reset");
    }

    if (s.leak_wet && !s_leak_alert && !leak_snoozed) {
        s_leak_alert = true;
        ESP_LOGE(TAG, "AUTONOMOUS INTERLOCK: Base perimeter leak rope is WET! Tripping cutoffs.");
        trigger_autonomous_safety_shutoff();
    }

    // ── Base Usable Hot Water Calculation (stratified 3-zone model) ─────
    float desired_temp = nvs_get_desired_temp();
    if (desired_temp < 35.0f || desired_temp > 65.0f) desired_temp = 42.0f;
    float mains_base = (s.mains_supply > 5.0f && s.mains_supply < 35.0f) ? s.mains_supply : bl_cold;
    if (mains_base < 5.0f) mains_base = 15.0f;
    float delta_t = desired_temp - mains_base;
    if (delta_t < 5.0f) delta_t = 20.0f;

    float base_usable = 0.0f;
    if (s.hot_outlet > mains_base + 2.0f) {
        float zone_vol = (float)tank_size / 3.0f;
        float top_f = (s.hot_outlet - mains_base) / delta_t;
        if (top_f < 0.0f) top_f = 0.0f;
        if (top_f > 1.4f) top_f = 1.4f;

        float mid_f = (s.cylinder_inlet - mains_base) / delta_t;
        if (mid_f < 0.0f) mid_f = 0.0f;
        if (mid_f > 1.4f) mid_f = 1.4f;

        float bot_f = 0.0f;
        if (s.mains_supply > mains_base + 3.0f) {
            bot_f = (s.mains_supply - mains_base) / delta_t;
            if (bot_f < 0.0f) bot_f = 0.0f;
            if (bot_f > 1.0f) bot_f = 1.0f;
        }

        base_usable = (top_f + mid_f + bot_f) * zone_vol;
        if (base_usable > (float)tank_size * 1.15f) base_usable = (float)tank_size * 1.15f;
    }
    float ratio = ((float)tank_size > 0.0f) ? (base_usable / (float)tank_size) : 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;

    // ── PRV / top-temperature events are NOT water-draw triggers ────────
    // There is no flow switch on the current hardware. A PRV thermal surge
    // is a safety event, not a domestic hot-water draw.
    if (tundish_surge) {
        s_tundish_alert = true;
        s_tundish_surge_delta = (max_tundish_rise > 0.0f)
                              ? max_tundish_rise
                              : (s.tundish - s_prev_tundish);
        ESP_LOGW(TAG, "PRV THERMAL SURGE (+%.1f°C) — safety event only",
                 s_tundish_surge_delta);
        trigger_autonomous_safety_shutoff();
    }

    // ── Dynamic Usable Hot Water ────────────────────────────────────────
    // During a deliberate software test, hold the stable usable snapshot
    // and subtract a synthetic draw at the configured rate.
    float usable = base_usable;
    float hot_pct = ratio * 100.0f;

    if (s_flow_active) {
        float elapsed_s = (float)(esp_timer_get_time() - s_flow_start_us) / 1000000.0f;
        float drawn_litres = elapsed_s * (s_flow_rate_lpm / 60.0f);
        usable = s_snapshot_usable - drawn_litres;
        if (usable < 0.0f) usable = 0.0f;
        hot_pct = ((float)tank_size > 0.0f)
                ? ((usable / (float)tank_size) * 100.0f) : 0.0f;
    }

    // ── Mains reference update ────────────────────────────────
    {
        float d_inlet = fabsf(s.cylinder_inlet - s_prev_inlet);
        float d_mains = fabsf(s.mains_supply   - s_prev_mains);

        if (fabsf(d_outlet) < 0.5f && d_inlet < 0.5f && d_mains < 0.05f) {
            s_mains_ref = s.mains_supply;
        }

        s_prev_outlet  = s.hot_outlet;
        s_prev_inlet   = s.cylinder_inlet;
        s_prev_mains   = s.mains_supply;
        s_prev_tundish = s.tundish;
    }

    // ── Showers / baths remaining ─────────────────────────────
    float showers = (shower_l > 0.0f) ? floorf(usable / shower_l) : 0.0f;
    float baths   = (bath_l   > 0.0f) ? floorf(usable / bath_l)   : 0.0f;

    // ── Recovery time (minutes) ───────────────────────────────
    float cold_mass  = (float)tank_size;
    float delta_T    = bl_hot - s.mains_supply;
    if (delta_T < 0.0f) delta_T = 0.0f;

    float energy_kwh = cold_mass * 4.186f * delta_T / 3600.0f;

    float heater_kw = imm_kw * (float)imm_count;
    if (heat_type == 2) {
        heater_kw = 24.0f; // Indirect / boiler
    }

    float recovery_min = 0.0f;
    if (heater_kw > 0.1f) {
        recovery_min = (energy_kwh / heater_kw) * 60.0f;
    }
    if (recovery_min > 999.0f) recovery_min = 999.0f;

    // ── Cost to recover (pence) ───────────────────────────────
    float cost_pence = energy_kwh * on_peak * 100.0f;

    // ── Publish ───────────────────────────────────────────────
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_result.hot_outlet          = s.hot_outlet;
    s_result.cylinder_inlet      = s.cylinder_inlet;
    s_result.mains_supply        = s.mains_supply;
    s_result.tundish             = s.tundish;
    s_result.leak_wet            = leak_snoozed ? false : s.leak_wet;
    s_result.leak_alert          = s_leak_alert;
    s_result.tundish_alert       = s_tundish_alert;
    s_result.tundish_surge_delta = s_tundish_surge_delta;
    s_result.flow_active         = s_flow_active;
    s_result.flow_rate_lpm       = s_flow_rate_lpm;
    s_result.snapshot_usable     = s_snapshot_usable;
    s_result.actual_usable_litres = base_usable;
    s_result.simulated_draw_litres = s_flow_active
        ? fmaxf(0.0f, s_snapshot_usable - usable) : 0.0f;
    s_result.simulation_target_litres = 0.0f;
    s_result.usable_litres       = usable;
    s_result.hot_pct             = hot_pct;
    s_result.showers_remaining   = showers;
    s_result.baths_remaining     = baths;
    s_result.recovery_min        = recovery_min;
    s_result.cost_pence          = cost_pence;
    s_result.valid               = true;
    xSemaphoreGive(s_mutex);

    ESP_LOGD(TAG,
             "usable=%.1fL (%.0f%%) flow=%d tundish_alert=%d leak=%d",
             usable, hot_pct, s_flow_active, s_tundish_alert, s_leak_alert);
}

calc_result_t calc_get(void)
{
    ensure_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    calc_result_t copy = s_result;
    xSemaphoreGive(s_mutex);
    return copy;
}

void calc_start_simulation(float litres, float rate_lpm)
{
    ensure_init();

    if (s_flow_active) s_flow_active = false;

    calc_result_t current = calc_get();
    s_snapshot_usable = current.usable_litres;
    if (s_snapshot_usable < 0.0f) s_snapshot_usable = 0.0f;

    if (rate_lpm >= 0.1f) {
        s_flow_rate_lpm = rate_lpm;
    } else {
        s_flow_rate_lpm = 4.0f; // 4.0 L/min = 1L every 15s
    }
    (void)litres;

    s_flow_start_us = esp_timer_get_time();
    s_flow_active = true;

    ESP_LOGI(TAG, "WATER-USE SIMULATION START: snapshot=%.1fL rate=%.1fL/min",
             s_snapshot_usable, s_flow_rate_lpm);
    calc_run();
}

void calc_stop_simulation(void)
{
    ensure_init();
    s_flow_active = false;
    s_flow_start_us = 0;
    s_snapshot_usable = 0.0f;
    calc_run();
    ESP_LOGI(TAG, "WATER-USE SIMULATION STOP — thermal model restored");
}

void calc_reset_alerts(void)
{
    ensure_init();
    int64_t now_ms = esp_timer_get_time() / 1000;
    // Snooze leak alarm for 5 minutes (300,000 ms) so demo is not interrupted
    s_leak_snooze_until_ms = now_ms + (5 * 60 * 1000);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_leak_alert          = false;
    s_tundish_alert       = false;
    s_flow_active         = false;
    s_tundish_surge_delta = 0.0f;
    s_result.leak_wet            = false;
    s_result.leak_alert          = false;
    s_result.tundish_alert       = false;
    s_result.flow_active         = false;
    s_result.tundish_surge_delta = 0.0f;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Safety alerts cleared — leak alert snoozed for 5 minutes");
}
