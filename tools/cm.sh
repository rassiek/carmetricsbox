#!/usr/bin/env bash
# CarMetrics dev commands (run inside the devenv container, or anywhere the
# toolchain from devenv/setup-toolchain.sh is installed).
#
#   tools/cm.sh build              firmware + web app + flash images  -> build/  (classic ESP32, core 2.0.17)
#   tools/cm.sh build3 [esp32|esp32s3]  compile with core 3.x  -> build/core3-<chip>/ (S3: carmetrics-s3-firmware.bin)
#   tools/cm.sh selftest [secs]    run the firmware self-test in the ESP32 emulator
#   tools/cm.sh emu [secs]         boot the firmware in the emulator and show the log
#   tools/cm.sh webtest            headless browser test of the web app (mock ESP32)
#   tools/cm.sh all                build + build3 esp32 + build3 esp32s3 + selftest + webtest
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${CM_PREFIX:-/opt/cm}"
export ARDUINO_DIRECTORIES_USER="${ARDUINO_DIRECTORIES_USER:-$PREFIX/Arduino}"
export ARDUINO_DIRECTORIES_DATA="${ARDUINO_DIRECTORIES_DATA:-$PREFIX/arduino15}"
CLI="$PREFIX/bin/arduino-cli"
CORE="$ARDUINO_DIRECTORIES_USER/hardware/espressif/esp32"
ESPTOOL="python3 $CORE/tools/esptool/esptool.py"
QEMU="$PREFIX/qemu/bin/qemu-system-xtensa"
SKETCH="$ROOT/firmware/CarMetrics"
OUT="$ROOT/build"
mkdir -p "$OUT"

compile() {   # compile <outdir> <fqbn-options> [extra cpp flags]
  local args=(--fqbn "espressif:esp32:esp32:$2" --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" --output-dir "$1")
  [ -n "${3:-}" ] && args+=(--build-property "compiler.cpp.extra_flags=$3")
  "$CLI" compile "${args[@]}" "$SKETCH" 2>&1 | grep -vE "^Error initializing instance" | grep -E "error|warning: unused|Sketch uses|Global variables" || true
  [ -f "$1/CarMetrics.ino.bin" ] || { echo "compile failed"; exit 1; }
}

merge() {     # merge <bindir> <flash-freq> <output>
  $ESPTOOL --chip esp32 merge_bin -o "$3" --flash_mode dio --flash_freq "$2" --flash_size 4MB \
    0x1000 "$1/CarMetrics.ino.bootloader.bin" 0x8000 "$1/CarMetrics.ino.partitions.bin" \
    0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$1/CarMetrics.ino.bin" \
    0x350000 "$OUT/littlefs.bin" > /dev/null
}

web() { python3 "$ROOT/tools/build_web.py" --mklittlefs "$CORE/tools/mklittlefs/mklittlefs" | tail -2; }

# Core 3.x lives in its own Arduino folder ($PREFIX/core3) so both cores can be used side by side.
cmd_build3() {
  local chip="${1:-esp32s3}" opts
  case "$chip" in
    esp32)   opts="PartitionScheme=min_spiffs" ;;
    esp32s3) opts="PSRAM=opi,FlashSize=4M,PartitionScheme=min_spiffs" ;;   # 4 MB header: one image fits 8 and 16 MB boards
    *) echo "chip must be esp32 or esp32s3"; exit 1 ;;
  esac
  local dir="$OUT/core3-$chip"
  rm -rf "$dir"
  ARDUINO_DIRECTORIES_USER="${CM_CORE3_USER:-$PREFIX/core3/Arduino}" ARDUINO_DIRECTORIES_DATA="${CM_CORE3_DATA:-$PREFIX/core3/arduino15}" \
    "$CLI" compile --fqbn "espressif:esp32:$chip:$opts" --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" --output-dir "$dir" "$SKETCH" 2>&1 \
    | grep -vE "^Error initializing instance|^Downloading index" | grep -E "error|Sketch uses|Global variables" || true
  [ -f "$dir/CarMetrics.ino.bin" ] || { echo "compile failed"; exit 1; }
  if [ "$chip" = esp32s3 ]; then
    cp "$dir/CarMetrics.ino.bin" "$OUT/carmetrics-s3-firmware.bin"
    [ -f "$OUT/littlefs.bin" ] || web
    $ESPTOOL --chip esp32s3 merge_bin -o "$OUT/carmetrics-s3-full-flash.bin" --flash_mode dio --flash_freq 80m --flash_size keep \
      0x0 "$dir/CarMetrics.ino.bootloader.bin" 0x8000 "$dir/CarMetrics.ino.partitions.bin" \
      0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$dir/CarMetrics.ino.bin" 0x350000 "$OUT/littlefs.bin" > /dev/null
  fi
  echo "core 3 $chip build OK"
}

