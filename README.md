# CarMetrics

An engine monitor that runs on a single ESP32. It reads temperature, pressure and voltage sensors,
shows them on an OLED in the cab and on a phone over Wi-Fi, sounds alarms, and switches relays.
Firmware and the web app can both be updated over Wi-Fi.

> **v4** replaces the Arduino Mega + ESP32 pair (kept in [`legacy/`](legacy)) with one ESP32.
> It has been compiled and run in Espressif's ESP32 emulator (QEMU), where a self-test of config,
> alerts, relays and OLED rendering passes. It has **not yet been tested on real hardware**.

## What it does

- **Sensors** – add, remove and reorder them from the web app, with no code changes:
  - DS18B20 1-Wire temperatures (coolant, intake, gearbox, transfer case…). The bus scan shows each sensor's live temperature so you can tell them apart.
  - EGT thermocouple through an **MCP9600/MCP9601** (I2C, no extra pins – recommended) or a **MAX6675** (SPI, 3 pins)
  - Analog through an **ESP32 pin** (ADC1, GPIO 32–39) or an **ADS1115** channel (16-bit, I2C). Each sensor has its own calibration, with presets for 0.5–4.5 V pressure senders and B25 battery modules.
  - BMP180 / BMP280 (auto-detected) for cabin temperature, pressure and altitude
- **Alerts** – high/low warning and critical limits per sensor, with hysteresis.
  - A wiring fault (broken wire, missing sensor) is an alert too.
  - Oil pressure and similar sensors can be set to only alarm while the engine is running (detected from battery voltage).
  - Mute from the web app. A new or worse alert un-mutes.
- **OLED** – choose SSD1306/SH1106 128×64 or SSD1327 128×128 in settings.
  - Each sensor appears as a text cell or a bar gauge with threshold ticks.
  - Alerting values are highlighted (critical ones blink); pages rotate if everything doesn't fit.
  - Optional clock.
- **Web app** – gauges, 30-minute charts (kept on the ESP32), sensor setup, relay controls, system settings.
  - Live updates over Server-Sent Events.
  - The whole app is 35 KB gzipped.
- **Relays** – on/off or blinking, active-low support, a chosen state at boot.
  - Pin checks block GPIOs that would break booting or clash with sensors.
- **Updates over Wi-Fi** – upload firmware or the web app from the System page, or use ArduinoOTA from the IDE.
  - Settings live in NVS, so they survive both kinds of update and power cuts while saving.
- **Admin password** protects settings, relays and updates (HTTP user `admin`).

- **Weather tab** – public weather for chosen Kenyan towns (Open-Meteo, no API key). Only works while the ESP32 has internet.
  - A reference town's sea-level pressure can calibrate the altimeter automatically.
  - Offline, use **System → Altitude calibration**: enter a known elevation.
- **Cloud upload** – downsampled metrics (average, min and max per 250 ms … 5 min) sent to **Zabbix** (trapper) or anything that speaks **InfluxDB line protocol** (VictoriaMetrics, InfluxDB, Grafana Cloud).
  - It only sends while online.
  - Offline data waits in a ring buffer: RAM first, then 4 KB blocks on flash, capped (default 256 KB), oldest dropped first.
  - It uploads oldest-first when the connection returns.

## Wiring (ESP32 is 3.3 V – not 5 V tolerant)

| Function | Default GPIO | Notes |
|---|---|---|
| I2C SDA / SCL | 21 / 22 | OLED, BMP180/280, ADS1115 share the bus |
| 1-Wire (DS18B20) | 4 | **4.7 kΩ pull-up to 3.3 V** (2.2 kΩ for long CAT6 runs). Power the sensors from 3.3 V |
| MAX6675 SCK / CS / SO | 5 / 17 / 16 | Only if you keep the MAX6675. Power it from **3.3 V** |
| MCP9600 / MCP9601 | I2C (0x60–0x67) | Replaces the MAX6675 – no GPIOs needed |
| Boost pressure | 32 | 0–30 psi sender through a divider |
| Oil pressure | 34 | 0–100 psi sender through a divider |
| Starter battery | 35 | B25 module (÷5) |
| Aux battery | 33 | B25 module, disabled by default |
| Buzzer / warning LED | 18 / 19 | |
| Relays | 26 / 27 | Disabled until you enable them |

All pins can be changed in the web app.

**EGT wiring:**

