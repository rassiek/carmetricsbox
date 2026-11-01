# CarMetrics: context for a new session

Give this file to Claude at the start of a new session (or say: *"Clone github.com/rassiek/carmetricsbox, branch v5, and read docs/CONTEXT.md, docs/v5-status.md and docs/v5-phase3-plan.md"*). It holds what was built, how, what has only been tested on a PC / emulator, and how to test and debug on the real boards.

Last updated: 1 Nov 2026, releases **5.1.0-beta.9** and **5.2.0-beta.1**.

---

## 1. The project

Engine monitor for older vehicles without OBD-II (Land Rover 300Tdi). ESP32 reads temperatures (DS18B20, EGT thermocouple), pressures (0.5–4.5 V senders), voltages, RPM, GPS, etc.; shows them on a phone (web app over the board's own Wi-Fi), on an OLED, sounds alarms, switches relays, uploads to a cloud. v5 adds board health, safe mode, vehicle features and **several boards** working together.

| Item | Where |
|---|---|
| Repository | github.com/rassiek/carmetricsbox, branch **v5** |
| Firmware (Arduino sketch) | `firmware/CarMetrics/` |
| Web app (served by the board) | `web/` (gzipped into `firmware/CarMetrics/data/www/` and `littlefs.bin`) |
| Docs | `docs/v5-design-and-backlog.md` (design), `docs/v5-status.md` (status + history), `docs/v5-phase3-plan.md` (multi-board), this file |
| Tools | `tools/cm.sh` (build / test), `tools/build_web.py`, `tools/webtest/` (mock board + browser test), `tools/prototest/` (PC tests), `devenv/` (toolchain) |

## 2. How work gets to GitHub (important)

Claude sessions so far could **not push** (no write access). The routine:

1. Claude commits locally, builds, and sends **release files** + a **`.patch`** named after the release.
2. You save them in a folder per release, e.g. `Downloads\V5\5.1.0-beta.9\`.
3. In your clone: `git am "<full path>\<file>.patch"` then `git push`.
4. If `git am` says *patch does not apply* / *already exists in index*: the patch is probably already on GitHub. Run `git am --abort`, check `git log --oneline -3`, and only apply patches that are newer.
5. PowerShell: give full paths; `*` and `...` are not expanded inside quotes.

To let a session push instead: install the Claude GitHub App on the repo (github.com/apps/claude/installations/new) and start the session with the repo selected.

## 3. Releases and which file goes where

The release label is `FW_RELEASE_51` / `FW_RELEASE_52` in `firmware/CarMetrics/Config.h`. It is shown in the web app header, on System → Status (firmware + web app) and on the OLED start screen. A **red banner** appears when the firmware and the web app come from different releases (e.g. only `littlefs.bin` was uploaded).

| Line | Boards | Files |
|---|---|---|
| **5.1** | Every board: classic ESP32 (4 MB) and ESP32-S3; any role (main / sensor / relay) | `carmetrics-5.1.x-full-flash.bin` (USB, 0x0), `-firmware.bin` + `-littlefs.bin` (Wi-Fi) |
| **5.2** | ESP32-S3 with **8 MB+ flash and PSRAM** (N8R8, N16R8); meant for the main board; any role | `carmetrics-5.2.x-full-flash.bin` (USB once: new partition table), then Wi-Fi updates with its `-firmware.bin` / `-littlefs.bin` |

- 5.1 extra boards work with a 5.2 main board (same protocol, `Proto::VERSION` 1, frames ≤ 250 B). Keep it that way: new features only as optional message fields or new message types that old boards ignore.
- USB flashing: https://espressif.github.io/esptool-js/ → address **0x0**, **don't tick Erase flash** (settings live in the `cfg` partition, which the full image doesn't touch).
- Wi-Fi updates: System → Firmware update: *Firmware* first, then *Web app*.
- Classic ESP32: build with **core 2.0.17** (`tools/cm.sh build`, ~1.47 MB of 1.66 MB). Core 3.x builds for the classic ESP32 are 1.63 MB: only ~70 KB left.

## 4. Toolchain and commands

`devenv/setup-toolchain.sh` (or the Dockerfile) installs arduino-cli, ESP32 core 2.0.17 and 3.3.12, libraries, QEMU. In a fresh Claude cloud session also: `pip install pyserial --break-system-packages` and `apt-get install -y libslirp0 libsdl2-2.0-0 libgcrypt20` (QEMU), Node + Playwright are preinstalled.

| Command | What |
|---|---|
| `tools/cm.sh build` | 5.1 classic ESP32 (core 2.0.17): `build/carmetrics-firmware.bin`, `carmetrics-full-flash.bin`, `littlefs.bin` |
| `tools/cm.sh build3 esp32s3` / `esp32` | compile with core 3.x (5.1 S3 image: `carmetrics-s3-*.bin`) |
| `tools/cm.sh build52` | 5.2 S3 8 MB: `build/carmetrics-52-*.bin` |
| `tools/cm.sh prototest` | PC tests of the board-to-board protocol, pairing, settings, firmware transfer, CAN framing, half floats (46 checks, sanitizers) |
| `tools/cm.sh selftest` | firmware self-test in the ESP32 emulator (64 checks, ~6 min; QEMU has no radio). QEMU's slow flash occasionally trips a watchdog mid-run and the later checks then fail: run it again on its own, without other builds in parallel |
| `tools/cm.sh webtest` | headless browser test of every page against `tools/webtest/mock_server.py` |
| `tools/cm.sh all` | all of the above (5.1) |

Release routine used so far: bump `FW_RELEASE_51` (and/or `_52`), run tests, commit, build, copy the three files renamed `carmetrics-<release>-*.bin`, `git format-patch` → `<release>.patch`.

## 5. Firmware map

| File | Role |
|---|---|
| `CarMetrics.ino` | setup / loop, config apply (`applyConfig`: settings that need a reboot) |
| `Config.*` | all settings, JSON in/out, validation (`validateConfig`), NVS storage (`cfg` partition; slots current / previous / good) |
| `State.*` | shared runtime state + `StateLock` (recursive mutex); `requestConfigApply` hands new settings to `loop()` |
| `Sensors.*`, `Outputs.*` (alerts, relays), `Display.*` (OLED), `History.*` (charts, half floats, chunked reply), `BlackBox.*`, `Cloud.*` (weather, upload, net task), `Gps.*`, `Vehicle.*` (ignition, sleep, turbo timer) | phase 0/1 features |
| `Health.*` | board metrics, safe mode, rollback, **cached storage stats**, **crash report from the core dump** |
| `WifiMgr.*` | access point + joining saved networks (join log on System page); extra boards never join |
| `Web.*` | HTTP API, WebSocket / SSE live data |
| `Proto.*` | board-to-board message format (plain C++) |
| `Link.h` | link interface + loopback (tests) |
| `BoardNet.*` | exchange logic: `MainSession` / `NodeSession` (pairing, heartbeats, settings push, values, commands, firmware transfer) |
| `EspNowLink.*` | ESP-NOW link (queue from radio callback, MAC learning, encryption keys) |
| `CanFrag.*`, `CanLink.*` | CAN framing + TWAI driver link; `MultiLink` (ESP-NOW + CAN) |
| `Boards.*` | glue: roles, channel search, NVS (`boards` / `node` namespaces), mirrors of remote sensors, firmware images, web functions |
| `Half.h` | 16-bit floats for chart history |
| `Selftest.cpp` | emulator self-test (only with `-DCM_EMULATOR_TEST -DCM_EMU_SELFTEST`) |

## 6. Multi-board: how it works

**Roles** (System → Settings → *Board role*; change = reboot): main (default) / sensor / relay / sensor + relay. One firmware for all.

**Link**: ESP-NOW by default. Main board on its access-point interface (follows the joined network's channel). Extra boards use the station interface, **never join Wi-Fi**, keep their own access point, and **search channels 1–13** (300 ms per channel, saying hello) until they hear the main board (beacon every 1 s, or immediate answer to a hello). CAN is option 2 (section 6.6).

**Pairing**: an unpaired extra board says hello every 2 s → main board's **Boards** page lists it under *Found* → name it, **Accept** → `ASSIGN` broadcast (by MAC) gives it id 1..14 → both store it (main: `boards/list`; board: `node/id`). A newly accepted board gets an empty sensor/relay list (its demo defaults would raise alarms).

**Settings ("share")**: each extra board's sensors and relays are edited on the main board's Sensors / Relays pages with the **board selector** (`?board=N`), kept in the main board's NVS (`boards/s<id>`), and sent in 200-byte chunks with revision = CRC-32 of the JSON (identical settings never re-applied), resent every 2 s until the board answers. The board validates (pins for *its* chip) and answers accepted / refused + reason (shown on the Boards page).

**Live values**: every second, up to 39 values per message, tagged with the settings revision. On the main board each remote sensor is mirrored as a `remote` sensor `b<id>_<sensorid>`, named "<board> <sensor>", same thresholds → appears on Gauges, Charts, alarms (main buzzer too), black box, cloud upload, OLED (add it on the Layout page). Board out of reach → **LINK LOST** (fault alarm if that sensor has fault alarms).

**Link loss**: no message for 5 s = lost (a 2–3 s Wi-Fi search on the main board must not trip it). Extra board puts each relay in its **safe state** (off / on / keep, per relay, Relays page "If link lost") and keeps its own alarms running.

**Commands**: relay on/off, mute, identify, reboot; resent with the same sequence number, so a relay acts exactly once.

**6.1 Encryption** (System → Settings, off by default in the 5.1 betas): on Accept the main board makes a random 16-byte key, sends it inside `ASSIGN` (optional trailing field), both add each other as encrypted ESP-NOW peers (PMK "CarMetrics-PMK-1" + per-board LMK). Broadcasts (search, hello, pairing) stay unencrypted. Changing it needs Forget + Accept. Weakness: the key travels in the clear once, at pairing.

**6.2 Firmware for other boards** (Boards page → *Firmware*): this board's own firmware (same chip) or an uploaded `-firmware.bin` (stored in the main board's **spare firmware slot**, replacing its own rollback image). `FW_OFFER` → `FW_DATA` (200 B) → `FW_ACK` (next offset / done / refused). The board writes with `esp_ota_*` (erase as it goes), checks magic + chip id + CRC, restarts; rollback after a crash within 60 s. Expect 1–3 minutes for 1.4 MB.

**6.3 Protocol** (`Proto.h`): frame = `'C'`, version 1, type, src, dst, seq (u16), len, payload, CRC-16. Types: 1 hello, 2 heartbeat, 3 config chunk, 4 config ack, 5 points, 6 command, 7 ack, 8 assign, 9 fw offer, 10 fw data, 11 fw ack. Board ids: 0 main, 1–14 extra, 15 unpaired, 255 everyone.

**6.4 Limits**: 3 extra boards (5.1), 6 (5.2). ESP-NOW v1 frames (≤ 250 B) so classic ESP32 on core 2.0.17 can take part.

**6.5 Channel coupling**: when the main board joins a network on another channel, ESP-NOW moves with it; extra boards lose it for a moment and search again. A phone connected to an extra board's own access point may drop at that moment.

**6.6 CAN**: System → Settings: *CAN link*, TX → transceiver D, RX → transceiver R, speed (500 kbit/s default, same on all boards), 120 Ω at both ends. Messages split into 8-byte frames (CAN id 0x100 + sender id from the MAC; 42 frames for 250 B). Main board: ESP-NOW + CAN together. Extra board with CAN on: CAN only (no channel search, no encryption). Bus-off is recovered automatically.

## 7. What has been tested, and how

| Area | PC test | Emulator | Mock web test | Real hardware |
|---|---|---|---|---|
| Wi-Fi joining fix, join log | – | – | yes | **yes** (user, 31 Oct: works) |
| Clock / weather fixes | – | yes | yes | **yes** (clock shown) |
| Storage tile, crash report, loop-stall fix, OOM crash fix | – | yes | yes | partly (crash report showed and was decoded) |
| OLED layout editor, marker style | – | yes (frames rendered) | yes | not yet |
| Protocol, pairing, settings push, values, commands, link loss | **yes** (loss 25–40 %) | – | – | **no** |
| ESP-NOW link, channel search | – | – | – | **no** |
| Remote sensors on gauges / charts / alarms | mirrors built | yes | yes | **no** |
| Encryption | key hand-over | – | yes | **no** (off by default) |
| Firmware for other boards | **yes** (15 % loss) | – | yes | **no** |
| CAN | framing | – | – | **no** |
| 5.2 S3 build | – | – (S3 not emulated) | – | **no** |
| Phase 1 hardware (ignition, RPM, GPS, INA226, MPU-6050, BME280, OTA rollback) | – | parts | – | **no** |

## 8. Bench test plan when the boards arrive

Call the boards **A** (main, the current one) and **B** (new). Flash both with the same 5.1 full image. Keep a serial monitor (115200 baud) on B, ideally on A too: the log lines are prefixed `[wifi]`, `[boards]`, `[espnow]`, `[can]`, `[crash]`, `[cfg]`, `[boot]`, `[ota]`.

**8.1 Pairing over ESP-NOW**
1. B: connect to its own `CarMetrics` Wi-Fi, System → Settings → role **Sensor + relay board**, save (reboots).
   Expect B serial: `[espnow] started on the station interface`, then channel hopping.
2. A: Boards page. Expect B under *Found* within ~5 s (name `carmetrics`, role, chip, signal).
   - Not listed: is A's role *main*? Is the B log showing `[espnow] started`? Put both close together. Check A's `/api/v2/boards` → `link.rx` increasing?
3. Name it (e.g. *Engine bay*), **Accept**. Expect toast *Paired as board 1*, card *online*, B serial `paired as board 1`.
   - Stays *found*: the `ASSIGN` broadcast was lost; Accept again (B also re-asks by saying hello; A re-sends `ASSIGN` when it hears a paired board with the wrong id).
4. Card → **Sensors…**: add one DS18B20 (pin 4 on B) or an analog sensor on a B pin, Save. Expect on the card *settings up to date*; B serial `new settings from the main board`.
   - *settings REFUSED: …*: the message is B's validation (pin not valid on B's chip, etc.).
5. Expect the value on the card, then on A's **Gauges** and **Charts** as "Engine bay …".
6. **Relays…** on the card: add a relay on a B pin, *If link lost* = Off, save; switch it from the card.
7. **Link loss**: power A off. After ~5 s B serial `link to the main board LOST: relays to their safe state`; relay goes to Off; B keeps its own alarms. Power A on: B finds it again (searching channels), card *online*.
8. **Channel change**: make A join another network (or hotspot) on a different channel. B should drop for a few seconds and come back.
9. **Wi-Fi search gap**: with A not joined to any network (it searches every ≤ 60 s), B must **not** report link loss.

**8.2 Encryption**: A System → Settings → *encrypt the link…* on, save. Boards → **Forget** B, wait for it under *Found*, **Accept**. Card shows *encrypted*; B's Boards page *Encryption on*. Repeat 8.1 steps 5–7.
- Pairs but goes offline right after: keys not applied on one side. Check both serial logs; Forget, turn encryption off, Accept again to confirm the rest works, then report.

**8.3 Firmware for another board**: on A's card → *Send this board's (5.1.x)* (same chip). Expect a progress bar, 1–3 min, then *sent*; B restarts and its card shows the new firmware. Also try *Send file* with a different release's `-firmware.bin`.
- Stalls: watch `fw` in `/api/v2/boards`; distance / interference; it gives up after 20 s without progress.
- After an update A's own rollback image is gone (if a file was used); that's expected.

**8.4 CAN** (two SN65HVD230 modules, twisted pair, 120 Ω at each end; transceiver VCC 3.3 V): on both boards System → Settings → *CAN link*, TX/RX pins, 500 kbit/s. B then uses only CAN. Expect the Boards page link pill *ESP-NOW + CAN, CAN running* on A; pairing and values as in 8.1.
- `bus off (recovering)`: wiring (CANH/CANL swapped), no termination, only one board powered, or different speeds.

**8.5 5.2 on an ESP32-S3 N8R8/N16R8** (as main): flash `carmetrics-5.2.x-full-flash.bin` over USB. Header shows 5.2; health panel: PSRAM, *charts … in PSRAM*, Firmware slot 3072 KB. Pair a 5.1 board B with it (8.1).

**8.6 Phase 1 hardware** (single board): ignition on GPIO 25 via opto (sleep after N min, wake on key), RPM from the W terminal (calibrate at idle), GPS on a free UART, INA226 / MPU-6050 / BME280, OTA rollback (install a build that crashes at boot → previous firmware comes back).

## 9. Debugging help

- **APIs**: `/api/info`, `/api/v2/system` (health, storage, crash), `/api/v2/wifi` (join log), `/api/v2/boards` (link counters, found, boards, `fw` progress, CAN state), `/api/v2/crash/dump` (core dump ELF).
- **Crash**: System → *Last crash* shows the backtrace. Send it together with the release; decode with the matching build's ELF: `xtensa-esp32-elf-addr2line -pfiaC -e build/fw/CarMetrics.ino.elf <addresses>`. If the ELF is gone, rebuild the exact commit: same code gives the same addresses (only the build date differs).
- **Memory**: health panel "lowest" and "largest block"; replies needing more than the largest block now answer *Low memory, try again* instead of crashing. Many open tabs / phones = many live-data clients (capped 6 WebSocket + 3 event-stream).
- **Loop time max** > ~100 ms: something holds the state lock; look at what was added recently in web handlers.
- **Settings on an extra board refused**: validation runs on that board with its own board profile (pins).
- **Remote values "waiting"**: the board hasn't accepted the current settings revision yet (card shows *sending settings…* / *REFUSED*).

## 10. Known limitations and next work

- **Touch display (phase 2)**: not started. JC3248W535 (ESP32-S3, 320×480 QSPI AXS15231B, capacitive touch) as a display-role board over ESP-NOW using this protocol; needs LVGL + the AXS15231B driver; buy the version with the case.
- ICM-42688 IMU driver (MPU-6050 works).
- Encryption key sent in the clear once at pairing.
- No CAN encryption (physical access needed).
- Classic ESP32 on core 3.x is close to the slot size; stay on 2.0.17 for it.
- History is RAM-only (lost at reboot).
- `scanWhileApInUse` and the extra boards' always-on access point share the radio with ESP-NOW.

## 11. Decisions made (so they aren't asked again)

| Decision | Choice |
|---|---|
| Arduino cores | support 2.0.17 and 3.x |
| Order | phase 3 (multi-board) before phase 2 (touch display) |
| Default board link | ESP-NOW; CAN as option 2 |
| First extra-board roles | sensor and relay together, one firmware |
| Wi-Fi searching on the main board with boards paired | allowed; link loss only after 5 s |
| Extra boards' access point | always on |
| Releases | 5.1 all boards (4 MB OK); 5.2 S3 main board, compatible with 5.1 boards |
| OLED labels | 3 characters: CLT coolant, IT intake, GBT gearbox, TCT transfer case, CT cabin, EGT, BST, OIL; marker style = original look |
| Ignition sense pin | GPIO 25 (RTC, internal pull-up) via opto-coupler, active low |
| Encryption | built, off by default until bench-tested |

## 12. Hardware notes

Parts list: see the chat of 31 Oct 2026 (AliExpress search terms); key items for the next tests: second ESP32 (ESP32-S3 N16R8 recommended, u.FL antenna version for the engine bay), 2× SN65HVD230 for CAN, JC3248W535 for the display. Sensor wiring: README.md "Wiring" (ESP32 is 3.3 V; dividers, pull-downs, TVS on every vehicle input).
