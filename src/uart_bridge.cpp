// CylinderIQ Hub V2 — uart_bridge.cpp (ESP32-S3)
// Receives JSON request lines from C6, dispatches to api_handlers,
// sends JSON response lines back. Also sends periodic sensor pushes.

#include "uart_bridge.h"
#include "api_handlers.h"
#include "calculations.h"

#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "uart_bridge";

// TX mutex — prevents interleaved output from rx_task and push_task
static SemaphoreHandle_t s_tx_mutex;

// Request to C6 tracking
static SemaphoreHandle_t s_c6_req_mutex = NULL;
static SemaphoreHandle_t s_c6_resp_ready = NULL;
static int               s_c6_resp_status = 500;
static char             *s_c6_resp_body = NULL;
static uint32_t          s_c6_req_id = 1;

// Working buffer for incoming line assembly
#define LINE_BUF_SIZE   4096

// ──────────────────────────────────────────────────────────────
// Init
// ──────────────────────────────────────────────────────────────

void uart_bridge_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex);

    s_c6_req_mutex  = xSemaphoreCreateMutex();
    s_c6_resp_ready = xSemaphoreCreateBinary();
    configASSERT(s_c6_req_mutex);
    configASSERT(s_c6_resp_ready);

    uart_config_t cfg = {
        .baud_rate           = UART_BRIDGE_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_BRIDGE_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_BRIDGE_PORT,
                                 UART_BRIDGE_TX_PIN,
                                 UART_BRIDGE_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    // Install driver with 8KB RX ring buffer, no TX ring buffer
    ESP_ERROR_CHECK(uart_driver_install(UART_BRIDGE_PORT,
                                        UART_BRIDGE_BUF, 0,
                                        0, NULL, 0));
    ESP_LOGI(TAG, "UART1 TX=%d RX=%d @ %d baud", UART_BRIDGE_TX_PIN, UART_BRIDGE_RX_PIN, UART_BRIDGE_BAUD);
}

// ──────────────────────────────────────────────────────────────
// Send
// ──────────────────────────────────────────────────────────────

void uart_bridge_send(const char *json_str)
{
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_BRIDGE_PORT, json_str, strlen(json_str));
    uart_write_bytes(UART_BRIDGE_PORT, "\n", 1);
    xSemaphoreGive(s_tx_mutex);
}

// ──────────────────────────────────────────────────────────────
// Response builder helpers
// ──────────────────────────────────────────────────────────────

static void send_response(uint32_t req_id, int status, const char *body_json)
{
    // Build outer envelope: {"type":"res","id":N,"status":S,"body":"..."}
    // The body is already a JSON string; we embed it as a string value.
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "res");
    cJSON_AddNumberToObject(root, "id", (double)req_id);
    cJSON_AddNumberToObject(root, "status", status);
    cJSON_AddStringToObject(root, "body", body_json ? body_json : "");
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out) {
        uart_bridge_send(out);
        free(out);
    }
}

// ──────────────────────────────────────────────────────────────
// RX task — reads lines, parses JSON, calls api_handlers
// ──────────────────────────────────────────────────────────────

void uart_bridge_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started");

    char *line = (char *)malloc(LINE_BUF_SIZE);
    configASSERT(line);
    int line_pos = 0;

    // Heartbeat: log total bytes received every 10 s so we can confirm
    // whether the S3 is "hearing" anything from the C6 at all.
    uint32_t total_bytes  = 0;
    uint32_t last_hb_tick = xTaskGetTickCount();

    uint8_t ch;
    for (;;) {
        int n = uart_read_bytes(UART_BRIDGE_PORT, &ch, 1, pdMS_TO_TICKS(100));

        // ── Heartbeat every 10 s ─────────────────────────────
        uint32_t now = xTaskGetTickCount();
        if ((now - last_hb_tick) >= pdMS_TO_TICKS(10000)) {
            ESP_LOGI(TAG, "RX heartbeat — bytes received in last 10 s: %lu",
                     (unsigned long)total_bytes);
            total_bytes  = 0;
            last_hb_tick = now;
        }

        if (n <= 0) continue;
        total_bytes++;

        if (ch == '\n' || ch == '\r') {
            if (line_pos == 0) continue;   // skip blank lines
            line[line_pos] = '\0';

            // ── Log raw line (debug only — too noisy at INFO) ────
            ESP_LOGD(TAG, "RAW RX [%d B]: %.*s", line_pos, line_pos < 120 ? line_pos : 120, line);

            line_pos = 0;

            // ── Parse incoming JSON line ──────────────────────
            cJSON *msg = cJSON_Parse(line);
            if (!msg) {
                ESP_LOGW(TAG, "JSON parse error: %.80s", line);
                continue;
            }

            cJSON *jtype   = cJSON_GetObjectItem(msg, "type");
            cJSON *jid     = cJSON_GetObjectItem(msg, "id");
            cJSON *jmethod = cJSON_GetObjectItem(msg, "method");
            cJSON *jpath   = cJSON_GetObjectItem(msg, "path");
            cJSON *jbody   = cJSON_GetObjectItem(msg, "body");

            if (!cJSON_IsString(jtype)) {
                cJSON_Delete(msg);
                continue;
            }

            if (strcmp(jtype->valuestring, "res") == 0) {
                cJSON *jstatus = cJSON_GetObjectItem(msg, "status");
                cJSON *jbody_res = cJSON_GetObjectItem(msg, "body");
                s_c6_resp_status = jstatus ? (int)jstatus->valuedouble : 500;
                if (s_c6_resp_body) free(s_c6_resp_body);
                s_c6_resp_body = (jbody_res && cJSON_IsString(jbody_res)) ? strdup(jbody_res->valuestring) : strdup("{}");
                xSemaphoreGive(s_c6_resp_ready);
                ESP_LOGD(TAG, "RES from C6: status=%d", s_c6_resp_status);
                cJSON_Delete(msg);
                continue;
            }

            if (strcmp(jtype->valuestring, "req") != 0) {
                cJSON_Delete(msg);
                ESP_LOGD(TAG, "Ignoring non-request message");
                continue;
            }

            uint32_t req_id = jid ? (uint32_t)jid->valuedouble : 0;
            const char *method = jmethod ? jmethod->valuestring : "GET";
            const char *path   = jpath   ? jpath->valuestring   : "/";
            const char *body   = (jbody && cJSON_IsString(jbody)) ? jbody->valuestring : "";

            ESP_LOGD(TAG, "REQ id=%lu %s %s", (unsigned long)req_id, method, path);

            // ── Dispatch to API handler ───────────────────────
            char *resp_body = NULL;
            int   resp_status = 200;

            api_dispatch(method, path, body, &resp_body, &resp_status);

            send_response(req_id, resp_status, resp_body ? resp_body : "{}");
            if (resp_body) free(resp_body);

            cJSON_Delete(msg);

        } else {
            // Accumulate character
            if (line_pos < LINE_BUF_SIZE - 1) {
                line[line_pos++] = (char)ch;
            } else {
                // Overrun — discard and reset
                ESP_LOGW(TAG, "RX line buffer overrun — discarding");
                line_pos = 0;
            }
        }
    }
    // Never reached
    free(line);
}