- **Where the amplifier goes:** put the thermocouple amplifier (MCP9600 or MAX6675) **in the cab box**, not the engine bay.
- **Probe cable:** run **K-type thermocouple extension cable** from the probe all the way to the amplifier's terminals. It must not be CAT6 or copper wire, or the reading will be wrong.
- **What this saves:** only two wires go through the firewall, instead of power plus three SPI lines over CAT6.
- **MCP9600 or MCP9601:** both sit on the existing I2C bus. The MCP9601 also reports an open or shorted probe.
- **If MCP9600 readings drop out:** set I2C speed to 100 kHz on the System page, because the chip stretches the I2C clock.

**Analog inputs.** The pressure senders are 5 V parts with a 0.5–4.5 V output. Bring that down below 3.3 V:

- **Divider:** 10 kΩ from the signal to the ESP32 pin, 20 kΩ from the pin to ground. This divides by 1.5, so 4.5 V becomes 3.0 V, which matches the default calibration.
- **Or an ADS1115:** power it from 3.3 V and keep the same dividers in front of it. It's much more accurate than the ESP32's own analog pins.

Also fit a **100 kΩ pull-down** (pin to ground) on every analog input. Without it, an unplugged or broken sender lets the pin float and show random values; with it, the pin reads 0 V and the sensor shows **FAULT**.

On each input, also fit a 1 kΩ series resistor, a 3.3 V zener/TVS to ground and a 100 nF capacitor to protect against engine-bay voltage spikes.

Analog sensors **must** use GPIO 32–39. The ESP32's other analog pins stop working while Wi-Fi is on.

## First-time install (USB)

The first flash must be over USB, because it installs the new partition layout. After that, everything can be done over Wi-Fi.

### Option A – no IDE: flash the full image

1. Open https://espressif.github.io/esptool-js/ in Chrome or Edge and connect the ESP32 over USB.
2. Flash `carmetrics-full-flash.bin` at address **0x0**. This one image holds the bootloader, partitions, firmware and web app.

### Option B – Arduino IDE 2

1. **Board:** install *esp32 by Espressif* (tested with 2.0.17) and select **ESP32 Dev Module**.
   - Set **Partition Scheme** to **Minimal SPIFFS**. This only sets the size limit; the real layout comes from `firmware/CarMetrics/partitions.csv`.
2. **Libraries:** install these from the Library Manager (versions tested):
   - ArduinoJson 7.2.1
   - ESP Async WebServer 3.12.1 *and* Async TCP 4.0.0 – the **ESP32Async** versions
   - U8g2 2.37.1
   - OneWire 2.3.8
   - DallasTemperature 4.0.6
   - Adafruit MAX6675 1.1.2
   - Adafruit BMP280 3.0.0
   - Adafruit BMP085 1.2.4
   - Adafruit ADS1X15 2.6.2
   - Adafruit MCP9600 2.0.4 (Adafruit Unified Sensor and BusIO install with these)
3. **Firmware:** open `firmware/CarMetrics/CarMetrics.ino` and upload.
4. **Web app:**
   1. Run `python3 tools/build_web.py`. This writes `firmware/CarMetrics/data/` and, if it finds `mklittlefs`, `build/littlefs.bin`.
   2. Get it onto the ESP32 in one of two ways:
      - Upload `data/` with the *LittleFS upload* plugin.
      - Join the Wi-Fi and open http://192.168.4.1. Until the web app is installed, the ESP32 shows a simple upload page; upload `littlefs.bin` there.

## Using it

1. **Connect:** join the Wi-Fi network **CarMetrics** (password `carmetrics`) and open http://192.168.4.1 or http://carmetrics.local.
2. **Change passwords:** on the **System** page, change the Wi-Fi and admin passwords (the admin password is also `carmetrics`).
3. **Assign temperature sensors:** on the **Sensors** page, press **Scan**, assign each 1-Wire address, then **Save changes**.
4. **Check the analog sensors:** the editor shows the live voltage at each sensor, which helps you confirm the calibration.

**Updating later:**

1. **Build the firmware file:** in Arduino IDE use *Sketch → Export Compiled Binary*. It writes `CarMetrics.ino.bin`.
2. **Build the web app file:** run `tools/build_web.py`. It writes `littlefs.bin`.
3. **Upload:** on the **System → Firmware & web app update** page, pick the file and press Upload. This works from a phone.

## Project layout

