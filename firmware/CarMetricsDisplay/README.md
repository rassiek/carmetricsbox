# CarMetrics Display

Touch screen for the CarMetrics main board: live values as dials, bars,
numbers or the OLED-style marker; relay buttons; mute; alarm banner; alarm
limits changed by a long press. Its pages are laid out on its own web page,
with a live preview.

| Board | Chip | Screen | Files |
|---|---|---|---|
| **JC3248W535** (Guition, buy the version with the case) | ESP32-S3, 16 MB flash, 8 MB PSRAM | 3.5" 320×480 IPS, AXS15231B (QSPI), capacitive touch | `carmetrics-display-jc3248w535-*.bin` |
| **Cheap Yellow Display** ESP32-2432S028R | ESP32, 4 MB, no PSRAM | 2.8" 320×240 ILI9341 (ST7789 on the 2-USB version), resistive touch | `carmetrics-display-cyd-*.bin` |

## First start

1. Flash `…-full-flash.bin` over USB at address **0x0** (https://espressif.github.io/esptool-js/, Chrome / Edge).
   - JC3248W535: hold **BOOT**, press **RST**, release BOOT if the port doesn't show up.
2. The screen shows *Waiting for the main board…* and a **Setup** tab.
3. On the **main board** (5.1.0-beta.11 or newer): **Boards** page → the display appears under *Found* (role *display*) → name it → **Accept**.
   Within a few seconds the display gets the list of everything the main board measures and switches, and shows it.
4. Its layout editor: Wi-Fi **CarMetrics-Display-xxxx** (password `carmetrics`) → http://192.168.5.1/.
   Once it has joined the main board's own Wi-Fi it is also on the main board's Boards page as a link (192.168.4.x).

## How it gets its data

- **ESP-NOW** (default): like the sensor / relay boards. The main board sends the list ("catalog") as the display's settings, then every value once a second. Relay, mute and limit commands go back the same way. Pairing, encryption (Forget + Accept after turning it on), firmware updates from the Boards page: as for the other boards.
- **Wi-Fi fallback**: the display also joins the main board's access point (`CarMetrics` / `carmetrics`, change in the display's Settings). When ESP-NOW is silent it reads `/api/v2/display/values` once a second; relays and limits then need the main board's admin password (display Settings).
- Nothing for 6 s: values show **LINK**, the status bar **NO LINK**.

## Using it

| Action | What happens |
|---|---|
| Swipe left / right, or tap a tab | next / previous page (your pages, *Relays*, *Setup*) |
| Tap a relay tile | switches it (outlined until the main board confirms) |
| Long press a value | its alarm limits: warn / crit, low / high, `+` `-` and on / off; **Save** sends only what changed |
| **MUTE** (top right) | mutes the main board 15 min (tap again to unmute) |
| Red banner | something is critical: tap = mute 15 min |
| Setup tab | addresses, link, brightness, rotate, restart |

## Web page (http://192.168.5.1/)

- **Layout**: pages (up to 8, name, grid of 1–4 columns × 1–6 rows), tiles (what, how: number / bar / dial / marker / relay, label up to 5 characters, size in grid cells). The preview is drawn at the screen's real size with live values. *Automatic layout* goes back to the generated one (dials first, then bars).
- **Settings**: name, brightness, rotation, dimming, panel type (CYD), Wi-Fi fallback, passwords, touch calibration.
- **Status**: link details, memory. **Identify** flashes the screen.
- **Firmware**: upload `carmetrics-display-…-firmware.bin` (rollback if it doesn't run for a minute).

## Touch calibration

- **CYD**: Settings → Touch shows the raw position while you touch. Touch the four corners; put the smallest / largest x and y in min / max. Taps wrong way round: try the flips / swap.
- **JC3248W535**: no calibration; if taps land mirrored, tick flip x / flip y (or swap).

## Building

`tools/cm.sh display` (both boards) or `tools/cm.sh display jc` / `cyd`. The sketch shares
`Proto.*`, `BoardNet.*`, `Link.h`, `EspNowLink.*` with `firmware/CarMetrics`; the script copies them
next to it in `build/sketchDisplay/`. Needs core 3.x and the GFX Library for Arduino 1.6.5 (pinned in `devenv/versions.env`).
`tools/cm.sh dispsim` draws the screens on a PC (`build/dispsim/*.png`) with the same code.

## Not tested on the hardware yet

Written and compiled for both boards, screens checked in the PC simulator; the panel / touch drivers
follow the GFX library's own settings for these boards. On first power-up check: picture appears,
colours right (CYD: else Settings → Panel), taps land where touched (else calibration / flips).
