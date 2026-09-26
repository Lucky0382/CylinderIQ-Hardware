// CylinderIQ Hub V2 — switch_control.cpp (ESP32-C6)
// Handles Zigbee switch control requests received from S3 over UART.

#include "switch_control.h"
#include "zigbee_coord.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "switch_ctrl";

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

static const char *slot_to_str(switch_id_t sw)
{
    switch (sw) {
        case SWITCH_TOP:    return "top";
        case SWITCH_BOTTOM: return "bottom";
        case SWITCH_SHOWER: return "shower";
        case SWITCH_BATH:   return "bath";
        default:            return "unknown";
    }
}

static bool parse_slot(const char *json_str, switch_id_t *out)
{
    if (!json_str || !json_str[0]) return false;
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return false;
    cJSON *jslot = cJSON_GetObjectItem(root, "slot");
    bool ok = false;
    if (jslot && cJSON_IsString(jslot)) {
        if (strcmp(jslot->valuestring, "top") == 0) {
            *out = SWITCH_TOP; ok = true;
        } else if (strcmp(jslot->valuestring, "bottom") == 0) {
            *out = SWITCH_BOTTOM; ok = true;
        } else if (strcmp(jslot->valuestring, "shower") == 0) {
            *out = SWITCH_SHOWER; ok = true;
        } else if (strcmp(jslot->valuestring, "bath") == 0) {
            *out = SWITCH_BATH; ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

void switch_control_dispatch(const char *method,
                             const char *path,
                             const char *body,
                             int        *out_status,
                             char      **out_body)
{
    *out_status = 200;
    *out_body = NULL;

    // GET /switches
    if (strcmp(path, "/switches") == 0 && strcmp(method, "GET") == 0) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "top",    switch_to_json(SWITCH_TOP));
        cJSON_AddItemToObject(root, "bottom", switch_to_json(SWITCH_BOTTOM));
        cJSON_AddItemToObject(root, "shower", switch_to_json(SWITCH_SHOWER));
        cJSON_AddItemToObject(root, "bath",   switch_to_json(SWITCH_BATH));

        uint16_t pan_id = 0;
        uint8_t channel = 0;
        bool online = false;
        zigbee_coord_get_network_info(&pan_id, &channel, &online);

        cJSON_AddBoolToObject(root, "coordinator_online", online);
        char pan_str[16];
        snprintf(pan_str, sizeof(pan_str), "0x%04X", pan_id);
        cJSON_AddStringToObject(root, "pan_id", pan_str);
        cJSON_AddNumberToObject(root, "channel", channel);

        zb_pair_state_t ps = zigbee_coord_pair_state();
        cJSON_AddBoolToObject(root, "permit_join_open", ps.open);
        cJSON_AddNumberToObject(root, "remaining_s", ps.remaining_s);

        *out_body = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        *out_status = 200;
        return;
    }

    // POST /switch/top/on
    if (strcmp(path, "/switch/top/on") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_TOP, true);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"top not paired\"}");
        return;
    }

    // POST /switch/top/off
    if (strcmp(path, "/switch/top/off") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_TOP, false);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"top not paired\"}");
        return;
    }

    // POST /switch/bottom/on
    if (strcmp(path, "/switch/bottom/on") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_BOTTOM, true);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"bottom not paired\"}");
        return;
    }

    // POST /switch/bottom/off
    if (strcmp(path, "/switch/bottom/off") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_BOTTOM, false);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"bottom not paired\"}");
        return;
    }

    // POST /switch/shower/on
    if (strcmp(path, "/switch/shower/on") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_SHOWER, true);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"shower valve not paired\"}");
        return;
    }

    // POST /switch/shower/off
    if (strcmp(path, "/switch/shower/off") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_SHOWER, false);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"shower valve not paired\"}");
        return;
    }

    // POST /switch/bath/on
    if (strcmp(path, "/switch/bath/on") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_BATH, true);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"bath valve not paired\"}");
        return;
    }

    // POST /switch/bath/off
    if (strcmp(path, "/switch/bath/off") == 0 && strcmp(method, "POST") == 0) {
        bool ok = zigbee_coord_switch_set(SWITCH_BATH, false);
        *out_status = ok ? 200 : 409;
        *out_body = strdup(ok ? "{\"ok\":true}" : "{\"error\":\"bath valve not paired\"}");
        return;
    }

    // POST /switch/pair
    if (strcmp(path, "/switch/pair") == 0 && strcmp(method, "POST") == 0) {
        switch_id_t slot;
        if (!parse_slot(body, &slot)) {
            *out_status = 400;
            *out_body = strdup("{\"error\":\"slot must be top, bottom, shower, or bath\"}");
            return;
        }
        int duration_s = 180;
        if (body && body[0]) {
            cJSON *root = cJSON_Parse(body);
            if (root) {
                cJSON *jdur = cJSON_GetObjectItem(root, "duration");
                if (jdur && cJSON_IsNumber(jdur) && jdur->valueint > 0 && jdur->valueint <= 255) {
                    duration_s = jdur->valueint;
                }
                cJSON_Delete(root);
            }
        }
        zigbee_coord_permit_join(slot, (uint8_t)duration_s);
        char resp[96];
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"slot\":\"%s\",\"duration_s\":%d}",
                 slot_to_str(slot), duration_s);
        *out_status = 200;
        *out_body = strdup(resp);
        return;
    }

    // GET /switch/pair
    if (strcmp(path, "/switch/pair") == 0 && strcmp(method, "GET") == 0) {
        zb_pair_state_t ps = zigbee_coord_pair_state();
        char resp[128];
        if (ps.open) {
            snprintf(resp, sizeof(resp),
                     "{\"open\":true,\"slot\":\"%s\",\"remaining_s\":%d}",
                     slot_to_str(ps.target),
                     ps.remaining_s);
        } else {
            snprintf(resp, sizeof(resp), "{\"open\":false,\"remaining_s\":0}");
        }
        *out_status = 200;
        *out_body = strdup(resp);
        return;
    }

    // POST /switch/clear
    if (strcmp(path, "/switch/clear") == 0 && strcmp(method, "POST") == 0) {
        switch_id_t slot;
        if (!parse_slot(body, &slot)) {
            *out_status = 400;
            *out_body = strdup("{\"error\":\"slot must be top, bottom, shower, or bath\"}");
            return;
        }
        zigbee_coord_clear(slot);
        *out_status = 200;
        *out_body = strdup("{\"ok\":true}");
        return;
    }

    // POST /switch/reset_network
    if (strcmp(path, "/switch/reset_network") == 0 && strcmp(method, "POST") == 0) {
        zigbee_coord_reset_network();
        *out_status = 200;
        *out_body = strdup("{\"ok\":true,\"message\":\"network reset initiated\"}");
        return;
    }

    ESP_LOGW(TAG, "Unknown route %s %s", method, path);
    *out_status = 404;
    *out_body = strdup("{\"error\":\"not found\"}");
}
