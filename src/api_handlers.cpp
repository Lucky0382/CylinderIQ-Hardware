// CylinderIQ Hub V2 — api_handlers.cpp (ESP32-S3)
// All REST endpoint logic. Reads sensors/NVS, returns allocated JSON strings.
// V1 /sensor/Name endpoints provided for DisplayIQ V2 ESPHome compatibility.

#include "api_handlers.h"
#include "calculations.h"
#include "nvs_store.h"
#include "sensors.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "api";

// ──────────────────────────────────────────────────────────────
// Helpers
// ──────────────────────────────────────────────────────────────

// Returns heap-allocated cJSON_PrintUnformatted string. Caller must free.
static char *json_to_str(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;  // may be NULL if OOM
}

static char *make_error(const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return strdup(buf);
}

// URL decode %20 etc. in-place (simple, handles %XX only).
static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t i = 0;
    size_t j = 0;
    while (src[i] && j < dst_len - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = {src[i+1], src[i+2], '\0'};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// Build sensor JSON object (reused by /sensors and /state)
static cJSON *build_sensors_json(const calc_result_t *r)
{
    cJSON *s = cJSON_CreateObject();
    cJSON_AddNumberToObject(s, "hot_outlet",        r->hot_outlet);
    cJSON_AddNumberToObject(s, "cylinder_inlet",    r->cylinder_inlet);
    cJSON_AddNumberToObject(s, "mains_supply",      r->mains_supply);
    cJSON_AddNumberToObject(s, "tundish",           r->tundish);
    cJSON_AddBoolToObject(  s, "leak_wet",          r->leak_wet);
    cJSON_AddNumberToObject(s, "usable_hot_litres", r->usable_litres);
    cJSON_AddNumberToObject(s, "hot_water_pct",     r->hot_pct);
    cJSON_AddNumberToObject(s, "showers_remaining", r->showers_remaining);
    cJSON_AddNumberToObject(s, "baths_remaining",   r->baths_remaining);
    cJSON_AddNumberToObject(s, "recovery_min",      r->recovery_min);
    cJSON_AddNumberToObject(s, "cost_pence",        r->cost_pence);
    return s;
}

// Build profile JSON object for profile id
static cJSON *build_profile_json(int pid)
{
    char name[25];
    nvs_get_profile_name(pid, name, sizeof(name));

    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id",   pid);
    cJSON_AddStringToObject(p, "name", name);
    cJSON_AddBoolToObject(p,   "eco",  nvs_get_profile_eco(pid));

    cJSON *presets = cJSON_CreateObject();
    cJSON_AddNumberToObject(presets, "shower",   nvs_get_preset(pid, "shower"));
    cJSON_AddNumberToObject(presets, "bath",     nvs_get_preset(pid, "bath"));
    cJSON_AddNumberToObject(presets, "washing",  nvs_get_preset(pid, "washing"));
    cJSON_AddNumberToObject(presets, "cleaning", nvs_get_preset(pid, "cleaning"));
    cJSON_AddNumberToObject(presets, "custom",   nvs_get_preset(pid, "custom"));
    cJSON_AddItemToObject(p, "presets", presets);

    return p;
}

// ──────────────────────────────────────────────────────────────
// V2 Handler functions — each returns allocated JSON string
// ──────────────────────────────────────────────────────────────

// GET /sensors
static char *handle_get_sensors(void)
{
    calc_result_t r = calc_get();
    if (!r.valid) return make_error("sensor data not yet available");
    return json_to_str(build_sensors_json(&r));
}

// GET /state
static char *handle_get_state(void)
{
    calc_result_t r = calc_get();
    cJSON *root = cJSON_CreateObject();

    // sensors
    cJSON_AddItemToObject(root, "sensors", build_sensors_json(&r));

    // wizard
    cJSON *wiz = cJSON_CreateObject();
    cJSON_AddBoolToObject(wiz,   "complete",      nvs_get_wiz_complete());
    cJSON_AddNumberToObject(wiz, "step",          nvs_get_wiz_step());
    cJSON_AddNumberToObject(wiz, "cylinder_type", nvs_get_cyl_type());
    cJSON_AddNumberToObject(wiz, "heating_type",  nvs_get_heat_type());
    cJSON_AddNumberToObject(wiz, "immersion_kw",  nvs_get_imm_kw());
    cJSON_AddNumberToObject(wiz, "immersion_count", nvs_get_imm_count());
    cJSON_AddNumberToObject(wiz, "tank_size_l",   nvs_get_tank_size());
    cJSON_AddNumberToObject(wiz, "baseline_cold", nvs_get_bl_cold());
    cJSON_AddNumberToObject(wiz, "baseline_hot",  nvs_get_bl_hot());
    cJSON_AddNumberToObject(wiz, "baseline_post_draw", nvs_get_bl_post_draw());
    cJSON_AddItemToObject(root, "wizard", wiz);

    // profiles
    cJSON *profiles = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
        cJSON_AddItemToArray(profiles, build_profile_json(i));
    }
    cJSON_AddItemToObject(root, "profiles", profiles);
    cJSON_AddNumberToObject(root, "active_profile", nvs_get_active_profile());

    // energy
    char supplier[33];
    nvs_get_energy_supplier(supplier, sizeof(supplier));
    cJSON *energy = cJSON_CreateObject();
    cJSON_AddStringToObject(energy, "supplier",         supplier);
    cJSON_AddNumberToObject(energy, "on_peak_ppm",      nvs_get_on_peak_ppm());
    cJSON_AddNumberToObject(energy, "off_peak_ppm",     nvs_get_off_peak_ppm());
    cJSON_AddNumberToObject(energy, "super_off_peak_ppm", nvs_get_super_off_ppm());
    cJSON_AddItemToObject(root, "energy", energy);

    return json_to_str(root);
}

