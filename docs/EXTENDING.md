# CarMetrics — extending it

How to add a sensor type, an actuator type, a board profile, a metrics output, a
preset or an API address, and where to put the tests. Design: `docs/DESIGN.md`;
API rules: `docs/API.md`.

Where code lives (5.4): shared parts in `firmware/libraries/CarMetricsCore/src`
(settings, sensors, actuators, board profiles, API core, web app) and
`CarMetricsLink/src` (board-to-board protocol); main-only parts in `firmware/CarMetrics`
(boards, displays, cloud, weather, black box, OLED: `WebMain.cpp` adds their routes);
worker-only parts in `firmware/CarMetricsWorker` (`Node`, `WebWorker.cpp`). A part
a firmware adds is announced in `/info` `capabilities` (`Web::setInfo`) and the web app
shows it only then (`CM.has(...)`).

General rules: one place decides each thing; settings are validated when saved
(`validateConfig`), never when used; hardware is only touched from `loop()`;
web handlers copy what they need under `StateLock` and never wait for hardware.

---

## 1. A sensor type

1. **Config.h** — add the value to `enum class SensorType`, and any new fields to
   `SensorConfig` (with a default).
2. **Config.cpp** — name it in `sensorTypeToStr` / its parser, write the new fields
   in `sensorToJson`, read and check them in `sensorsFromJson` (plain error
   messages: *"Sensor 'x': …"*), claim its pins in `validateConfig`.
3. **Sensors.cpp** — `begin()`: set up the hardware for each sensor of the type;
   `update()`: read without blocking for more than a few ms (start a conversion,
   come back next pass) and call `setOk(i, value)` / set a fault status.
4. **Discover.cpp** (optional) — return its I2C address in `i2cDriver()` so
   *Discover* names it.
5. **web/schema.js** — one entry in `sensorTypes` and its fields in `sensor`
   (`show: is('newtype')`). The forms, lists and dashboard need nothing else.
6. **Tests** — a case in `Selftest.cpp` (valid + one refused setting) and, if it
   has a JSON field, the round trip test passes on its own.

## 2. An actuator type

1. `ActType` in **Config.h**, fields in `RelayConfig`, JSON in `relayToJson` /
   `relaysFromJson`, pins (and PWM channel count) in `validateConfig`.
2. **Outputs.cpp** — `onValue` / `offValue` / `safeValue` (what "on", "off" and
   the safe state mean), `settle()` (limits, ramp) and `drive()` (the pins).
   PWM goes through `Pwm.h` (works on both Arduino cores, 8 outputs).
3. **Relays::command / apply** — which command fields it takes.
4. **Boards.cpp** `commandToJson` — how a value sent by radio maps to it.
5. **web/schema.js** — `actTypes` + fields; **web/lists.js** `control()` — its
   on-screen control.
6. **Selftest.cpp** — command and rule cases (see the fan / winch / flap tests).

## 3. A control rule

`RuleType` + `ruleToStr` + parsing in Config.cpp, then `ruleWant()` in
Outputs.cpp returns what the rule asks for (or `false` = can't tell → safe
value). Add it to `rules` in web/schema.js.

## 4. A board profile

**Board.cpp** — one `BoardProfile` entry: GPIO masks (`valid`, `output`, `adc1`,
`caution`, `rtc`), default pins (`BoardPins`), fixed parts (`BoardFixed`:
built-in RGB, relays + polarity, opto inputs) and a note people will read. Give the
source of the pin map in a comment. A board that should start pre-set gets a worker
build variant: a `-D` flag checked in `Board::detected()`, a line in the `CM_VARIANT`
block of Config.h and a case in `cmd_worker` in `tools/cm.sh` (see `relay-x8`).
Test: a "defaults valid" case in Selftest.cpp. Main boards stay on the built-in S3
profiles; a one-off worker board needs no code at all: System → Hardware → Custom board.

## 5. A metrics output

**Cloud.cpp** — add the protocol name to `CloudConfig.protocol`, encode a batch
of records (see the Zabbix and InfluxDB encoders and `selftestEncode`), send it
from the net task, return retry on failure (records stay buffered on flash).
Settings fields go to `cloudToJson` / `cloudFromJson` and web/system.js (Data →
Metrics upload).

## 6. A preset

Add `web/presets/<name>.json`: any part of the settings (same shape as
`GET /api/v1/settings`), merged on top of the current ones by
`POST /api/v1/presets/apply`. No firmware change.

## 7. An API address

**Web.cpp** (every board) or **WebMain.cpp** / **WebWorker.cpp** (one firmware) — a handler `void hThing(Ctx& c)` and one line in the route table
(`method, path, scope, handler`); `*` matches one path part (`c.p[0]`). Reply with
`sendJson` / `sendOk` / `sendError`. Document it in `docs/API.md` (and the
changelog table there). Add a mock version to `tools/webtest/mock_server.py` if
the web app uses it.

### Adding API v2 later

v1 stays as it is. A v2 is a second route table served under `/api/v2` that
reuses the handlers which did not change; `GET /info` then lists `api: [1, 2]`.
Remove v1 only in a later minor release, after the web app, the displays and
known integrations have moved (see `docs/DESIGN.md`).

## 8. Tests to run

| Command | What |
|---|---|
| `tools/cm.sh prototest` | board protocol, firmware transfer, CAN, display link, API path matching (PC, seconds) |
| `tools/cm.sh selftest` | the main firmware in the ESP32 emulator (~6 min, run it alone) |
| `tools/cm.sh selftest-worker` | the worker firmware in the emulator: role, running alone, custom board, settings (~2 min) |
| `tools/cm.sh webtest` | web app (main board and worker) + display page in a headless browser against the mock board |
| `tools/cm.sh dispsim` | touch display screens drawn on the PC |
| `tools/cm.sh all` | everything, plus all builds |
