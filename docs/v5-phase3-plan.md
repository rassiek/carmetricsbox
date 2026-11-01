# CarMetrics v5 – Phase 3 (multi-board) plan

31 Oct 2026. Agreed before building; replaces the CAN-first order in the [design](v5-design-and-backlog.md).

## Decisions

| Question | Answer |
|---|---|
| Default link | **ESP-NOW**; CAN (SN65HVD230) is the optional second link |
| First board roles | **Sensor board and relay board together** (one firmware, role chosen in settings) |
| Wi-Fi searching on the main board while boards are paired | **Allowed** (≤ 60 s back-off); "link lost" only after ~5 s so a 2–3 s search doesn't trip alarms or relay safe states |
| Extra boards' own access point | **Always on**, with a small status/setup page |
| Hardware for testing | One spare classic ESP32 now (the other is in the car); emulator with a simulated link for protocol and roles |

## Build order

| Step | Backlog | What | Testable without a 2nd board |
|---|---|---|---|
| 1 | #17 | Protocol: envelope, hello/announce, heartbeat, config push (fragmented), point stream, commands, acks. Transport interface with a loopback link. **Message format done** (`Proto.*`, `Link.h`); the exchange logic (who sends what when) comes with step 2 | Yes (PC test + emulator) |
| 2 | #20, #21 | Roles in one firmware: **sensor board** (reads + local alarms, OLED/buzzer), **relay board** (safe state off/on/hold on link loss). Main board registers remote points `board.point`. **Exchange logic done** (`BoardNet.*`: pairing, heartbeats, config push, points, commands); firmware wiring next | Yes (loopback) |
| 3 | #19 | ESP-NOW link: channel hunt 1–13, encrypted pairing (accept on the main board), heartbeat 1 s, lost after 5 s, re-hunt | Needs 2 boards |
| 4 | #22 | Boards page: list, pair/accept, per-board settings, link quality; remote points in gauges, charts, alarms, black box, cloud | Mock server + emulator |
| 5 | #18 | CAN link (TWAI + SN65HVD230), same messages, segmented into 8-byte frames | Needs 2 boards + transceivers |
| 6 | #23 | Firmware updates for other boards from the main board (same chip: send its own image) | Needs 2 boards |

## Design notes

- **Message size:** every message fits ESP-NOW v1 (250 bytes), so classic ESP32s on core 2.0.17 can take part. Larger payloads (config) are split and reassembled with a sequence number and CRC.
- **Channel:** ESP-NOW has to share the main board's radio channel, which follows the joined network. Extra boards hunt across channels and re-hunt after a link loss. Because their access point is always on, it moves with that channel too: a phone connected to an extra board's AP may drop when the main board changes network.
- **Main board interface:** ESP-NOW on the main board's AP interface (always up), so it works whether or not the station side is joined.
- **Ownership:** the main board owns the whole configuration and pushes each board its share; boards keep it in their own NVS and run without the main board.
- **Failure behaviour:** as in the design: sensor boards alarm locally; relay boards go to their safe state after the timeout; points show "link lost" and a fault alert after 3 missed heartbeats (with the 5 s rule above).
- **Limits:** up to 3 extra boards in this phase (ESP-NOW allows more; encrypted peers are limited, ~6 in AP+STA mode).

## Protocol (step 1, built)

`firmware/CarMetrics/Proto.h` / `.cpp`, `Link.h`. Plain C++, tested on a PC with `tools/cm.sh prototest` (address and undefined-behaviour sanitizers) and once on the chip in the emulator self-test.

