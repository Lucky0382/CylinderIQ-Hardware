# Handover: Web Design Agent — CylinderIQ Hub Web Dashboard

**Target Agent:** Web Front-End Engineer / Embedded Web Specialist  
**Project:** CylinderIQ Embedded On-Chip Web Dashboard & Web Client  
**Context:** Green Energy Group (GEG) Cyprus Renewable Curtailment Monetization Pilot  

---

## 1. Scope & Objective

The Web Design Agent is responsible for the web-based interfaces of CylinderIQ:
1. **Embedded On-Chip Hub Web Dashboard:** Served directly by the ESP32-S3 microcontroller (`http://192.168.4.1/`) from C++ flash memory.
2. **Cloud/Local Web Application:** Desktop and tablet browser experience for residents, installers, and fleet managers.

*(Note: Per project guidelines, this handover excludes subjective visual styling advice and outlines strictly technical requirements, functional specifications, data models, state machines, and API contracts).*

---

## 2. Microcontroller Constraints for On-Chip Web UI

When modifying or replacing the embedded dashboard in `src/web_dashboard.cpp`:
- **Single-Page Self-Contained:** Must be delivered as a single raw string literal in C++ (`R"rawliteral(...)rawliteral"`).
- **Offline / Zero-CDN Dependency:** When connected to the ESP32-S3 SoftAP (`CylinderIQ`), the user has no internet access. Do NOT rely on external CDNs (e.g. Google Fonts, unpkg, cdnjs, external FontAwesome). All icons should be inline SVGs or unicode emojis; all styles must be internal CSS.
- **Resource Footprint:** Keep payload size under 50KB uncompressed to avoid HTTP server socket buffer overruns on the ESP32-S3.
- **Polling Strategy:** Use `setInterval()` polling at **1000ms–2000ms** intervals with `fetch()`. Always wrap in `try/catch` with request deduplication to prevent flooding the microcontroller.

---

## 3. Functional Requirements & Technical Data Contracts

### A. Telemetry & Sensor States
The UI must poll and display 4 distinct physical sensor readings and 2 calculated metrics:
- **`hot_outlet`**: Temperature in °C at the top outlet pipe.
- **`cyl_inlet`**: Temperature in °C at the cold inlet pipe downstream of the non-return valve.
- **`mains_supply`**: Temperature in °C of incoming water mains.
- **`tundish`**: Temperature in °C on the pressure relief discharge pipe.
- **`usable_liters`**: Calculated volume of water above usable threshold (typically $\ge 40^\circ\text{C}$).
- **`stored_kwh`**: Thermal storage calculated from tank water volume and $\Delta T$.
- **`leak_detected`**: Boolean state from the physical leak rope sensor.

### B. Zigbee Immersion Switch Management
Controls two independent high-current loads via the ESP32-C6 coordinator:
- **Slots:** `top` (Element 1) and `bottom` (Element 2).
- **Slot States:**
  - `paired` (boolean)
  - `state` (`"on"` | `"off"`)
  - `addr` (16-bit hexadecimal Zigbee short address, e.g. `"0x4A12"`)
- **Actions Required:**
  - `POST /switch/top/on` / `POST /switch/top/off`
  - `POST /switch/bottom/on` / `POST /switch/bottom/off`
  - `POST /switch/clear` with payload `{"slot": "top"|"bottom"}`

### C. Zigbee Pairing State Machine
- **Trigger:** User initiates pairing for a slot (`POST /switch/pair` with `{"slot":"top", "duration":180}`).
- **Coordinator Response:** Returns `{"ok":true, "slot":"top", "duration_s":180}`.
- **Status Polling:** Frontend polls `GET /switch/pair`:
  - When `open == true`: UI displays an active countdown banner showing remaining seconds (`remaining_s`).
  - When `open == false` or `remaining_s == 0`: Banner automatically dismisses.
  - When `paired == true` during pairing: UI updates switch badge to "Paired", displays 16-bit short address, and enables On/Off toggle buttons.

### D. Network & Coordinator Diagnostics
The UI must display RF health indicators:
- **Zigbee Channel:** Fixed to Channel 20 (2450 MHz).
- **Zigbee PAN ID:** Displayed in 16-bit hex (e.g. `0xA276`).
- **Coordinator Status:** Online / Offline boolean.
- **Wi-Fi Mode:** SoftAP status (`CylinderIQ`) and Station status (Connected / Disconnected / IP Address).

### E. Home Assistant Integration Assistance
- **Endpoints:** `GET /ha/status` and `GET /ha/yaml`.
- **Functionality:** Provides auto-discovery YAML code snippets for Home Assistant users to copy/paste into their `configuration.yaml` with pre-populated sensor entity definitions.

---

## 4. API Specification Summary

| Method | Path | Request Body | Response Format | Description |
| :--- | :--- | :--- | :--- | :--- |
| `GET` | `/` | None | `text/html` | Embedded Web Dashboard |
| `GET` | `/api/status` | None | JSON | Full system telemetry, temperatures, metrics, and switch states |
| `GET` | `/switches` | None | JSON | Zigbee coordinator status, PAN ID, channel, and switch slot data |
| `POST` | `/switch/<slot>/<state>` | None | JSON `{"ok":true}` | Toggles switch slot (`top` or `bottom`) to `on` or `off` |
| `POST` | `/switch/pair` | `{"slot":"top","duration":180}` | JSON `{"ok":true,"duration_s":180}` | Opens Zigbee network for pairing |
| `GET` | `/switch/pair` | None | JSON `{"open":bool,"slot":str,"remaining_s":int}` | Polls pairing countdown status |
| `POST` | `/switch/clear` | `{"slot":"top"}` | JSON `{"ok":true}` | Unpairs slot and deletes stored NVS binding |
| `POST` | `/api/wifi` | `{"ssid":"...","password":"..."}` | JSON `{"ok":true}` | Provisions home Wi-Fi station credentials |
| `GET` | `/ha/yaml` | None | `text/plain` | Generates copy-paste YAML for Home Assistant integration |

---

## 5. Where to Find Inspiration & Existing Code

1. **Live Embedded Hub Dashboard Implementation:**
   - `C:\Users\AAEin\Documents\cylinderiq-hub-v2\src\web_dashboard.cpp`
2. **Enterprise UI Architecture Template (726 lines of CSS, layout, SVG gauges, and animations):**
   - `C:\Users\AAEin\Documents\cylinderiq-hub-v2\display_iq_dashboard.html`
3. **React / Next.js Implementation:**
   - `C:\Users\AAEin\Documents\cylinderiq-dashboard\components\ImmersionOsUi.tsx`
4. **Firmware HTTP Endpoint Implementation & Dispatches:**
   - `C:\Users\AAEin\Documents\cylinderiq-hub-v2\src\http_server.cpp`
   - `C:\Users\AAEin\Documents\cylinderiq-hub-v2\src\api_handlers.cpp`
   - `C:\Users\AAEin\Documents\cylinderiq-hub-v2\src_c6\switch_control.cpp`
