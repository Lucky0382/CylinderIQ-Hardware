# CylinderIQ Ecosystem — Project Summary & GreenSyn Application Master Document

**Document Purpose:** Master project reference and GreenSyn Accelerator application responses for CylinderIQ (CIQ), DisplayIQ (DIQ), ImmersionIQ (IIQ), TapIQ (TIQ), Safety Assist (SA), Grid Assist (GA), and IQOS (IQ).

---

## 1. Product Ecosystem Overview

### Hardware & Physical Topology
1. **CylinderIQ (CIQ):** Remote hot water cylinder sensor hub (3x DS18B20 temp sensors for tank calculations, 1x DS18B20 discharge pipe sensor, 1x leak detection rope).
2. **DisplayIQ (DIQ):** Remote 3.5" touchscreen display (user check-in, usable liters, system status, immersion boost).
3. **ImmersionIQ (IIQ):** Mains-supplied wall-mounted smart immersion controller (dual 110V-240V supplies for peak/off-peak twin immersion control via relays, 5" touchscreen interface, boost, timer, VPP settings & stored usage data).
4. **TapIQ (TIQ):** Remote inline smart valve for shower and tap outlets (15mm & 22mm BSP fittings, wireless Zigbee connectivity, flow sensor for metrics tracking).

### Software & Cloud Topology
1. **Safety Assist (SA):** SaaS cloud-based B2B asset monitoring software (real-time leak detection & notification, remote asset management & monitoring, Legionella monitoring).
2. **Grid Assist (GA):** Cloud-based grid-to-asset management software (VPP interface for domestic hot water cylinders).
3. **IQOS (IQ):** Native iOS, Android, and Cloud applications (user check-in, user presets & comfort settings, live data & usage tracking, timer/programmer function, boost settings, full system control).

---

## 2. GreenSyn Accelerator Application Answers

### SECTION A. THE BASICS
* **Q1. Company Name:** CylinderIQ
* **Q2. Company URL:** `https://cylinderiq.com`
* **Q2. One-line Description:** `VPP platform turning water heaters into thermal grid batteries.` *(49 chars)*
* **Q3. Incorporation & Team Location:** Operating pre-incorporation as a founder-led venture based in the UK/EU, ready to formally incorporate upon selection.
* **Q4. Product Demo Link:** `[YouTube / Loom Video Link]` (Showing CIQ sensor hub, DIQ touchscreen display UI, 3-point pipe temp sensors, tundish discharge probe, leak rope, and smart switch control).

### SECTION B. DOMAIN DECLARATION
* **Q5. Primary Domain:** **`Curtailment monetization`** *(Tier 1 - Highest Priority)*
* **Q6. Secondary Domain:** **`VPP / DERMS platforms`** *(Tier 3)*
* **Q7. Domain Problem (150 words):**  
  Isolated grids and high-PV markets face severe renewable curtailment during peak insolation, wasting zero-carbon energy while grid operators struggle with frequency instability. Concurrently, millions of residential hot water cylinders draw power during peak evening grid hours at high cost, while landlords and social housing providers suffer from zero visibility over thermal efficiency, Legionella safety compliance, and catastrophic water leaks. Current Demand Response solutions rely on expensive single-purpose batteries or intrusive appliances. Incumbent water heating controls are dumb timers or closed systems that cannot aggregate flexible loads dynamically. This leaves gigawatt-hours of domestic thermal storage capacity untapped, preventing utilities from soaking up curtailed solar energy while housing providers face mounting maintenance and flood damage claims.
* **Q8. Technical Solution (250 words):**  
  CylinderIQ delivers **IQOS**, an edge-to-cloud VPP platform that aggregates domestic hot water cylinders into a distributed, flexible thermal battery array for solar curtailment absorption and grid balancing.  
  Our ecosystem comprises 7 integrated components: **CIQ (CylinderIQ)** sensor hub, **DIQ (DisplayIQ)** touchscreen UI, **IIQ (ImmersionIQ)** dual 20A smart wall controller, **TIQ (TapIQ)** inline smart valve, **SA (Safety Assist)** B2B SaaS monitoring, **GA (Grid Assist)** VPP engine, and **IQOS (IQ)** native applications.  
  To de-risk pilot deployment and eliminate certification delays, our MVP utilizes off-the-shelf pre-certified components (Espressif ESP32 modules and pre-certified 20A smart immersion switches).  
  CIQ monitors a 3-point plumbing temperature array (Mains Cold Inlet, Post-NRV Tank Inlet, Hot Water Outlet), calculating usable hot water volume and stored kWh in real time on the **DIQ** UI. **Safety Assist** pairs physical leak rope with a dedicated discharge pipe temperature sensor ($\Delta T$) to alert operators to weeping pressure relief valves, NRV backflow failures, and pinhole leaks before damage occurs, alongside automated Legionella safety monitoring.
* **Q9. Domain Relevance (150 words):**  
  Curtailment monetization requires rapid, scalable, high-capacity load-sinking. Hot water cylinders are the largest unmanaged electrical load in residential buildings, representing 3–5 kWh of flexible thermal storage capacity per home. CylinderIQ directly solves curtailment by turning these static assets into dynamically dispatchable thermal batteries that absorb excess solar generation in real time via **Grid Assist (GA)**. By combining thermal telemetry, automated PRV leak safety (**Safety Assist**), **DisplayIQ (DIQ)** visibility, and fleet-wide VPP orchestration, CylinderIQ provides the exact dispatch mechanism utilities need to convert clipped renewable energy into revenue.
* **Q10. Proposed 3-Month GEG Pilot (150 words):**  
  We propose piloting **IQOS & Grid Assist across a 100-to-500 unit fleet of thermal assets** within Green Energy Group's Cyprus operating footprint as a 15-minute electrical retrofit.  
  *Execution Strategy:* Deploy CIQ MVP Sensor Hubs paired with DIQ interfaces to control pre-certified 20A smart switches. Installations will be executed directly by our in-house field team (2x Part-P certified electricians, 1x G3 unvented plumber). This pilot validates our VPP dispatch software while co-testing our **ImmersionIQ (IIQ)** and **TapIQ (TIQ)** hardware roadmap.  
  *What We Will Measure:* 1) Total kWh of curtailed solar power absorbed as stored heat; 2) Response latency to grid balancing events; 3) Tenant bill savings via dynamic tariff optimization; 4) Automated Legionella compliance, NRV backflow alerts, and leak detection reliability via Safety Assist.

