# CarMetrics API v1

Status: **draft for review** (implemented in 5.3.0). One API for the bundled web
app, the touch displays and any integration (dashboards, Home Assistant, scripts).

Base address: `http://<board>/api/v1` (e.g. `http://carmetrics.local/api/v1`).
Only the **main board** serves the full API; worker boards serve `info`, `points`,
`system` and `network` for their own setup page.

---

## 1. Conventions

| Topic | Rule |
|---|---|
| Format | JSON in and out (`Content-Type: application/json`), UTF-8 |
| Ids | lower case `a-z 0-9 _`, up to 31 characters, chosen by the user or generated (`coolant_temp`, `fan`) |
| Points | everything with a value: sensors, actuators, system figures. Address `<board>.<id>`: `local.coolant_temp`, `b1.oil_pressure` (`b1` = paired board 1) |
| Status | `ok`, `waiting`, `disabled`, `not_set_up`, `not_found`, `fault`, `link_lost` |
| Alarm level | `ok`, `warn`, `fault`, `crit` |
| Time | `t`: milliseconds since start; `epoch`: Unix seconds when the clock is set, else absent |
| Numbers | SI-ish units as configured per sensor, value as number or `null` when there is none |
| Partial updates | `PATCH` changes only the fields sent |
| Lists | `GET` returns `{"items": [...]}` plus paging (`?limit=&after=`) where lists can be long |

### 1.1 Errors

HTTP status + one shape:

```json
{"error": {"code": "invalid", "message": "Pin 34 is input only", "field": "pin"}}
```

| HTTP | `code` | When |
|---|---|---|
| 400 | `invalid` | a field is wrong (`field` names it) |
| 401 | `auth` | password or key missing / wrong |
| 403 | `scope` | key lacks the scope |
| 404 | `not_found` | unknown id / address |
| 409 | `conflict` | e.g. id already exists, board busy updating |
| 503 | `busy` | low memory or a long job running; retry after `Retry-After` seconds |

### 1.2 Authentication

| Who | How | Scope |
|---|---|---|
| Anyone on the network | nothing | **read**: every `GET` except secrets, mute |
| Web app user | HTTP Basic, user `admin`, the admin password | all |
| Integration | `Authorization: Bearer <key>` | the key's scope: `read`, `control` (actuators, mute, limits), `admin` (everything) |

Settings → *Require a key or password for reading too* turns off anonymous reads.

---

## 2. Endpoints

### 2.1 Device

| Request | Reply / effect |
|---|---|
| `GET /api/v1` | index: `{"api": 1, "links": {...}}` |
| `GET /info` | `appName`, `machineName`, `firmware`, `build`, `variant`, `api: [1]`, `protocol: 1`, `role`, `board` (profile id + name), `chip`, `capabilities: ["actuators","pwm","history","cloud",...]`, `uptimeS`, `epoch` |
| `GET /system` | health: memory, PSRAM, storage, CPU, loop timing, tasks, Wi-Fi, reset history, safe mode |
| `GET /system/crash` / `DELETE` | last crash summary (backtrace) / clear it |
| `GET /system/crash/dump` | core dump file (ELF) |
| `POST /system/reboot` | restart |
| `POST /system/safe-mode/leave` | leave safe mode |
| `POST /system/time` `{"epoch": 1794700000, "tzMinutes": 180}` | set the clock (the web app does this automatically) |

### 2.2 Live values

| Request | Reply |
|---|---|
| `GET /points` | all points, compact: `{"t":..., "epoch":..., "level":"warn", "muted":false, "items":[{"a":"local.coolant_temp","v":92.4,"st":"ok","lvl":"ok"}, ...]}` |
| `GET /points?meta=1` | adds `name`, `label`, `unit`, `decimals`, `min`, `max`, `limits` `{wl,wh,cl,ch}`, `kind` (`sensor` / `actuator` / `system`), `board` |
| `GET /points?a=local.coolant_temp,b1.oil_pressure` | only those |
| `GET /points/{address}` | one point with meta |
| `GET /points/{address}/history?minutes=30&step=5` | `{"step":5,"from":<epoch or t>,"v":[...]}` (nulls where no data) |