| Message | Direction | Content | Size |
|---|---|---|---|
| hello | board → all | role, chip, channel, MAC, firmware, name, config revision, point count | 45–64 B |
| heartbeat | both, 1 s | uptime, config revision, worst alarm level, flags (safe mode, relays safe, needs config, muted), RSSI, point count | 22 B |
| config | main → board | the board's share of the settings (JSON), 200 B chunks, CRC-32 over the whole; any order, duplicates fine, newer revision restarts | ≤ 223 B per chunk, up to 32 KB |
| config-ack | board → main | accepted / bad CRC / invalid (with reason) / too big | ≤ 64 B |
| points | board → main | up to 39 values per frame: value, status, alarm level, by index; tagged with the config revision | ≤ 248 B |
| command | main → board | relay on/off/blink, mute, identify, reboot | 17 B |
| ack | board → main | answer to a command | 13 B |
| assign | main → all | pairing: "the board with this MAC is now board N" (or unpaired again) | 18–41 B |

Frames: `'C'`, version, type, from, to, sequence number, length, payload, CRC-16. Damaged, foreign, truncated or other-version frames are dropped; a per-board sequence tracker drops duplicates and counts lost frames for the link-quality figure.

## Exchange logic (step 2, first half, built)

`BoardNet.h` / `.cpp`: `NodeSession` on an extra board, `MainSession` on the main board, both on any `Link`. Tested on a PC: a main board and a sensor + relay board over a loopback link with simulated time.

| Behaviour | Tested |
|---|---|
| Unpaired board says hello every 2 s; main board lists it as "found"; accept → `ASSIGN` (by MAC) gives it id 1; stored on both sides | yes |
| Settings pushed in chunks, resent every 2 s until acknowledged; revision = CRC-32 of the settings, so identical settings are never re-applied (also after a main board restart) | 25 % and 40 % random loss, 8 seeds |
| Refused settings come back with the board's reason; the board keeps its last good settings | yes |
| Live values + local alarm level every second, ignored if they belong to another settings revision | yes |
| Relay command acts exactly once although answers are lost (resent with the same sequence number; the board answers again without acting) | yes |
| 3 s silence (Wi-Fi search) = nothing happens; 7 s = link lost on both sides (relays to safe state), then automatic recovery | yes |
| Main board restart: paired boards restored from storage, back online without re-pairing | yes |
| Forget: board returns to unpaired and is offered again | yes |

## Firmware wiring, first visible round (built, needs two boards to try)

| Piece | Where | What it does |
|---|---|---|
| Role setting | System → Settings → *Board role* (`sys.role`: main / sensor / relay / sensor_relay); change = reboot | Picks main board or extra board behaviour |
| ESP-NOW link | `EspNowLink.*` | Main board on its AP interface; extra boards on the station interface. Frames queued from the radio callback, handled in `loop()`. Learns each board's MAC from its frames; unicast to known boards, broadcast otherwise. **Not encrypted yet.** |
| Channel search | `Boards.cpp` | Extra board hops channels 1–13 every 300 ms, saying hello, until it hears the main board (its 1 s beacon or the immediate answer to a hello); searches again when out of range for 5 s |
| Extra board | `Boards.cpp`, `WifiMgr` | Never joins a Wi-Fi network (keeps its AP); runs the sensors / relays it is sent, with its own alarms; sends values every second; relays to their **safe state** (off / on / keep, per relay) when the link is lost |
| Main board | `Boards.cpp` | Paired boards and each board's settings ("share": its sensors + relays) kept in NVS (`boards` namespace); shares sent with revision = CRC-32; a newly accepted board gets an empty share (so its demo defaults don't raise alarms) |
| Boards page | `boards.html` | Found boards with name + **Accept**; paired boards: online, signal both ways, lost messages, alarm level, settings accepted / refused (with the board's reason), live values, relay buttons, **Forget** |
| Board selector | Sensors and Relays pages (`?board=N`) | Edit another board's sensors / relays on the main board; saved there and sent. Pins are checked by the board itself |
| API | `GET /api/v2/boards`, `POST /api/v2/boards/accept {mac,name}`, `/forget {id}`, `GET/POST /api/v2/boards/config`, `POST /api/v2/boards/relay {id,index,on}` | |

Not yet: remote values on the Gauges / Charts / alarms / black box / cloud (next round), encryption, CAN, remote firmware updates.
