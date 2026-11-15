# CarMetrics: building the firmware from GitHub

This guide takes you from nothing to firmware files you can flash, on a Windows PC.
You don't need to install Arduino or any ESP32 tools: everything runs inside a
ready-made Docker "box" that has exactly the tools the firmware was developed with.

**Time:** about 30 minutes the first time (mostly downloads), 2–5 minutes after that.
**Disk space:** about 6 GB.

---

## What you will get

Since 5.4 there are two firmwares: the **main** board (ESP32-S3 with 8 or 16 MB flash only) and the lean
**worker** (sensors and actuators; it works with a main board or on its own). Ready-made relay boards get a
worker build that starts with their relays set up.

| You want to flash… | Command (see *Build the firmware*) | Files it makes in the `build` folder |
|---|---|---|
| Main board, ESP32-S3 with 8 or 16 MB flash | `tools/cm.sh main` | `carmetrics-main-s3-full-flash.bin`, `carmetrics-main-s3-firmware.bin` |
| Worker, ESP32-S3 (4 MB or more): also keeps charts | `tools/cm.sh worker s3` | `carmetrics-worker-s3-full-flash.bin`, `carmetrics-worker-s3-firmware.bin` |
| Worker, classic ESP32 (DevKit, WROOM, WROVER) | `tools/cm.sh worker esp32` | `carmetrics-worker-esp32-…` |
| Worker, ESP32-C3 (SuperMini, DevKitM) | `tools/cm.sh worker c3` | `carmetrics-worker-c3-…` |
| Worker, ESP32-C6 | `tools/cm.sh worker c6` | `carmetrics-worker-c6-…` |
| ESP32 Relay X8 (8 relays) | `tools/cm.sh worker relay-x8` | `carmetrics-relay-x8-…` |
| KinCony KC868-A4 | `tools/cm.sh worker kc868-a4` | `carmetrics-kc868-a4-…` |
| LilyGO T-Relay | `tools/cm.sh worker t-relay` | `carmetrics-t-relay-…` |
| Touch display JC3248W535 / Cheap Yellow Display | `tools/cm.sh display jc` / `display cyd` | `carmetrics-display-…-full-flash.bin`, `…-firmware.bin` |

`tools/cm.sh worker` alone builds all workers; `tools/cm.sh release` builds everything.

**full-flash** = for USB at address **0x0** (one file). **firmware** = for updating over Wi-Fi; the web app is inside it,
so it is the only file you need. A board refuses the other kind's firmware (a main firmware on a worker or the other way round).

A worker with a board that is not in the list: **System → Hardware → Custom board** (its pins, checked against the chip).

## Install the tools (once)