### 2.3 Alerts

| Request | Reply / effect |
|---|---|
| `GET /alerts` | active alarms: `[{"a":"local.coolant_temp","lvl":"crit","since":...,"text":"Coolant 111 °C"}]`, `muted`, `mutedUntil` |
| `POST /alerts/mute` `{"minutes": 15}` | mute (0 = unmute). Scope `control` |

### 2.4 Sensors

| Request | Reply / effect |
|---|---|
| `GET /sensors?board=local` | sensor settings of that board (`local` or `b1`…) |
| `GET /sensors/{id}?board=` | one |
| `POST /sensors?board=` | add one (body = settings). Returns it with its id |
| `PATCH /sensors/{id}?board=` | change fields, e.g. `{"limits":{"wh":98}}` or `{"name":"Oil temp"}`. Scope `control` for limits, `admin` otherwise |
| `DELETE /sensors/{id}?board=` | remove |
| `GET /schema/sensors` | every sensor type: fields, units, ranges, choices, presets, which boards support it |
| `POST /sensors/discover?board=` | start a scan; `GET /sensors/discover?board=` → `{"running":false,"i2c":[{"addr":"0x48","driver":"ads1115"}],"onewire":[{"address":"28FF…","tempC":21.5}],"analog":[{"source":"ads1115@0x48/0","volts":1.82}]}` |

### 2.5 Actuators

| Request | Reply / effect |
|---|---|
| `GET /actuators?board=` | settings + state of each: `{"id":"fan","type":"pwm","state":{"duty":40,"by":"rule"}}` |
| `POST`, `PATCH`, `DELETE /actuators/{id}?board=` | as sensors |
| `POST /actuators/{id}/command?board=` | `{"on":true}`, `{"duty":60}`, `{"direction":"reverse","speed":30}`, `{"angle":45}`, `{"auto":true}` (back to its rule); optional `"forMinutes": 10` for a timed override. Scope `control` |
| `GET /schema/actuators`, `GET /schema/rules` | types and their fields |

### 2.6 Boards (other boards and displays)

| Request | Reply / effect |
|---|---|
| `GET /boards` | link status + `found` (unpaired, heard) + `items` (paired: id, name, role, chip, firmware, online, signal, loss %, settings state, display info) |
| `POST /boards` `{"mac":"..","name":"Engine bay"}` | pair (accept) |
| `PATCH /boards/{id}` `{"name":".."}` | rename |
| `DELETE /boards/{id}` | forget |
| `POST /boards/{id}/command` | `{"cmd":"identify"|"reboot"|"mute"|"resend","minutes":15}` |
| `POST /boards/{id}/firmware` `{"source":"self"}` / `PUT /boards/firmware-file` (raw file body) then `{"source":"file"}` / `{"source":"cancel"}` | update another board; progress in `GET /boards/{id}` → `firmware` |

### 2.7 Settings

| Request | Reply / effect |
|---|---|
| `GET /settings` | all sections (passwords blank) |
| `GET` / `PATCH /settings/{section}` | `ui` (names, units, theme, preset), `network`, `hardware` (profile, buses, OLED, RGB, buzzer, ignition, GPS), `alarms` (running detection, turbo timer), `data` (cloud, weather, altitude), `dashboard` (pages and tiles), `oled` (pages) |
| `GET /schema/settings` | fields of every section |
| `GET /backup` / `POST /backup` | export / import everything (JSON) |
| `POST /settings/reset` `{"keep":["network"]}` | back to defaults |
| `GET /presets`, `POST /presets/apply` `{"preset":"boat"}` | presets (`docs/DESIGN.md` §7) |

### 2.8 Hardware and pins

| Request | Reply / effect |
|---|---|
| `GET /pins` | the board profile's pins with abilities, limits and what each is assigned to, plus warnings |
| `POST /pins/{gpio}/test` `{"mode":"blink"}` / `{"mode":"read"}` | bench test: blink an output for 5 s / read an input (reply has the level) |
| `POST /hardware/status-light/test` `{"which":"builtin"|"external"}` | cycle red, green, blue |

