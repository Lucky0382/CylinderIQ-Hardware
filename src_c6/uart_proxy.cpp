// CylinderIQ Hub V2 — uart_proxy.cpp (ESP32-C6)
// Serialises HTTP requests to S3 over UART1.
// One request in-flight at a time (mutex-based serialisation).
// The S3 push messages ({"type":"push",...}) are consumed and discarded
// here — the C6 does not cache them; DisplayIQ always polls via HTTP.

#include "uart_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <atomic>
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "uart_proxy";

// Request serialisation: one outstanding request at a time
static SemaphoreHandle_t s_req_mutex;

// Response delivery: rx_task signals this semaphore when a response arrives
static SemaphoreHandle_t s_resp_ready;

// Shared between rx_task (writer) and uart_proxy_request (reader)
// Protected by s_req_mutex (only one caller can be waiting at a time)
static int    s_resp_status;
static char  *s_resp_body;   // heap-allocated; caller of uart_proxy_request takes ownership

// Monotonic request ID counter (wraps at 0xFFFF per handoff spec)
static std::atomic<uint32_t> s_next_id{1};

#define LINE_BUF_SIZE 4096

// ──────────────────────────────────────────────────────────────
// Init
// ──────────────────────────────────────────────────────────────

void uart_proxy_init(void)
{
    s_req_mutex  = xSemaphoreCreateMutex();
    s_resp_ready = xSemaphoreCreateBinary();
    configASSERT(s_req_mutex);
    configASSERT(s_resp_ready);

    uart_config_t cfg = {
        .baud_rate           = UART_PROXY_BAUD,
        .data_bits           = UART_DATA_8_BITS,
        .parity              = UART_PARITY_DISABLE,
        .stop_bits           = UART_STOP_BITS_1,
        .flow_ctrl           = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk          = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PROXY_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PROXY_PORT,
                                 UART_PROXY_TX_PIN,
                                 UART_PROXY_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PROXY_PORT,
                                        UART_PROXY_BUF, 0,
                                        0, NULL, 0));
    ESP_LOGI(TAG, "UART1 TX=%d RX=%d @ %d baud", UART_PROXY_TX_PIN, UART_PROXY_RX_PIN, UART_PROXY_BAUD);
}

// ──────────────────────────────────────────────────────────────
// RX task — reads response/push lines from S3
// ──────────────────────────────────────────────────────────────

void uart_proxy_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started");

    char *line = (char *)malloc(LINE_BUF_SIZE);
    configASSERT(line);
    int line_pos = 0;
    uint8_t ch;

    for (;;) {
        int n = uart_read_bytes(UART_PROXY_PORT, &ch, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (ch == '\n' || ch == '\r') {
            if (line_pos == 0) continue;
            line[line_pos] = '\0';
            line_pos = 0;

            cJSON *msg = cJSON_Parse(line);
            if (!msg) {
                ESP_LOGW(TAG, "JSON parse error: %.80s", line);
                continue;
            }

            cJSON *jtype = cJSON_GetObjectItem(msg, "type");
            if (!jtype || !cJSON_IsString(jtype)) {
                cJSON_Delete(msg);
                continue;
            }

            if (strcmp(jtype->valuestring, "res") == 0) {
                // Response to a pending request
                cJSON *jstatus = cJSON_GetObjectItem(msg, "status");
                cJSON *jbody   = cJSON_GetObjectItem(msg, "body");

                int   status = jstatus ? (int)jstatus->valuedouble : 500;
                char *body   = (jbody && cJSON_IsString(jbody))
                               ? strdup(jbody->valuestring)
                               : strdup("{}");

                // Deliver to waiting uart_proxy_request() caller
                // s_req_mutex is held by that caller so this is safe
                s_resp_status = status;
                if (s_resp_body) free(s_resp_body);
                s_resp_body = body;
                xSemaphoreGive(s_resp_ready);

                ESP_LOGD(TAG, "RES status=%d body=%.60s", status, body);

            } else if (strcmp(jtype->valuestring, "push") == 0) {
                // Unsolicited push from S3 — discard (DisplayIQ polls via HTTP)
                ESP_LOGD(TAG, "Push received (discarded)");
            }

            cJSON_Delete(msg);

        } else {
            if (line_pos < LINE_BUF_SIZE - 1) {
                line[line_pos++] = (char)ch;
            } else {
                ESP_LOGW(TAG, "RX overrun — discarding");
                line_pos = 0;
            }
        }
    }
    free(line);
}

// ──────────────────────────────────────────────────────────────
// uart_proxy_request — called by HTTP handlers
// ──────────────────────────────────────────────────────────────

bool uart_proxy_request(const char *method,
                        const char *path,
                        const char *body,
                        int        *out_status,
                        char      **out_body)
{
    // Serialise: only one HTTP request in-flight to S3 at a time
    if (xSemaphoreTake(s_req_mutex, pdMS_TO_TICKS(UART_PROXY_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Request mutex timeout — S3 busy");
        *out_status = 503;
        *out_body   = strdup("{\"error\":\"S3 busy\"}");
        return false;
    }

    uint32_t req_id = s_next_id.fetch_add(1) & 0xFFFF;

    // Build request JSON
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type",   "req");
    cJSON_AddNumberToObject(req, "id",     (double)req_id);
    cJSON_AddStringToObject(req, "method", method);
    cJSON_AddStringToObject(req, "path",   path);
    cJSON_AddStringToObject(req, "body",   body ? body : "");
    char *req_str = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    if (!req_str) {
        xSemaphoreGive(s_req_mutex);
        *out_status = 500;
        *out_body   = strdup("{\"error\":\"OOM\"}");
        return false;
    }

    ESP_LOGD(TAG, "REQ id=%lu %s %s", (unsigned long)req_id, method, path);

    // Clear any stale response semaphore from a previous aborted request
    xSemaphoreTake(s_resp_ready, 0);
    s_resp_body = NULL;

    // Send over UART
    uart_write_bytes(UART_PROXY_PORT, req_str, strlen(req_str));
    uart_write_bytes(UART_PROXY_PORT, "\n", 1);
    free(req_str);

    // Wait for response
    bool got_resp = (xSemaphoreTake(s_resp_ready, pdMS_TO_TICKS(UART_PROXY_TIMEOUT_MS)) == pdTRUE);

    if (got_resp) {
        *out_status = s_resp_status;
        *out_body   = s_resp_body;   // transfer ownership to caller
        s_resp_body = NULL;
    } else {
        ESP_LOGE(TAG, "S3 response timeout for %s %s", method, path);
        *out_status = 504;
        *out_body   = strdup("{\"error\":\"S3 timeout\"}");
    }

    xSemaphoreGive(s_req_mutex);
    return got_resp;
}