### SECTION C. FOUNDERS & TEAM
* **Q11. Founders & Team List:**  
  * `[Founder Name]`, Lead Systems Engineer & Founder, 100% Equity, LinkedIn: `[URL]`, Full-Time.  
  * In-house Field Operations Team: 2x Part-P Certified Electricians, 1x G3 Unvented Systems Plumber, 1x Specialist Installer.
* **Q12. Founder Background (100 words):** Founder-led venture with extensive hands-on experience in IoT firmware engineering, building automation, and energy storage systems, backed by a certified field installation team.
* **Q13. Technical Builder:** All firmware, coordinator architecture, API design, algorithm development, and hardware prototyping are built in-house by the founder. Field installation protocols are designed by our certified Part-P electricians and G3 plumber.
* **Q14. Team Track Record (150 words):** Built, bench-tested, and validated the complete CIQ IoT hardware, DIQ display UI, and edge algorithms. Key achievements include: 1) Developing a 3-point pipe temperature telemetry array (Mains Inlet, Post-NRV Tank Inlet, Hot Outlet) and thermal algorithm that calculates usable volume and stored kWh; 2) Designing a tundish discharge $\Delta T$ sensing method for pressure relief valve weeping; 3) Building DIQ touchscreen display UI; 4) Implementing local coordinator binding to 20A immersion switches; 5) Structuring the development roadmap for IIQ and TIQ.

