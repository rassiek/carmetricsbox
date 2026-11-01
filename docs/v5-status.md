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
| 6 | Saved Wi-Fi networks | Done | Up to 8 with priority; v4 single network migrates automatically. Join logic reworked after the bench test, see "Wi-Fi fix" below |
| 7 | Ignition sense + deep sleep | Done | Debounced input, sleep N min after key-off (postponed while someone is connected), wake on ignition (RTC GPIO, validated); upload buffer saved to flash first |
| 8 | RPM | Done | Interrupt pulse counting, 120 µs de-glitch, calibrate from a known RPM (`/api/rpm/calibrate`) |
| 9 | GPS | Done | NMEA RMC + GGA with checksum, speed / altitude / satellites / heading / HDOP sensors, sets the clock; absent module = "not found", never alarms by default |
| 10 | Turbo cool-down timer | Done | Indicator only; uses RPM and/or speed when available; dashboard banner + OLED screen + beep; hot shutdowns logged to the black box |
| 11 | Layout editor | Done | Dashboard tiles (order, wide, bar / value / trend line) and OLED order with page breaks (`layout.html`) |
| 12 | Computed sensors | Done | A − B, A + B, rate per minute, minutes in warning, A − curve(B) |
| 13 | Alarm black box | Done | 30 s before + 30 s after each new warning/critical alarm, newest 10 kept on LittleFS, Events page |
| 14 | Other optional sensors | Done | INA226, MPU-6050 roll/pitch, switch inputs, BME280 humidity, non-linear analog curves (fuel senders). ICM-42688 not yet |
| 15–16 | Touch display | Not started | After phase 3 |
| 17 | Board-to-board protocol | Format + exchange logic done | `Proto.*`, `Link.h`, `BoardNet.*`; PC test `tools/cm.sh prototest` (36 checks incl. two boards over a lossy simulated link); see [phase 3 plan](v5-phase3-plan.md) |
| 19–22 | ESP-NOW, board roles, Boards page | First round built | Role setting, ESP-NOW link with channel search, Boards page (pair / status / values / relays), board selector on Sensors + Relays, relay safe state. Needs two boards to try. Remote values on gauges/charts/alarms next |
| 22b | Remote values on gauges / charts / alarms / black box / cloud / OLED | Done (5.1.0-beta.8) | Mirrored `remote` sensors on the main board |
| 19b | Encrypted board links | Done, off by default | Key per board at pairing; turn on after the first two-board test |
| 23 | Firmware updates for other boards | Built (5.1.0-beta.9), PC-tested | Needs two boards to try |
| 18 | CAN link | Built (5.1.0-beta.9), framing PC-tested | Needs two boards + SN65HVD230s |
| — | 5.2 for ESP32-S3 8 MB+ | Built (5.2.0-beta.1) | `tools/cm.sh build52`; needs an S3 N8R8/N16R8 |

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
- **Wi-Fi joining is unreliable:** it joined the saved network (HouseLAN) only once. Probable causes found and fixed, see below; not yet retested.
- Known display bug, fixed in source: firmware slot showed 2978 KB instead of 1664 KB.
- Over-the-air uploads failed earlier ("Wrong Magic Byte" = the full USB image was uploaded; later drops while the laptop kept leaving the AP). The System page now checks files before uploading.

## Wi-Fi fix (31 Oct 2026, after the bench test)

Same join code as v4, so the problem probably predates v5. Likely causes found in the code, all addressed in `WifiMgr.cpp`:

