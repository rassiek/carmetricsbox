# CarMetrics v4 – status (2026-10-31)

Branch `v4` off `V3` in rassiek/carmetricsbox, 5 commits; the latest is b9a7028. The session can't push, so the work is delivered as a git bundle and a source zip. The firmware runs on the user's ESP32, flashed from Arduino IDE 1.8.20 with core 2.0.17.

## Features since the first v4 commit

| Commit | What it added |
|---|---|
| 3d7c0b8 | MCP9600/MCP9601 I2C thermocouple support (MAX6675 still supported); I2C speed setting (100/400 kHz) |
| 06c00f1 | Digital-tile dashboard that fits one screen; grouped charts (1-Wire temps, pressures, EGT, voltages); polling fallback for live data; per-sensor "alarm if missing/faulty" (off for EGT, offset now 0); analog values clamped to range; 0.35 V sender fault; 100 kΩ pull-down advice |
| 06a0004 | Weather tab (Open-Meteo, 41 Kenyan towns, reference town auto-calibrates the altimeter, RAM only); known-altitude calibration; cloud upload (avg/min/max at 250 ms–5 min) to Zabbix trapper (packed `carmetrics.json` or per-metric, optional TLS-PSK) or InfluxDB line protocol; offline buffer (RAM queue, then full 4 KB chunks to a capped LittleFS ring); NTP when online |
| b9a7028 | Docker dev environment (`devenv/`, `tools/cm.sh`), VS Code devcontainer |

## Decisions
- **Hardware:**
  - One ESP32 does everything.
  - Analog inputs use ADC1 pins or ADS1115 channels, with dividers to 3.3 V.
  - The OLED type is selectable.
- **Settings storage:** in the `cfg` NVS partition, so OTA updates don't erase them.
  - Partitions: nvs 0x9000, otadata 0xE000, app0 0x10000, app1 0x1B0000, LittleFS 0x350000 (576K), cfg 0x3E0000, coredump 0x3F0000.
- **Upload protocols:** the Prometheus Pushgateway was rejected because it drops timestamps, so buffered history would be lost.
  - Use VictoriaMetrics via Influx line protocol for Prometheus/Grafana.
  - Prometheus OTLP is a possible phase 2.
- **No gzip:** the ROM deflate needs about 100 KB of heap.
- **Defaults:** AP `CarMetrics` / `carmetrics`; admin `admin` / `carmetrics`.

## Verified
- **Emulator self-test, 33/33**, run in QEMU and inside the Docker image. Covers config, alerts, relays, history, MCP9600, Influx/Zabbix payloads and offline flash buffering, plus OLED frames.
- **Web UI test, 18/18**, Playwright against the mock ESP32.
- **Docker image** builds from a clean Ubuntu 24.04 base; `tools/cm.sh build` gives an identical size (1,243,105 bytes).
- **Not verified in the sandbox:** the optional Node/Chromium install stage (nodejs.org and the Playwright CDN are blocked here).

## Not yet done / next
- **Real-hardware tests:** sensors, Wi-Fi client mode, weather fetch, uploads, flash write timing.
- **Push v4** from the user's PC with the bundle.
- **Ideas:**
  - RPM from the alternator W terminal (300Tdi has no OBD)
  - Ignition sense + deep sleep
  - Trip logging / peak hold
  - Rules → relays
  - Turbo cool-down
  - PWA
  - OLED dimming
  - CI release builds
  - Update from GitHub
  - Ratiometric correction
  - Compile check against ESP32 core 3.x