```
firmware/CarMetrics/   ESP32 firmware (Arduino sketch, split into modules – see header of CarMetrics.ino)
web/                   web app sources (plain HTML/JS/CSS, no build step besides gzip)
tools/build_web.py     gzips web/ into firmware data/ and builds littlefs.bin
legacy/                previous Arduino + ESP32 versions
```

## Internet features

The ESP32 needs to **join a Wi-Fi network that has internet**. Set **System → Settings → Also join network**, e.g. your phone's hotspot.

- It keeps its own `CarMetrics` network at the same time.
- The clock comes from NTP once online.

### Weather

- **Turn it on:** in the Weather tab, tick up to 12 towns and pick a reference town.
- **Altitude:** with *"Use its sea-level pressure"* on, altitude is corrected for the day's weather. The weather value is kept in RAM only and never written to flash.

### Cloud upload (System → Cloud upload)

**How much the buffer holds.** A sample is ~130 bytes with 10 sensors. At 1 minute per point, 256 KB holds ~34 hours of driving; the page shows the estimate for your settings.

**Flash wear.** Flash is only written while offline, in full 4 KB blocks, and only while the engine runs. At 1-minute sampling that is a few blocks per hour of driving: decades of life.

**Zabbix.**
1. Create a host (default name `carmetrics`).
2. Add the items for the mode you chose:
   - **Packed (default):** one trapper item `carmetrics.json`, type *Text*. Add dependent items with JSONPath preprocessing, e.g. `$.coolant_temp[0]` (avg), `$.coolant_temp[1]` (min), `$.coolant_temp[2]` (max).
   - **Per metric:** trapper items `<sensor id>.avg`, `.min`, `.max` (Numeric float).
3. Each value carries its own timestamp, so buffered data lands at the right time.
4. Optional: TLS-PSK (identity + hex key).

**Prometheus / Grafana.**
- **Recommended route:** a Prometheus-compatible store that accepts timestamped pushes, e.g. VictoriaMetrics at `http://<vm>:8428/write`. Grafana can then query it with PromQL.
- **Influx data model:** measurement `carmetrics`, tag `device=<hostname>`, fields `<sensor id>`, `<sensor id>_min`, `<sensor id>_max`, timestamps in milliseconds.
- **Why not the Pushgateway:** it rejects timestamps, so buffered history would be lost.

**HTTPS** is encrypted, but certificates are not verified.

**Web app updates** erase any unsent backlog on flash. The System page warns you first.

### Dev environment (Docker)

`devenv/` contains the exact toolchain used during development: arduino-cli, ESP32 core 2.0.17, pinned libraries and Espressif's ESP32 emulator (QEMU). It also includes headless web tests.

See [devenv/README.md](devenv/README.md). In short:

```
docker build -t carmetrics-dev devenv
docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh all
```

### Emulator self-test

`Selftest.cpp` compiles only with `-DCM_EMULATOR_TEST -DCM_EMU_SELFTEST`.

- It skips Wi-Fi and the ADC (QEMU emulates neither) and feeds fake sensor values.
- It checks config validation, saving and reloading, alert levels, mute and relays.
- It prints the OLED frame buffer as PBM images.

Normal builds don't include any of it.

### HTTP API

| Method & path | Auth | Purpose |
|---|---|---|
| `GET /events` | | Live state every second (`state`) and config changes (`config`) |
| `GET /api/state` | | Current readings, alert levels and relay states |
| `GET /api/info` | | Version, memory, network status |
| `GET /api/config` | | Full config, without passwords |
| `GET /api/history` | | Chart history |
| `GET /api/onewire` | | Last 1-Wire bus scan |
| `POST /api/onewire/scan` | | Start a new bus scan |
| `POST /api/config/sensors` | ✔ | Replace the sensor list |
| `POST /api/config/relays` | ✔ | Replace the relay list |
| `POST /api/config/system` | ✔ | Change system settings |
| `POST /api/config/import` | ✔ | Restore a config backup |
| `POST /api/config/reset` | ✔ | Reset sensors and relays to defaults |
| `GET /api/config/export` | ✔ | Download a config backup |
| `POST /api/relays/control` | ✔ | `{id, on, blinkMs}` |
| `POST /api/alerts/mute` | | `{minutes}`, where 0 = unmute |
| `POST /api/time` | | `{epochMs}` – set the clock |
| `POST /api/update?target=firmware\|filesystem` | ✔ | Install an update |
| `POST /api/reboot` | ✔ | Reboot |