| Problem | Fix |
|---|---|
| Back-off doubled to **5 min** after a few misses, so the board looked dead | Capped at 60 s; reset when networks change, on connection loss and by **Search now** |
| `WiFi.begin(ssid, pass)` threw away the scan result; the driver scanned again (slower, AP interrupted twice, may pick another router) | Join with the channel + BSSID from our scan; up to 3 routers per round, best first |
| Any failure = 20 s wait + back-off; the ESP32 often fails the first handshake | 3 quick tries per router (1.5 s apart), 2 for password-like reasons, next router for "not found" |
| No disconnect reason recorded | Event handler records the ESP-IDF reason code; shown in plain words |
| AP started on channel 1, then jumped to the joined network's channel and dropped the laptop (the OTA drop-outs) | Last joined network (SSID, BSSID, channel) kept in NVS (`cmwifi`); AP starts on that channel and the board rejoins it directly, no scan |
| Hidden networks never matched (scan skipped them) | Scan includes hidden; if nothing saved is visible, the best saved network is tried by name |
| WPA/TKIP-only routers refused by the core's WPA2 minimum | Minimum security follows what the scan reports |
| Hostname set after `WiFi.mode()` (ignored on 2.0.17), modem sleep on | Hostname first, sleep off |

**Join log:** System page → *Wi-Fi joins* (also `GET /api/v2/wifi`, serial `[wifi]` lines): state, radio channel, remembered network, last scan (name, signal, channel, security, ★ = saved), last 20 attempts with reason codes. **Search now** (`POST /api/v2/wifi/search`) scans immediately; while joined it only refreshes the list.

Tested: builds on 2.0.17 (1.35 MB) and 3.3.12 (ESP32 1.52 MB, S3 1.50 MB), emulator self-test 61/61 (2 new), web UI test (3 new). The radio itself is untested: needs the bench.

