#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — HTTP Server (ESP32-C6)
//
// Runs esp_http_server on port 80.
// All routes forward their request to S3 via uart_proxy_request()
// and return the S3 response body verbatim as HTTP response.
//
// Routes registered:
//   V1 (DisplayIQ ESPHome compat):
//     GET /sensor/*
//
//   V2:
//     GET  /sensors
//     GET  /state
//     GET  /profiles
//     POST /profiles/*
//     POST /active_profile
//     GET  /presets/*
//     POST /presets/*
//     GET  /wizard
//     POST /wizard/step
//     POST /wizard/complete
//     POST /calibrate/cold
//     POST /calibrate/hot
//     POST /calibrate/post_draw
//     GET  /energy
//     POST /energy
//     POST /learn/start
//     POST /learn/stop
//
// Call http_server_start() once after wifi_ap_init().
// ──────────────────────────────────────────────────────────────

void http_server_start(void);

#ifdef __cplusplus
}
#endif
