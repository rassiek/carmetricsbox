# CarMetrics backlog (captured 2026-10-31, not started)

## 1. Multiple saved Wi-Fi networks
- **List:** up to about 8 networks (phone hotspot, home, office), each with a priority. Join the best known network that's in range.
- **Radio constraint:** the ESP32 has one radio, so its own `CarMetrics` network follows the channel of whatever network it joins, and scanning briefly disrupts it.
- **Mitigation:** scan only while disconnected, with backoff. Optionally scan only when the engine is off, or when nobody is connected to the CarMetrics network.
- **Effort:** small.

## 2. Board selector / profiles
- **What a profile holds:** valid GPIOs, ADC1 pins, strapping and boot pins, PSRAM, flash size and partitions, onboard LED, default I2C pins, cores.
- **Where it's used:**
  - The web UI only offers valid pins.
  - Validation uses the profile.
  - `tools/cm.sh build --board X` picks the matching build target.
- **Candidate profiles:**
  - ESP32 DevKit (WROOM-32)
  - WROVER: GPIO16/17 are taken by PSRAM, which clashes with the current MAX6675 defaults
  - ESP32-S3: more RAM/PSRAM, different ADC pins, native USB
  - ESP32-C3: single core, few pins
  - CYD "yellow board"
- **Note:** an S3 or C3 is a different firmware binary; a profile can't convert one chip into another.
- **Effort:** small to medium.

## 3. Board metrics
- **What to show:**
  - Uptime and reset-reason history
  - Free heap, minimum free heap, largest free block, PSRAM
  - Per-core CPU load (idle-hook counters)
  - Task stack high-water marks
  - Loop-time max/avg
  - Wi-Fi RSSI
  - Flash usage and writes
  - Number of connected browser clients
- **Where:** the System page, and optionally the cloud upload, as `sys_*` metrics.
- **Effort:** small.

## 4. Multiple boards
**Load:** one board is not close to overloaded today. CPU is mostly idle and about 200 KB of RAM is free.

**What splitting really buys:**
- **Less wiring through the firewall:** a sensor node in the engine bay sends data, not 10 analog wires.
- **Isolation:** a UI crash can't stop the alarms.
- **Scale:** many sensors and relays.

**Roles:**
- **Hub:** web UI, API, configuration, history, cloud upload, weather.
- **Nodes:** sensors, relays, display.
- **Default:** a single board runs the hub with a local node.
- **Mixing:** any combination per board.

**Transport options:**
- **ESP-NOW:**
  - No router needed and latency of a few ms.
  - Encryption is limited to a handful of peers.
  - Must share the Wi-Fi channel, which the joined network dictates.
  - Good for display and convenience nodes.
- **CAN bus** (ESP32 TWAI + SN65HVD230 transceiver, about $2):
  - The vehicle standard: differential, noise immune, runs over a single twisted pair.
  - Best for safety-relevant links (engine-bay sensors, relays).
- **RS-485/Modbus:** an alternative wired option.

**Design rules:**
- Nodes announce what they can do; the hub pushes their configuration.
- One data model of "points": sensor, relay or computed.
- Alarms are evaluated locally on the sensor node, so they work even if the link drops.
- Relays drop to a safe default state if the link is lost (heartbeat).
- The hub updates nodes' firmware over the link.

**Effort:** large; phase it.

## 5. Widget / layout editor + CYD display client
- **One layout model** (JSON) for the dashboard tiles, the OLED pages and remote displays.
  - Widgets: number, bar, gauge, sparkline, status, relay button, mute button.
  - Pages, sizes and order are all configurable.
- **CYD display client:** ESP32-2432S028R ("Cheap Yellow Display").
  - Hardware: 2.8" 320×240 ILI9341 or ST7789 screen (depends on variant), XPT2046 touch, light sensor (LDR), RGB LED, SD slot.
  - Firmware: separate, using LVGL.
  - Connection: joins the hub over Wi-Fi with live updates via WebSocket/SSE and REST for actions, or uses ESP-NOW.
  - Input: mute, relay toggles, page change, brightness from the LDR.
- **Effort:** medium.

## Extra ideas
- **WebSocket:** two-way, which displays and nodes need; SSE is one-way.
- **Computed sensors:**
  - Boost = absolute manifold pressure (MAP) − BMP pressure
  - Rate-of-change alarms (e.g. coolant rising faster than X °C/min)
  - Time-in-warning counters
  - Oil pressure vs RPM curve
- **Event "black box":** on an alarm, save 30 s before and after to flash, browsable in the UI.
- **GPS (NEO-6M/M8N over UART):** speed, location, altitude cross-check, time source, trip logs, geotagged alarms.
- **Safe-mode boot:** after repeated crashes, start with the previous good config and no outputs.
- **RPM** from the alternator W terminal.
- **Ignition sense + deep sleep.**
- **Turbo cool-down timer.**
- **Phone-installable app (PWA)** and night mode.
- **Cloud backup** of config and config versioning.
- **Remote relay control over the internet:** only with strong auth; probably avoid.

## Suggested phasing

| Phase | Items | Why |
|---|---|---|
| A | 1, 3, 2 (basic profiles) | Small, immediately useful |
| B | 5: layout model + CYD display client | Proves the "remote node over API" approach |
| C | 4: multi-board on the same node protocol; CAN for an engine-bay sensor node | Largest; builds on B |
