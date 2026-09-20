// CylinderIQ Hub V2 — ble_server.cpp (ESP32-S3)
//
// NimBLE GATT server with three characteristics:
//   Sensor Notify  (S3 → App): live /sensors JSON every 5s
//   Command Write  (App → S3): wizard/config/calibration JSON commands
//   Response Notify(S3 → App): per-command ACK with result fields
//
// Command write dispatches directly into api_dispatch() — all business
// logic stays in api_handlers, zero duplication here.

#include "ble_server.h"
#include "api_handlers.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

static const char *TAG = "ble_server";

// ── Connection tracking ──────────────────────────────────────────────────────

static uint16_t s_conn_handle  = BLE_HS_CONN_HANDLE_NONE;

// ── Characteristic value handles (set by ble_gatts_add_svcs) ────────────────

static uint16_t s_sensor_val_handle = 0;
static uint16_t s_resp_val_handle   = 0;

// ── UUIDs — 128-bit little-endian (NimBLE byte order) ───────────────────────
// Canonical:    4cad87f1-00XX-0000-0000-000000000001
// BLE_UUID128_INIT reverses the canonical UUID byte order.

// Service: 4cad87f1-0000-0000-0000-000000000001
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00, 0xf1,0x87,0xad,0x4c);

// Sensor Notify: 4cad87f1-0001-0000-0000-000000000001
static const ble_uuid128_t s_sensor_uuid = BLE_UUID128_INIT(
    0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x01,0x00, 0xf1,0x87,0xad,0x4c);

// Command Write: 4cad87f1-0002-0000-0000-000000000001
static const ble_uuid128_t s_cmd_uuid = BLE_UUID128_INIT(
    0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x02,0x00, 0xf1,0x87,0xad,0x4c);

// Response Notify: 4cad87f1-0003-0000-0000-000000000001
static const ble_uuid128_t s_resp_uuid = BLE_UUID128_INIT(
    0x01,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,
    0x00,0x00,0x03,0x00, 0xf1,0x87,0xad,0x4c);

// ── Forward declarations ─────────────────────────────────────────────────────

static void ble_server_start_adv(void);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);

// ── Command dispatcher ───────────────────────────────────────────────────────
//
// Translates BLE command JSON → (method, path, body) → api_dispatch().
// Returns a heap-allocated response JSON string; caller must free().
// Response always includes the original "cmd" field for client correlation.

static void handle_ble_command(const char *json_in, char **resp_out)
{
    *resp_out = NULL;

    cJSON *msg = cJSON_Parse(json_in);
    if (!msg) {
        *resp_out = strdup("{\"ok\":false,\"err\":\"json_parse\"}");
        return;
    }

    cJSON *jcmd = cJSON_GetObjectItem(msg, "cmd");
    if (!cJSON_IsString(jcmd)) {
        cJSON_Delete(msg);
        *resp_out = strdup("{\"ok\":false,\"err\":\"no_cmd\"}");
        return;
    }

    const char *cmd    = jcmd->valuestring;
    const char *method = "POST";
    char        path[64];

    // Build body: duplicate msg then remove routing fields
    cJSON *body = cJSON_Duplicate(msg, /*recurse=*/true);
    cJSON_DeleteItemFromObject(body, "cmd");

    // Map cmd → REST path (and strip path-embedded fields from body)
    if (strcmp(cmd, "wizard_step") == 0) {
        strncpy(path, "/wizard/step", sizeof(path));

    } else if (strcmp(cmd, "wizard_complete") == 0) {
        strncpy(path, "/wizard/complete", sizeof(path));

    } else if (strcmp(cmd, "energy") == 0) {
        strncpy(path, "/energy", sizeof(path));

    } else if (strcmp(cmd, "calibrate_cold") == 0) {
        strncpy(path, "/calibrate/cold", sizeof(path));

    } else if (strcmp(cmd, "calibrate_hot") == 0) {
        strncpy(path, "/calibrate/hot", sizeof(path));

    } else if (strcmp(cmd, "calibrate_post_draw") == 0) {
        strncpy(path, "/calibrate/post_draw", sizeof(path));

    } else if (strcmp(cmd, "active_profile") == 0) {
        strncpy(path, "/active_profile", sizeof(path));

    } else if (strcmp(cmd, "update_profile") == 0) {
        cJSON *jid = cJSON_GetObjectItem(msg, "id");
        int id = cJSON_IsNumber(jid) ? (int)jid->valuedouble : 0;
        snprintf(path, sizeof(path), "/profiles/%d", id);
        cJSON_DeleteItemFromObject(body, "id");

    } else if (strcmp(cmd, "set_preset") == 0) {
        cJSON *jpid   = cJSON_GetObjectItem(msg, "profile_id");
        cJSON *jpname = cJSON_GetObjectItem(msg, "preset");
        int pid          = cJSON_IsNumber(jpid)  ? (int)jpid->valuedouble    : 0;
        const char *pname = cJSON_IsString(jpname) ? jpname->valuestring : "custom";
        snprintf(path, sizeof(path), "/presets/%d/%s", pid, pname);
        cJSON_DeleteItemFromObject(body, "profile_id");
        cJSON_DeleteItemFromObject(body, "preset");

    } else if (strcmp(cmd, "learn_start") == 0) {
        strncpy(path, "/learn/start", sizeof(path));

    } else if (strcmp(cmd, "learn_stop") == 0) {
        strncpy(path, "/learn/stop", sizeof(path));

    } else {
        cJSON_Delete(body);
        cJSON_Delete(msg);
        *resp_out = strdup("{\"ok\":false,\"err\":\"unknown_cmd\"}");
        return;
    }

    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    cJSON_Delete(msg);

    // Dispatch to REST handler — all business logic lives there
    char *api_resp  = NULL;
    int   api_status = 200;
    api_dispatch(method, path, body_str ? body_str : "{}", &api_resp, &api_status);
    if (body_str) free(body_str);

    ESP_LOGD(TAG, "BLE cmd=%s → %s %s → status=%d", cmd, method, path, api_status);

    // Wrap response with cmd field for client correlation
    cJSON *resp_obj = (api_resp && api_resp[0] != '\0') ? cJSON_Parse(api_resp) : NULL;
    if (api_resp) free(api_resp);
    if (!resp_obj) resp_obj = cJSON_CreateObject();

    // Ensure "ok" is present even if api_dispatch returned non-JSON
    if (!cJSON_GetObjectItem(resp_obj, "ok")) {
        cJSON_AddBoolToObject(resp_obj, "ok", api_status == 200);
    }
    cJSON_AddStringToObject(resp_obj, "cmd", cmd);

    *resp_out = cJSON_PrintUnformatted(resp_obj);
    cJSON_Delete(resp_obj);
}

