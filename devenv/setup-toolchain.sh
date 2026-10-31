#!/usr/bin/env bash
# Installs the exact CarMetrics toolchain into $CM_PREFIX (default /opt/cm):
# arduino-cli, ESP32 Arduino core (git tag + its own tool download script),
# pinned libraries, and Espressif's QEMU. Used by the Dockerfile; also works
# directly on Ubuntu 24.04 / WSL2.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC1091
source "$HERE/versions.env"
PREFIX="${CM_PREFIX:-/opt/cm}"
ARD="$PREFIX/Arduino"
mkdir -p "$PREFIX/bin" "$ARD/hardware/espressif" "$ARD/libraries" "$PREFIX/arduino15"

echo "== arduino-cli $ARDUINO_CLI_VERSION"
curl -fsSL "https://github.com/arduino/arduino-cli/releases/download/v${ARDUINO_CLI_VERSION}/arduino-cli_${ARDUINO_CLI_VERSION}_Linux_64bit.tar.gz" \
  | tar xz -C "$PREFIX/bin" arduino-cli

echo "== ESP32 Arduino core $ESP32_CORE_TAG (+ compiler, esptool, mklittlefs)"
if [ ! -d "$ARD/hardware/espressif/esp32" ]; then
  git clone -q --depth 1 -b "$ESP32_CORE_TAG" https://github.com/espressif/arduino-esp32.git "$ARD/hardware/espressif/esp32"
fi
(cd "$ARD/hardware/espressif/esp32/tools" && python3 get.py > /dev/null)
rm -rf "$ARD/hardware/espressif/esp32/tools/dist"   # downloaded archives, no longer needed

echo "== libraries"
echo "$LIBS" | while IFS='|' read -r name url sha ver; do
  [ -z "$name" ] && continue
  dst="$ARD/libraries/$name"
  [ -d "$dst" ] && continue
  echo "   $name $ver"
  git init -q "$dst"
  git -C "$dst" fetch -q --depth 1 "$url" "$sha"
  git -C "$dst" checkout -q FETCH_HEAD
  rm -rf "$dst/.git"
done

echo "== QEMU ($QEMU_RELEASE)"
curl -fsSL "https://github.com/espressif/qemu/releases/download/${QEMU_RELEASE}/${QEMU_ASSET}" | tar xJ -C "$PREFIX"
ln -sf "$PREFIX/qemu/bin/qemu-system-xtensa" "$PREFIX/bin/qemu-system-xtensa"

# arduino-cli normally downloads 'ctags' from downloads.arduino.cc to generate
# .ino prototypes. CarMetrics.ino declares everything before use, so a no-op
# ctags is enough and keeps the build fully offline/reproducible.
mkdir -p "$PREFIX/fakectags"
printf '#!/bin/sh\nexit 0\n' > "$PREFIX/fakectags/ctags"
chmod +x "$PREFIX/fakectags/ctags"

echo "== done: $PREFIX"