// ──────────────────────────────────────────────────────────────
// Push task — sends sensor snapshot every 5 s (unsolicited)
// ──────────────────────────────────────────────────────────────

void uart_bridge_push_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Push task started");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        calc_result_t r = calc_get();
        if (!r.valid) continue;

        // {"type":"push","sensors":{...}}
        cJSON *root    = cJSON_CreateObject();
        cJSON *sensors = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "push");

        cJSON_AddNumberToObject(sensors, "hot_outlet",      r.hot_outlet);
        cJSON_AddNumberToObject(sensors, "cylinder_inlet",  r.cylinder_inlet);
        cJSON_AddNumberToObject(sensors, "mains_supply",    r.mains_supply);
        cJSON_AddNumberToObject(sensors, "usable_hot_litres", r.usable_litres);
        cJSON_AddNumberToObject(sensors, "hot_water_pct",   r.hot_pct);
        cJSON_AddNumberToObject(sensors, "showers_remaining", r.showers_remaining);
        cJSON_AddNumberToObject(sensors, "baths_remaining", r.baths_remaining);
        cJSON_AddNumberToObject(sensors, "recovery_min",    r.recovery_min);
        cJSON_AddNumberToObject(sensors, "cost_pence",      r.cost_pence);
        cJSON_AddItemToObject(root, "sensors", sensors);

        char *out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (out) {
            uart_bridge_send(out);
            free(out);
            ESP_LOGD(TAG, "Sensor push sent");
        }
    }
}

// ──────────────────────────────────────────────────────────────
// uart_bridge_request_c6 — forward switch requests to C6
// ──────────────────────────────────────────────────────────────

bool uart_bridge_request_c6(const char *method,
                            const char *path,
                            const char *body,
                            int        *out_status,
                            char      **out_body)
{
    if (!s_c6_req_mutex) {
        *out_status = 503;
        *out_body = NULL;
        return false;
    }

    if (xSemaphoreTake(s_c6_req_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "C6 request mutex timeout");
        *out_status = 503;
        *out_body   = NULL;
        return false;
    }

    uint32_t req_id = s_c6_req_id++ & 0xFFFF;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type",   "req");
    cJSON_AddNumberToObject(req, "id",     (double)req_id);
    cJSON_AddStringToObject(req, "method", method);
    cJSON_AddStringToObject(req, "path",   path);
    cJSON_AddStringToObject(req, "body",   body ? body : "");
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (!req_str) {
        xSemaphoreGive(s_c6_req_mutex);
        *out_status = 500;
        *out_body   = NULL;
        return false;
    }

    // Flush any leftover semaphore
    xSemaphoreTake(s_c6_resp_ready, 0);
    if (s_c6_resp_body) {
        free(s_c6_resp_body);
        s_c6_resp_body = NULL;
    }

    uart_bridge_send(req_str);
    free(req_str);

    bool got_resp = (xSemaphoreTake(s_c6_resp_ready, pdMS_TO_TICKS(500)) == pdTRUE);
    if (got_resp) {
        *out_status = s_c6_resp_status;
        *out_body   = s_c6_resp_body;   // caller takes ownership
        s_c6_resp_body = NULL;
    } else {
        ESP_LOGD(TAG, "C6 response timeout for %s %s", method, path);
        *out_status = 504;
        *out_body   = NULL;
    }

    xSemaphoreGive(s_c6_req_mutex);
    return got_resp;
}

