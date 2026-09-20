#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — Switch Control (ESP32-C6)
//
// Dispatches switch requests from S3 received over UART1.
// Supports:
//   GET  /switches
//   POST /switch/top/on
//   POST /switch/top/off
//   POST /switch/bottom/on
//   POST /switch/bottom/off
//   POST /switch/pair
//   GET  /switch/pair
//   POST /switch/clear
//
// Sets *out_status to HTTP status code (200, 400, 404, 409).
// Sets *out_body to heap-allocated JSON string (caller frees).
// ──────────────────────────────────────────────────────────────

void switch_control_dispatch(const char *method,
                             const char *path,
                             const char *body,
                             int        *out_status,
                             char      **out_body);

#ifdef __cplusplus
}
#endif
