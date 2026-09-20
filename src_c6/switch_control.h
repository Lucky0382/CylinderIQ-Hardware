// CylinderIQ Hub V2 — switch_control.h (ESP32-C6)
// HTTP handlers for Zigbee switch control endpoints.
// These are handled LOCALLY on the C6 — NOT forwarded to S3.

#pragma once
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

// GET  /switches          → {"top":{...},"bottom":{...}}
esp_err_t handler_get_switches(httpd_req_t *req);

// POST /switch/top/on     → ZCL On  to top MOES
// POST /switch/top/off    → ZCL Off to top MOES
esp_err_t handler_switch_top_on (httpd_req_t *req);
esp_err_t handler_switch_top_off(httpd_req_t *req);

// POST /switch/bottom/on  → ZCL On  to bottom MOES
// POST /switch/bottom/off → ZCL Off to bottom MOES
esp_err_t handler_switch_bottom_on (httpd_req_t *req);
esp_err_t handler_switch_bottom_off(httpd_req_t *req);

// POST /switch/pair       body: {"slot":"top"}  or {"slot":"bottom"}
//   Opens permit join window for 60s. Next MOES to join fills that slot.
esp_err_t handler_switch_pair(httpd_req_t *req);

// GET  /switch/pair       → {"open":true,"slot":"top","remaining_s":45}
esp_err_t handler_get_pair_state(httpd_req_t *req);

// POST /switch/clear      body: {"slot":"top"}
//   Forgets a paired switch from NVS.
esp_err_t handler_switch_clear(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
