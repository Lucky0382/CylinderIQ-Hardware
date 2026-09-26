#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — NVS store (ESP32-S3)
//
// Namespace: "cylinderiq"
// All floats stored as blob (sizeof(float)) per handoff spec.
// All getters return a default value if the key is absent.
//
// Call nvs_store_init() once from app_main before any get/set.
// All functions are synchronous and safe to call from any task
// (they acquire an internal mutex before every NVS transaction).
// ──────────────────────────────────────────────────────────────

#define NVS_NAMESPACE   "cylinderiq"

// ── Wizard / calibration ─────────────────────────────────────
bool    nvs_get_wiz_complete(void);
void    nvs_set_wiz_complete(bool v);

int32_t nvs_get_wiz_step(void);
void    nvs_set_wiz_step(int32_t v);

int32_t nvs_get_cyl_type(void);      // 0-5
void    nvs_set_cyl_type(int32_t v);

int32_t nvs_get_heat_type(void);     // 0=none 1=immersion 2=boiler
void    nvs_set_heat_type(int32_t v);

float   nvs_get_imm_kw(void);        // default 3.0
void    nvs_set_imm_kw(float v);

int32_t nvs_get_imm_count(void);     // 1 or 2
void    nvs_set_imm_count(int32_t v);

int32_t nvs_get_tank_size(void);     // litres, default 150
void    nvs_set_tank_size(int32_t v);

uint8_t nvs_get_tank_actual(void);   // 0=estimated 1=measured
void    nvs_set_tank_actual(uint8_t v);

float   nvs_get_bl_cold(void);       // baseline cold temp (mains)
void    nvs_set_bl_cold(float v);

float   nvs_get_bl_hot(void);        // baseline max hot temp
void    nvs_set_bl_hot(float v);

float   nvs_get_bl_post_draw(void);  // baseline post-draw temp
void    nvs_set_bl_post_draw(float v);

// ── Profiles ─────────────────────────────────────────────────
int32_t nvs_get_active_profile(void);
void    nvs_set_active_profile(int32_t v);

// name: up to 24 chars, always NUL-terminated.
// buf must be at least 25 bytes.
void    nvs_get_profile_name(int profile_id, char *buf, size_t buf_len);
void    nvs_set_profile_name(int profile_id, const char *name);

bool    nvs_get_profile_eco(int profile_id);
void    nvs_set_profile_eco(int profile_id, bool eco);

// preset_name: "shower" | "bath" | "washing" | "cleaning" | "custom"
float   nvs_get_preset(int profile_id, const char *preset_name);
void    nvs_set_preset(int profile_id, const char *preset_name, float litres);

// ── Energy ───────────────────────────────────────────────────
void    nvs_get_energy_supplier(char *buf, size_t buf_len);  // max 32 chars
void    nvs_set_energy_supplier(const char *supplier);

float   nvs_get_on_peak_ppm(void);       // pence/kWh, default 0.29
void    nvs_set_on_peak_ppm(float v);

float   nvs_get_off_peak_ppm(void);      // default 0.12
void    nvs_set_off_peak_ppm(float v);

float   nvs_get_super_off_ppm(void);     // default 0.08
void    nvs_set_super_off_ppm(float v);

// ── Sensor ROM mapping ──────────────────────────────────────
// Stores auto-discovered DS18B20 ROM addresses indexed by role.
// 4 sensors × 8 bytes = 32-byte blob in NVS key "sr_roms".
bool    nvs_get_sensor_roms(uint8_t roms[4][8]);   // false if not stored
void    nvs_set_sensor_roms(const uint8_t roms[4][8]);
void    nvs_clear_sensor_roms(void);

// ── Lifecycle ────────────────────────────────────────────────
// Must be called once at boot before any get/set.
void    nvs_store_init(void);

// Erase ALL cylinderiq namespace keys (factory reset).
void    nvs_store_erase_all(void);

#ifdef __cplusplus
}
#endif
