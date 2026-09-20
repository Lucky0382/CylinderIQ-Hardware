#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — BLE GATT Server (ESP32-S3)
//
// Service UUID:  4cad87f1-0000-0000-0000-000000000001
//   Sensor Notify: 4cad87f1-0001-0000-0000-000000000001
//     S3 → App, ~5s interval, JSON mirrors GET /sensors response.
//   Command Write: 4cad87f1-0002-0000-0000-000000000001
//     App → S3, JSON: {"cmd":"<name>", ...fields}
//     Dispatches directly to api_handlers — no duplication of logic.
//   Response Notify: 4cad87f1-0003-0000-0000-000000000001
//     S3 → App, JSON: {"cmd":"<name>","ok":true/false, ...extra fields}
//     Sent after every command write for app-level ACK.
//
// Call ble_server_init() once from app_main after nvs_store_init().
// Call ble_server_notify_sensors() from the 5s push task.
// ──────────────────────────────────────────────────────────────

// Initialise NimBLE stack, register GATT service, start advertising.
// Starts the NimBLE host FreeRTOS task internally.
void ble_server_init(void);

// Push a sensor JSON string to all subscribed BLE clients.
// Safe to call from any task. No-op if no client is connected.
// sensors_json must be a valid JSON object string (same shape as GET /sensors).
void ble_server_notify_sensors(const char *sensors_json);

#ifdef __cplusplus
}
#endif
