// CylinderIQ Hub V2 — web_dashboard.cpp (ESP32-S3)
// Web Management Dashboard served at http://192.168.4.1/

#include "web_dashboard.h"

static const char s_dashboard_html[] = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>CylinderIQ Hub</title>
<style>
  :root {
    --bg: #0f172a;
    --card: #1e293b;
    --border: #334155;
    --text: #f8fafc;
    --muted: #94a3b8;
    --primary: #38bdf8;
    --primary-hover: #0ea5e9;
    --success: #22c55e;
    --danger: #ef4444;
    --warning: #f59e0b;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 16px; min-height: 100vh; }
  .container { max-width: 800px; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }
  header { display: flex; justify-content: space-between; align-items: center; padding-bottom: 12px; border-bottom: 1px solid var(--border); }
  h1 { font-size: 1.5rem; display: flex; align-items: center; gap: 8px; }
  .badge { background: #064e3b; color: #34d399; font-size: 0.75rem; padding: 4px 8px; border-radius: 9999px; font-weight: 600; }
  .card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 16px; }
  h2 { font-size: 1.1rem; margin-bottom: 12px; color: var(--primary); display: flex; align-items: center; gap: 8px; }
  .grid-3 { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 12px; }
  .grid-2 { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 12px; }
  .metric { background: #0f172a; padding: 12px; border-radius: 8px; text-align: center; border: 1px solid #1e293b; }
  .metric-label { font-size: 0.75rem; color: var(--muted); margin-bottom: 4px; text-transform: uppercase; }
  .metric-value { font-size: 1.4rem; font-weight: 700; color: #fff; }
  .metric-unit { font-size: 0.85rem; font-weight: normal; color: var(--muted); }
  .progress-bar-bg { background: #334155; height: 10px; border-radius: 5px; overflow: hidden; margin-top: 8px; }
  .progress-bar { background: linear-gradient(90deg, #38bdf8, #22c55e); height: 100%; width: 0%; transition: width 0.3s; }
  
  .switch-box { background: #0f172a; border: 1px solid var(--border); border-radius: 8px; padding: 12px; display: flex; flex-direction: column; gap: 10px; }
  .switch-header { display: flex; justify-content: space-between; align-items: center; }
  .switch-name { font-weight: 600; font-size: 0.95rem; }
  .status-tag { font-size: 0.7rem; padding: 2px 6px; border-radius: 4px; font-weight: 600; }
  .status-paired { background: #064e3b; color: #34d399; }
  .status-unpaired { background: #450a0a; color: #f87171; }
  .status-on { background: #1e3a8a; color: #60a5fa; }
  .status-off { background: #334155; color: #94a3b8; }
  
  .btn-group { display: flex; gap: 8px; flex-wrap: wrap; }
  button { cursor: pointer; border: none; border-radius: 6px; padding: 8px 12px; font-size: 0.85rem; font-weight: 600; transition: all 0.2s; }
  .btn-primary { background: var(--primary); color: #0f172a; }
  .btn-primary:hover { background: var(--primary-hover); }
  .btn-success { background: var(--success); color: #fff; }
  .btn-danger { background: var(--danger); color: #fff; }
  .btn-secondary { background: var(--border); color: var(--text); }
  .btn-secondary:hover { background: #475569; }
  
  .banner { background: #1e1b4b; border: 1px solid #4338ca; color: #c7d2fe; padding: 12px; border-radius: 8px; font-size: 0.9rem; display: none; }
  .banner.active { display: block; animation: pulse 2s infinite; }
  @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }

  input[type="text"], input[type="password"] {
    background: #0f172a; border: 1px solid var(--border); color: #fff; padding: 8px 12px; border-radius: 6px; width: 100%; margin-bottom: 8px;
  }
  code { background: #0f172a; padding: 2px 6px; border-radius: 4px; font-family: monospace; font-size: 0.85rem; color: #38bdf8; }
  pre { background: #0f172a; padding: 12px; border-radius: 8px; overflow-x: auto; font-family: monospace; font-size: 0.8rem; color: #94a3b8; border: 1px solid #1e293b; }
</style>
</head>
<body>
<div class="container">
  <header>
    <h1><span>♨️</span> CylinderIQ Hub</h1>
    <span class="badge" id="hub-badge">Hub Active</span>
  </header>

  <!-- Pair Window Active Banner -->
  <div id="pair-banner" class="banner">
    ⏳ <strong>Zigbee Pairing Active:</strong> Open network for slot <span id="pair-slot-label" style="text-transform: uppercase;">TOP</span>. Put your MOES switch in pairing mode (hold button 5s until LED flashes). <strong id="pair-countdown">60s</strong> remaining.
  </div>

  <!-- Live Water & Sensor Telemetry -->
  <div class="card">
    <h2>Hot Water & Temperatures</h2>
    <div class="grid-3">
      <div class="metric">
        <div class="metric-label">Hot Outlet</div>
        <div class="metric-value" id="val-hot-outlet">--</div>
        <div class="metric-unit">°C</div>
      </div>
      <div class="metric">
        <div class="metric-label">Cylinder Inlet</div>
        <div class="metric-value" id="val-cyl-inlet">--</div>
        <div class="metric-unit">°C</div>
      </div>
      <div class="metric">
        <div class="metric-label">Mains Supply</div>
        <div class="metric-value" id="val-mains">--</div>
        <div class="metric-unit">°C</div>
      </div>
    </div>
    
    <div style="margin-top: 14px;">
      <div style="display: flex; justify-content: space-between; font-size: 0.85rem;">
        <span style="color: var(--muted);">Usable Hot Water</span>
        <strong><span id="val-litres">--</span> L (<span id="val-pct">--</span>%)</strong>
      </div>
      <div class="progress-bar-bg">
        <div id="litres-bar" class="progress-bar"></div>
      </div>
    </div>

    <div class="grid-3" style="margin-top: 14px;">
      <div class="metric">
        <div class="metric-label">Showers Left</div>
        <div class="metric-value" id="val-showers">--</div>
      </div>
      <div class="metric">
        <div class="metric-label">Baths Left</div>
        <div class="metric-value" id="val-baths">--</div>
      </div>
      <div class="metric">
        <div class="metric-label">Recovery Time</div>
        <div class="metric-value" id="val-recovery">--</div>
        <div class="metric-unit">min</div>
      </div>
    </div>
  </div>

  <!-- Zigbee 20A Immersion Switches Panel -->
  <div class="card">
    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; flex-wrap: wrap; gap: 8px;">
      <h2 style="margin: 0;">Zigbee 20A Smart Immersion Switches</h2>
      <div style="font-size: 0.85rem; color: var(--muted); display: flex; gap: 10px; align-items: center;">
        <span>Coordinator: <strong id="coord-status" style="color: var(--warning)">Checking...</strong></span>
        <span>CH: <strong id="coord-channel">25</strong></span>
        <span>PAN: <strong id="coord-pan">--</strong></span>
      </div>
    </div>
    <div class="grid-2">
      <!-- Top Immersion Switch -->
      <div class="switch-box">
        <div class="switch-header">
          <div class="switch-name">Top Immersion (Peak/Boost)</div>
          <span id="top-paired-badge" class="status-tag status-unpaired">Unpaired</span>
        </div>
        <div style="display: flex; justify-content: space-between; align-items: center;">
          <span style="font-size: 0.85rem; color: var(--muted);">Relay State:</span>
          <span id="top-state-badge" class="status-tag status-off">OFF</span>
        </div>
        <div style="font-size: 0.8rem; color: var(--muted);" id="top-addr">Address: None</div>
        <div class="btn-group">
          <button class="btn-success" onclick="toggleSwitch('top', 'on')">Turn ON</button>
          <button class="btn-danger" onclick="toggleSwitch('top', 'off')">Turn OFF</button>
          <button class="btn-primary" onclick="startPair('top')">Pair Top</button>
          <button class="btn-secondary" onclick="clearSwitch('top')">Unpair</button>
        </div>
      </div>

      <!-- Bottom Immersion Switch -->
      <div class="switch-box">
        <div class="switch-header">
          <div class="switch-name">Bottom Immersion (Off-Peak/Base)</div>
          <span id="bot-paired-badge" class="status-tag status-unpaired">Unpaired</span>
        </div>
        <div style="display: flex; justify-content: space-between; align-items: center;">
          <span style="font-size: 0.85rem; color: var(--muted);">Relay State:</span>
          <span id="bot-state-badge" class="status-tag status-off">OFF</span>
        </div>
        <div style="font-size: 0.8rem; color: var(--muted);" id="bot-addr">Address: None</div>
        <div class="btn-group">
          <button class="btn-success" onclick="toggleSwitch('bottom', 'on')">Turn ON</button>
          <button class="btn-danger" onclick="toggleSwitch('bottom', 'off')">Turn OFF</button>
          <button class="btn-primary" onclick="startPair('bottom')">Pair Bottom</button>
          <button class="btn-secondary" onclick="clearSwitch('bottom')">Unpair</button>
        </div>
      </div>
    </div>
  </div>

  <!-- Hardware Momentary Buttons Guide -->
  <div class="card">
    <h2>Hardware Commissioning Buttons</h2>
    <p style="font-size: 0.85rem; color: var(--muted); margin-bottom: 8px;">
      You can also use physical momentary push buttons wired directly to the ESP32-S3 pins (wired to GND, internal pull-up enabled):
    </p>
    <div style="display: flex; flex-direction: column; gap: 6px; font-size: 0.85rem;">
      <div>🔘 <code>GPIO0 (On-board BOOT button)</code>: Short press = <strong>Pair Top</strong> | Long press (&gt;1.5s) = <strong>Pair Bottom</strong></div>
      <div>🔘 <code>GPIO4 (External Button 1)</code>: Short press = <strong>Pair Top</strong> | Long press (&gt;1.5s) = <strong>Toggle Top ON/OFF</strong></div>
      <div>🔘 <code>GPIO5 (External Button 2)</code>: Short press = <strong>Pair Bottom</strong> | Long press (&gt;1.5s) = <strong>Toggle Bottom ON/OFF</strong></div>
    </div>
  </div>

  <!-- Home Wi-Fi Setup for Home Assistant -->
  <div class="card">
    <h2>Home Wi-Fi Connection (for Home Assistant)</h2>
    <p style="font-size: 0.85rem; color: var(--muted); margin-bottom: 12px;">
      Connect CylinderIQ to your home router so Home Assistant can log and graph data. (SoftAP <code>CylinderIQ</code> stays active!)
    </p>
    <div style="max-width: 400px;">
      <input type="text" id="wifi-ssid" placeholder="Home Wi-Fi SSID">
      <input type="password" id="wifi-pass" placeholder="Wi-Fi Password">
      <button class="btn-primary" onclick="connectHomeWifi()">Connect to Home Wi-Fi</button>
    </div>
  </div>

  <!-- Home Assistant Drop-in YAML -->
  <div class="card">
    <h2>Home Assistant Integration</h2>
    <p style="font-size: 0.85rem; color: var(--muted); margin-bottom: 8px;">
      To add to Home Assistant, place this package in <code>/config/packages/cylinderiq.yaml</code>:
    </p>
    <pre>sensor:
  - platform: rest
    name: "CylinderIQ"
    resource: "http://192.168.4.1/sensors" # or cylinderiq.local
    scan_interval: 5
    json_attributes:
      - hot_outlet
      - cylinder_inlet
      - mains_supply
      - usable_hot_litres
      - hot_water_pct
      - showers_remaining
      - baths_remaining
      - recovery_min
      - cost_pence
    value_template: "{{ value_json.hot_water_pct }}"
    unit_of_measurement: "%"
</pre>
  </div>
</div>

<script>
async function updateTelemetry() {
  try {
    const res = await fetch('/sensors');
    if (res.ok) {
      const data = await res.json();
      document.getElementById('val-hot-outlet').innerText = (data.hot_outlet || 0).toFixed(1);
      document.getElementById('val-cyl-inlet').innerText = (data.cylinder_inlet || 0).toFixed(1);
      document.getElementById('val-mains').innerText = (data.mains_supply || 0).toFixed(1);
      document.getElementById('val-litres').innerText = (data.usable_hot_litres || 0).toFixed(0);
      
      const pct = Math.min(100, Math.max(0, data.hot_water_pct || 0));
      document.getElementById('val-pct').innerText = pct.toFixed(0);
      document.getElementById('litres-bar').style.width = pct + '%';
      
      document.getElementById('val-showers').innerText = (data.showers_remaining || 0).toFixed(1);
      document.getElementById('val-baths').innerText = (data.baths_remaining || 0).toFixed(1);
      document.getElementById('val-recovery').innerText = (data.recovery_min || 0).toFixed(0);
    }
  } catch(e) {}
}

async function updateSwitches() {
  try {
    const res = await fetch('/switches');
    if (res.ok) {
      const data = await res.json();
      // Top
      const topBadge = document.getElementById('top-paired-badge');
      const topState = document.getElementById('top-state-badge');
      const topAddr = document.getElementById('top-addr');
      if (data.top && data.top.paired) {
        topBadge.innerText = 'Paired';
        topBadge.className = 'status-tag status-paired';
        topAddr.innerText = 'Address: ' + (data.top.addr || 'Paired');
      } else {
        topBadge.innerText = 'Unpaired';
        topBadge.className = 'status-tag status-unpaired';
        topAddr.innerText = 'Address: None';
      }
      if (data.top && data.top.state === 'on') {
        topState.innerText = 'ON';
        topState.className = 'status-tag status-on';
      } else {
        topState.innerText = 'OFF';
        topState.className = 'status-tag status-off';
      }

      if (data.coordinator_online !== undefined) {
        const coordEl = document.getElementById('coord-status');
        if (coordEl) {
          coordEl.innerText = data.coordinator_online ? 'Ready' : 'Offline (Check UART wires)';
          coordEl.style.color = data.coordinator_online ? 'var(--success)' : 'var(--danger)';
        }
      }
      if (data.channel) {
        const chEl = document.getElementById('coord-channel');
        if (chEl) chEl.innerText = data.channel;
      }
      if (data.pan_id) {
        const panEl = document.getElementById('coord-pan');
        if (panEl) panEl.innerText = data.pan_id;
      }

      // Bottom
      const botBadge = document.getElementById('bot-paired-badge');
      const botState = document.getElementById('bot-state-badge');
      const botAddr = document.getElementById('bot-addr');
      if (data.bottom && data.bottom.paired) {
        botBadge.innerText = 'Paired';
        botBadge.className = 'status-tag status-paired';
        botAddr.innerText = 'Address: ' + (data.bottom.addr || 'Paired');
      } else {
        botBadge.innerText = 'Unpaired';
        botBadge.className = 'status-tag status-unpaired';
        botAddr.innerText = 'Address: None';
      }
      if (data.bottom && data.bottom.state === 'on') {
        botState.innerText = 'ON';
        botState.className = 'status-tag status-on';
      } else {
        botState.innerText = 'OFF';
        botState.className = 'status-tag status-off';
      }

      // Success feedback if targeted switch is confirmed paired
      const slotEl = document.getElementById('pair-slot-label');
      const activeSlot = slotEl ? (slotEl.innerText || '').toLowerCase() : '';
      if (activeSlot === 'top' && data.top && data.top.paired && pairInterval) {
        clearInterval(pairInterval);
        pairInterval = null;
        const banner = document.getElementById('pair-banner');
        banner.style.background = '#064e3b';
        banner.style.borderColor = '#10b981';
        banner.style.color = '#ecfdf5';
        banner.innerHTML = '🎉 <strong>Pairing Success!</strong> TOP switch paired cleanly! Address: <code>' + (data.top.addr || 'Paired') + '</code>';
        setTimeout(() => { banner.classList.remove('active'); banner.removeAttribute('style'); }, 8000);
      } else if (activeSlot === 'bottom' && data.bottom && data.bottom.paired && pairInterval) {
        clearInterval(pairInterval);
        pairInterval = null;
        const banner = document.getElementById('pair-banner');
        banner.style.background = '#064e3b';
        banner.style.borderColor = '#10b981';
        banner.style.color = '#ecfdf5';
        banner.innerHTML = '🎉 <strong>Pairing Success!</strong> BOTTOM switch paired cleanly! Address: <code>' + (data.bottom.addr || 'Paired') + '</code>';
        setTimeout(() => { banner.classList.remove('active'); banner.removeAttribute('style'); }, 8000);
      }
    }
  } catch(e) {}
}

let pairInterval = null;
let localRemaining = 0;

function startLocalCountdown(slot, duration) {
  localRemaining = duration;
  const banner = document.getElementById('pair-banner');
  banner.removeAttribute('style');
  banner.innerHTML = '⏳ <strong>Zigbee Pairing Active:</strong> Open network for slot <span id="pair-slot-label" style="text-transform: uppercase;">' + slot.toUpperCase() + '</span>. Put your MOES switch in pairing mode (hold button 5s until LED flashes). <strong id="pair-countdown">' + localRemaining + 's</strong> remaining.';
  banner.classList.add('active');

  if (pairInterval) clearInterval(pairInterval);
  pairInterval = setInterval(() => {
    localRemaining--;
    const cdEl = document.getElementById('pair-countdown');
    if (localRemaining <= 0) {
      clearInterval(pairInterval);
      pairInterval = null;
      banner.classList.remove('active');
    } else if (cdEl) {
      cdEl.innerText = localRemaining + 's';
    }
  }, 1000);
}

async function updatePairState() {
  try {
    const res = await fetch('/switch/pair');
    if (res.ok) {
      const data = await res.json();
      if (data.open && data.remaining_s > 0) {
        if (!pairInterval) {
          startLocalCountdown(data.slot || 'top', data.remaining_s);
        } else {
          localRemaining = data.remaining_s;
          document.getElementById('pair-countdown').innerText = localRemaining + 's';
        }
      }
    }
  } catch(e) {}
}

async function startPair(slot) {
  try {
    const res = await fetch('/switch/pair', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({slot: slot})
    });
    if (!res.ok) {
      const err = await res.json().catch(() => ({}));
      alert('Coordinator connection issue: ' + (err.error || 'Offline. Please check the 3 UART wires: S3 GPIO17->C6 GPIO5, S3 GPIO18<-C6 GPIO4, and GND->GND.'));
      return;
    }
    startLocalCountdown(slot, 180);
  } catch(e) {
    alert('Pair request failed: ' + e);
  }
}

async function toggleSwitch(slot, state) {
  try {
    await fetch('/switch/' + slot + '/' + state, { method: 'POST' });
    updateSwitches();
  } catch(e) {}
}

async function clearSwitch(slot) {
  if (!confirm('Unpair ' + slot + ' switch?')) return;
  try {
    await fetch('/switch/clear', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({slot: slot})
    });
    updateSwitches();
  } catch(e) {}
}

async function connectHomeWifi() {
  const ssid = document.getElementById('wifi-ssid').value;
  const pass = document.getElementById('wifi-pass').value;
  if (!ssid) { alert('Please enter Wi-Fi SSID'); return; }
  try {
    const res = await fetch('/wifi/connect', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({ssid: ssid, password: pass})
    });
    if (res.ok) alert('Connecting to ' + ssid + '... Hub will remain reachable on CylinderIQ SoftAP!');
  } catch(e) { alert('Failed to save Wi-Fi config'); }
}

setInterval(updateTelemetry, 2000);
setInterval(updateSwitches, 2500);
setInterval(updatePairState, 1500);
updateTelemetry();
updateSwitches();
updatePairState();
</script>
</body>
</html>
)rawliteral";

const char *get_dashboard_html(void)
{
    return s_dashboard_html;
}
