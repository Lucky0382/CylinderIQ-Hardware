#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — WiFi AP (ESP32-S3)
//
// Starts a SoftAP:
//   SSID:     "CylinderIQ"
//   Password: "cylinderiq"
//   IP:       192.168.4.1 (fixed, assigned to AP interface)
//   DHCP:     192.168.4.2 – 192.168.4.5 for clients
//   Max clients: 4
// ──────────────────────────────────────────────────────────────

#define WIFI_AP_SSID        "CylinderIQ"
#define WIFI_AP_PASSWORD    "cylinderiq"
#define WIFI_AP_MAX_CONN    4
#define WIFI_AP_CHANNEL     1

void wifi_ap_init(void);

#ifdef __cplusplus
}
#endif
