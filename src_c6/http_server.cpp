// CylinderIQ Hub V2 — http_server.cpp (ESP32-C6)
// All HTTP routes. Sensor routes forwarded to S3 via uart_proxy.
// Switch routes handled locally on C6 via switch_control.

#include "http_server.h"
#include "uart_proxy.h"
#include "switch_control.h"

#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "http_server";

// CORS headers — required by ImmersionOS Android (Capacitor WebView)
static void add_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

// OPTIONS pre-flight (browsers send before cross-origin POST)
static esp_err_t handler_options(httpd_req_t *req)
{
    add_cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// ──────────────────────────────────────────────────────────────
// Helper: generic forward handler
// Reads POST body if present, calls S3, returns S3 response.
// ──────────────────────────────────────────────────────────────

static esp_err_t forward_to_s3(httpd_req_t *req,
                                const char  *method,
                                const char  *path_override)  // NULL → use req->uri
{
    // ── Read POST body ────────────────────────────────────────
    char *body = NULL;
    if (req->content_len > 0 && req->content_len < 4096) {
        body = (char *)malloc(req->content_len + 1);
        if (body) {
            int received = httpd_req_recv(req, body, req->content_len);
            if (received <= 0) {
                free(body);
                body = NULL;
            } else {
                body[received] = '\0';
            }
        }
    }

    // Determine path to forward: use path_override if provided,
    // otherwise use the URI from the request (includes query string if any)
    const char *path = path_override ? path_override : req->uri;

    // ── Forward to S3 ─────────────────────────────────────────
    int   resp_status = 500;
    char *resp_body   = NULL;
    uart_proxy_request(method, path, body ? body : "", &resp_status, &resp_body);
    if (body) free(body);

    // ── Send HTTP response ────────────────────────────────────
    httpd_resp_set_type(req, "application/json");
    add_cors_headers(req);

    char status_str[8];
    snprintf(status_str, sizeof(status_str), "%d", resp_status);
    // esp_http_server uses the status line, not just the code
    if      (resp_status == 200) httpd_resp_set_status(req, "200 OK");
    else if (resp_status == 400) httpd_resp_set_status(req, "400 Bad Request");
    else if (resp_status == 404) httpd_resp_set_status(req, "404 Not Found");
    else if (resp_status == 503) httpd_resp_set_status(req, "503 Service Unavailable");
    else if (resp_status == 504) httpd_resp_set_status(req, "504 Gateway Timeout");
    else                         httpd_resp_set_status(req, "500 Internal Server Error");

    const char *send = resp_body ? resp_body : "{}";
    esp_err_t err = httpd_resp_send(req, send, strlen(send));
    if (resp_body) free(resp_body);

    return err;
}

// ──────────────────────────────────────────────────────────────
// V1 compatibility: GET /sensor/<name>
// The wildcard URI captures everything after /sensor/
// ──────────────────────────────────────────────────────────────

static esp_err_t handler_sensor_v1(httpd_req_t *req)
{
    // req->uri is already URL-encoded, e.g. "/sensor/Hot%20Outlet"
    // Forward as-is; S3 api_handlers.cpp does the URL decoding
    return forward_to_s3(req, "GET", req->uri);
}

// ──────────────────────────────────────────────────────────────
// V2 GET handlers
// ──────────────────────────────────────────────────────────────

static esp_err_t handler_get_sensors(httpd_req_t *req)      { return forward_to_s3(req, "GET",  "/sensors"); }
static esp_err_t handler_get_state(httpd_req_t *req)        { return forward_to_s3(req, "GET",  "/state"); }
static esp_err_t handler_get_profiles(httpd_req_t *req)     { return forward_to_s3(req, "GET",  "/profiles"); }
static esp_err_t handler_get_wizard(httpd_req_t *req)       { return forward_to_s3(req, "GET",  "/wizard"); }
static esp_err_t handler_get_energy(httpd_req_t *req)       { return forward_to_s3(req, "GET",  "/energy"); }

// GET /presets/* — wildcard, pass URI through
static esp_err_t handler_get_presets(httpd_req_t *req)      { return forward_to_s3(req, "GET",  req->uri); }

// ──────────────────────────────────────────────────────────────
// V2 POST handlers
// ──────────────────────────────────────────────────────────────

static esp_err_t handler_post_profile(httpd_req_t *req)         { return forward_to_s3(req, "POST", req->uri); }
static esp_err_t handler_post_active_profile(httpd_req_t *req)  { return forward_to_s3(req, "POST", "/active_profile"); }
static esp_err_t handler_post_preset(httpd_req_t *req)          { return forward_to_s3(req, "POST", req->uri); }
static esp_err_t handler_post_wizard_step(httpd_req_t *req)     { return forward_to_s3(req, "POST", "/wizard/step"); }
static esp_err_t handler_post_wizard_complete(httpd_req_t *req) { return forward_to_s3(req, "POST", "/wizard/complete"); }
static esp_err_t handler_post_cal_cold(httpd_req_t *req)        { return forward_to_s3(req, "POST", "/calibrate/cold"); }
static esp_err_t handler_post_cal_hot(httpd_req_t *req)         { return forward_to_s3(req, "POST", "/calibrate/hot"); }
static esp_err_t handler_post_cal_post_draw(httpd_req_t *req)   { return forward_to_s3(req, "POST", "/calibrate/post_draw"); }
static esp_err_t handler_post_energy(httpd_req_t *req)          { return forward_to_s3(req, "POST", "/energy"); }
static esp_err_t handler_post_learn_start(httpd_req_t *req)     { return forward_to_s3(req, "POST", "/learn/start"); }
static esp_err_t handler_post_learn_stop(httpd_req_t *req)      { return forward_to_s3(req, "POST", "/learn/stop"); }

// ──────────────────────────────────────────────────────────────
// Route table
// ──────────────────────────────────────────────────────────────

// esp_http_server wildcard: URI ending with '*' matches any suffix.
// Order matters — more specific URIs must be registered before wildcards.

static const httpd_uri_t s_routes[] = {
    // ── CORS OPTIONS pre-flight (must be first) ───────────────
    { .uri = "/*",                 .method = HTTP_OPTIONS, .handler = handler_options,              .user_ctx = NULL },

    // ── Switch control (handled locally on C6) ────────────────
    { .uri = "/switches",          .method = HTTP_GET,  .handler = handler_get_switches,            .user_ctx = NULL },
    { .uri = "/switch/top/on",     .method = HTTP_POST, .handler = handler_switch_top_on,           .user_ctx = NULL },
    { .uri = "/switch/top/off",    .method = HTTP_POST, .handler = handler_switch_top_off,          .user_ctx = NULL },
    { .uri = "/switch/bottom/on",  .method = HTTP_POST, .handler = handler_switch_bottom_on,        .user_ctx = NULL },
    { .uri = "/switch/bottom/off", .method = HTTP_POST, .handler = handler_switch_bottom_off,       .user_ctx = NULL },
    { .uri = "/switch/pair",       .method = HTTP_POST, .handler = handler_switch_pair,             .user_ctx = NULL },
    { .uri = "/switch/pair",       .method = HTTP_GET,  .handler = handler_get_pair_state,          .user_ctx = NULL },
    { .uri = "/switch/clear",      .method = HTTP_POST, .handler = handler_switch_clear,            .user_ctx = NULL },

    // ── V1 compat — wildcard last ─────────────────────────────
    { .uri = "/sensor/*",          .method = HTTP_GET,  .handler = handler_sensor_v1,               .user_ctx = NULL },

    // ── V2 GET (forwarded to S3) ──────────────────────────────
    { .uri = "/sensors",           .method = HTTP_GET,  .handler = handler_get_sensors,             .user_ctx = NULL },
    { .uri = "/state",             .method = HTTP_GET,  .handler = handler_get_state,               .user_ctx = NULL },
    { .uri = "/profiles",          .method = HTTP_GET,  .handler = handler_get_profiles,            .user_ctx = NULL },
    { .uri = "/presets/*",         .method = HTTP_GET,  .handler = handler_get_presets,             .user_ctx = NULL },
    { .uri = "/wizard",            .method = HTTP_GET,  .handler = handler_get_wizard,              .user_ctx = NULL },
    { .uri = "/energy",            .method = HTTP_GET,  .handler = handler_get_energy,              .user_ctx = NULL },

    // ── V2 POST (forwarded to S3) ─────────────────────────────
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

// ──────────────────────────────────────────────────────────────
// Start
// ──────────────────────────────────────────────────────────────

void http_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 40;   // bumped for switch routes
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

    ESP_LOGI(TAG, "HTTP server started on port 80 — %zu routes", ROUTE_COUNT);
}