### SECTION D. PROGRESS & TRACTION
* **Q15. PoC / MVP State (200 words):**  
  *What Works Today:* Live hardware prototype running on ESP32 microcontrollers. Operational 3-point pipe temp telemetry (Mains Inlet, Post-NRV Tank Inlet, Hot Outlet), usable volume/kWh calculations, DIQ display UI, leak rope monitoring, tundish discharge pipe $\Delta T$ temp alerting (Safety Assist), and smart switch binding (Grid Assist).  
  *In Progress / Roadmap:* Development of **ImmersionIQ (IIQ)** (mains-supplied smart dual 20A wall controller with 5" touchscreen) and **TapIQ (TIQ)** (15mm & 22mm inline smart valve with flow sensor) for full commercial launch.
* **Q16. Traction & Pilots (150 words):** Currently operating in live alpha test status on physical hardware setups to validate algorithm accuracy, sensor durability, and edge reliability. Fully certified field installation team (2x Part-P electricians, 1x G3 plumber) ready for immediate deployment.
* **Q17. Key Metric (50 words):** **Dispatchable Thermal Energy Capacity Managed (kWh).** Currently ~5 kWh on our primary test rig, targeted to scale across a 500-unit pilot fleet.
* **Q18. Funding & Runway:** Bootstrapped to date ($0 external equity funding raised). Low burn rate funded directly by founder.

### SECTION E. THE IDEA & MARKET
* **Q19. Earned Secret (200 words):**  
  Most energy platforms treat water heaters as simple binary ON/OFF switches. Our earned secret is twofold: 1) *Strategic Pipe Telemetry & Safety Physics:* Incumbents use expensive flow meters to measure water consumption. We proved that monitoring a strategic 3-point pipe array combined with tundish discharge pipe temperature differentials ($\Delta T$) catches pressure relief valve weeping, expansion vessel faults, and NRV backflow failures instantly at a fraction of the cost. 2) *De-risked Pre-Certified Hardware Strategy:* Rather than risking long certification delays, CIQ MVP utilizes off-the-shelf pre-certified components acting as a local coordinator for immediate GEG pilots, while developing **IIQ** and **TIQ** in parallel for enterprise scale.
* **Q20. Competitors & Differentiation (150 words):**  
  *Competitors:* Smart immersion hardware makers (e.g., Mixergy, T-Smart) and home battery platforms. *Substitutes:* Basic timers, smart plugs, and "do nothing."  
  *Why We Win:* 1) 15-Minute Low-CAPEX Retrofit: Installs on existing tanks for a fraction of Mixergy's £1,500+ cost; 2) Local Standalone Switching: CIQ hub binds directly to heavy-duty 20A switches; 3) Integrated Safety Moat: Safety Assist offers Legionella compliance, NRV backflow alerts, PRV discharge detection, and leak protection; 4) DisplayIQ UI: High-impact display for residents; 5) In-House Part-P & G3 Plumbing Team ready for immediate field deployment.
* **Q21. Business Model (150 words):**  
  1) *B2B SaaS (Social Housing / Landlords):* B2B monthly subscription for automated Legionella compliance, NRV backflow alerts, PRV weeping detection, and leak damage prevention (**Safety Assist**). 2) *VPP Capacity & Dispatch Fees (Utilities / IPPs):* Managed capacity fee plus revenue share on solar curtailment arbitrage and grid balancing events (**Grid Assist**). 3) *End-User Incentive:* Zero subscription fee for tenants; dynamic tariff shifting reduces heating bills, with additional VPP profit-sharing credits.
* **Q22. Technical/Regulatory Risks & Mitigation (150 words):**  
  *Risk:* Firmware reliability for real-time grid dispatch and high-voltage safety compliance.  
  *Mitigation Strategy:* 1) Pre-Certified 20A Switching (MVP): High-voltage risk and regulatory delays are eliminated by controlling pre-certified 20A heavy-duty immersion switches built for continuous 3kW resistive loads; 2) Certified Installation SOPs: Field trials are designed and executed by our in-house Part-P electricians and G3 plumber.

### SECTION F. PROGRAM & COMMITMENT
* **Q23. Program Availability:** Yes.
* **Q24. Program Terms (3% Equity SAFE + ROFR):** Yes.
* **Q25. Requirements from GEG Beyond Capital (100 words):** Access to Green Energy Group’s operating renewable asset portfolio in Cyprus to deploy a pilot, access to solar curtailment data, and market regulatory navigation.
* **Q26. Anything Else (100 words):** CylinderIQ was born out of hands-on physical hardware experimentation. Backed by an in-house certified installation team (Part-P & G3 plumber), a 3-point pipe sensor array, **DisplayIQ (DIQ)** display UI, pre-certified 20A switching, and an **ImmersionIQ / TapIQ** roadmap, CylinderIQ offers immediate pilot execution readiness for Green Energy Group.
