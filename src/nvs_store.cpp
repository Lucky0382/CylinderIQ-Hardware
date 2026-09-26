// CylinderIQ Hub V2 — nvs_store.cpp (ESP32-S3)
// NVS persistence layer. All floats stored as blob (sizeof(float)).
// All getters return safe defaults on first boot / missing keys.

#include "nvs_store.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_store";
static SemaphoreHandle_t s_nvs_mutex;

// ──────────────────────────────────────────────────────────────
// Internal helpers
// ──────────────────────────────────────────────────────────────

static nvs_handle_t nvs_open_rw(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return 0;
    }
    return h;
}

static nvs_handle_t nvs_open_ro(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return 0;  // Key may simply not exist yet — caller handles default
    }
    return h;
}

// Float stored as 4-byte blob for cross-platform reliability.
static float nvs_get_float_default(const char *key, float def)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_ro();
    float val = def;
    if (h) {
        size_t len = sizeof(float);
        nvs_get_blob(h, key, &val, &len);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
    return val;
}

static void nvs_set_float_key(const char *key, float val)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        esp_err_t err = nvs_set_blob(h, key, &val, sizeof(float));
        if (err == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
}

static int32_t nvs_get_i32_default(const char *key, int32_t def)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_ro();
    int32_t val = def;
    if (h) {
        nvs_get_i32(h, key, &val);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
    return val;
}

static void nvs_set_i32_key(const char *key, int32_t val)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        nvs_set_i32(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
}

static uint8_t nvs_get_u8_default(const char *key, uint8_t def)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_ro();
    uint8_t val = def;
    if (h) {
        nvs_get_u8(h, key, &val);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
    return val;
}

static void nvs_set_u8_key(const char *key, uint8_t val)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        nvs_set_u8(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
}

// ──────────────────────────────────────────────────────────────
// NVS key builders for per-profile and per-preset values
// NVS key max length is 15 chars.
// Profile key  "p0_name" (7 chars) — fits easily.
// Preset key   "p0_shower" (9 chars) — fits.
// ──────────────────────────────────────────────────────────────

static void profile_key(char *buf, int pid, const char *suffix)
{
    snprintf(buf, 16, "p%d_%s", pid, suffix);
}

// Default preset values matching ImmersionOS profile 0 defaults
static float preset_default(int pid, const char *name)
{
    // Profile 0 defaults
    if (pid == 0) {
        if (strcmp(name, "shower")   == 0) return 42.0f;
        if (strcmp(name, "bath")     == 0) return 85.0f;
        if (strcmp(name, "washing")  == 0) return 30.0f;
        if (strcmp(name, "cleaning") == 0) return 18.0f;
    }
    // Profile 1 defaults
    if (pid == 1) {
        if (strcmp(name, "shower")   == 0) return 38.0f;
        if (strcmp(name, "bath")     == 0) return 75.0f;
        if (strcmp(name, "washing")  == 0) return 28.0f;
        if (strcmp(name, "cleaning") == 0) return 16.0f;
    }
    // Profile 2 defaults
    if (pid == 2) {
        if (strcmp(name, "shower")   == 0) return 34.0f;
        if (strcmp(name, "bath")     == 0) return 70.0f;
        if (strcmp(name, "washing")  == 0) return 22.0f;
        if (strcmp(name, "cleaning") == 0) return 14.0f;
    }
    return 0.0f;
}

// ──────────────────────────────────────────────────────────────
// Public API
// ──────────────────────────────────────────────────────────────

void nvs_store_init(void)
{
    s_nvs_mutex = xSemaphoreCreateMutex();
    configASSERT(s_nvs_mutex);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased (no free pages or version mismatch)");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "NVS initialised");
}

void nvs_store_erase_all(void)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "NVS namespace erased — factory reset");
    }
    xSemaphoreGive(s_nvs_mutex);
}

// ── Wizard / calibration ─────────────────────────────────────

bool    nvs_get_wiz_complete(void)  { return (bool)nvs_get_u8_default("wiz_complete", 0); }
void    nvs_set_wiz_complete(bool v){ nvs_set_u8_key("wiz_complete", (uint8_t)v); }

int32_t nvs_get_wiz_step(void)      { return nvs_get_i32_default("wiz_step", 0); }
void    nvs_set_wiz_step(int32_t v) { nvs_set_i32_key("wiz_step", v); }

int32_t nvs_get_cyl_type(void)      { return nvs_get_i32_default("cyl_type", 0); }
void    nvs_set_cyl_type(int32_t v) { nvs_set_i32_key("cyl_type", v); }

int32_t nvs_get_heat_type(void)     { return nvs_get_i32_default("heat_type", 1); }  // default direct
void    nvs_set_heat_type(int32_t v){ nvs_set_i32_key("heat_type", v); }

float   nvs_get_imm_kw(void)        { return nvs_get_float_default("imm_kw", 3.0f); }
void    nvs_set_imm_kw(float v)     { nvs_set_float_key("imm_kw", v); }

int32_t nvs_get_imm_count(void)     { return nvs_get_i32_default("imm_count", 1); }
void    nvs_set_imm_count(int32_t v){ nvs_set_i32_key("imm_count", v); }

int32_t nvs_get_tank_size(void)     { return nvs_get_i32_default("tank_size", 150); }
void    nvs_set_tank_size(int32_t v){ nvs_set_i32_key("tank_size", v); }

uint8_t nvs_get_tank_actual(void)   { return nvs_get_u8_default("tank_actual", 0); }
void    nvs_set_tank_actual(uint8_t v){ nvs_set_u8_key("tank_actual", v); }

