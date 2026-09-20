// CylinderIQ Hub V2 — switch_control.cpp (ESP32-C6)
// HTTP handlers for Zigbee switch control.
// All endpoints handled LOCALLY on C6 — not forwarded to S3.

#include "switch_control.h"
#include "zigbee_coord.h"

#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "switch_ctrl";

// ── Helpers ───────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t *req, int status_code, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    switch (status_code) {
        case 200: httpd_resp_set_status(req, "200 OK");               break;
        case 400: httpd_resp_set_status(req, "400 Bad Request");      break;
        case 404: httpd_resp_set_status(req, "404 Not Found");        break;
        case 409: httpd_resp_set_status(req, "409 Conflict");         break;
        default:  httpd_resp_set_status(req, "500 Internal Error");   break;
    }
    return httpd_resp_send(req, json, strlen(json));
}

static cJSON *switch_to_json(switch_id_t sw)
{
    zb_switch_t st = zigbee_coord_switch_get(sw);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "paired", st.paired);
    cJSON_AddStringToObject(obj, "state", st.on ? "on" : "off");
    if (st.paired) {
        char addr_str[8];
        snprintf(addr_str, sizeof(addr_str), "0x%04X", st.short_addr);
        cJSON_AddStringToObject(obj, "addr", addr_str);
    }
    return obj;
}

// Read body from POST request (max 256 bytes)
static bool read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    if (req->content_len == 0 || req->content_len >= buf_len) return false;
    int r = httpd_req_recv(req, buf, req->content_len);
    if (r <= 0) return false;
    buf[r] = '\0';
    return true;
}

// Parse {"slot":"top"} or {"slot":"bottom"} → switch_id
static bool parse_slot(const char *json_str, switch_id_t *out)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return false;
    cJSON *jslot = cJSON_GetObjectItem(root, "slot");
    bool ok = false;
    if (jslot && cJSON_IsString(jslot)) {
        if (strcmp(jslot->valuestring, "top") == 0) {
            *out = SWITCH_TOP; ok = true;
        } else if (strcmp(jslot->valuestring, "bottom") == 0) {
            *out = SWITCH_BOTTOM; ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

// ── GET /switches ─────────────────────────────────────────────

esp_err_t handler_get_switches(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "top",    switch_to_json(SWITCH_TOP));
    cJSON_AddItemToObject(root, "bottom", switch_to_json(SWITCH_BOTTOM));

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t err = send_json(req, 200, s ? s : "{}");
    if (s) free(s);
    return err;
}

// ── POST /switch/top/on|off ───────────────────────────────────

esp_err_t handler_switch_top_on(httpd_req_t *req)
{
    bool ok = zigbee_coord_switch_set(SWITCH_TOP, true);
    return send_json(req, ok ? 200 : 409,
                     ok ? "{\"ok\":true}" : "{\"error\":\"top not paired\"}");
}

esp_err_t handler_switch_top_off(httpd_req_t *req)
{
    bool ok = zigbee_coord_switch_set(SWITCH_TOP, false);
    return send_json(req, ok ? 200 : 409,
                     ok ? "{\"ok\":true}" : "{\"error\":\"top not paired\"}");
}

// ── POST /switch/bottom/on|off ────────────────────────────────

esp_err_t handler_switch_bottom_on(httpd_req_t *req)
{
    bool ok = zigbee_coord_switch_set(SWITCH_BOTTOM, true);
    return send_json(req, ok ? 200 : 409,
                     ok ? "{\"ok\":true}" : "{\"error\":\"bottom not paired\"}");
}

esp_err_t handler_switch_bottom_off(httpd_req_t *req)
{
    bool ok = zigbee_coord_switch_set(SWITCH_BOTTOM, false);
    return send_json(req, ok ? 200 : 409,
                     ok ? "{\"ok\":true}" : "{\"error\":\"bottom not paired\"}");
}

// ── POST /switch/pair ─────────────────────────────────────────

esp_err_t handler_switch_pair(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_json(req, 400, "{\"error\":\"missing body\"}");
    }

    switch_id_t slot;
    if (!parse_slot(body, &slot)) {
        return send_json(req, 400, "{\"error\":\"slot must be top or bottom\"}");
    }

    zigbee_coord_permit_join(slot, 60);

    char resp[80];
    snprintf(resp, sizeof(resp),
             "{\"ok\":true,\"slot\":\"%s\",\"duration_s\":60}",
             slot == SWITCH_TOP ? "top" : "bottom");
    return send_json(req, 200, resp);
}

// ── GET /switch/pair ──────────────────────────────────────────

esp_err_t handler_get_pair_state(httpd_req_t *req)
{
    zb_pair_state_t ps = zigbee_coord_pair_state();
    char resp[96];
    if (ps.open) {
        snprintf(resp, sizeof(resp),
                 "{\"open\":true,\"slot\":\"%s\",\"remaining_s\":%d}",
                 ps.target == SWITCH_TOP ? "top" : "bottom",
                 ps.remaining_s);
    } else {
        snprintf(resp, sizeof(resp), "{\"open\":false}");
    }
    return send_json(req, 200, resp);
}

// ── POST /switch/clear ────────────────────────────────────────

esp_err_t handler_switch_clear(httpd_req_t *req)
{
    char body[256];
    if (!read_body(req, body, sizeof(body))) {
        return send_json(req, 400, "{\"error\":\"missing body\"}");
    }

    switch_id_t slot;
    if (!parse_slot(body, &slot)) {
        return send_json(req, 400, "{\"error\":\"slot must be top or bottom\"}");
    }

    zigbee_coord_clear(slot);
    return send_json(req, 200, "{\"ok\":true}");
}
