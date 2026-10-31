# CarMetrics dev environment (Docker)

This is the toolchain used to develop and test CarMetrics:

| Tool | Version |
|---|---|
| Base image | Ubuntu 24.04 |
| arduino-cli | 1.1.1 |
| ESP32 Arduino core | 2.0.17 (ESP32 builds, emulator) and 3.3.12 (ESP32 and ESP32-S3 builds) |
| Firmware libraries | exact commits in `versions.env` |
| Emulator | Espressif's ESP32 QEMU 9.0.0 (2024-06-06) |
| Web tests (optional) | Node 22 + Playwright/Chromium |

## What you can do with it

| Command | What it does |
|---|---|
| `tools/cm.sh build` | Builds the firmware (classic ESP32, core 2.0.17), web app and flash images into `build/` |
| `tools/cm.sh build3 esp32s3` | Compiles with core 3.x for the ESP32-S3 (`build/carmetrics-s3-firmware.bin`); `build3 esp32` for a classic ESP32 |
| `tools/cm.sh selftest` | Boots the firmware in the **ESP32 emulator** and runs the built-in self-test (about 60 checks). Writes the log and OLED screenshots to `build/emu/` |
| `tools/cm.sh emu 60` | Boots the firmware in the emulator for 60 s and shows the serial log |
| `tools/cm.sh webtest` | Runs a headless Chrome test of every web page against a mock ESP32. Screenshots go to `build/webtest/` |
| `tools/cm.sh all` | All of the above |

**What the emulator can't do:** it has no Wi-Fi radio, ADC or real I2C/1-Wire devices. The self-test build fakes sensor values and skips Wi-Fi. Weather, uploads and real sensors still need the real board.

## Windows (Docker Desktop)

1. **Install Docker Desktop** (WSL 2 backend) and start it.
2. **Open PowerShell** in your `carmetricsbox` folder.
3. **Build the image once.** It takes about 15 minutes and creates an image of about 4.5 GB (both ESP32 cores):
   ```powershell
   docker build -t carmetrics-dev devenv
   ```
   - To skip the web-test browser (about 500 MB smaller), use `docker build --build-arg WITH_WEBTEST=0 -t carmetrics-dev devenv`.
4. **Run the commands:**
   ```powershell
   docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh build
   docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh selftest
   docker run --rm -v "${PWD}:/work" carmetrics-dev tools/cm.sh webtest
   ```
   - For an interactive shell: `docker run --rm -it -v "${PWD}:/work" carmetrics-dev`.

**Line endings.** If Git on Windows converted the scripts to CRLF, you'll see `$'\r': command not found`. Fix it with `git config core.autocrlf input`, then re-checkout. The repo's `.gitattributes` pins `*.sh` to LF.

## VS Code

Open the folder and choose **"Reopen in Container"**; `.devcontainer/devcontainer.json` uses this Dockerfile. Then run `tools/cm.sh …` in the terminal.

## Without Docker (Ubuntu 24.04 / WSL 2)

1. Install the packages:
   ```bash
   sudo apt install ca-certificates curl git xz-utils bzip2 unzip zip python3 python3-pil python3-serial libslirp0 libsdl2-2.0-0 libglib2.0-0 libgcrypt20
   ```
2. Install the toolchain:
   ```bash
   sudo CM_PREFIX=/opt/cm bash devenv/setup-toolchain.sh
   ```
3. Run `tools/cm.sh build` (or `selftest`).

## Flashing a board from the container

Docker Desktop can't pass USB serial ports through to containers on Windows. Build in the container, then flash from Windows with one of:

- https://espressif.github.io/esptool-js/ — flash `build/carmetrics-full-flash.bin` at 0x0.
- The **System** page OTA upload, using `carmetrics-firmware.bin` and `littlefs.bin`.