// GET /profiles
static char *handle_get_profiles(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) {
        cJSON_AddItemToArray(arr, build_profile_json(i));
    }
    return json_to_str(arr);
}

// POST /profiles/{id}   body: {"name":"Alex","eco":false}
static char *handle_post_profile(int pid, const char *body, int *status)
{
    if (pid < 0 || pid > 2) { *status = 400; return make_error("invalid profile id"); }
    cJSON *req = cJSON_Parse(body);
    if (!req) { *status = 400; return make_error("bad JSON"); }

    cJSON *jname = cJSON_GetObjectItem(req, "name");
    cJSON *jeco  = cJSON_GetObjectItem(req, "eco");

    if (jname && cJSON_IsString(jname)) nvs_set_profile_name(pid, jname->valuestring);
    if (jeco  && cJSON_IsBool(jeco))    nvs_set_profile_eco(pid, cJSON_IsTrue(jeco));

    cJSON_Delete(req);
    return strdup("{\"ok\":true}");
}

// POST /active_profile   body: {"profile_id":0}
static char *handle_post_active_profile(const char *body, int *status)
{
    cJSON *req = cJSON_Parse(body);
    if (!req) { *status = 400; return make_error("bad JSON"); }
    cJSON *jpid = cJSON_GetObjectItem(req, "profile_id");
    if (!jpid || !cJSON_IsNumber(jpid)) {
        cJSON_Delete(req);
        *status = 400;
        return make_error("missing profile_id");
    }
    int pid = (int)jpid->valuedouble;
    cJSON_Delete(req);
    if (pid < 0 || pid > 2) { *status = 400; return make_error("invalid profile_id"); }
    nvs_set_active_profile(pid);
    return strdup("{\"ok\":true}");
}

// GET /presets/{pid}
static char *handle_get_presets(int pid, int *status)
{
    if (pid < 0 || pid > 2) { *status = 404; return make_error("not found"); }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "profile_id", pid);
    cJSON *presets = cJSON_CreateObject();
    cJSON_AddNumberToObject(presets, "shower",   nvs_get_preset(pid, "shower"));
    cJSON_AddNumberToObject(presets, "bath",     nvs_get_preset(pid, "bath"));
    cJSON_AddNumberToObject(presets, "washing",  nvs_get_preset(pid, "washing"));
    cJSON_AddNumberToObject(presets, "cleaning", nvs_get_preset(pid, "cleaning"));
    cJSON_AddNumberToObject(presets, "custom",   nvs_get_preset(pid, "custom"));
    cJSON_AddItemToObject(root, "presets", presets);
    return json_to_str(root);
}