1. **Git for Windows**: [https://git-scm.com/download/win](https://git-scm.com/download/win), install with the default options.
2. **Docker Desktop**: [https://www.docker.com/products/docker-desktop/](https://www.docker.com/products/docker-desktop/), install, keep *Use WSL 2* ticked, restart the PC when asked.
3. Start **Docker Desktop** and wait until it says *Engine running* (bottom left). It must be running whenever you build.
4. Open **PowerShell** (Start menu → type *PowerShell*) and check that both work (each prints a version number):

```powershell
git --version
docker --version
```

If one says *not recognized*, close PowerShell, open it again, and if needed restart the PC.

## Get the code (once)

In PowerShell:

```powershell
git config --global core.autocrlf input
cd $HOME
git clone -b v5 https://github.com/rassiek/carmetricsbox.git
cd carmetricsbox
```

- The first line stops Git from changing line endings in the build scripts (otherwise you get `$'\r': command not found` later).
- You now have the code in `C:\Users\<you>\carmetricsbox`.

**Already have the folder?** Bring it up to date instead:

```powershell
cd $HOME\carmetricsbox
git checkout v5
git pull
```

## Build the tool box (once, about 15–20 minutes)

Still in the `carmetricsbox` folder:

```powershell
docker build -t carmetrics-dev devenv
```

It downloads Ubuntu, the Arduino command-line tools, both ESP32 cores, the libraries (exact versions) and the emulator. At the end you see `naming to docker.io/library/carmetrics-dev`. You only redo this when the files in the `devenv` folder change (the release notes say so).

Smaller box without the browser used for the web tests (saves ~500 MB): `docker build --build-arg WITH_WEBTEST=0 -t carmetrics-dev devenv`.

## Build the firmware

Always from the `carmetricsbox` folder, with Docker Desktop running:

```powershell
docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh main
```

- `-v "${PWD}:/work"` lets the box see your folder; the results land in **`carmetricsbox\build\`**.
- A good build ends with a line like `Sketch uses 1474669 bytes (75%)` and the list of files.

For another board, replace `main` by its command from the table under *What you will get*, for example (`worker` alone builds all workers, `display` both screens):

```powershell
docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh worker c3
docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh display
```

Open the folder in Explorer: `explorer build`.

## Flash a board

### Over USB (first time, or after a partition change)

1. Plug the board in with a **data** USB cable.
2. Open [https://espressif.github.io/esptool-js/](https://espressif.github.io/esptool-js/) in **Chrome or Edge**.
3. Baud rate 921600 → **Connect** → pick the board's port. If nothing connects: hold **BOOT**, press and release **RST/EN**, release BOOT, try again. No port at all: install the USB driver (CH340 or CP210x, depending on the board).
4. Flash address **0x0**, file: the **`…-full-flash.bin`**.
5. **Don't tick *Erase flash*** (your settings would be lost; the full image leaves them alone).
6. **Program**, wait for *Leaving…*, press **RST**.

**Going to 5.4:** a 5.3 board that becomes a **worker** can take `carmetrics-worker-…-firmware.bin` over Wi-Fi
(settings are kept). One USB flash of the worker's full-flash file gives it the worker layout with bigger firmware slots;
the build prints a *NOTE* when a worker no longer fits the 5.3 layout. C3 and C6 boards are new: USB first.

### Over Wi-Fi (later updates)

- **Main board / worker on its own Wi-Fi:** its web page → **System → Data** → *Firmware update* → its `…-firmware.bin` (one file).
- **A worker from the main board:** **Boards** tab → that board's card → *Firmware* → choose its `…-firmware.bin` (worker or relay board file for its chip).
- **Touch display:** its own page (http://192.168.5.1/ on its Wi-Fi *CarMetrics-Display-xxxx*) → **Firmware**, or from the main board's Boards page.

## Check it worked

- Main board: the header of its web page and **System → Status → Firmware** show the release name.
- Display: the **Setup** tab on the screen shows its firmware.
- The release name comes from `firmware/libraries/CarMetricsCore/src/Config.h` (`FW_VERSION`) and, for the display, `firmware/CarMetricsDisplay/DispConfig.h` (`DISP_VERSION`). If you build your own changes, give them your own name there (e.g. `5.3.0-beta.1-rk1`) so you can tell them apart. `GET /api/v1/info` also shows the build variant.

## Run the tests (optional)

```powershell
docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh all
```

About 25 minutes: protocol tests, all builds, the display screens drawn on the PC (`build\dispsim\*.png`), the main and worker self-tests in the ESP32 emulator, and the web pages (main board and worker) in a headless browser (`build\webtest\*.png`). It ends with `SELFTEST PASSED` and `ALL PASSED`.

## Building a specific release

Releases are commits on the `v5` branch. To build an older one:

```powershell
git log --oneline -15          # find it, e.g. f73529d 5.1.0-beta.10 ...
git checkout f73529d
docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh build   # before 5.4; 5.4 and later: release
git checkout v5                # back to the newest
```

## When something goes wrong

| Message | Fix |
|---|---|
| `error during connect` / `Docker daemon is not running` | Start Docker Desktop, wait for *Engine running* |
| `$'\r': command not found` | Line endings: `git config --global core.autocrlf input`, then `git rm --cached -r . -q` and `git reset --hard` (or clone again) |
| `Unable to find image 'carmetrics-dev'` | *Build the tool box* wasn't done (or the box was named differently) |
| `compile failed` | Scroll up for the first `error:` line. After a `git pull`, build the tool box again if `devenv` changed |
| `no space left on device` | Docker Desktop → Troubleshoot → *Clean / Purge data*, then build the tool box again; or free disk space |
| `invalid reference format` in `-v` | Use PowerShell (not cmd.exe), keep the quotes: `-v "${PWD}:/work"` |
| Flash tool can't connect | BOOT + RST (see *Flash a board*), another cable, close other programs using the port (Arduino IDE serial monitor) |
| Board boots into *SAFE MODE* after flashing | It restarted after crashes: System page → *Last crash*, keep the text, then *Leave safe mode* |

## Without Docker (Linux or WSL Ubuntu 24.04)

```bash
sudo apt install ca-certificates curl git xz-utils bzip2 unzip zip python3 python3-pil python3-serial g++ libslirp0 libsdl2-2.0-0 libglib2.0-0 libgcrypt20
git clone -b v5 https://github.com/rassiek/carmetricsbox.git && cd carmetricsbox
sudo CM_PREFIX=/opt/cm bash devenv/setup-toolchain.sh
tools/cm.sh main
```

## Arduino IDE (if you prefer it)

Possible, but the scripts above set many options for you. Copy `firmware/libraries/CarMetricsLink` and `CarMetricsCore` into your Arduino `libraries` folder and run `tools/build_web.py` once (it writes the web app into `CarMetricsCore/src/WebApp.h`). Main firmware: `firmware/CarMetrics/CarMetrics.ino`, *ESP32S3 Dev Module*, core **3.x**, flash 8 MB, PSRAM *OPI*, partitions from `firmware/partitions-8MB-v52.csv`, build flag `-DCM_V52`. Worker: `firmware/CarMetricsWorker/CarMetricsWorker.ino` (it has its own `partitions.csv`), add `-DCM_WORKER` to the build flags (and `-DCM_WORKER_PLUS` on an S3). Libraries at the versions in `devenv/versions.env`. Display: core **3.x**, copy `Proto.*`, `BoardNet.*`, `Link.h`, `EspNowLink.*` from `firmware/CarMetrics/` next to `CarMetricsDisplay.ino` first; JC3248W535: *ESP32S3 Dev Module*, PSRAM *OPI*, USB CDC on boot *Enabled*.
