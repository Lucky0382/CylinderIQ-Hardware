// CylinderIQ Hub V2 — http_server.cpp (ESP32-S3)
// Runs esp_http_server on port 80.
// All sensor, calculation, profile, wizard, and energy routes dispatch in-memory
// via api_dispatch(). Switch routes are forwarded to C6 via uart_bridge_request_c6().

#include "http_server.h"
#include "api_handlers.h"
#include "uart_bridge.h"

#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "http_server_s3";

// ── CORS Headers ───────────────────────────────────────────────
static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

static esp_err_t handler_options(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ── Helpers ───────────────────────────────────────────────────
static char *read_req_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        return NULL;
    }
    char *body = (char *)malloc(req->content_len + 1);
    if (!body) return NULL;
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        free(body);
        return NULL;
    }
    body[received] = '\0';
    return body;
}

static esp_err_t send_json_response(httpd_req_t *req, int status, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    add_cors_headers(req);

    if      (status == 200) httpd_resp_set_status(req, "200 OK");
    else if (status == 204) httpd_resp_set_status(req, "204 No Content");
    else if (status == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (status == 404) httpd_resp_set_status(req, "404 Not Found");
    else if (status == 409) httpd_resp_set_status(req, "409 Conflict");
    else if (status == 503) httpd_resp_set_status(req, "503 Service Unavailable");
    else if (status == 504) httpd_resp_set_status(req, "504 Gateway Timeout");
    else                    httpd_resp_set_status(req, "500 Internal Server Error");

    const char *to_send = body ? body : "{}";
    return httpd_resp_send(req, to_send, strlen(to_send));
}

// ── Local In-Memory S3 API Dispatcher ──────────────────────────
static esp_err_t dispatch_local_s3(httpd_req_t *req,
                                  const char  *method,
                                  const char  *path_override)
{
    char *body = read_req_body(req);
    const char *path = path_override ? path_override : req->uri;

    char *resp_body = NULL;
    int   resp_status = 200;

    api_dispatch(method, path, body ? body : "", &resp_body, &resp_status);
    if (body) free(body);

    esp_err_t err = send_json_response(req, resp_status, resp_body);
    if (resp_body) free(resp_body);
    return err;
}

// ── V1 Compat: GET /sensor/<name> ──────────────────────────────
static esp_err_t handler_sensor_v1(httpd_req_t *req)
{
    return dispatch_local_s3(req, "GET", req->uri);
}

// ── V2 GET Handlers ────────────────────────────────────────────
static esp_err_t handler_get_sensors(httpd_req_t *req)  { return dispatch_local_s3(req, "GET", "/sensors"); }
static esp_err_t handler_get_state(httpd_req_t *req)    { return dispatch_local_s3(req, "GET", "/state"); }
static esp_err_t handler_get_profiles(httpd_req_t *req) { return dispatch_local_s3(req, "GET", "/profiles"); }
static esp_err_t handler_get_wizard(httpd_req_t *req)   { return dispatch_local_s3(req, "GET", "/wizard"); }
static esp_err_t handler_get_energy(httpd_req_t *req)   { return dispatch_local_s3(req, "GET", "/energy"); }
static esp_err_t handler_get_presets(httpd_req_t *req)  { return dispatch_local_s3(req, "GET", req->uri); }

// ── V2 POST Handlers ───────────────────────────────────────────
static esp_err_t handler_post_profile(httpd_req_t *req)         { return dispatch_local_s3(req, "POST", req->uri); }
static esp_err_t handler_post_active_profile(httpd_req_t *req)  { return dispatch_local_s3(req, "POST", "/active_profile"); }
static esp_err_t handler_post_preset(httpd_req_t *req)          { return dispatch_local_s3(req, "POST", req->uri); }
static esp_err_t handler_post_wizard_step(httpd_req_t *req)     { return dispatch_local_s3(req, "POST", "/wizard/step"); }
static esp_err_t handler_post_wizard_complete(httpd_req_t *req) { return dispatch_local_s3(req, "POST", "/wizard/complete"); }
static esp_err_t handler_post_cal_cold(httpd_req_t *req)        { return dispatch_local_s3(req, "POST", "/calibrate/cold"); }
static esp_err_t handler_post_cal_hot(httpd_req_t *req)         { return dispatch_local_s3(req, "POST", "/calibrate/hot"); }
static esp_err_t handler_post_cal_post_draw(httpd_req_t *req)   { return dispatch_local_s3(req, "POST", "/calibrate/post_draw"); }
static esp_err_t handler_post_energy(httpd_req_t *req)          { return dispatch_local_s3(req, "POST", "/energy"); }
static esp_err_t handler_post_learn_start(httpd_req_t *req)     { return dispatch_local_s3(req, "POST", "/learn/start"); }
static esp_err_t handler_post_learn_stop(httpd_req_t *req)      { return dispatch_local_s3(req, "POST", "/learn/stop"); }

// ── Switch Control (Forwarded to C6 via UART) ─────────────────
static esp_err_t handler_get_switches(httpd_req_t *req)
{
    int status = 503;
    char *body = NULL;
    if (uart_bridge_request_c6("GET", "/switches", "", &status, &body) && body) {
        esp_err_t err = send_json_response(req, status, body);
        free(body);
        return err;
    }
    // Safe fallback if C6 coordinator is offline / not yet booted:
    const char *fallback = "{\"top\":{\"paired\":false,\"state\":\"off\"},\"bottom\":{\"paired\":false,\"state\":\"off\"},\"coordinator_online\":false}";
    return send_json_response(req, 200, fallback);
}

static esp_err_t forward_switch_request(httpd_req_t *req, const char *method)
{
    char *req_body = read_req_body(req);
    int status = 503;
    char *resp_body = NULL;

    bool ok = uart_bridge_request_c6(method, req->uri, req_body ? req_body : "", &status, &resp_body);
    if (req_body) free(req_body);

    if (ok && resp_body) {
        esp_err_t err = send_json_response(req, status, resp_body);
        free(resp_body);
        return err;
    }

    return send_json_response(req, 503, "{\"ok\":false,\"error\":\"Zigbee coordinator offline\"}");
}

static esp_err_t handler_switch_top_on(httpd_req_t *req)     { return forward_switch_request(req, "POST"); }
static esp_err_t handler_switch_top_off(httpd_req_t *req)    { return forward_switch_request(req, "POST"); }
static esp_err_t handler_switch_bottom_on(httpd_req_t *req)  { return forward_switch_request(req, "POST"); }
static esp_err_t handler_switch_bottom_off(httpd_req_t *req) { return forward_switch_request(req, "POST"); }
static esp_err_t handler_switch_pair(httpd_req_t *req)       { return forward_switch_request(req, "POST"); }
static esp_err_t handler_get_pair_state(httpd_req_t *req)    { return forward_switch_request(req, "GET");  }
static esp_err_t handler_switch_clear(httpd_req_t *req)      { return forward_switch_request(req, "POST"); }

// ── Route Table ────────────────────────────────────────────────
static const httpd_uri_t s_routes[] = {
    // CORS OPTIONS pre-flight
    { .uri = "/*",                 .method = HTTP_OPTIONS, .handler = handler_options,              .user_ctx = NULL },

    // Switch control (forwarded to C6 coordinator)
    { .uri = "/switches",          .method = HTTP_GET,  .handler = handler_get_switches,            .user_ctx = NULL },
    { .uri = "/switch/top/on",     .method = HTTP_POST, .handler = handler_switch_top_on,           .user_ctx = NULL },
    { .uri = "/switch/top/off",    .method = HTTP_POST, .handler = handler_switch_top_off,          .user_ctx = NULL },
    { .uri = "/switch/bottom/on",  .method = HTTP_POST, .handler = handler_switch_bottom_on,        .user_ctx = NULL },
    { .uri = "/switch/bottom/off", .method = HTTP_POST, .handler = handler_switch_bottom_off,       .user_ctx = NULL },
    { .uri = "/switch/pair",       .method = HTTP_POST, .handler = handler_switch_pair,             .user_ctx = NULL },
    { .uri = "/switch/pair",       .method = HTTP_GET,  .handler = handler_get_pair_state,          .user_ctx = NULL },
    { .uri = "/switch/clear",      .method = HTTP_POST, .handler = handler_switch_clear,            .user_ctx = NULL },

    // V1 ESPHome compatibility (wildcard)
    { .uri = "/sensor/*",          .method = HTTP_GET,  .handler = handler_sensor_v1,               .user_ctx = NULL },

    // V2 GET routes
    { .uri = "/sensors",           .method = HTTP_GET,  .handler = handler_get_sensors,             .user_ctx = NULL },
    { .uri = "/state",             .method = HTTP_GET,  .handler = handler_get_state,               .user_ctx = NULL },
    { .uri = "/profiles",          .method = HTTP_GET,  .handler = handler_get_profiles,            .user_ctx = NULL },
    { .uri = "/presets/*",         .method = HTTP_GET,  .handler = handler_get_presets,             .user_ctx = NULL },
    { .uri = "/wizard",            .method = HTTP_GET,  .handler = handler_get_wizard,              .user_ctx = NULL },
    { .uri = "/energy",            .method = HTTP_GET,  .handler = handler_get_energy,              .user_ctx = NULL },

    // V2 POST routes
    { .uri = "/profiles/*",        .method = HTTP_POST, .handler = handler_post_profile,            .user_ctx = NULL },
    { .uri = "/active_profile",    .method = HTTP_POST, .handler = handler_post_active_profile,     .user_ctx = NULL },
    { .uri = "/presets/*",         .method = HTTP_POST, .handler = handler_post_preset,             .user_ctx = NULL },
    { .uri = "/wizard/step",       .method = HTTP_POST, .handler = handler_post_wizard_step,        .user_ctx = NULL },
    { .uri = "/wizard/complete",   .method = HTTP_POST, .handler = handler_post_wizard_complete,    .user_ctx = NULL },
    { .uri = "/calibrate/cold",    .method = HTTP_POST, .handler = handler_post_cal_cold,           .user_ctx = NULL },
    { .uri = "/calibrate/hot",     .method = HTTP_POST, .handler = handler_post_cal_hot,            .user_ctx = NULL },
    { .uri = "/calibrate/post_draw",.method = HTTP_POST,.handler = handler_post_cal_post_draw,      .user_ctx = NULL },
    { .uri = "/energy",            .method = HTTP_POST, .handler = handler_post_energy,             .user_ctx = NULL },
    { .uri = "/learn/start",       .method = HTTP_POST, .handler = handler_post_learn_start,        .user_ctx = NULL },
    { .uri = "/learn/stop",        .method = HTTP_POST, .handler = handler_post_learn_stop,         .user_ctx = NULL },
};

#define ROUTE_COUNT (sizeof(s_routes) / sizeof(s_routes[0]))

// ── Server Start ───────────────────────────────────────────────
void http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 40;
    config.uri_match_fn     = httpd_uri_match_wildcard;
    config.stack_size       = 8192;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    for (size_t i = 0; i < ROUTE_COUNT; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &s_routes[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register route %s: %s",
                     s_routes[i].uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "HTTP server started on port 80 (%zu routes)", ROUTE_COUNT);
}
