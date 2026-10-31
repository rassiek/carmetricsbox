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
| 1 | #17 | Protocol: envelope, hello/announce, heartbeat, config push (fragmented), point stream, commands, acks. Transport interface with a loopback link | Yes (emulator) |
| 2 | #20, #21 | Roles in one firmware: **sensor board** (reads + local alarms, OLED/buzzer), **relay board** (safe state off/on/hold on link loss). Main board registers remote points `board.point` | Yes (loopback) |
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
