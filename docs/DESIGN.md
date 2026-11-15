# CarMetrics 5.4 — design

Status: **draft for review** (before the 5.3.0 code changes). Replaces the scattered
phase plans as the one description of how CarMetrics is built and how it grows.

CarMetrics watches a machine — a car by default, but equally a boat, a tractor, a
generator or a pump house — with a few ESP32 boards: it reads sensors, shows them on
a phone, an OLED and touch displays, raises alarms, switches outputs and uploads
data. Everything that names the machine is configurable (section 7).

---

## 1. Versions

| Item | Version | Rule |
|---|---|---|
| Firmware (all boards) | **5.3.0** | `MAJOR.MINOR.PATCH`, pre-releases `5.3.0-beta.N` |
| Web API | **v1** (`/api/v1`) | grows only; breaking changes go to `/api/v2` (section 1.2) |
| Board-to-board protocol | **1** | new message types / optional trailing fields only |
| Settings format | 5.3 reads every older format | old settings are converted on first start |

History: V1 was the original Arduino build; the V3, v4 and v5 branches were the
ESP32 development line; 5.1 / 5.2 were two build lines (classic ESP32, S3 8 MB)
that 5.3 merges into one version.

### 1.1 Slow numbering

- **5.3.x** — fixes only.
- **5.4, 5.5 …** — a meaningful feature set ("touch displays", "PWM control"); a few
  times a year at most.
- **6.0** — only if older boards, saved settings or integrations would stop working.
- Betas don't consume numbers: `5.3.0-beta.1`, `-beta.2` … then `5.3.0`.
- Git: work on `v5`; each release is tagged `v5.3.0` and published as a GitHub
  Release with the firmware files attached. `main` is updated at releases.

### 1.2 How API v2 would be added

1. v2 lives next to v1 (`/api/v2/...`); both are served for at least one major
   firmware release.
2. Every v1 response that has a v2 replacement carries
   `Deprecation: true` and `Link: </api/v2/...>; rel="successor-version"`.
3. `GET /api/v1/info` → `"api": [1, 2]` tells clients what the board speaks.
4. `docs/API.md` keeps a changelog per API version; the touch display and the
   bundled web app move first.
5. Within v1 these are **not** breaking (allowed any time): new endpoints, new
   fields in replies, new optional request fields, new enum values that clients
   may ignore. Removing or renaming a field, changing a type or a meaning **is**.

---

## 2. Boards and roles

Since 5.4 each role has its own firmware, built from shared libraries
(`firmware/libraries/CarMetricsLink`: board-to-board protocol and links;
`CarMetricsCore`: settings, sensors, actuators, alerts, health, Wi-Fi, the API core and the web app).

| Role | Firmware | Boards | Does |
|---|---|---|---|
| **Main** | `firmware/CarMetrics` | ESP32-S3 with PSRAM, 8 or 16 MB (N8R8, N16R8) — built-in profiles only | web app, dashboards, 8 h charts, alarms, black box, cloud upload with offline buffer, weather, presets; pairs and manages the other boards and displays, shows all their values (gauge board filter) |
| **Worker** | `firmware/CarMetricsWorker` | ESP32-S3 4 MB+, classic ESP32, ESP32-C3, ESP32-C6, ready-made relay boards (Relay X8, KC868-A4, T-Relay); any other board through a **custom board** definition | its own sensors and outputs, its own alarms and rules, safe state on link loss; its own web page (only its own gauges and actuators) — works with a main board or alone; no file system, no cloud, weather, OLED or other boards |
| **Display** | `firmware/CarMetricsDisplay` | JC3248W535 (S3, 3.5" 320×480), Cheap Yellow Display ESP32-2432S028R (2.8" 320×240) | pages of dials / bars / numbers, relay buttons, mute, alarm banner, limit editor; own layout editor |

