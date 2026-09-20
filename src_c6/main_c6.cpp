// CylinderIQ Hub V2 — main_c6.cpp (ESP32-C6 Network Processor)
//
// Boot sequence:
//   1. UART1 proxy init (to S3)
//   2. WiFi AP start (blocks until AP ready)
//   3. Zigbee coordinator init (Wi-Fi + 802.15.4 coex enabled here)
//   4. HTTP server start
//   5. Start uart_proxy_rx_task

#include "http_server.h"
#include "uart_proxy.h"
#include "wifi_ap.h"
#include "zigbee_coord.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main_c6";

// Stub out temperature_sensor_install so legacy conflict check is bypassed
extern "C" esp_err_t __wrap_temperature_sensor_install(const void *config, void **ret)
{
    return ESP_OK;
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "CylinderIQ Hub V2 — C6 booting");

    // ── 1. UART proxy (before WiFi — S3 may boot faster) ─────
    uart_proxy_init();

    xTaskCreatePinnedToCore(
        uart_proxy_rx_task, "uart_rx",
        4096, NULL, 6,
        NULL, 0);

    // ── 2. WiFi AP ────────────────────────────────────────────
    wifi_ap_init();

    // ── 3. Zigbee coordinator ─────────────────────────────────
    // Must be after wifi_ap_init() so coex manager is ready.
    // Starts its own FreeRTOS task internally.
    zigbee_coord_init();

    // ── 4. HTTP server ────────────────────────────────────────
    http_server_start();

    ESP_LOGI(TAG, "C6 ready — http://192.168.4.1  Zigbee coordinator active");
    // app_main returns; scheduler takes over
}

