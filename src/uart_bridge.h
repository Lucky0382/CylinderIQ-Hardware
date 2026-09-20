#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — UART Bridge (ESP32-S3 side)
//
// UART1: GPIO17 TX, GPIO18 RX @ 921600 baud, 8N1
// Protocol: line-delimited JSON (one JSON object per \n)
//
// Inbound from C6:
//   {"type":"req","id":N,"method":"GET","path":"/sensors","body":""}\n
//
// Outbound to C6 (responses):
//   {"type":"res","id":N,"status":200,"body":"<json-string>"}\n
//
// Outbound to C6 (unsolicited sensor push every 5 s):
//   {"type":"push","sensors":{...}}\n
//
// Call uart_bridge_init() once, then start uart_bridge_rx_task()
// and uart_bridge_push_task() as FreeRTOS tasks.
// ──────────────────────────────────────────────────────────────

#define UART_BRIDGE_PORT    UART_NUM_1
#define UART_BRIDGE_TX_PIN  17      // GPIO17 S3→C6
#define UART_BRIDGE_RX_PIN  18      // GPIO18 C6→S3
#define UART_BRIDGE_BAUD    921600
#define UART_BRIDGE_BUF     8192    // RX ring buffer size

// Initialise UART1 hardware. Call before starting tasks.
void uart_bridge_init(void);

// FreeRTOS task: reads lines from C6, dispatches to api_handlers,
// sends {"type":"res",...} responses. Never returns.
void uart_bridge_rx_task(void *arg);

// FreeRTOS task: sends {"type":"push","sensors":{...}} every 5 s.
// Never returns.
void uart_bridge_push_task(void *arg);

// Send a pre-formatted response JSON string over UART (appends \n).
// Safe to call from any task — uses internal mutex.
void uart_bridge_send(const char *json_str);

#ifdef __cplusplus
}
#endif
