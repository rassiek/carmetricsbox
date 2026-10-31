# CarMetrics – project handoff (31 Oct 2026)

This is the complete record of the work so far: the V3 review, the v4 build and the v5 design. It's written so a new Claude session (or a person) can pick up with no other context. The other files in `docs/` hold the detail:

| File | What it is |
|---|---|
| `HANDOFF.md` | This summary: context, what was done, decisions, how to resume |
| `v3-architecture-review.md` | The 23 issues found in V3 before testing |
| `v4-status.md` | v4 commits, decisions, test results, next steps |
| `v5-design-and-backlog.md` + `v5-architecture.svg` | The v5 design and its 23-item phased backlog (not started) |
| `backlog-notes.md` | First backlog notes that led to the v5 design (kept for history) |

---

## 1. Context

- **Owner:** Rassy Kariuki (GitHub `rassiek`). Repo: `github.com/rassiek/carmetricsbox`.
- **Vehicle:** Land Rover 300Tdi, no OBD-II. CarMetrics is a home-built engine monitor in a box in the cab.
- **History:** V1/V2 were Arduino + ESP sketches. V3 (branch `V3`) put everything on one ESP32. v4 (branch `v4`, built in this session) is a rewrite of V3.
- **Hardware today:**
  - Classic ESP32 DevKit (WROOM-32, 4 MB flash).
  - DS18B20 1-Wire temperature sensors over a CAT6 run.
  - EGT thermocouple on a MAX6675; MCP9600 now supported as an alternative with fewer wires.
  - Pressure senders (oil, boost) and battery voltage via dividers to 3.3 V, on ADC1 pins or an ADS1115.
  - BMP180/BMP280 barometric sensor (used for altitude).
  - 1.5" SSD1327 128×128 OLED.
  - Relays and buzzer.
- **Dev setup:** Windows PC, Arduino IDE 1.8.20, ESP32 Arduino core 2.0.17. Repo cloned at `$HOME\carmetricsbox`.
- **Working style:** discuss and clarify the design before building.

---

## 2. What v4 delivered (branch `v4`, 5 commits on top of V3)

| Commit | Summary |
|---|---|
| `fc71c15` | Single-ESP32 firmware rewrite as modules, new web app, OTA updates |
| `3d7c0b8` | MCP9600/MCP9601 I2C thermocouple support; I2C speed setting |
| `06c00f1` | Digital-tile dashboard (fits one screen), grouped charts, sensor-missing handling |
| `06a0004` | Weather tab, cloud upload with offline buffer, altitude calibration |
| `b9a7028` | Reproducible dev environment (Docker + ESP32 emulator) |

### V3 problems fixed

All the P0/P1 items in `v3-architecture-review.md` were addressed:

- **Readings:** keyed by `sensor.id` everywhere, with per-sensor calibration. That fixes the blank gauges and charts, and boost reading about 3.3× high.
- **Non-blocking loop:** 1-Wire conversions no longer block for about 750 ms per pass.
- **Thread safety:** a mutex (`StateLock`) protects config changes; POST handlers are proper JSON routes, and blocking work is moved out of web callbacks.
- **Alerts:**
  - Every threshold is evaluated, with hysteresis and mute/acknowledge.
  - Optional engine-running gate.
  - Optional per-sensor "alarm if missing/faulty". EGT has it off; its offset is now 0 instead of −8.
- **Relays:**
  - Admin login required.
  - Active-low option, safe boot states, pin-conflict validation.
- **Storage:**
  - Settings in their own NVS partition (`cfg`), so OTA and filesystem uploads don't erase them.
  - LittleFS replaces SPIFFS.
- **Analog:**
  - `analogReadMilliVolts` with averaging; values clamped to range.
  - A reading below 0.35 V is treated as a sender fault.
- **Display and platform:**
  - The OLED type is selectable (SSD1327 / SSD1306 / SH1106).
  - Maintained ESP32Async web server.
  - ArduinoJson 7; legacy sketches moved to `legacy/`.

### New features in v4

- **OTA:** firmware and web-app updates from the System page.
- **Sensors:** add/remove from the UI. Types are DS18B20, MAX6675, MCP9600, BMP, direct ADC and ADS1115.
- **Dashboard:** digital tiles that fit one screen in any orientation.
- **Charts:** grouped (1-Wire temperatures, pressures, EGT, others by unit) with auto-zoom.
- **Weather tab:** Open-Meteo, 41 Kenyan towns, works only when online. The reference town's sea-level pressure calibrates the BMP altimeter. A known-altitude calibration is also available.
- **Wi-Fi:** client and access point at the same time; NTP time when online.
- **Cloud upload:**
  - Targets: Zabbix trapper (packed `carmetrics.json` item, or one item per metric, optional TLS-PSK) or InfluxDB line protocol (also works with VictoriaMetrics, Grafana).
  - Downsampling: avg/min/max at 250 ms–5 min.
  - Offline buffer: a RAM queue, then full 4 KB chunks to a capped LittleFS ring. That holds about 34 h at 1-minute resolution. Flash wear was assessed and is acceptable because only full chunks are written while offline.
- **Dev environment:**
  - `devenv/` Dockerfile, VS Code devcontainer, `tools/cm.sh` (build | selftest | emu | webtest | all).
  - QEMU ESP32 emulator self-test (33 checks).
  - Playwright UI tests against a mock ESP32.

### Verified

| Test | Result |
|---|---|
| Emulator self-test | 33/33 pass |
| Web UI test | 18/18 pass |
| Docker image | Builds from a clean Ubuntu 24.04 |
| Firmware | Compiles to 1,243,105 bytes |
| Arduino IDE | You flashed v4 from Arduino IDE 1.8.20 |