cmd_build() {
  web
  rm -rf "$OUT/fw" && compile "$OUT/fw" "PartitionScheme=min_spiffs"
  cp "$OUT/fw/CarMetrics.ino.bin" "$OUT/carmetrics-firmware.bin"
  merge "$OUT/fw" 80m "$OUT/carmetrics-full-flash.bin"
  ls -la "$OUT"/carmetrics-*.bin "$OUT/littlefs.bin"
}

run_qemu() {  # run_qemu <flags> <seconds>
  [ -f "$OUT/littlefs.bin" ] || web
  # QEMU needs 40 MHz DIO flash; it has no Wi-Fi radio or ADC (CM_EMULATOR_TEST skips/fakes those)
  rm -rf "$OUT/emu" && compile "$OUT/emu" "PartitionScheme=min_spiffs,FlashFreq=40,FlashMode=dio,DebugLevel=info" "$1"
  merge "$OUT/emu" 40m "$OUT/emu/flash.bin"
  echo "== running emulator for $2 s (log: build/emu/qemu.log)"
  timeout "$2" "$QEMU" -nographic -machine esp32 -m 4M -drive "file=$OUT/emu/flash.bin,if=mtd,format=raw" > "$OUT/emu/qemu.log" 2>&1 || true
}

cmd_selftest() {
  run_qemu "-DCM_EMULATOR_TEST -DCM_EMU_SELFTEST" "${1:-330}"
  grep -a "\[test\] \(PASS\|FAIL\|DONE\)" "$OUT/emu/qemu.log" || true
  python3 "$ROOT/tools/emu/pbm2png.py" "$OUT/emu/qemu.log" "$OUT/emu"
  grep -aq "DONE: .* 0 failed" "$OUT/emu/qemu.log" || { echo "SELFTEST FAILED (see build/emu/qemu.log)"; exit 1; }
  echo "SELFTEST PASSED"
}

cmd_emu() {
  run_qemu "-DCM_EMULATOR_TEST" "${1:-60}"
  grep -av "^$\|^ets \|^rst:\|^configsip\|^clk_drv\|^mode:\|^load:\|^entry" "$OUT/emu/qemu.log" | head -200
}

cmd_webtest() {
  python3 "$ROOT/tools/webtest/mock_server.py" "$ROOT/web" 8099 > "$OUT/mock.log" 2>&1 &
  MOCK_PID=$!
  trap 'kill ${MOCK_PID:-} 2>/dev/null || true' EXIT   # also when a test fails (set -e)
  sleep 1.5
  node "$ROOT/tools/webtest/ui_test.js" "$OUT/webtest" http://127.0.0.1:8099/
}

case "${1:-}" in
  build) cmd_build ;;
  build3) cmd_build3 "${2:-esp32s3}" ;;
  selftest) cmd_selftest "${2:-}" ;;
  emu) cmd_emu "${2:-}" ;;
  webtest) cmd_webtest ;;
  all) cmd_build && cmd_build3 esp32 && cmd_build3 esp32s3 && cmd_selftest && cmd_webtest ;;
  *) sed -n '2,11p' "$0"; exit 1 ;;
esac