**Bench check for the fix:** flash `carmetrics-full-flash.bin` over USB (the web app changed too, so a firmware-only OTA isn't enough), open System → Wi-Fi joins, and check: joins HouseLAN within ~10 s of boot; reboot → rejoins via "remembered" without a scan; turn the hotspot / router off and on → "connection lost", rejoin within a minute; laptop stays on the CarMetrics AP throughout. If it still fails, the reason codes in the log say why.

## Clock and weather fixes (31 Oct 2026)

- **Clock "not set":** the web app sent the browser's time only once per browser tab, so after a reboot or OTA with the tab still open the board never got the time again (NTP needs Wi-Fi, which was failing). The live state now says when the board has no clock and the web app sends the time whenever that happens. The source (internet, GPS, browser) is shown on the System page, and the board's time is shown in the header on every page.
- **Weather "InvalidInput":** Open-Meteo sends a chunked HTTP/1.1 reply and the JSON parser read the raw stream including the chunk sizes. The request now uses HTTP/1.0. Error text is clearer ("reply not readable").

## Memory and storage (31 Oct 2026)

Measured from the build map and the emulator; the classic ESP32 is the tight one.

| Change | Saving |
|---|---|
| Chart history stored as 16-bit floats (~3 significant digits, NaN kept; every value round-trips) | Half the RAM: 15 sensors x 30 min = 10.8 KB instead of 21.6 KB. Matters more with remote boards' points added |
| Chart history sent as a chunked reply instead of being built in RAM first | Removes a ~30 KB temporary peak on every Charts page load (would be ~250 KB with 4 h of history) |
| Weather reply filtered while parsing (only `current` + elevation) | Smaller JSON document while TLS already holds ~40 KB |
| Boards with PSRAM (S3 N8R8/N16R8, WROVER) keep the history in PSRAM, 4 h instead of 30 min | Internal RAM stays free for Wi-Fi, TLS, ESP-NOW |
| Health panel: "largest block" now counts internal RAM only; shows chart memory and where it lives | Honest numbers on PSRAM boards |

Flash: firmware 1.36 MB of the 1.62 MB slot on core 2.0.17 (~260 KB free), 1.50–1.52 MB on core 3.x (~120 KB free). Keep the classic 4 MB board on **core 2.0.17**; the bigger partition layout comes with the 8/16 MB S3. Biggest flash users are the Wi-Fi/TLS/lwIP libraries (~500 KB, needed); our own largest is `Config.cpp` (54 KB, JSON handling). Options not taken yet: drop ArduinoOTA (IDE uploads over the network; ~6 KB flash, 1.7 KB RAM), since the web page does updates.

## Bench test 1 Nov 2026: fixes

Health panel showed a **crash reset**, **loop max ~1 s**, **lowest free 77 KB** with **9 live clients**, and no storage figures.

- **Loop stalls (~1 s):** `/api/info`, the health panel and the cloud status walked the whole LittleFS (`usedBytes()`) while holding the state lock, every 5 s with the System page open; the main loop waited for the lock. Storage figures are now measured from the net task once a minute, without the lock, and cached.
- **Storage tile:** file system used / total, split into web app, black box, upload buffer, plus settings (NVS) use.
- **Crash report:** the ESP32 already writes a core dump to flash on a crash; the System page now shows it (task, cause, address, backtrace, whether it was this firmware), with **Download crash dump** and **Clear**. The 1 Nov crash happened with build `10cbe4c`; its ELF is kept to decode the backtrace.
- **Live-data clients:** the event-stream fallback is capped at 3 clients (WebSocket stays at 6); stale ones from phones that left the access point no longer pile up.
- **Cloud "Recording: no":** by design while the engine isn't running; the status now says so and which box to untick for bench tests.

## Crash 1 Nov 2026: decoded and fixed

`async_tcp` aborted: `AsyncResponseStream` grew its buffer a few bytes at a time while ArduinoJson wrote a reply (`cbuf::resize` → `new[]` failed → `abort`). Out of memory, made likely by many live-data clients each holding queued messages. Fixed: reply buffer sized once from `measureJson`, a 503 "low memory" answer instead of crashing, and live data skips clients that aren't keeping up (one shared buffer for all WebSocket clients).

## Releases: 5.1 and 5.2 (decided 1 Nov 2026)

| Line | Boards | What |
|---|---|---|
| **5.1** (now, `5.1.0-beta.N`) | Every board: classic ESP32 (4 MB) and ESP32-S3; main, sensor and relay roles | Everything built so far; fits the 4 MB layout. This is what gets tested now |
| **5.2** (later) | Main board **must be an ESP32-S3** (8/16 MB, PSRAM); its firmware also runs on any S3 | Bigger partitions, more features, richer layout / touch display. **Must stay compatible with 5.1 sensor and relay boards**: same board-to-board protocol (`Proto::VERSION` 1, frames ≤ 250 B), new features only as optional additions |

The release label (`FW_VERSION` in `Config.h`) is shown in the web app header, on the System page (firmware and web app) and on the OLED start screen; the web app warns when firmware and web app come from different releases. Release files are named after it.

## OLED layout editor (5.1.0-beta.7)

Layout page → *OLED screen*: per value a **3-character label** (Suggested labels: CLT coolant, IT intake, GBT gearbox, TCT transfer case, CT cabin, EGT, BST, OIL), style **Text / Bar / Marker** (marker = the original look: threshold ticks in the frame and an "I" needle at the value), **threshold sliders** for bar and marker values, page breaks, **values per text line** (auto / 2 / 3 / 4, smaller font for 3–4), and a **live preview** of every page drawn like the firmware draws it. New default labels for fresh configs: CLT, IT, GBT, TCT, CT; EGT / boost / oil / coolant default to Marker.

## Next session

1. Wi-Fi fix bench-tested OK by the user (31 Oct).
2. **Phase 3 (multi-board), confirmed before phase 2.** Plan agreed: [v5-phase3-plan.md](v5-phase3-plan.md) (ESP-NOW default, CAN optional, sensor + relay roles together). Step 1 done; step 2 exchange logic done (`BoardNet`). Next: wire it into the firmware: role setting, sensor board / relay board behaviour, remote points in the main board's state, history and alarms; then the ESP-NOW link (step 3). Note: ESP-NOW must share the channel of the network the main board joins, so remote boards need to follow channel changes.
3. Bigger partition layout for 8/16 MB boards before multi-board (core 3.x builds have ~120–200 KB free).

Toolchain notes for a new session: `devenv/` (Docker or `setup-toolchain.sh`) installs arduino-cli, core 2.0.17 + 3.3.12, libraries and QEMU. `tools/cm.sh build | build3 esp32s3 | selftest | webtest`.
