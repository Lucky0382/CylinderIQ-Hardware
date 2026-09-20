// CylinderIQ Hub V2 — main_c6.cpp (ESP32-C6 Dedicated Zigbee Coordinator)
//
// Boot sequence:
//   1. UART1 bridge init (to S3)
//   2. Start uart_proxy_rx_task
//   3. Zigbee coordinator init (100% dedicated 802.15.4 radio, no Wi-Fi)

#include "uart_proxy.h"
#include "zigbee_coord.h"
#include "nvs_flash.h"

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
    ESP_LOGI(TAG, "CylinderIQ Hub V2 — C6 Dedicated Zigbee Coordinator booting");

    // ── 0. NVS flash init (required for RF calibration and Zigbee network parameters)
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Dedicated Zigbee NVS partition required by esp-zigbee-sdk platform datasets
    esp_err_t zb_err = nvs_flash_init_partition("zigbee");
    if (zb_err == ESP_ERR_NVS_NO_FREE_PAGES || zb_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("zigbee"));
        zb_err = nvs_flash_init_partition("zigbee");
    }
    if (zb_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize 'zigbee' partition: %s", esp_err_to_name(zb_err));
    } else {
        ESP_LOGI(TAG, "Dedicated 'zigbee' NVS partition initialized successfully");
    }

    // Also initialize 'zb_storage' partition if present
    esp_err_t zbs_err = nvs_flash_init_partition("zb_storage");
    if (zbs_err == ESP_ERR_NVS_NO_FREE_PAGES || zbs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("zb_storage"));
        zbs_err = nvs_flash_init_partition("zb_storage");
    }
    if (zbs_err == ESP_OK) {
        ESP_LOGI(TAG, "Dedicated 'zb_storage' NVS partition initialized successfully");
    }

    // ── 1. UART bridge to S3 ─────────────────────────────────
    uart_proxy_init();

    xTaskCreatePinnedToCore(
        uart_proxy_rx_task, "uart_rx",
        4096, NULL, 4,
        NULL, 0);

    // ── 2. Zigbee coordinator ────────────────────────────────
    zigbee_coord_init();

    ESP_LOGI(TAG, "C6 ready — Dedicated Zigbee 3.0 Coordinator active");
    // app_main returns; scheduler takes over
}