The role is decided by the firmware (`setFirmwareRole` first thing in `setup()`), not a setting.
Each image carries a role tag (`RoleScan`), so a board refuses the other role's firmware over Wi-Fi
and the main board never sends its own firmware to another board.

### 2.1 Build variants (one version, several files)

| Variant | Files | Boards |
|---|---|---|
| `main-s3` | `carmetrics-main-s3-…` | main board (8 or 16 MB, PSRAM) |
| `worker-s3` | `carmetrics-worker-s3-…` | S3 workers (4 MB+); also keeps chart history (`CM_WORKER_PLUS`) |
| `worker-esp32`, `worker-c3`, `worker-c6` | `carmetrics-worker-<chip>-…` | generic workers |
| `relay-x8`, `kc868-a4`, `t-relay` | `carmetrics-<board>-…` | worker builds that start with the board's profile and relays |
| `display-jc3248w535`, `display-cyd` | `carmetrics-display-…` | touch displays |

Each comes as `-full-flash.bin` (USB, address 0x0, keeps settings) and `-firmware.bin`
(Wi-Fi; the web app is compiled into it). Worker layout (`firmware/CarMetricsWorker/partitions.csv`):
two 1.875 MB firmware slots, settings and core dump at the same place as 5.3, no file system.

### 2.2 Board profiles

A profile describes one physical board. Chosen automatically where possible (chip,
flash, PSRAM are detected), otherwise on System → Hardware.

```
id, name, chip, flash MB, PSRAM MB
pins: for every GPIO — usable, input only, analog (ADC1 / ADC2), touch,
      strapping (boot), used by flash / PSRAM / USB, built-in LED, notes
fixed parts: built-in RGB (pin, type), relays (pins, active level),
             opto inputs, buttons, display, I2C / 1-Wire connectors
defaults: role, bus pins, which inputs / outputs exist
```

Profiles in 5.3: ESP32-S3 DevKitC N8R8 / N16R8 (built-in RGB on 48, or 38 on
v1.1), ESP32 DevKit 38-pin, ESP32 Relay X8, LilyGO T-Relay, KinCony KC868-A4,
JC3248W535, CYD, plus "CarMetrics carrier" (breakout + PC817 + BSS138 at fixed pins,
`docs/WIRING.md`).

Every pin chosen in any setting is checked against the profile when saving:
outputs on input-only pins, ADC2 while Wi-Fi is on, strapping pins with pull-ups,
pins used twice → refused with a plain message, or warned where it can still work.

#### ESP32 Relay X8 (LC Technology, AliExpress)

| Item | Value |
|---|---|
| Relays 1–8 | GPIO 32, 33, 25, 26, 27, 14, 12, 13 (polarity to be confirmed on the bench; profile default active high) |
| LED | GPIO 23 |
| Power | 7–30 V DC or 5 V DC (separate terminals); ~100 mA idle, ~0.5 A all relays on; relays 10 A |
| Flashing | 3.3 V USB-to-serial adapter on the TX / RX / GND header, GPIO 0 button held during reset; afterwards updates come over ESP-NOW from the main board |
| Caution | relay 7 is on GPIO 12, a strapping pin: the profile keeps it off during boot |

Sources: ESPHome device page "ESP32E Relay X8", Blakadder template "ESP32_Relay_X8".

---

## 3. Inside the firmware

### 3.1 Tasks

| Task | Core | Job |
|---|---|---|
| loop | 1 | read sensors (non-blocking state machines), evaluate alarms and actuator rules, board link, OLED / RGB, apply settings |
| web (async TCP) | 0 | HTTP API, WebSocket, static web app |
| net | 0 | Wi-Fi joining, cloud upload, weather, storage statistics |
| board link | 1 (from loop) | ESP-NOW / CAN frames queued by the radio callback, handled in loop |

Shared state is behind one lock; web handlers copy what they need and never wait
for hardware. Settings changes are validated in the handler, then applied by loop.

### 3.2 Extensible parts ("drivers")

Every kind of thing is a self-contained driver registered in a table. Nothing else
in the code switches on a type name.