### 2.9 Network

| Request | Reply / effect |
|---|---|
| `GET /network` | access point, joined network, IP, signal, saved networks, last scan results, join log |
| `POST /network/scan` | scan now (results in `GET /network` a few seconds later) |
| `POST /network/join` `{"ssid":"..","pass":".."}` | save and join |
| `DELETE /network/saved/{ssid}` | forget a network |

### 2.10 Events, firmware, keys

| Request | Reply / effect |
|---|---|
| `GET /events` / `GET /events/{id}` / `DELETE /events` | black box: list / one event with the minute of data around it / clear |
| `PUT /firmware?target=app` / `?target=webapp` | raw file body (`application/octet-stream`) → installs, restarts |
| `GET /keys`, `POST /keys` `{"name":"Grafana","scope":"read"}` (the key is shown once), `DELETE /keys/{id}` | API keys. Scope `admin` |

---

## 3. Live updates (WebSocket)

`ws://<board>/api/v1/ws`

```json
→ {"type":"auth","key":"…"}                       (optional: control / admin over the socket)
→ {"type":"subscribe","topics":["points","alerts","events","config"],"a":["local.coolant_temp"]}
← {"type":"points","t":123456,"v":{"local.coolant_temp":[92.4,"ok","ok"]}}   (every second, changed points only after the first)
← {"type":"alerts","level":"crit","muted":false,"items":[...]}               (on change)
← {"type":"config","rev":"a1b2c3d4"}                                          (settings changed: reload what you show)
→ {"type":"command","id":7,"path":"/actuators/fan/command","body":{"duty":60}}
← {"type":"ack","id":7,"ok":true}
```

Server-Sent Events at `/api/v1/events-stream` carry the same `points` / `alerts` /
`config` messages for clients without WebSocket.

---

## 4. Touch displays

Over ESP-NOW nothing changes (catalog as settings, values each second, commands).
Over Wi-Fi a display uses `GET /points?meta=1`, `GET /actuators`, the WebSocket
and `POST /actuators/{id}/command`, `POST /alerts/mute`, `PATCH /sensors/{id}` with
a key of scope `control`. The old `/api/v2/display/*` addresses are removed.

---

## 5. Examples

```sh
# all values with names and units
curl http://carmetrics.local/api/v1/points?meta=1

# switch the fan to 60 % for 10 minutes (integration key with "control" scope)
curl -X POST -H "Authorization: Bearer cm_8f3…" -H "Content-Type: application/json" \
     -d '{"duty":60,"forMinutes":10}' http://carmetrics.local/api/v1/actuators/fan/command

# change the coolant warning limit
curl -X PATCH -u admin:<password> -H "Content-Type: application/json" \
     -d '{"limits":{"wh":98}}' http://carmetrics.local/api/v1/sensors/coolant_temp
```

---

## 6. Changes from the 5.1 / 5.2 API

| Old | v1 |
|---|---|
| `/api/state`, `/api/v2/points` | `/points` |
| `/api/config`, `/api/config/sensors`, `/api/config/relays`, `/api/config/system`… | `/sensors`, `/actuators`, `/settings/{section}` |
| `/api/relays/control`, `/api/v2/command` | `/actuators/{id}/command`, `/alerts/mute` |
| `/api/v2/boards/*` | `/boards/*` |
| `/api/v2/display/*` | `/points?meta=1`, WebSocket, `/actuators/{id}/command` |
| `/api/v2/wifi`, `/api/v2/wifi/search` | `/network`, `/network/scan`, `/network/join` |
| `/api/blackbox*` | `/events*` |
| `/api/update` (multipart) | `PUT /firmware` (raw body) |
| `/ws`, `/events` | `/api/v1/ws`, `/api/v1/events-stream` |

The old addresses are removed in 5.3.0; the bundled web app and the touch display
firmware move in the same release.

## 7. Changelog

| API | Firmware | Change |
|---|---|---|
| v1 | 5.3.0 | first documented API |
