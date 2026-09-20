#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — UART Proxy (ESP32-C6 side)
//
// UART1: GPIO4 TX, GPIO5 RX @ 921600 baud, 8N1
// (Cross-wired to S3: C6-GPIO4 TX → S3-GPIO18 RX)
//                       (C6-GPIO5 RX ← S3-GPIO17 TX)
//
// Sends JSON request lines to S3, waits for matching JSON response.
// Multiple simultaneous HTTP requests are serialised via a mutex:
// one request in-flight to S3 at a time. Timeout: 3 s.
//
// Call uart_proxy_init() once, then start uart_proxy_rx_task().
// HTTP handlers call uart_proxy_request() to get a response.
// ──────────────────────────────────────────────────────────────

#define UART_PROXY_PORT     UART_NUM_1
#define UART_PROXY_TX_PIN   4       // C6 GPIO4 → S3 GPIO18(RX)
#define UART_PROXY_RX_PIN   5       // C6 GPIO5 ← S3 GPIO17(TX)
#define UART_PROXY_BAUD     921600
#define UART_PROXY_BUF      8192    // RX ring buffer size
#define UART_PROXY_TIMEOUT_MS 3000  // max wait for S3 response

// Initialise UART1 hardware and internal state. Call before starting task.
void uart_proxy_init(void);

// FreeRTOS task: reads response lines from S3, delivers to waiting callers.
// Never returns. Stack: 4096 bytes.
void uart_proxy_rx_task(void *arg);

// Send a request to S3 and block until a response arrives (or timeout).
// method  — "GET" or "POST"
// path    — URL path (URL-encoded), e.g. "/sensor/Hot%20Outlet"
// body    — request body JSON string, or "" for GET
// out_status  — HTTP status code from S3 response
// out_body    — heap-allocated response body JSON (caller must free), or NULL
// Returns true if a response was received within timeout.
bool uart_proxy_request(const char *method,
                        const char *path,
                        const char *body,
                        int        *out_status,
                        char      **out_body);

#ifdef __cplusplus
}
#endif
