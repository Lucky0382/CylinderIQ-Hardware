// CylinderIQ Hub V2 — uart_proxy.cpp (ESP32-C6)
// Listens for switch control requests from S3, dispatches to switch_control,
// and returns responses over UART1.

#include "uart_proxy.h"
#include "switch_control.h"

#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "uart_c6";

static SemaphoreHandle_t s_tx_mutex = NULL;
#define LINE_BUF_SIZE 4096

void uart_proxy_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    configASSERT(s_tx_mutex);

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

void uart_proxy_send(const char *json_str)
{
    if (!s_tx_mutex || !json_str) return;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PROXY_PORT, json_str, strlen(json_str));
    uart_write_bytes(UART_PROXY_PORT, "\n", 1);
    xSemaphoreGive(s_tx_mutex);
}

void uart_proxy_rx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "RX task started @ %d baud", UART_PROXY_BAUD);

    char *line = (char *)malloc(LINE_BUF_SIZE);
    configASSERT(line);
    int line_pos = 0;
    uint8_t rx_buf[128];

    uint32_t total_bytes  = 0;
    uint32_t last_hb_tick = xTaskGetTickCount();

    for (;;) {
        int n = uart_read_bytes(UART_PROXY_PORT, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(50));

        uint32_t now = xTaskGetTickCount();
        if ((now - last_hb_tick) >= pdMS_TO_TICKS(10000)) {
            if (total_bytes > 0) {
                ESP_LOGI(TAG, "RX heartbeat — bytes received from S3 in last 10 s: %lu",
                         (unsigned long)total_bytes);
            }
            total_bytes  = 0;
            last_hb_tick = now;
        }

        if (n <= 0) continue;
        total_bytes += n;

        for (int i = 0; i < n; i++) {
            char ch = (char)rx_buf[i];
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

                if (strcmp(jtype->valuestring, "req") == 0) {
                    // Request from S3!
                    cJSON *jid     = cJSON_GetObjectItem(msg, "id");
                    cJSON *jmethod = cJSON_GetObjectItem(msg, "method");
                    cJSON *jpath   = cJSON_GetObjectItem(msg, "path");
                    cJSON *jbody   = cJSON_GetObjectItem(msg, "body");

                    uint32_t req_id = jid ? (uint32_t)jid->valuedouble : 0;
                    const char *method = (jmethod && cJSON_IsString(jmethod)) ? jmethod->valuestring : "GET";
                    const char *path   = (jpath && cJSON_IsString(jpath)) ? jpath->valuestring : "/";
                    const char *body   = (jbody && cJSON_IsString(jbody)) ? jbody->valuestring : "";

                    ESP_LOGI(TAG, "REQ id=%lu %s %s", (unsigned long)req_id, method, path);

                    int resp_status = 500;
                    char *resp_body = NULL;
                    switch_control_dispatch(method, path, body, &resp_status, &resp_body);

                    // Send response to S3: {"type":"res","id":N,"status":S,"body":"..."}
                    cJSON *res = cJSON_CreateObject();
                    cJSON_AddStringToObject(res, "type", "res");
                    cJSON_AddNumberToObject(res, "id", (double)req_id);
                    cJSON_AddNumberToObject(res, "status", resp_status);
                    cJSON_AddStringToObject(res, "body", resp_body ? resp_body : "{}");
                    char *res_str = cJSON_PrintUnformatted(res);
                    cJSON_Delete(res);
                    if (resp_body) free(resp_body);

                    if (res_str) {
                        uart_proxy_send(res_str);
                        free(res_str);
                    }

                } else if (strcmp(jtype->valuestring, "push") == 0) {
                    ESP_LOGD(TAG, "Sensor push from S3 received");
                }

                cJSON_Delete(msg);

            } else {
                if (line_pos < LINE_BUF_SIZE - 1) {
                    line[line_pos++] = ch;
                } else {
                    ESP_LOGW(TAG, "RX overrun — discarding");
                    line_pos = 0;
                }
            }
        }
    }
    free(line);
}