float   nvs_get_bl_cold(void)       { return nvs_get_float_default("bl_cold", 14.0f); }
void    nvs_set_bl_cold(float v)    { nvs_set_float_key("bl_cold", v); }

float   nvs_get_bl_hot(void)        { return nvs_get_float_default("bl_hot", 65.0f); }
void    nvs_set_bl_hot(float v)     { nvs_set_float_key("bl_hot", v); }

float   nvs_get_bl_post_draw(void)  { return nvs_get_float_default("bl_post_draw", 55.0f); }
void    nvs_set_bl_post_draw(float v){ nvs_set_float_key("bl_post_draw", v); }

// ── Profiles ─────────────────────────────────────────────────

int32_t nvs_get_active_profile(void)     { return nvs_get_i32_default("active_profile", 0); }
void    nvs_set_active_profile(int32_t v){ nvs_set_i32_key("active_profile", v); }

static const char *default_profile_names[3] = {"Alex", "Sam", "Guest"};

void nvs_get_profile_name(int pid, char *buf, size_t buf_len)
{
    char key[16];
    profile_key(key, pid, "name");
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_ro();
    if (h) {
        size_t len = buf_len;
        esp_err_t err = nvs_get_str(h, key, buf, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            xSemaphoreGive(s_nvs_mutex);
            return;
        }
    }
    xSemaphoreGive(s_nvs_mutex);
    // Default name
    const char *def = (pid >= 0 && pid < 3) ? default_profile_names[pid] : "Profile";
    strncpy(buf, def, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

void nvs_set_profile_name(int pid, const char *name)
{
    char key[16];
    profile_key(key, pid, "name");
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        nvs_set_str(h, key, name);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
}

bool nvs_get_profile_eco(int pid)
{
    char key[16];
    profile_key(key, pid, "eco");
    return (bool)nvs_get_u8_default(key, 0);
}

void nvs_set_profile_eco(int pid, bool eco)
{
    char key[16];
    profile_key(key, pid, "eco");
    nvs_set_u8_key(key, (uint8_t)eco);
}

float nvs_get_preset(int pid, const char *preset_name)
{
    char key[16];
    profile_key(key, pid, preset_name);
    return nvs_get_float_default(key, preset_default(pid, preset_name));
}

void nvs_set_preset(int pid, const char *preset_name, float litres)
{
    char key[16];
    profile_key(key, pid, preset_name);
    nvs_set_float_key(key, litres);
}

// ── Energy ───────────────────────────────────────────────────

void nvs_get_energy_supplier(char *buf, size_t buf_len)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_ro();
    if (h) {
        size_t len = buf_len;
        esp_err_t err = nvs_get_str(h, "energy_supplier", buf, &len);
        nvs_close(h);
        if (err == ESP_OK) {
            xSemaphoreGive(s_nvs_mutex);
            return;
        }
    }
    xSemaphoreGive(s_nvs_mutex);
    strncpy(buf, "Unknown", buf_len - 1);
    buf[buf_len - 1] = '\0';
}

void nvs_set_energy_supplier(const char *supplier)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        nvs_set_str(h, "energy_supplier", supplier);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_nvs_mutex);
}

float nvs_get_on_peak_ppm(void)      { return nvs_get_float_default("on_peak_ppm", 0.29f); }
void  nvs_set_on_peak_ppm(float v)   { nvs_set_float_key("on_peak_ppm", v); }

float nvs_get_off_peak_ppm(void)     { return nvs_get_float_default("off_peak_ppm", 0.12f); }
void  nvs_set_off_peak_ppm(float v)  { nvs_set_float_key("off_peak_ppm", v); }

float nvs_get_super_off_ppm(void)    { return nvs_get_float_default("super_off_ppm", 0.08f); }
void  nvs_set_super_off_ppm(float v) { nvs_set_float_key("super_off_ppm", v); }

// ── Sensor ROM mapping ──────────────────────────────────────

bool nvs_get_sensor_roms(uint8_t roms[4][8])
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_ro();
    bool ok = false;
    if (h) {
        size_t len = 32;  // 4 × 8 bytes
        esp_err_t err = nvs_get_blob(h, "sr_roms", roms, &len);
        nvs_close(h);
        if (err == ESP_OK && len == 32) {
            // Validate: all 4 ROMs must start with DS18B20 family code 0x28
            ok = (roms[0][0] == 0x28 && roms[1][0] == 0x28 &&
                  roms[2][0] == 0x28 && roms[3][0] == 0x28);
        }
    }
    xSemaphoreGive(s_nvs_mutex);
    return ok;
}

void nvs_set_sensor_roms(const uint8_t roms[4][8])
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        esp_err_t err = nvs_set_blob(h, "sr_roms", roms, 32);
        if (err == ESP_OK) nvs_commit(h);
        nvs_close(h);
        ESP_LOGI("nvs_store", "Sensor ROMs saved to NVS (32 bytes)");
    }
    xSemaphoreGive(s_nvs_mutex);
}

void nvs_clear_sensor_roms(void)
{
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    nvs_handle_t h = nvs_open_rw();
    if (h) {
        nvs_erase_key(h, "sr_roms");
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI("nvs_store", "Sensor ROMs cleared from NVS");
    }
    xSemaphoreGive(s_nvs_mutex);
}

int32_t nvs_get_ow_gpio(void)
{
    return nvs_get_i32_default("ow_gpio", 7);  // Default GPIO 7 per user hardware
}

void nvs_set_ow_gpio(int32_t pin)
{
    nvs_set_i32_key("ow_gpio", pin);
}