| Kind | Interface (simplified) | Examples in 5.3 |
|---|---|---|
| **Sensor type** | `schema()` fields + defaults, `validate(cfg)`, `begin(cfg)`, `read(now) -> value/status`, `discover()` optional | DS18B20, ADS1115 (+ presets: 0.5–4.5 V sender, 12 V via B25, custom divider), ESP32 analog, MAX6675, MCP9600, BMP/BME280, INA226, MPU-6050, GPS field, RPM (pulse), switch, computed, remote |
| **Actuator type** | `schema()`, `validate`, `begin`, `apply(state)`, `safeState()` | relay (on/off/flash), PWM (duty, frequency), H-bridge (direction + speed), servo (angle) |
| **Control rule** | `schema()`, `evaluate(inputs) -> request` | manual, threshold with hysteresis, curve (sensor → duty), condition (e.g. "engine running"), schedule |
| **Metrics output** | `schema()`, `begin`, `send(batch) -> ok/retry` | Zabbix, InfluxDB line protocol (MQTT / Home Assistant later) |
| **Status light** | `show(level, linkOk)` | built-in addressable RGB, external 3-pin RGB (both at once, own brightness) |
| **Board profile** | data table (section 2.2) | as listed |

`schema()` returns the settings fields (name, type, unit, range, choices, help
text, which board profiles allow it). The web app builds its forms from
`GET /api/v1/schema/...`, so a new driver needs **no web app change**.
`docs/EXTENDING.md` walks through adding each kind with a full example and its tests.

### 3.3 Actuators

- One list for relays, PWM outputs, motor drivers and servos (the old Relays page
  becomes **Actuators**).
- Each has: name, type, pin(s), the rule that drives it, limits (min / max duty,
  angle range), kick-start (full power for N ms when starting), ramp rate,
  **safe state** when the link to the main board is lost, manual override with
  a timeout.
- Rules are evaluated every loop pass; the strongest of "manual override",
  "safe state", "rule" wins in that order.
- On worker boards the rules run locally (they keep working without the main
  board); the main board edits them and mirrors their state.

### 3.4 Discovery

- I2C scan: known addresses are matched to drivers (ADS1115 0x48–0x4B,
  BMP/BME280 0x76/0x77, MPU-6050 0x68/0x69, INA226 0x40–0x4F, OLED 0x3C/0x3D,
  MCP9600 0x60–0x67) and offered as "Add".
- 1-Wire scan: DS18B20 addresses with live temperature.
- Analog: live volts on every ADS1115 channel and analog pin to tell wires apart.
- On a worker the scan runs there and the result is shown on the main board.

### 3.5 Board-to-board link

Unchanged protocol 1 (frames ≤ 250 bytes, CRC, sequence numbers; ESP-NOW default,
CAN optional; settings pushed as JSON with revision = CRC-32; values every second;
commands acted on once; firmware transfer). See `docs/v5-phase3-plan.md` until it
moves into this file. 5.3 adds only optional fields and keeps 5.1 / 5.2 boards working.

### 3.6 Settings storage

NVS partition `cfg`, one JSON document per section, slots *current / previous /
good* (safe mode starts from *good*). Unknown fields are kept, so a downgrade
doesn't lose settings written by a newer release.

---

## 4. Web app

Five pages; every name below is a default (section 7).

| Page | Contents |
|---|---|
| **Dashboard** | tiles: dial, bar, number, trend line, tilt (roll / pitch), relay button. Views: Gauges / Charts / Events. Weather card. ✎ edits the layout in place (pages, tiles, sizes, names) |
| **Sensors** | list (board selector for other boards), editor built from the driver schema, **Discover** |
| **Actuators** | list, editor, rule editor (threshold / curve / condition), manual controls |
| **Boards** | other boards and displays: pair, status, controls, firmware (main board only) |
| **System** | **Status** (health, crash, firmware, backup) · **Network** (own Wi-Fi, scan and saved networks in one list, join log) · **Hardware** (board profile, pin map + pin tester, buses, OLED + its layout, RGB, buzzer, ignition, GPS, engine-running detection, turbo timer) · **Data** (cloud upload, weather, altitude) |

