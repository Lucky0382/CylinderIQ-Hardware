// CylinderIQ Hub V2 — main_s3.cpp (ESP32-S3 Application Processor)
//
// Boot sequence:
//   1. NVS init
//   2. Sensor GPIO init
//   3. UART1 bridge init (to C6)
//   4. Start sensor_task        (core 0, priority 5)
//   5. Start calc_task          (core 0, priority 4)
//   6. Start uart_bridge_rx_task (core 1, priority 6)
//   7. Start uart_bridge_push_task (core 1, priority 3)
//
// The S3 has no WiFi — it is air-gapped and talks only via UART1.

#include "api_handlers.h"
#include "ble_server.h"
#include "calculations.h"
#include "http_server.h"
#include "nvs_store.h"
#include "sensors.h"
#include "uart_bridge.h"
#include "wifi_ap.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main_s3";

// ──────────────────────────────────────────────────────────────
// BLE push task — sends sensor JSON to connected BLE client every 5s.
// Calls api_dispatch("GET", "/sensors") to get the exact same JSON
// the REST endpoint returns, then hands it to ble_server_notify_sensors().
// Core 1, priority 2 (lowest — UART tasks take priority).
// ──────────────────────────────────────────────────────────────
static void ble_push_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "BLE push task started");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        char *body  = NULL;
        int   status = 200;
        api_dispatch("GET", "/sensors", "", &body, &status);
        if (body && status == 200) {
            ble_server_notify_sensors(body);
        }
        if (body) free(body);
    }
}

// ──────────────────────────────────────────────────────────────
// Calculation task — runs calc_run() after every sensor cycle.
// Sensor task takes ~810 ms per cycle; we poll every 1 s to
// ensure we always have a fresh result ready for UART dispatch.
// ──────────────────────────────────────────────────────────────
static void calc_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Calc task started");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        calc_run();
    }
}

// ──────────────────────────────────────────────────────────────
// app_main
// ──────────────────────────────────────────────────────────────
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "CylinderIQ Hub V2 — S3 booting");

    // ── 1. NVS ───────────────────────────────────────────────
    nvs_store_init();

    // Log key config so first-boot state is visible in serial monitor
    ESP_LOGI(TAG, "tank_size=%d L  heat_type=%d  imm_kw=%.1f x%d",
             (int)nvs_get_tank_size(),
             (int)nvs_get_heat_type(),
             nvs_get_imm_kw(),
             (int)nvs_get_imm_count());
    ESP_LOGI(TAG, "bl_cold=%.2f  bl_hot=%.2f  wiz_complete=%d",
             nvs_get_bl_cold(),
             nvs_get_bl_hot(),
             (int)nvs_get_wiz_complete());

    // ── 2. Sensors ───────────────────────────────────────────
    sensors_init();

    // ── 3. UART bridge ───────────────────────────────────────
    uart_bridge_init();

    // ── 4. WiFi SoftAP ───────────────────────────────────────
    wifi_ap_init();

    // ── 5. HTTP server ───────────────────────────────────────
    http_server_start();

    // ── 6. BLE server ────────────────────────────────────────
    // NimBLE host task is started internally by ble_server_init().
    ble_server_init();

    // ── 5-9. FreeRTOS tasks ──────────────────────────────────
    // sensor_task: one-wire is slow (750 ms/cycle), pin to core 0
    xTaskCreatePinnedToCore(
        sensors_task, "sensors",
        4096, NULL, 5,
        NULL, 0);

    // calc_task: lightweight math, core 0 alongside sensor_task
    xTaskCreatePinnedToCore(
        calc_task, "calc",
        4096, NULL, 4,
        NULL, 0);

    // uart_bridge_rx_task: high priority, must respond quickly to C6 requests
    xTaskCreatePinnedToCore(
        uart_bridge_rx_task, "uart_rx",
        8192, NULL, 6,
        NULL, 1);

    // uart_bridge_push_task: low priority periodic push to C6
    xTaskCreatePinnedToCore(
        uart_bridge_push_task, "uart_push",
        4096, NULL, 3,
        NULL, 1);

    // ble_push_task: lowest priority — pushes /sensors JSON over BLE every 5s
    xTaskCreatePinnedToCore(
        ble_push_task, "ble_push",
        4096, NULL, 2,
        NULL, 1);

    ESP_LOGI(TAG, "All tasks started — S3 running");
    // app_main returns; scheduler takes over
}