// POST /presets/{pid}/{name}   body: {"litres":42.0}
static char *handle_post_preset(int pid, const char *preset_name,
                                 const char *body, int *status)
{
    if (pid < 0 || pid > 2) { *status = 400; return make_error("invalid profile id"); }
    cJSON *req = cJSON_Parse(body);
    if (!req) { *status = 400; return make_error("bad JSON"); }
    cJSON *jl = cJSON_GetObjectItem(req, "litres");
    if (!jl || !cJSON_IsNumber(jl)) {
        cJSON_Delete(req);
        *status = 400;
        return make_error("missing litres");
    }
    nvs_set_preset(pid, preset_name, (float)jl->valuedouble);
    cJSON_Delete(req);
    return strdup("{\"ok\":true}");
}

// GET /wizard
static char *handle_get_wizard(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "complete",      nvs_get_wiz_complete());
    cJSON_AddNumberToObject(root, "step",          nvs_get_wiz_step());
    cJSON_AddNumberToObject(root, "cylinder_type", nvs_get_cyl_type());
    cJSON_AddNumberToObject(root, "heating_type",  nvs_get_heat_type());
    cJSON_AddNumberToObject(root, "immersion_kw",  nvs_get_imm_kw());
    cJSON_AddNumberToObject(root, "immersion_count", nvs_get_imm_count());
    cJSON_AddNumberToObject(root, "tank_size_l",   nvs_get_tank_size());
    cJSON_AddNumberToObject(root, "baseline_cold", nvs_get_bl_cold());
    cJSON_AddNumberToObject(root, "baseline_hot",  nvs_get_bl_hot());
    cJSON_AddNumberToObject(root, "baseline_post_draw", nvs_get_bl_post_draw());
    return json_to_str(root);
}

// POST /wizard/step  body: {"step":3,"cylinder_type":2,...}
static char *handle_post_wizard_step(const char *body, int *status)
{
    cJSON *req = cJSON_Parse(body);
    if (!req) { *status = 400; return make_error("bad JSON"); }

    cJSON *j;
    if ((j = cJSON_GetObjectItem(req, "step"))           && cJSON_IsNumber(j)) nvs_set_wiz_step((int32_t)j->valuedouble);
    if ((j = cJSON_GetObjectItem(req, "cylinder_type"))  && cJSON_IsNumber(j)) nvs_set_cyl_type((int32_t)j->valuedouble);
    if ((j = cJSON_GetObjectItem(req, "heating_type"))   && cJSON_IsNumber(j)) nvs_set_heat_type((int32_t)j->valuedouble);
    if ((j = cJSON_GetObjectItem(req, "immersion_kw"))   && cJSON_IsNumber(j)) nvs_set_imm_kw((float)j->valuedouble);
    if ((j = cJSON_GetObjectItem(req, "immersion_count"))&& cJSON_IsNumber(j)) nvs_set_imm_count((int32_t)j->valuedouble);
    if ((j = cJSON_GetObjectItem(req, "tank_size_l"))    && cJSON_IsNumber(j)) nvs_set_tank_size((int32_t)j->valuedouble);

    cJSON_Delete(req);
    return strdup("{\"ok\":true}");
}

// POST /wizard/complete
static char *handle_post_wizard_complete(void)
{
    nvs_set_wiz_complete(true);
    nvs_set_wiz_step(10);
    return strdup("{\"ok\":true}");
}

// POST /calibrate/cold — store current mains_supply as cold baseline
static char *handle_calibrate_cold(int *status)
{
    sensor_readings_t s = sensors_get();
    if (!s.valid) { *status = 503; return make_error("sensor not ready"); }
    nvs_set_bl_cold(s.mains_supply);
    ESP_LOGI(TAG, "Cold baseline set: %.2f C", s.mains_supply);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"baseline_cold\":%.2f}", s.mains_supply);
    return strdup(buf);
}

// POST /calibrate/hot — store current hot_outlet as hot baseline
static char *handle_calibrate_hot(int *status)
{
    sensor_readings_t s = sensors_get();
    if (!s.valid) { *status = 503; return make_error("sensor not ready"); }
    nvs_set_bl_hot(s.hot_outlet);
    ESP_LOGI(TAG, "Hot baseline set: %.2f C", s.hot_outlet);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"baseline_hot\":%.2f}", s.hot_outlet);
    return strdup(buf);
}

// POST /calibrate/post_draw — store current hot_outlet as post-draw baseline
static char *handle_calibrate_post_draw(int *status)
{
    sensor_readings_t s = sensors_get();
    if (!s.valid) { *status = 503; return make_error("sensor not ready"); }
    nvs_set_bl_post_draw(s.hot_outlet);
    ESP_LOGI(TAG, "Post-draw baseline set: %.2f C", s.hot_outlet);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"baseline_post_draw\":%.2f}", s.hot_outlet);
    return strdup(buf);
}