The pages follow `GET /info` `capabilities`: on a worker there is no Boards tab, no charts / events
unless it keeps history, no weather, cloud, presets, OLED or altitude; System → Status shows its main
board (paired, link, forget) and System → Hardware its custom board definition. A gauge without a
reading shows "--" and one short reason on the tile.

Shared building blocks (cards, forms from schema, tables, the dial drawing) live in
one module; the dial is the same design as on the touch display. Every page works at
phone width; the Dashboard fits one screen in both orientations.

---

## 5. API

`docs/API.md` (v1). The web app, the touch displays and integrations all use it.
Integrations authenticate with **API keys** (System → Status → API keys) instead of
the admin password; keys have a scope (read / control / admin) and can be withdrawn.

---

## 6. Safety rules (kept from v5)

- Outputs go to their safe state on link loss and stay there until the link is back
  and a rule or a person changes them.
- Safe mode after three crash restarts: outputs off, internet features paused,
  settings from the last good set.
- New firmware is confirmed after 60 s of running, otherwise the previous one boots.
- Nothing from the 5 V side reaches an ESP32 pin without a level shifter or opto-coupler
  (`docs/WIRING.md`); pin checks warn about it where the profile knows the hardware.

---

## 7. Names, labels and presets (nothing hard-coded)

Everything a person reads is a setting with a default:

| What | Setting | Default |
|---|---|---|
| Product / app name (header, browser tab, OLED start screen, display title) | `ui.appName` | CarMetrics |
| Machine name ("my Defender", "MV Aurora", "Borehole 2") | `ui.machineName` | (empty) |
| Page names | `ui.pages.dashboard/sensors/actuators/boards/system` | Dashboard, Sensors, Actuators, Boards, System |
| Dashboard pages and tiles | in the layout | Main, … |
| Sensor, actuator, board names and short labels (3–5 characters) | on each item | from the preset |
| "Running" condition wording | `ui.runningWord` | "engine running" (a boat: "engine running", a pump house: "pump running") |
| Units shown | per sensor + global preference (°C/°F, bar/psi/kPa, km/h / mph / knots, L / gal) | metric |
| Network names | access point name, host name (`http://<name>.local`) | CarMetrics, carmetrics |

**Presets** fill in a starting set of sensors, actuators, names, units and limits
on a fresh board (System → Hardware → *Start from preset*, or the first-start page):

| Preset | Starting point |
|---|---|
| **Car / 4×4** (default) | coolant, intake, gearbox, transfer case, EGT, boost, oil pressure, battery, cabin, altitude, tilt |
| **Boat** | engine coolant, oil pressure, exhaust, house and start batteries, bilge level / pump, fuel, tilt (heel / trim) |
| **Farm / generator** | engine temperatures and oil, battery, fuel, running hours, pump outputs, tank level |
| **Blank** | nothing; build your own |

Presets are plain JSON files in the web app (`presets/*.json`), so new ones can be
added or shared without firmware changes. The touch displays and OLED take every
name from the main board.

---

## 8. Documents

| File | Contents |
|---|---|
| `DESIGN.md` | this file |
| `API.md` | API v1 reference, live updates, keys, errors, versioning |
| `EXTENDING.md` | adding sensor / actuator / rule / output types and board profiles |
| `WIRING.md` | power, dividers, B25, PC817 (alternator, switches), ADS1115, level shifter, PWM / motor modules, engine-bay and cabin boards, Ethernet-cable link, pinouts per board |
| `PARTS.md` | parts list per board with AliExpress search words |
| `BUILDING.md` | building firmware from GitHub |
| `CONTEXT.md` | hand-over notes for a new Claude session |
