#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — WiFi Dual Mode AP + STA (ESP32-S3)
//
// Starts SoftAP:
//   SSID:     "CylinderIQ"
//   Password: "cylinderiq"
//   IP:       192.168.4.1 (fixed, assigned to AP interface)
//   DHCP:     192.168.4.2 – 192.168.4.5
//
// Also supports STA client mode (connects to Home Wi-Fi):
//   - Allows Home Assistant on local LAN to poll hub & log data
//   - SoftAP remains active simultaneously!
// ──────────────────────────────────────────────────────────────

#define WIFI_AP_SSID        "CylinderIQ"
#define WIFI_AP_PASSWORD    "cylinderiq"
#define WIFI_AP_MAX_CONN    4
#define WIFI_AP_CHANNEL     1

void wifi_ap_init(void);
void wifi_connect_sta(const char *ssid, const char *password);
bool wifi_get_sta_ip(char *out_ip, size_t max_len);

#ifdef __cplusplus
}
#endif
