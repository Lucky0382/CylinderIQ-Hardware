#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — HTTP Server (ESP32-S3)
//
// Runs esp_http_server on port 80.
// All calculation, sensor, profile, energy, wizard, calibration,
// and state endpoints are dispatched directly to memory via api_dispatch().
// Switch endpoints (/switches, /switch/*) are forwarded over UART
// to the C6 Zigbee coordinator via uart_bridge_request_c6().
//
// Call http_server_start() once after wifi_ap_init().
// ──────────────────────────────────────────────────────────────

void http_server_start(void);

#ifdef __cplusplus
}
#endif
