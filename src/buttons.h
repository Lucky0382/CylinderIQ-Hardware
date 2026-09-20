#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ──────────────────────────────────────────────────────────────
// CylinderIQ Hub V2 — Hardware Buttons (ESP32-S3)
//
// Supports physical commissioning & manual override buttons:
//   - GPIO0: On-board BOOT button (Zero wiring required!)
//       * Short press (< 1.5s): Pair Top Switch
//       * Long press (> 1.5s):  Pair Bottom Switch
//
//   - GPIO4: External Button 1 (Top Immersion Switch)
//       * Short press (< 1.5s): Pair Top Switch (60s window)
//       * Long press (> 1.5s):  Toggle Top Switch ON/OFF
//
//   - GPIO5: External Button 2 (Bottom Immersion Switch)
//       * Short press (< 1.5s): Pair Bottom Switch (60s window)
//       * Long press (> 1.5s):  Toggle Bottom Switch ON/OFF
//
// Wiring: Momentary push button connected between GPIO pin and GND.
// Uses internal pull-up resistors — NO external resistors needed!
// ──────────────────────────────────────────────────────────────

#define BUTTON_BOOT_GPIO    0   // On-board BOOT button
#define BUTTON_TOP_GPIO     4   // External Button 1 (Top)
#define BUTTON_BOT_GPIO     5   // External Button 2 (Bottom)

void buttons_init(void);

#ifdef __cplusplus
}
#endif