// GET /energy
static char *handle_get_energy(void)
{
    char supplier[33];
    nvs_get_energy_supplier(supplier, sizeof(supplier));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "supplier",           supplier);
    cJSON_AddNumberToObject(root, "on_peak_ppm",        nvs_get_on_peak_ppm());
    cJSON_AddNumberToObject(root, "off_peak_ppm",       nvs_get_off_peak_ppm());
    cJSON_AddNumberToObject(root, "super_off_peak_ppm", nvs_get_super_off_ppm());
    return json_to_str(root);
}

// POST /energy  body: {"supplier":"Octopus","on_peak_ppm":0.29,...}
static char *handle_post_energy(const char *body, int *status)
{
    cJSON *req = cJSON_Parse(body);
    if (!req) { *status = 400; return make_error("bad JSON"); }
    cJSON *j;
    if ((j = cJSON_GetObjectItem(req, "supplier"))           && cJSON_IsString(j)) nvs_set_energy_supplier(j->valuestring);
    if ((j = cJSON_GetObjectItem(req, "on_peak_ppm"))        && cJSON_IsNumber(j)) nvs_set_on_peak_ppm((float)j->valuedouble);
    if ((j = cJSON_GetObjectItem(req, "off_peak_ppm"))       && cJSON_IsNumber(j)) nvs_set_off_peak_ppm((float)j->valuedouble);
    if ((j = cJSON_GetObjectItem(req, "super_off_peak_ppm")) && cJSON_IsNumber(j)) nvs_set_super_off_ppm((float)j->valuedouble);
    cJSON_Delete(req);
    return strdup("{\"ok\":true}");
}

// POST /learn/start  body: {"profile_id":0,"preset":0}
// V2 records water level at start — actual learning stored externally
static float s_learn_water_start = 0.0f;

static char *handle_learn_start(const char *body)
{
    calc_result_t r = calc_get();
    s_learn_water_start = r.usable_litres;
    ESP_LOGI(TAG, "Learn start: %.0f L in tank", s_learn_water_start);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"water_start_litres\":%.1f}", s_learn_water_start);
    return strdup(buf);
}

// POST /learn/stop  → returns litres consumed
static char *handle_learn_stop(void)
{
    calc_result_t r = calc_get();
    float consumed = s_learn_water_start - r.usable_litres;
    if (consumed < 0.0f) consumed = 0.0f;
    ESP_LOGI(TAG, "Learn stop: consumed %.1f L", consumed);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"litres\":%.1f}", consumed);
    return strdup(buf);
}

// ──────────────────────────────────────────────────────────────
// V1 compatibility — /sensor/Name%20With%20Spaces
// Returns {"value":N,"state":"N"}
// ──────────────────────────────────────────────────────────────

static char *handle_v1_sensor(const char *sensor_name_decoded, int *status)
{
    calc_result_t r = calc_get();
    float val = 0.0f;
    bool found = true;

    if      (strcmp(sensor_name_decoded, "Hot Outlet")             == 0) val = r.hot_outlet;
    else if (strcmp(sensor_name_decoded, "Cylinder Inlet")         == 0) val = r.cylinder_inlet;
    else if (strcmp(sensor_name_decoded, "Mains Supply")           == 0) val = r.mains_supply;
    else if (strcmp(sensor_name_decoded, "Tundish")                == 0) val = r.tundish;
    else if (strcmp(sensor_name_decoded, "Usable Hot Water")       == 0) val = r.usable_litres;
    else if (strcmp(sensor_name_decoded, "Hot Water Percentage")   == 0) val = r.hot_pct;
    else if (strcmp(sensor_name_decoded, "Showers Remaining")      == 0) val = r.showers_remaining;
    else if (strcmp(sensor_name_decoded, "Baths Remaining")        == 0) val = r.baths_remaining;
    else if (strcmp(sensor_name_decoded, "Recovery Time Remaining")== 0) val = r.recovery_min;
    else if (strcmp(sensor_name_decoded, "Cost to Recover Now")    == 0) val = r.cost_pence;
    else {
        found = false;
    }

    if (!found) {
        *status = 404;
        return make_error("unknown sensor");
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"value\":%.2f,\"state\":\"%.2f\"}", val, val);
    return strdup(buf);
}