**Not yet tested on real hardware:** sensors end to end, Wi-Fi client mode, weather fetch, cloud uploads, and flash write timing.

### Key settings and facts

- **Defaults:**
  - Access point `CarMetrics`, password `carmetrics`.
  - Admin `admin` / `carmetrics`. Change both.
- **Partitions** (`firmware/CarMetrics/partitions.csv`): nvs 0x9000 (20K), otadata 0xE000, app0 0x10000 (1.6M), app1 0x1B0000 (1.6M), LittleFS 0x350000 (576K), cfg 0x3E0000 (64K), coredump 0x3F0000.
- **Libraries** (exact versions in `devenv/versions.env`): ESPAsyncWebServer 3.12.1 and AsyncTCP 4.0.0 (ESP32Async), ArduinoJson 7.2.1, U8g2, DallasTemperature, OneWire, Adafruit MAX6675/MCP9600/BMP280/BMP085/ADS1X15/BusIO/Unified Sensor.
- **Rejected options:**
  - Prometheus Pushgateway, because it drops timestamps and buffered history would be lost.
  - gzip compression on the board, because there isn't enough RAM.

### Arduino IDE gotchas already solved

- **`Adafruit_ADS1X15.h` missing:** install the libraries listed above.
- **`mbedtls_md5_starts_ret` errors:** caused by old `ESPAsyncWebServer` copies (me-no-dev / lacamera forks) in `Documents\Arduino\libraries`. Move them out and keep only the ESP32Async 3.12.1 and AsyncTCP 4.0.0 versions.
- **Web app not installed:** upload the LittleFS image (`littlefs.bin`), or upload the web app from the System page.

---

## 3. v5 – phases 0 and 1 built (31 Oct 2026)

Commit `c7c6287` on the `v4` branch builds backlog items 1–14. See `v5-status.md` for the per-item status, test results, and a bench test checklist. Decisions: support core 2.0.17 **and** 3.x; test on the current ESP32 now and an ESP32-S3 later; do **multi-board (phase 3) before the touch display (phase 2)**. Phases 2 and 3 are not started.

### Original v5 design

See `v5-design-and-backlog.md`. The headline decisions:

- **Boards:**
  - One board stays the default and complete setup.
  - Dual-core chips only: classic ESP32 and ESP32-S3. The ESP32-S3 N8R8 is recommended. C2/C3/C5/C6/H2/S2/P4 are excluded.
- **Firmware platform:** move to ESP32 Arduino core 3.x (ESP-IDF 5).
- **Displays:**
  - The web UI is primary.
  - The OLED is optional.
  - The touch display is optional; the Guition JC3248W535 (ESP32-S3, 3.5", capacitive) is the recommended buy.
- **Board-to-board links:** support both.
  - CAN (built-in controller + SN65HVD230 transceiver) for wired, safety-relevant links.
  - ESP-NOW for wireless and convenience links.
- **Optional inputs** never affect critical functions when absent: GPS, RPM from the alternator W terminal, ignition sense, INA226, IMU, fuel level, warning switches, BME280.
- **New features:**
  - Saved Wi-Fi networks, board profiles, board metrics.
  - Point data model (`board.point`), WebSocket and `/api/v2`.
  - Layout editor for web tiles, OLED and touch display.
  - Turbo cool-down timer (indicator only).
  - Alarm black box, computed sensors, deep sleep, safe-mode boot with firmware rollback.
- **Backlog:** 23 items in 4 phases.
  - Phase 0: foundation.
  - Phase 1: single-board features.
  - Phase 2: touch display.
  - Phase 3: multi-board.
- **Estimate:** about 6–9 Claude sessions, roughly one phase per session. Real-hardware testing sets the calendar pace.
- **Open questions:** listed at the end of the design doc (main board choice, touch display model, which second board first, CAT6 pairs for CAN, GPS antenna, turbo-timer EGT threshold, phase order).

---

## 4. Outstanding actions

1. **Push v4 to GitHub.** The Claude session couldn't push (the repo wasn't authorised for it). From PowerShell, with `carmetrics-v4.bundle` in Downloads:
   ```powershell
   cd $HOME\carmetricsbox
   git fetch "$HOME\Downloads\carmetrics-v4.bundle" v4:v4 --force
   git checkout v4
   git push -u origin v4
   ```
   The bundle now also contains this `docs/` folder (commit 6).
2. **Test v4 on the real board**, using the "not yet tested" list above.
3. **Bench-test v5** with the checklist in `v5-status.md`, then start phase 3 (multi-board).

---

## 5. How to resume in a new Claude account

1. **Push v4** (step 1 above) so the code and these docs are on GitHub. If that isn't possible, keep `carmetrics-handoff.zip`.
2. **Start a new chat or project** and attach `docs/HANDOFF.md` and `docs/v5-design-and-backlog.md` (or the whole zip).
3. **Use a starting prompt like:**
   > This is my CarMetrics ESP32 project (repo github.com/rassiek/carmetricsbox, branch v4). Read HANDOFF.md, v5-design-and-backlog.md and v5-status.md. Phases 0 and 1 are built; my bench-test results are: … Let's plan phase 3 (multi-board). Discuss before building.
4. **For builds and tests**, the new session needs the source. Either connect GitHub so it can clone the repo, or attach `carmetrics-v4-source.zip`. The dev toolchain is reproduced by `devenv/setup-toolchain.sh` or the Dockerfile.
