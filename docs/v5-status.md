# CarMetrics v5 – build status (31 Oct 2026)

Phases 0 and 1 of the [v5 design](v5-design-and-backlog.md) are built on the `v5` branch (branched from `v4`).
Phases 2 (touch display) and 3 (multi-board) are not started.

## Decisions taken before building

| Question | Answer |
|---|---|
| Arduino core | Support **both 2.0.17 and 3.x** (tested with 3.3.12) |
| Test boards | Current classic ESP32 now, ESP32-S3 when it arrives |
| Scope | Phase 0, then straight on into phase 1 |
| Order after phase 1 | **Multi-board (phase 3) before the touch display (phase 2)** |

## Backlog items

| # | Item | Status | Notes |
|---|---|---|---|
| 1 | Core 3.x | Done | Same source builds on 2.0.17 (ESP32) and 3.3.12 (ESP32, ESP32-S3). `tools/cm.sh build3` |
| 2 | Board profiles | Done | ESP32 DevKit, ESP32 WROVER, ESP32-S3 N8R8 / N16R8 / quad-PSRAM. Valid, output, ADC1, RTC and "caution" pins; default pins; "move pins to this board's defaults" |
| 3 | Board metrics | Done | Uptime, boot count, last 10 reset reasons, heap / largest block / PSRAM, CPU per core (tick sampling), loop time, task stacks, Wi-Fi, flash writes, clients, chip temperature. Optional `sys_*` cloud upload |
| 4 | Point model, `/api/v2`, WebSocket | Done | `local.<id>` points, `/ws` with auth + commands, SSE and v1 API kept. Web app uses WebSocket, falls back to SSE, then polling |
| 5 | Rollback + safe mode | Done | New firmware confirmed after 60 s; 3 crash resets → safe mode (known-good config, relays off, no internet). Config keeps "previous" (undo) and "good" (after 5 min healthy) copies |
| 6 | Saved Wi-Fi networks | Done | Up to 8 with priority; async scan with back-off; optional "don't scan while the AP is in use"; v4 single network migrates automatically |
| 7 | Ignition sense + deep sleep | Done | Debounced input, sleep N min after key-off (postponed while someone is connected), wake on ignition (RTC GPIO, validated); upload buffer saved to flash first |
| 8 | RPM | Done | Interrupt pulse counting, 120 µs de-glitch, calibrate from a known RPM (`/api/rpm/calibrate`) |
| 9 | GPS | Done | NMEA RMC + GGA with checksum, speed / altitude / satellites / heading / HDOP sensors, sets the clock; absent module = "not found", never alarms by default |
| 10 | Turbo cool-down timer | Done | Indicator only; uses RPM and/or speed when available; dashboard banner + OLED screen + beep; hot shutdowns logged to the black box |
| 11 | Layout editor | Done | Dashboard tiles (order, wide, bar / value / trend line) and OLED order with page breaks (`layout.html`) |
| 12 | Computed sensors | Done | A − B, A + B, rate per minute, minutes in warning, A − curve(B) |
| 13 | Alarm black box | Done | 30 s before + 30 s after each new warning/critical alarm, newest 10 kept on LittleFS, Events page |
| 14 | Other optional sensors | Done | INA226, MPU-6050 roll/pitch, switch inputs, BME280 humidity, non-linear analog curves (fuel senders). ICM-42688 not yet |
| 15–23 | Touch display, multi-board | Not started | Next: phase 3 (multi-board) as agreed |

## How it was tested

- **Builds** (firmware slot is 1.62 MB):

  | Core | Board | Size |
  |---|---|---|
  | 2.0.17 | classic ESP32 | 1.34 MB |
  | 3.3.12 | classic ESP32 | 1.50 MB |
  | 3.3.12 | ESP32-S3 | 1.48 MB |

  Core 3.x builds leave ~120-200 KB free. Phase 3 should give 8/16 MB boards a bigger partition table (2.5 MB+ slots).
- **Emulator self-test (QEMU, classic ESP32), 59 checks, all passing:** config validation, board profiles, Wi-Fi list migration, curves, GPS parsing, computed sensors, turbo timer, black box recording, config undo slot, board metrics, plus all v4 checks.
- **Web UI test (Playwright vs. mock ESP32 with a real WebSocket):** dashboard fit, layout editor, sparkline tiles, turbo banner, events page, sensor editors (computed, RPM), Wi-Fi list, turbo settings, board panel, health panel, live data over WebSocket.
- **Emulator-only workarounds:** the task watchdog on core 0 is off, the firmware-confirm flash write is skipped and the board-metrics call doesn't walk the filesystem, because QEMU's flash takes seconds per write. Safe mode was seen to trigger in the emulator during development (a crash loop from a bug, since fixed): the 4th boot started in safe mode as designed.

## Not verified (needs the real boards)

- Wi-Fi scanning / joining and the AP channel follow, deep sleep and wake, RPM pulses from a real W terminal, GPS module, INA226 / MPU-6050 / BME280 hardware, CPU-load accuracy, OTA rollback on a real crash, ESP32-S3 on hardware.

## Bench test checklist

1. Flash v5 over USB on the current board; check **System → Board** says *ESP32 DevKit* and the health panel fills in.
2. Add your phone hotspot under **Networks to join**; confirm weather loads, then turn the hotspot off and on.
3. Upload the same firmware over Wi-Fi; the health panel should show *on trial*, then *confirmed* after 60 s.
4. Add an RPM sensor (through the opto/comparator board), calibrate at idle against a tachometer.
5. Enable the turbo timer; after a drive, idle and watch the banner count down.
6. Trigger a warning (e.g. lower a threshold) and check the recording on **Events**.
7. If fitted: ignition input with sleep after 2 min; GPS on a free UART pin.

## Bench test results (31 Oct 2026, classic ESP32)

- v5 installed over USB (`carmetrics-full-flash.bin`); settings carried over from v4.
- Board health panel works: CPU 5% / 3%, loop 0.3 ms average (247 ms max once), ~118 KB free, task stacks 4–9 KB headroom.
- **Wi-Fi joining is unreliable:** it joined the saved network (HouseLAN) only once. Cause not yet known.
- Known display bug, fixed in source: firmware slot showed 2978 KB instead of 1664 KB.
- Over-the-air uploads failed earlier ("Wrong Magic Byte" = the full USB image was uploaded; later drops while the laptop kept leaving the AP). The System page now checks files before uploading.

## Next session

1. **Wi-Fi diagnostics and fix:** log each join attempt (networks seen, RSSI, chosen network, disconnect reason codes) on the System page; more persistent retries; check the AP channel switch and laptop drop-outs.
2. **Next phase:** the user asked for "phase 2" after testing. Earlier the agreed order was multi-board (phase 3) before the touch display (phase 2, needs a JC3248W535 board). Confirm which at the start.
3. Bigger partition layout for 8/16 MB boards before multi-board (core 3.x builds have ~120–200 KB free).

Toolchain notes for a new session: `devenv/` (Docker or `setup-toolchain.sh`) installs arduino-cli, core 2.0.17 + 3.3.12, libraries and QEMU. `tools/cm.sh build | build3 esp32s3 | selftest | webtest`.