// ── GATT characteristic callbacks ────────────────────────────────────────────

static int sensor_chr_access_cb(uint16_t conn_hdl, uint16_t attr_hdl,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // Notify-only — no read data
    return 0;
}

static int resp_chr_access_cb(uint16_t conn_hdl, uint16_t attr_hdl,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

static int cmd_chr_access_cb(uint16_t conn_hdl, uint16_t attr_hdl,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > 512) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    char *buf = (char *)malloc(len + 1);
    if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;

    ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
    buf[len] = '\0';
    ESP_LOGI(TAG, "BLE CMD [%d B]: %.80s", len, buf);

    char *resp = NULL;
    handle_ble_command(buf, &resp);
    free(buf);

    // Notify response characteristic
    if (resp && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(resp, strlen(resp));
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn_handle, s_resp_val_handle, om);
            if (rc != 0) {
                ESP_LOGW(TAG, "resp notify failed: %d", rc);
            } else {
                ESP_LOGD(TAG, "BLE RESP: %.80s", resp);
            }
        }
        free(resp);
    } else if (resp) {
        free(resp);
    }

    return 0;
}

// ── GATT service table ───────────────────────────────────────────────────────

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            // Sensor Notify — S3 pushes sensor JSON every 5s
            {
                .uuid       = &s_sensor_uuid.u,
                .access_cb  = sensor_chr_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_sensor_val_handle,
            },
            // Command Write — App sends wizard/config/calibration commands
            {
                .uuid      = &s_cmd_uuid.u,
                .access_cb = cmd_chr_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE,
            },
            // Response Notify — S3 ACKs every command
            {
                .uuid       = &s_resp_uuid.u,
                .access_cb  = resp_chr_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_resp_val_handle,
            },
            { 0 }, // terminator
        },
    },
    { 0 }, // terminator
};

// ── GAP advertising ──────────────────────────────────────────────────────────

static void ble_server_start_adv(void)
{
    struct ble_gap_adv_params adv_params = {};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  // undirected connectable
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  // general discoverable
    adv_params.itvl_min  = BLE_GAP_ADV_ITVL_MS(150); // 150 ms interval
    adv_params.itvl_max  = BLE_GAP_ADV_ITVL_MS(250); // 250 ms interval

    const char *name  = ble_svc_gap_device_name();

    // Primary advertising data: flags + device name (so name is instantly visible to all scanners)
    struct ble_hs_adv_fields fields = {};
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name                  = (const uint8_t *)name;
    fields.name_len              = (uint8_t)strlen(name);
    fields.name_is_complete      = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields failed: %d", rc);
    }

    // Scan response: 128-bit service UUID (for app filtering)
    struct ble_hs_adv_fields rsp = {};
    rsp.uuids128                 = (ble_uuid128_t *)&s_svc_uuid;
    rsp.num_uuids128             = 1;
    rsp.uuids128_is_complete     = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE advertising as \"%s\"", name);
    } else {
        ESP_LOGW(TAG, "ble_gap_adv_start failed: %d (will retry on next disconnect)", rc);
    }
}

// ── GAP event handler ────────────────────────────────────────────────────────

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE client connected, conn_handle=%d", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "BLE connect failed, status=%d — restarting adv",
                     event->connect.status);
            ble_server_start_adv();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE client disconnected, reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_server_start_adv();
        break;

    default:
        break;
    }
    return 0;
}

// ── NimBLE host task + sync ──────────────────────────────────────────────────

static void on_sync(void)
{
    // Ensure a valid public address is available
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }
    ble_server_start_adv();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d — will re-sync", reason);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();  // blocks until nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ── Public API ───────────────────────────────────────────────────────────────

void ble_server_init(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.reset_cb       = on_reset;
    ble_hs_cfg.sync_cb        = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;  // no persistent bonding

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("CylinderIQ");

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }

    // Start NimBLE host on core 1, priority 5 (same as uart_rx — high priority)
    nimble_port_freertos_init(ble_host_task);
    ESP_LOGI(TAG, "BLE server initialised — service 4cad87f1-...-0001");
}

void ble_server_notify_sensors(const char *sensors_json)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (!sensors_json || sensors_json[0] == '\0') return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(sensors_json, strlen(sensors_json));
    if (!om) return;

    int rc = ble_gatts_notify_custom(s_conn_handle, s_sensor_val_handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "sensor notify failed: %d", rc);
    }
}
