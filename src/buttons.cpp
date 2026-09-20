// CylinderIQ Hub V2 — buttons.cpp (ESP32-S3)
// Physical button handling for Zigbee pairing and relay toggles.

#include "buttons.h"
#include "uart_bridge.h"

#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

static const char *TAG = "buttons";

typedef struct {
    gpio_num_t pin;
    uint32_t   press_start_tick;
    bool       is_down;
} button_tracker_t;

static button_tracker_t s_btn_boot = { (gpio_num_t)BUTTON_BOOT_GPIO, 0, false };
static button_tracker_t s_btn_top  = { (gpio_num_t)BUTTON_TOP_GPIO,  0, false };
static button_tracker_t s_btn_bot  = { (gpio_num_t)BUTTON_BOT_GPIO,  0, false };

static void trigger_pair(const char *slot)
{
    ESP_LOGI(TAG, "Hardware button: Triggering Zigbee pairing for %s switch...", slot);
    char req_body[64];
    snprintf(req_body, sizeof(req_body), "{\"slot\":\"%s\"}", slot);

    int status = 503;
    char *resp_body = NULL;
    bool ok = uart_bridge_request_c6("POST", "/switch/pair", req_body, &status, &resp_body);
    if (ok && resp_body) {
        ESP_LOGI(TAG, "Pair response [%d]: %s", status, resp_body);
        free(resp_body);
    } else {
        ESP_LOGW(TAG, "Failed to reach Zigbee coordinator (C6 offline?)");
    }
}

static void toggle_switch(const char *slot)
{
    ESP_LOGI(TAG, "Hardware button: Toggling %s switch...", slot);

    int status = 503;
    char *resp_body = NULL;
    bool ok = uart_bridge_request_c6("GET", "/switches", "", &status, &resp_body);

    bool currently_on = false;
    if (ok && resp_body) {
        cJSON *root = cJSON_Parse(resp_body);
        if (root) {
            cJSON *sw_obj = cJSON_GetObjectItem(root, slot);
            if (sw_obj) {
                cJSON *state = cJSON_GetObjectItem(sw_obj, "state");
                if (state && cJSON_IsString(state) && strcmp(state->valuestring, "on") == 0) {
                    currently_on = true;
                }
            }
            cJSON_Delete(root);
        }
        free(resp_body);
    }

    char path[32];
    snprintf(path, sizeof(path), "/switch/%s/%s", slot, currently_on ? "off" : "on");
    ESP_LOGI(TAG, "Sending %s command...", path);

    char *toggle_resp = NULL;
    uart_bridge_request_c6("POST", path, "", &status, &toggle_resp);
    if (toggle_resp) free(toggle_resp);
}

static void process_button(button_tracker_t *btn, int btn_id)
{
    // Active LOW (pressed = 0, released = 1)
    bool pressed = (gpio_get_level(btn->pin) == 0);
    uint32_t now = xTaskGetTickCount();

    if (pressed && !btn->is_down) {
        // Button down
        btn->is_down = true;
        btn->press_start_tick = now;
    } else if (!pressed && btn->is_down) {
        // Button released
        btn->is_down = false;
        uint32_t duration_ms = (now - btn->press_start_tick) * portTICK_PERIOD_MS;

        if (duration_ms >= 40 && duration_ms < 1500) {
            // Short press
            ESP_LOGI(TAG, "Button GPIO%d short press (%lu ms)", btn->pin, (unsigned long)duration_ms);
            if (btn_id == 0) {
                // BOOT button short press -> Pair Top
                trigger_pair("top");
            } else if (btn_id == 1) {
                // Top button short press -> Pair Top
                trigger_pair("top");
            } else if (btn_id == 2) {
                // Bottom button short press -> Pair Bottom
                trigger_pair("bottom");
            }
        } else if (duration_ms >= 1500) {
            // Long press
            ESP_LOGI(TAG, "Button GPIO%d long press (%lu ms)", btn->pin, (unsigned long)duration_ms);
            if (btn_id == 0) {
                // BOOT button long press -> Pair Bottom
                trigger_pair("bottom");
            } else if (btn_id == 1) {
                // Top button long press -> Toggle Top relay
                toggle_switch("top");
            } else if (btn_id == 2) {
                // Bottom button long press -> Toggle Bottom relay
                toggle_switch("bottom");
            }
        }
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Buttons task started (BOOT:GPIO0, TOP:GPIO4, BOT:GPIO5)");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20)); // 20ms debounce polling
        process_button(&s_btn_boot, 0);
        process_button(&s_btn_top,  1);
        process_button(&s_btn_bot,  2);
    }
}

void buttons_init(void)
{
    gpio_config_t cfg = {};
    cfg.mode         = GPIO_MODE_INPUT;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type    = GPIO_INTR_DISABLE;

    // Configure GPIO0 (BOOT), GPIO4 (Top Button), GPIO5 (Bottom Button)
    cfg.pin_bit_mask = (1ULL << BUTTON_BOOT_GPIO) |
                       (1ULL << BUTTON_TOP_GPIO)  |
                       (1ULL << BUTTON_BOT_GPIO);
    ESP_ERROR_CHECK(gpio_config(&cfg));

    xTaskCreatePinnedToCore(buttons_task, "buttons", 4096, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "Hardware buttons initialized on GPIO0 (Boot), GPIO4 (Top), GPIO5 (Bottom)");
}
