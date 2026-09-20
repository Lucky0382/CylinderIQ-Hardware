#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — UART Bridge (ESP32-C6 side)
//
// UART1: GPIO4 TX, GPIO5 RX @ 921600 baud, 8N1
// Cross-wired to S3: C6-GPIO4 TX → S3-GPIO18 RX
//                    C6-GPIO5 RX ← S3-GPIO17 TX
//
// Inbound from S3:
//   {"type":"req","id":N,"method":"GET","path":"/switches","body":""}\n
// Outbound to S3:
//   {"type":"res","id":N,"status":200,"body":"..."}\n
// ──────────────────────────────────────────────────────────────

#define UART_PROXY_PORT        UART_NUM_1
#define UART_PROXY_TX_PIN      4       // C6 GPIO4 → S3 GPIO18(RX)
#define UART_PROXY_RX_PIN      5       // C6 GPIO5 ← S3 GPIO17(TX)
#define UART_PROXY_BAUD        921600
#define UART_PROXY_BUF         8192    // RX ring buffer size

void uart_proxy_init(void);
void uart_proxy_rx_task(void *arg);
void uart_proxy_send(const char *json_str);

#ifdef __cplusplus
}
#endif
