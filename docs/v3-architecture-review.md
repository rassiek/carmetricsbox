# CarMetrics V3 – Architecture Review (pre-testing)

Reviewed: `rassiek/carmetricsbox`, branch **V3** (commit 0455193, Jun 2025), `EngineMonitorESP32_Integrated.ino` + `data/` web UI. Review date: 2026-10-30. Not yet compiled or run on hardware.

## Current architecture
Single ESP32. `loop()` = readAllSensors → checkAllSensorAlerts → handleGPIOOutputs → updateOLED → sendSensorEvents (SSE every 3 s) → periodic OneWire discovery.
Web: ESPAsyncWebServer in AP mode (`EngineMonitorAP`), static files from SPIFFS, JSON APIs for sensors, config and GPIO. Config persisted as JSON in SPIFFS.

## P0 – will break testing or damage hardware
1. **5 V → 3.3 V logic (hardware).** The old system was a 5 V Arduino. The ESP32 is NOT 5 V tolerant.
   - 1-Wire pull-up must go to 3.3 V, not the 5 V engine-bay supply. Long CAT6 run at 3.3 V may need a stronger pull-up (~2.2 kΩ).
   - Power the MAX6675 from 3.3 V (otherwise SO drives 5 V into the ESP32).
   - Pressure transducers (0.5–4.5 V) need a divider; code assumes ~0.66 scaling (ADC 410–3686).
   - Use ADC1 pins only (GPIO 32–39). ADC2 does not work while Wi-Fi is on, and the settings page lets you pick any pin.
2. **Main loop blocks ~750 ms per iteration.** `sensorsMain.requestTemperatures()` runs every loop pass with DallasTemperature's default blocking conversion (12-bit = 750 ms). Fix: `setWaitForConversion(false)`, request every ~1 s, read after conversion time; optionally 10/11-bit resolution.
3. **Reading-key mismatch – several gauges and the whole charts page show nothing.**
   - Backend renames only CT/Oil/BAT1/EGT/CTMP; everything else is emitted under its raw id.
   - script.js expects IMT, BST, GBT, TCT, and maps battery only for id `bat1_voltage` (default id is `bat_voltage`).
   - charts.html expects `cabinTemperature`, `coolantTemperature`, `boost` etc. — none exist.
   - Fix: one source of truth – emit readings keyed by `sensor.id`; frontend uses the id directly.
4. **One pressure calibration for all sensors.** `PRESSURE_TRANSDUCER_MAX_PSI = 100` is global, but boost uses a 0–30 psi transducer → boost reads ~3.3× high. Needs per-sensor calibration.
5. **Race conditions between web handlers and loop().** AsyncTCP callbacks run in a separate FreeRTOS task. `/api/setconfig` and `/api/gpioconfig` rewrite `configuredSensors[]`/`configuredGPIOPins[]` (including `String` fields and the count reset to 0) while loop() iterates them → possible crashes or heap corruption. Fix: handlers queue the change; loop() applies it (or guard with a mutex).

## P1 – correctness and safety
6. **POST handling via global `onRequestBody`** with no registered POST routes. Requests fall to the catch-all handler, which may send a second response (500). Works by accident; fragile. Also shares `static String` buffers across requests. Register proper POST handlers (e.g. `AsyncCallbackJsonWebHandler`).
7. **Blocking work inside async callbacks.** `/api/onewire/unconfigured` runs a bus scan with `delay(100)`; SPIFFS writes and verbose JSON logging also run in the TCP task → watchdog resets / dropped connections.
8. **GPIO config POST drops `enabled`.** `tempConf` isn't initialised and `enabled` isn't parsed (garbage value). `handleGPIOOutputs()` ignores `enabled` anyway. Removing a pin leaves it driven at its last level. No validation stops a relay pin colliding with I2C/1-Wire/sensor pins.
9. **Unauthenticated relay control.** Hard-coded AP password `password123`, no auth on `/api/gpio/control`, default "Fuel Pump Relay" on GPIO26. Anyone who joins the AP can switch vehicle loads. Also needs active-low relay support and safe boot states (avoid strapping pins; GPIO5 used for EGT CS is a strapping pin).
10. **Alert logic.**
    - If a lower-critical threshold is set, the lower-warning one is never evaluated (`else if`).
    - A failed sensor (−999) raises no alert, so a dead coolant sensor goes silent.
    - Low-oil-pressure alarm fires with the engine off (needs an "engine running" gate, e.g. battery > 13.2 V for N s).
    - No hysteresis or mute/acknowledge.
    - `buzz()`/`flash()` use `delay()`.
    - Alert state isn't shown in the web UI.
11. **Non-atomic config saves.** Power loss mid-write (ignition off) corrupts JSON → silent fallback to defaults, overwriting your config. Write to a temp file then rename; keep a backup.
12. **Default coolant 1-Wire address is a placeholder** (28FFAABBCCDDEE01). Coolant reads N/A until reassigned.
13. **ADC accuracy.** Assumes a 3.3 V reference and linear ADC. Use `analogReadMilliVolts()` plus averaging/filtering (also smooths needle jitter).
14. **Sensor errors sent as −999** in /readings → gauges peg to minimum. Send `null` plus a status field instead.

## P2 – structure and maintainability
15. **Make sensor config the single source of truth:** id, label, type, pins, calibration (inMin/inMax V → outMin/outMax), units, gauge min/max, thresholds, hysteresis, engineRunningOnly. Frontend builds gauges from `/api/sensors`, removing the hard-coded per-id switch in script.js. The OLED layout uses slot assignments instead of hard-coded ids.
16. **Push, don't poll:** `/events` SSE is already sent every 3 s but no page uses it. Gauges poll `/readings` every 3 s; charts every 5 s; GPIO every 2 s.
17. **Keep history on the device:** a RAM ring buffer (e.g. 30 min at 5 s) exposed at `/history`, so charts survive a page reload. Optionally log to an SD card.
18. **Split the 1,090-line .ino into modules:** sensors, config, web, display, alerts, gpio. Remove commented-out duplicate handlers and debug noise.
19. **Platform choices:**
    - SPIFFS is deprecated → use LittleFS.
    - The me-no-dev ESPAsyncWebServer is unmaintained → use the mathieucarbou fork.
    - Consider ArduinoJson instead of Arduino_JSON.
    - Use PlatformIO with pinned library versions.
20. **Serve web assets gzipped.** `data/` is 472 KB (highcharts.js alone 283 KB) over a soft-AP link.
21. **Discovery workflow:** a separate discovery pin (GPIO25) means physically moving each sensor to find its address. Alternative: scan the main bus and list unknown addresses.
22. **Other:**
    - Clock lost with the Arduino RTC: OLED shows "Time N/A". Options: sync time from the browser on connect, or add an RTC.
    - mDNS (`carmetrics.local`).
    - Move legacy Arduino/ESP32 sketches into `/legacy`.
    - Merge V3 → main and update the README.
23. **Display:** code targets a 128×64 SSD1306; the box had a 128×128 SSD1327. The BMP180 in the box is not detected by `Adafruit_BMP280`.

## Suggested order
Hardware level checks (1) → non-blocking loop (2) → id-keyed readings + per-sensor calibration (3, 4, 15) → thread-safe config/POST handlers (5–8) → alerts (10) → the rest.
