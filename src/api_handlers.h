#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — API Handlers (ESP32-S3)
//
// api_dispatch() is the single entry point called by uart_bridge.
// It routes (method, path) to the appropriate handler function,
// each of which returns an allocated JSON body string and HTTP status.
//
// V2 endpoints:
//   GET  /sensors
//   GET  /state
//   GET  /profiles
//   POST /profiles/{id}
//   POST /active_profile
//   GET  /presets/{pid}
//   POST /presets/{pid}/{name}
//   GET  /wizard
//   POST /wizard/step
//   POST /wizard/complete
//   POST /calibrate/cold
//   POST /calibrate/hot
//   POST /calibrate/post_draw
//   GET  /energy
//   POST /energy
//   POST /learn/start
//   POST /learn/stop
//
// V1 compatibility (DisplayIQ V2 ESPHome polling style):
//   GET  /sensor/Hot%20Outlet             → {"value":62.5,"state":"62.5"}
//   GET  /sensor/Cylinder%20Inlet
//   GET  /sensor/Mains%20Supply
//   GET  /sensor/Usable%20Hot%20Water
//   GET  /sensor/Hot%20Water%20Percentage
//   GET  /sensor/Showers%20Remaining
//   GET  /sensor/Baths%20Remaining
//   GET  /sensor/Recovery%20Time%20Remaining
//   GET  /sensor/Cost%20to%20Recover%20Now
// ──────────────────────────────────────────────────────────────

// Dispatch a request. On return:
//   *resp_body  — heap-allocated JSON string (caller must free), may be NULL
//   *resp_status — HTTP status code (200, 400, 404, 500 …)
void api_dispatch(const char *method,
                  const char *path,
                  const char *body,
                  char      **resp_body,
                  int        *resp_status);

#ifdef __cplusplus
}
#endif
