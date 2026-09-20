// CylinderIQ Hub V2 — wifi_ap.cpp (ESP32-S3)
// SoftAP setup (192.168.4.1) + optional STA connection to Home Wi-Fi.

#include "wifi_ap.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_s3";

static EventGroupHandle_t s_wifi_event_group;
#define AP_STARTED_BIT  BIT0
#define STA_CONNECTED_BIT BIT1

static esp_netif_t *s_ap_netif  = NULL;
static esp_netif_t *s_sta_netif = NULL;
static char s_sta_ip[32] = {0};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_START) {
            ESP_LOGI(TAG, "SoftAP started — SSID: %s (192.168.4.1)", WIFI_AP_SSID);
            xEventGroupSetBits(s_wifi_event_group, AP_STARTED_BIT);
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP client connected — MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5]);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "STA disconnected from Home Wi-Fi — reconnecting in 5s...");
            s_sta_ip[0] = '\0';
            xEventGroupClearBits(s_wifi_event_group, STA_CONNECTED_BIT);
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "STA connected to Home Network! Local IP: %s", s_sta_ip);
            xEventGroupSetBits(s_wifi_event_group, STA_CONNECTED_BIT);
        }
    }
}

static void nvs_save_sta_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool nvs_load_sta_creds(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err1 = nvs_get_str(h, "ssid", out_ssid, &ssid_len);
    esp_err_t err2 = nvs_get_str(h, "pass", out_pass, &pass_len);
    nvs_close(h);
    return (err1 == ESP_OK && err2 == ESP_OK && strlen(out_ssid) > 0);
}

void wifi_connect_sta(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return;
    ESP_LOGI(TAG, "Connecting to Home Wi-Fi: %s", ssid);

    nvs_save_sta_creds(ssid, password ? password : "");

    wifi_config_t sta_cfg = {};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid));
    if (password) {
        strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password));
    }
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    esp_wifi_connect();
}

bool wifi_get_sta_ip(char *out_ip, size_t max_len)
{
    if (s_sta_ip[0] != '\0') {
        strncpy(out_ip, s_sta_ip, max_len);
        return true;
    }
    return false;
}

void wifi_ap_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // 1. Create AP interface
    s_ap_netif = esp_netif_create_default_wifi_ap();

    // Set static IP 192.168.4.1 on AP interface
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    esp_netif_dhcps_start(s_ap_netif);

    // 2. Create STA interface (for Home LAN & Home Assistant)
    s_sta_netif = esp_netif_create_default_wifi_sta();

    // 3. Init WiFi driver
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_country_t country = {
        .cc = "GB",
        .schan = 1,
        .nchan = 13,
        .max_tx_power = 20,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    esp_wifi_set_country(&country);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Dual Mode: AP + STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Configure AP
    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid,     WIFI_AP_SSID,     sizeof(ap_cfg.ap.ssid));
    strncpy((char *)ap_cfg.ap.password, WIFI_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len       = strlen(WIFI_AP_SSID);
    ap_cfg.ap.channel        = WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = WIFI_AP_MAX_CONN;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
    ap_cfg.ap.pmf_cfg.required = false;
    ap_cfg.ap.pmf_cfg.capable  = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(78);

    // Wait for AP to come up
    xEventGroupWaitBits(s_wifi_event_group, AP_STARTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "AP ready — SSID: %s — IP 192.168.4.1", WIFI_AP_SSID);

    // Check if saved Home Wi-Fi credentials exist
    char saved_ssid[32] = {0};
    char saved_pass[64] = {0};
    if (nvs_load_sta_creds(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass))) {
        ESP_LOGI(TAG, "Found saved Home Wi-Fi credentials for: %s. Connecting...", saved_ssid);
        wifi_connect_sta(saved_ssid, saved_pass);
    }
}