// ──────────────────────────────────────────────────────────────
// Main dispatcher
// ──────────────────────────────────────────────────────────────

void api_dispatch(const char *method,
                  const char *path,
                  const char *body,
                  char      **resp_body,
                  int        *resp_status)
{
    *resp_status = 200;
    *resp_body   = NULL;

    // ── V1 compatibility: GET /sensor/<name> ─────────────────
    if (strncmp(path, "/sensor/", 8) == 0) {
        char decoded[128];
        url_decode(decoded, path + 8, sizeof(decoded));
        ESP_LOGD(TAG, "V1 sensor: '%s'", decoded);
        *resp_body = handle_v1_sensor(decoded, resp_status);
        return;
    }

    // ── V2 routes ─────────────────────────────────────────────

    if (strcmp(path, "/sensors") == 0 && strcmp(method, "GET") == 0) {
        *resp_body = handle_get_sensors();
        return;
    }

    if (strcmp(path, "/state") == 0 && strcmp(method, "GET") == 0) {
        *resp_body = handle_get_state();
        return;
    }

    if (strcmp(path, "/profiles") == 0 && strcmp(method, "GET") == 0) {
        *resp_body = handle_get_profiles();
        return;
    }

    // POST /profiles/{id}
    if (strncmp(path, "/profiles/", 10) == 0 && strcmp(method, "POST") == 0) {
        int pid = atoi(path + 10);
        *resp_body = handle_post_profile(pid, body, resp_status);
        return;
    }

    if (strcmp(path, "/active_profile") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_post_active_profile(body, resp_status);
        return;
    }

    // GET /presets/{pid}
    if (strncmp(path, "/presets/", 9) == 0 && strcmp(method, "GET") == 0) {
        // Check if it's /presets/{pid} (no trailing slash) or /presets/{pid}/{name}
        const char *after = path + 9;
        const char *slash = strchr(after, '/');
        if (slash == NULL) {
            int pid = atoi(after);
            *resp_body = handle_get_presets(pid, resp_status);
        } else {
            *resp_status = 405;
            *resp_body = make_error("use POST to update preset");
        }
        return;
    }

    // POST /presets/{pid}/{name}
    if (strncmp(path, "/presets/", 9) == 0 && strcmp(method, "POST") == 0) {
        const char *after = path + 9;
        const char *slash = strchr(after, '/');
        if (slash) {
            char pid_str[8];
            size_t pid_len = (size_t)(slash - after);
            if (pid_len < sizeof(pid_str)) {
                memcpy(pid_str, after, pid_len);
                pid_str[pid_len] = '\0';
                int pid = atoi(pid_str);
                *resp_body = handle_post_preset(pid, slash + 1, body, resp_status);
            } else {
                *resp_status = 400;
                *resp_body = make_error("malformed path");
            }
        } else {
            *resp_status = 400;
            *resp_body = make_error("missing preset name in path");
        }
        return;
    }

    if (strcmp(path, "/wizard") == 0 && strcmp(method, "GET") == 0) {
        *resp_body = handle_get_wizard();
        return;
    }

    if (strcmp(path, "/wizard/step") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_post_wizard_step(body, resp_status);
        return;
    }

    if (strcmp(path, "/wizard/complete") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_post_wizard_complete();
        return;
    }

    if (strcmp(path, "/calibrate/cold") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_calibrate_cold(resp_status);
        return;
    }

    if (strcmp(path, "/calibrate/hot") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_calibrate_hot(resp_status);
        return;
    }

    if (strcmp(path, "/calibrate/post_draw") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_calibrate_post_draw(resp_status);
        return;
    }

    if (strcmp(path, "/energy") == 0 && strcmp(method, "GET") == 0) {
        *resp_body = handle_get_energy();
        return;
    }

    if (strcmp(path, "/energy") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_post_energy(body, resp_status);
        return;
    }

    if (strcmp(path, "/learn/start") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_learn_start(body);
        return;
    }

    if (strcmp(path, "/learn/stop") == 0 && strcmp(method, "POST") == 0) {
        *resp_body = handle_learn_stop();
        return;
    }

    // Not found
    ESP_LOGW(TAG, "No handler for %s %s", method, path);
    *resp_status = 404;
    *resp_body   = make_error("not found");
}
