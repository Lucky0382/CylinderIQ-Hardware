// CylinderIQ Hub V2 — main_c6.cpp (ESP32-C6 Dedicated Zigbee Coordinator)
//
// Boot sequence:
//   1. UART1 bridge init (to S3)
//   2. Start uart_proxy_rx_task
//   3. Zigbee coordinator init (100% dedicated 802.15.4 radio, no Wi-Fi)

#include "uart_proxy.h"
#include "zigbee_coord.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main_c6";

// Stub out temperature_sensor_install so legacy conflict check is bypassed
extern "C" esp_err_t __wrap_temperature_sensor_install(const void *config, void **ret)
{
    return ESP_OK;
}

static void console_rx_task(void *arg)
{
    (void)arg;
    char line[64];
    while (1) {
        if (fgets(line, sizeof(line), stdin)) {
            char *p = strpbrk(line, "\r\n");
            if (p) *p = '\0';
            if (strlen(line) == 0) continue;

            if (strcmp(line, "pair") == 0 || strcmp(line, "p") == 0 || strcmp(line, "pair top") == 0) {
                ESP_LOGI(TAG, "Console CMD: Opening pairing window for 180s (TOP slot)");
                zigbee_coord_permit_join(SWITCH_TOP, 180);
            } else if (strcmp(line, "pair bot") == 0 || strcmp(line, "pair bottom") == 0) {
                ESP_LOGI(TAG, "Console CMD: Opening pairing window for 180s (BOTTOM slot)");
                zigbee_coord_permit_join(SWITCH_BOTTOM, 180);
            } else if (strcmp(line, "pair shower") == 0 || strcmp(line, "pair shw") == 0) {
                ESP_LOGI(TAG, "Console CMD: Opening pairing window for 180s (SHOWER slot)");
                zigbee_coord_permit_join(SWITCH_SHOWER, 180);
            } else if (strcmp(line, "pair bath") == 0 || strcmp(line, "pair bth") == 0) {
                ESP_LOGI(TAG, "Console CMD: Opening pairing window for 180s (BATH slot)");
                zigbee_coord_permit_join(SWITCH_BATH, 180);
            } else if (strcmp(line, "status") == 0 || strcmp(line, "s") == 0) {
                zb_switch_t top = zigbee_coord_switch_get(SWITCH_TOP);
                zb_switch_t bot = zigbee_coord_switch_get(SWITCH_BOTTOM);
                zb_switch_t shw = zigbee_coord_switch_get(SWITCH_SHOWER);
                zb_switch_t bth = zigbee_coord_switch_get(SWITCH_BATH);
                zb_pair_state_t ps = zigbee_coord_pair_state();
                uint16_t pan = 0; uint8_t ch = 0; bool online = false;
                zigbee_coord_get_network_info(&pan, &ch, &online);
                const char *targets[] = {"TOP", "BOTTOM", "SHOWER", "BATH"};
                const char *tgt = (ps.target >= 0 && ps.target < 4) ? targets[ps.target] : "NONE";
                ESP_LOGI(TAG, "=== COORDINATOR STATUS ===");
                ESP_LOGI(TAG, "Network:       channel=%d PAN=0x%04X online=%s", ch, pan, online ? "YES" : "NO");
                ESP_LOGI(TAG, "TOP switch:    paired=%d short=0x%04X state=%s ep=%d",
                         top.paired, top.short_addr, top.on ? "ON" : "OFF", top.endpoint);
                ESP_LOGI(TAG, "BOTTOM switch: paired=%d short=0x%04X state=%s ep=%d",
                         bot.paired, bot.short_addr, bot.on ? "ON" : "OFF", bot.endpoint);
                ESP_LOGI(TAG, "SHOWER valve:  paired=%d short=0x%04X state=%s ep=%d",
                         shw.paired, shw.short_addr, shw.on ? "OPEN" : "CLOSED", shw.endpoint);
                ESP_LOGI(TAG, "BATH valve:    paired=%d short=0x%04X state=%s ep=%d",
                         bth.paired, bth.short_addr, bth.on ? "OPEN" : "CLOSED", bth.endpoint);
                ESP_LOGI(TAG, "Pairing window: open=%d target=%s remaining=%ds",
                         ps.open, tgt, ps.remaining_s);
                ESP_LOGI(TAG, "==========================");
            } else if (strcmp(line, "reset") == 0 || strcmp(line, "r") == 0) {
                ESP_LOGW(TAG, "Console CMD: Resetting Zigbee network to factory default...");
                zigbee_coord_reset_network();
            } else if (strcmp(line, "top_on") == 0 || strcmp(line, "on") == 0) {
                ESP_LOGI(TAG, "Console CMD: Turn TOP switch ON");
                zigbee_coord_switch_set(SWITCH_TOP, true);
            } else if (strcmp(line, "top_off") == 0 || strcmp(line, "off") == 0) {
                ESP_LOGI(TAG, "Console CMD: Turn TOP switch OFF");
                zigbee_coord_switch_set(SWITCH_TOP, false);
            } else if (strcmp(line, "bot_on") == 0) {
                ESP_LOGI(TAG, "Console CMD: Turn BOTTOM switch ON");
                zigbee_coord_switch_set(SWITCH_BOTTOM, true);
            } else if (strcmp(line, "bot_off") == 0) {
                ESP_LOGI(TAG, "Console CMD: Turn BOTTOM switch OFF");
                zigbee_coord_switch_set(SWITCH_BOTTOM, false);
            } else if (strcmp(line, "shower_on") == 0 || strcmp(line, "shower_open") == 0) {
                ESP_LOGI(TAG, "Console CMD: Open SHOWER valve");
                zigbee_coord_switch_set(SWITCH_SHOWER, true);
            } else if (strcmp(line, "shower_off") == 0 || strcmp(line, "shower_close") == 0) {
                ESP_LOGI(TAG, "Console CMD: Close SHOWER valve");
                zigbee_coord_switch_set(SWITCH_SHOWER, false);
            } else if (strcmp(line, "bath_on") == 0 || strcmp(line, "bath_open") == 0) {
                ESP_LOGI(TAG, "Console CMD: Open BATH valve");
                zigbee_coord_switch_set(SWITCH_BATH, true);
            } else if (strcmp(line, "bath_off") == 0 || strcmp(line, "bath_close") == 0) {
                ESP_LOGI(TAG, "Console CMD: Close BATH valve");
                zigbee_coord_switch_set(SWITCH_BATH, false);
            } else {
                ESP_LOGI(TAG, "Available console commands: pair [top|bot|shower|bath], status, top_on, top_off, bot_on, bot_off, shower_on, shower_off, bath_on, bath_off, reset");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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


    // ── 1. UART bridge to S3 ─────────────────────────────────
    uart_proxy_init();

    xTaskCreatePinnedToCore(
        uart_proxy_rx_task, "uart_rx",
        4096, NULL, 4,
        NULL, 0);

    // ── 2. Zigbee coordinator ────────────────────────────────
    zigbee_coord_init();

    // ── 3. Console interactive command task ──────────────────
    xTaskCreate(console_rx_task, "console_cmd", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "C6 ready — Dedicated Zigbee 3.0 Coordinator active");
    // app_main returns; scheduler takes over
}

