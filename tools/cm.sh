#!/usr/bin/env bash
# CarMetrics dev commands (run inside the devenv container, or anywhere the
# toolchain from devenv/setup-toolchain.sh is installed).
#
#   tools/cm.sh main               main board firmware (ESP32-S3, 8/16 MB): build/carmetrics-main-s3-*.bin
#   tools/cm.sh worker [v|all]     worker firmware; v = s3 | esp32 | c3 | c6 | relay-x8 | kc868-a4 | t-relay
#   tools/cm.sh display [jc|cyd]   touch display firmware (JC3248W535 and/or Cheap Yellow Display)
#   tools/cm.sh release            all of the above (every file of a release, in build/)
#   tools/cm.sh selftest [secs]    main firmware self-test in the ESP32 emulator
#   tools/cm.sh selftest-worker    worker firmware self-test in the ESP32 emulator
#   tools/cm.sh emu [secs]         boot the main firmware in the emulator and show the log
#   tools/cm.sh webtest            headless browser test of the web app (mock boards)
#   tools/cm.sh prototest          board-to-board protocol + API path tests on this PC (seconds)
#   tools/cm.sh dispsim            display screens drawn on this PC (build/dispsim/*.png) + display model tests
#   tools/cm.sh all                prototest + release + dispsim + both self-tests + webtest
# USB: …-full-flash.bin at 0x0. Wi-Fi: …-firmware.bin (the web app is inside it).
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
WORKER="$ROOT/firmware/CarMetricsWorker"
LIBS="$ROOT/firmware/libraries"           # CarMetricsLink + CarMetricsCore, shared by every firmware
LINK="$LIBS/CarMetricsLink/src"; CORELIB="$LIBS/CarMetricsCore/src"
OUT="$ROOT/build"
mkdir -p "$OUT"

# arduino-cli copies a sketch into its build cache and never deletes files that left
# the sketch (they would still compile): clear that copy before each build.
fresh() { rm -rf "$HOME"/.cache/arduino/sketches/*/sketch 2>/dev/null || true; }

compile() {   # compile <outdir> <fqbn-options> [extra cpp flags]
  fresh
  local args=(--libraries "$LIBS" --fqbn "espressif:esp32:esp32:$2" --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" --output-dir "$1")
  [ -n "${3:-}" ] && args+=(--build-property "compiler.cpp.extra_flags=$3")
  "$CLI" compile "${args[@]}" "$SKETCH" 2>&1 | grep -vE "^Error initializing instance" | grep -E "error|warning: unused|Sketch uses|Global variables" || true
  [ -f "$1/CarMetrics.ino.bin" ] || { echo "compile failed"; exit 1; }
}

merge() {     # merge <bindir> <flash-freq> <output>: full-flash image for USB (firmware + whole web app partition)
  $ESPTOOL --chip esp32 merge_bin -o "$3" --flash_mode dio --flash_freq "$2" --flash_size 4MB \
    0x1000 "$1/CarMetrics.ino.bootloader.bin" 0x8000 "$1/CarMetrics.ino.partitions.bin" \
    0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$1/CarMetrics.ino.bin" \
    0x350000 "$OUT/littlefs.bin" > /dev/null
}

# Real room in the app slot (the arduino-cli "Maximum" figure ignores our partition table).
slotcheck() {   # slotcheck <bin> <slot size> <name>
  local n; n=$(stat -c %s "$1"); local max=$(( $2 ))
  echo "firmware slot $3: $n of $max bytes ($(( n * 100 / max )) %)"
  [ "$n" -le "$max" ] || { echo "FIRMWARE TOO BIG for the $3 slot"; exit 1; }
}

web() { python3 "$ROOT/tools/build_web.py" --mklittlefs "$CORE/tools/mklittlefs/mklittlefs" | tail -2; }

# Main board: ESP32-S3 with 8 or 16 MB flash and PSRAM. Compiled with -DCM_V52 and the
# 8 MB partition table (the sketch's own 4 MB table is for the emulator self-test).
cmd_main() {
  local s="$OUT/sketch52/CarMetrics" dir="$OUT/v52"
  fresh; rm -rf "$OUT/sketch52" "$dir" && mkdir -p "$s"
  cp "$SKETCH"/*.h "$SKETCH"/*.cpp "$SKETCH"/*.ino "$s"/
  cp "$ROOT/firmware/partitions-8MB-v52.csv" "$s/partitions.csv"
  python3 "$ROOT/tools/build_web.py" --mklittlefs "$CORE/tools/mklittlefs/mklittlefs" --line 52 --fs-size 0x1C0000 \
    --data "$OUT/v52data" --out "$dir/littlefs.bin" | tail -2
  ARDUINO_DIRECTORIES_USER="${CM_CORE3_USER:-$PREFIX/core3/Arduino}" ARDUINO_DIRECTORIES_DATA="${CM_CORE3_DATA:-$PREFIX/core3/arduino15}" \
    "$CLI" compile --libraries "$LIBS" --fqbn "espressif:esp32:esp32s3:PSRAM=opi,FlashSize=8M,PartitionScheme=default_8MB" \
      --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" --build-property "compiler.cpp.extra_flags=-DCM_V52" \
      --output-dir "$dir" "$s" 2>&1 | grep -vE "^Error initializing instance|^Downloading index" | grep -E "error|Sketch uses|Global variables" || true
  [ -f "$dir/CarMetrics.ino.bin" ] || { echo "main compile failed"; exit 1; }
  cp "$dir/CarMetrics.ino.bin" "$OUT/carmetrics-main-s3-firmware.bin"
  $ESPTOOL --chip esp32s3 merge_bin -o "$OUT/carmetrics-main-s3-full-flash.bin" --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 "$dir/CarMetrics.ino.bootloader.bin" 0x8000 "$dir/CarMetrics.ino.partitions.bin" \
    0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$dir/CarMetrics.ino.bin" 0x610000 "$dir/littlefs.bin" > /dev/null
  slotcheck "$dir/CarMetrics.ino.bin" 0x300000 "ESP32-S3 8 MB"
  echo "main board (ESP32-S3, 8 MB+) build OK: $OUT/carmetrics-main-s3-*.bin"
}

# Worker firmware (firmware/CarMetricsWorker): one source, a build per chip / board.
#   s3: ESP32-S3 4 MB+ with chart history | esp32, c3, c6: generic boards |
#   relay-x8, kc868-a4, t-relay: ready-made relay boards (their profile preselected)
cmd_worker() {
  local v="${1:-all}"
  if [ "$v" = all ]; then for x in s3 esp32 c3 c6 relay-x8 kc868-a4 t-relay; do cmd_worker "$x"; done; return; fi
  web   # the web app goes into the firmware: always rebuild it
  local dir="$OUT/worker-$v" flags="-DCM_WORKER" core=3 fqbn chip boot=0x0 name="carmetrics-worker-$v"
  case "$v" in
    s3)       fqbn="esp32s3:FlashSize=4M,PartitionScheme=min_spiffs"; chip=esp32s3; flags="$flags -DCM_WORKER_PLUS" ;;
    esp32)    fqbn="esp32:PartitionScheme=min_spiffs"; chip=esp32; core=2; boot=0x1000 ;;
    c3)       fqbn="esp32c3:FlashSize=4M,PartitionScheme=min_spiffs,CDCOnBoot=cdc"; chip=esp32c3 ;;
    c6)       fqbn="esp32c6:FlashSize=4M,PartitionScheme=min_spiffs,CDCOnBoot=cdc"; chip=esp32c6 ;;
    relay-x8) fqbn="esp32:PartitionScheme=min_spiffs"; chip=esp32; core=2; boot=0x1000; flags="$flags -DCM_RELAY_X8"; name="carmetrics-relay-x8" ;;
    kc868-a4) fqbn="esp32:PartitionScheme=min_spiffs"; chip=esp32; core=2; boot=0x1000; flags="$flags -DCM_KC868_A4"; name="carmetrics-kc868-a4" ;;
    t-relay)  fqbn="esp32:PartitionScheme=min_spiffs"; chip=esp32; core=2; boot=0x1000; flags="$flags -DCM_T_RELAY"; name="carmetrics-t-relay" ;;
    *) echo "worker: s3 | esp32 | c3 | c6 | relay-x8 | kc868-a4 | t-relay | all"; exit 1 ;;
  esac
  fresh; rm -rf "$dir"
  local env=()
  [ "$core" = 3 ] && env=(env ARDUINO_DIRECTORIES_USER="${CM_CORE3_USER:-$PREFIX/core3/Arduino}" ARDUINO_DIRECTORIES_DATA="${CM_CORE3_DATA:-$PREFIX/core3/arduino15}")
  "${env[@]}" "$CLI" compile --libraries "$LIBS" --fqbn "espressif:esp32:$fqbn" \
    --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" --build-property "compiler.cpp.extra_flags=$flags" \
    --output-dir "$dir" "$WORKER" 2>&1 | grep -vE "^Error initializing instance|^Downloading index" | grep -E "error|Sketch uses" || true
  [ -f "$dir/CarMetricsWorker.ino.bin" ] || { echo "worker $v: compile failed"; exit 1; }
  cp "$dir/CarMetricsWorker.ino.bin" "$OUT/$name-firmware.bin"
  # USB image: boot loader, partition table (worker layout), fresh boot data (starts the
  # first slot), firmware. Settings (cfg partition, same place as 5.3) are not touched.
  $ESPTOOL --chip "$chip" merge_bin -o "$OUT/$name-full-flash.bin" --flash_mode dio --flash_freq 80m --flash_size 4MB \
    "$boot" "$dir/CarMetricsWorker.ino.bootloader.bin" 0x8000 "$dir/CarMetricsWorker.ino.partitions.bin" \
    0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$dir/CarMetricsWorker.ino.bin" > /dev/null
  slotcheck "$dir/CarMetricsWorker.ino.bin" 0x1E0000 "worker $v"
  # Boards still on the 5.3 layout (1.6 MB slots) update over the air too: warn when it would not fit.
  if [ "$chip" = esp32 ] || [ "$chip" = esp32s3 ]; then
    local n; n=$(stat -c %s "$dir/CarMetricsWorker.ino.bin")
    [ "$n" -le $((0x1A0000)) ] || echo "NOTE: $v is bigger than a 5.3-layout slot: such boards need one USB flash"
  fi
}

# Touch display firmware (firmware/CarMetricsDisplay). The board-to-board files are
# shared with the main firmware: copied next to the display sketch in a temp copy.
cmd_display() {
  local which="${1:-all}"
  python3 "$ROOT/tools/build_display_web.py"
  local s="$OUT/sketchDisplay/CarMetricsDisplay"
  rm -rf "$OUT/sketchDisplay" && mkdir -p "$s"
  cp "$ROOT/firmware/CarMetricsDisplay"/*.h "$ROOT/firmware/CarMetricsDisplay"/*.cpp "$ROOT/firmware/CarMetricsDisplay"/*.ino "$s"/
  local c3=(env ARDUINO_DIRECTORIES_USER="${CM_CORE3_USER:-$PREFIX/core3/Arduino}" ARDUINO_DIRECTORIES_DATA="${CM_CORE3_DATA:-$PREFIX/core3/arduino15}")
  if [ "$which" = all ] || [ "$which" = jc ]; then
    local dir="$OUT/display-jc"; rm -rf "$dir"
    "${c3[@]}" "$CLI" compile --libraries "$LIBS" --fqbn "espressif:esp32:esp32s3:PSRAM=opi,FlashSize=4M,PartitionScheme=min_spiffs,CDCOnBoot=cdc" \
      --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" --output-dir "$dir" "$s" 2>&1 \
      | grep -vE "^Error initializing instance|^Downloading index" | grep -E "error|Sketch uses|Global variables" || true
    [ -f "$dir/CarMetricsDisplay.ino.bin" ] || { echo "display (JC3248W535) compile failed"; exit 1; }
    cp "$dir/CarMetricsDisplay.ino.bin" "$OUT/carmetrics-display-jc3248w535-firmware.bin"
    $ESPTOOL --chip esp32s3 merge_bin -o "$OUT/carmetrics-display-jc3248w535-full-flash.bin" --flash_mode dio --flash_freq 80m --flash_size 4MB \
      0x0 "$dir/CarMetricsDisplay.ino.bootloader.bin" 0x8000 "$dir/CarMetricsDisplay.ino.partitions.bin" \
      0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$dir/CarMetricsDisplay.ino.bin" > /dev/null
    echo "display JC3248W535 build OK"
  fi
  if [ "$which" = all ] || [ "$which" = cyd ]; then
    local dir="$OUT/display-cyd"; rm -rf "$dir"
    "${c3[@]}" "$CLI" compile --libraries "$LIBS" --fqbn "espressif:esp32:esp32:PartitionScheme=min_spiffs" \
      --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" --output-dir "$dir" "$s" 2>&1 \
      | grep -vE "^Error initializing instance|^Downloading index" | grep -E "error|Sketch uses|Global variables" || true
    [ -f "$dir/CarMetricsDisplay.ino.bin" ] || { echo "display (CYD) compile failed"; exit 1; }
    cp "$dir/CarMetricsDisplay.ino.bin" "$OUT/carmetrics-display-cyd-firmware.bin"
    $ESPTOOL --chip esp32 merge_bin -o "$OUT/carmetrics-display-cyd-full-flash.bin" --flash_mode dio --flash_freq 40m --flash_size 4MB \
      0x1000 "$dir/CarMetricsDisplay.ino.bootloader.bin" 0x8000 "$dir/CarMetricsDisplay.ino.partitions.bin" \
      0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$dir/CarMetricsDisplay.ino.bin" > /dev/null
    echo "display CYD build OK"
  fi
}

# The display's screens on this PC: the real Ui / Model code and Arduino_GFX's drawing
# core into a frame buffer (tools/dispsim), saved as PNGs; plus checks of the display logic.
cmd_dispsim() {
  local d="$OUT/dispsim" G="$ARDUINO_DIRECTORIES_USER/libraries/GFX_Library_for_Arduino/src"
  rm -rf "$d" && mkdir -p "$d"
  python3 "$ROOT/tools/dispsim/extract_fonts.py" "$ARDUINO_DIRECTORIES_USER/libraries/U8g2_Arduino" "$d/gen"
  gcc -c -O1 -I "$d/gen" "$d/gen/fonts.c" -o "$d/fonts.o"
  g++ -std=c++17 -O1 -w -fsanitize=address,undefined -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 \
    -I "$ROOT/tools/dispsim/shim" -I "$d/gen" -I "$G" -I "$ARDUINO_DIRECTORIES_USER/libraries/ArduinoJson/src" -I "$ROOT/firmware/CarMetricsDisplay" \
    "$ROOT/tools/dispsim/sim.cpp" "$ROOT/tools/dispsim/shim/Print.cpp" "$ROOT/firmware/CarMetricsDisplay/Model.cpp" "$ROOT/firmware/CarMetricsDisplay/Ui.cpp" \
    "$G/Arduino_GFX.cpp" "$G/Arduino_G.cpp" "$d/fonts.o" -o "$d/sim"
  "$d/sim" "$d"
  python3 -c "
import sys,glob
from PIL import Image
for f in sorted(glob.glob(sys.argv[1]+'/*.ppm')):
    Image.open(f).save(f[:-4]+'.png')
" "$d" && rm -f "$d"/*.ppm
  echo "screens: $d/*.png"
}

run_qemu() {  # run_qemu <flags> <seconds>
  web   # the web app goes into the firmware: always rebuild it
  # QEMU needs 40 MHz DIO flash; it has no Wi-Fi radio or ADC (CM_EMULATOR_TEST skips/fakes those)
  rm -rf "$OUT/emu" && compile "$OUT/emu" "PartitionScheme=min_spiffs,FlashFreq=40,FlashMode=dio,DebugLevel=info" "$1"
  merge "$OUT/emu" 40m "$OUT/emu/flash.bin"
  python3 -c "import sys;f=open(sys.argv[1],'ab');f.write(b'\\xff'*(4194304-f.tell()))" "$OUT/emu/flash.bin"   # QEMU needs a full 4 MB image (erased = 0xFF)
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

# Worker self-test: the generic worker (classic ESP32, worker partition layout, no file system) in QEMU.
cmd_selftest_worker() {
  local dir="$OUT/emu-worker"
  web
  fresh; rm -rf "$dir"
  "$CLI" compile --libraries "$LIBS" --fqbn "espressif:esp32:esp32:FlashFreq=40,FlashMode=dio,DebugLevel=info" \
    --build-property "runtime.tools.ctags.path=$PREFIX/fakectags" \
    --build-property "compiler.cpp.extra_flags=-DCM_WORKER -DCM_EMULATOR_TEST -DCM_EMU_SELFTEST" \
    --output-dir "$dir" "$WORKER" 2>&1 | grep -vE "^Error initializing instance" | grep -E "error|Sketch uses" || true
  [ -f "$dir/CarMetricsWorker.ino.bin" ] || { echo "worker self-test: compile failed"; exit 1; }
  $ESPTOOL --chip esp32 merge_bin -o "$dir/flash.bin" --flash_mode dio --flash_freq 40m --flash_size 4MB \
    0x1000 "$dir/CarMetricsWorker.ino.bootloader.bin" 0x8000 "$dir/CarMetricsWorker.ino.partitions.bin" \
    0xe000 "$CORE/tools/partitions/boot_app0.bin" 0x10000 "$dir/CarMetricsWorker.ino.bin" > /dev/null
  python3 -c "import sys;f=open(sys.argv[1],'ab');f.write(b'\\xff'*(4194304-f.tell()))" "$dir/flash.bin"
  echo "== running worker in the emulator for ${1:-60} s (log: build/emu-worker/qemu.log)"
  timeout "${1:-60}" "$QEMU" -nographic -machine esp32 -m 4M -drive "file=$dir/flash.bin,if=mtd,format=raw" > "$dir/qemu.log" 2>&1 || true
  grep -a "\[test\] \(PASS\|FAIL\|DONE\)" "$dir/qemu.log" || true
  grep -aq "DONE: .* 0 failed" "$dir/qemu.log" || { echo "WORKER SELFTEST FAILED (see build/emu-worker/qemu.log)"; exit 1; }
  echo "WORKER SELFTEST PASSED"
}

cmd_emu() {
  run_qemu "-DCM_EMULATOR_TEST" "${1:-60}"
  grep -av "^$\|^ets \|^rst:\|^configsip\|^clk_drv\|^mode:\|^load:\|^entry" "$OUT/emu/qemu.log" | head -200
}

cmd_prototest() {
  mkdir -p "$OUT/prototest"
  g++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I "$LINK" -I "$CORELIB" \
    "$ROOT/tools/prototest/proto_test.cpp" "$ROOT/tools/prototest/boards_test.cpp" "$ROOT/tools/prototest/fw_test.cpp" "$ROOT/tools/prototest/can_test.cpp" "$ROOT/tools/prototest/display_test.cpp" "$ROOT/tools/prototest/route_test.cpp" "$LINK/CanFrag.cpp" "$LINK/Proto.cpp" "$LINK/BoardNet.cpp" \
    -o "$OUT/prototest/proto_test"
  "$OUT/prototest/proto_test"
}

cmd_webtest() {
  python3 "$ROOT/tools/webtest/mock_server.py" "$ROOT/web" 8099 > "$OUT/mock.log" 2>&1 &
  MOCK_PID=$!
  trap 'kill ${MOCK_PID:-} 2>/dev/null || true' EXIT   # also when a test fails (set -e)
  sleep 1.5
  node "$ROOT/tools/webtest/ui_test.js" "$OUT/webtest" http://127.0.0.1:8099/
  python3 "$ROOT/tools/webtest/mock_server.py" "$ROOT/web" 8097 worker > "$OUT/mock_worker.log" 2>&1 &
  WMOCK_PID=$!
  trap 'kill ${MOCK_PID:-} ${WMOCK_PID:-} 2>/dev/null || true' EXIT
  sleep 1.5
  node "$ROOT/tools/webtest/ui_worker_test.js" "$OUT/webtest" http://127.0.0.1:8097/
  python3 "$ROOT/tools/webtest/display_mock.py" "$ROOT/firmware/CarMetricsDisplay/web/index.html" 8098 > "$OUT/display_mock.log" 2>&1 &
  DMOCK_PID=$!
  trap 'kill ${MOCK_PID:-} ${WMOCK_PID:-} ${DMOCK_PID:-} 2>/dev/null || true' EXIT
  sleep 1
  node "$ROOT/tools/webtest/display_test.js" "$OUT/webtest" http://127.0.0.1:8098/
}

case "${1:-}" in
  main) cmd_main ;;
  build52) cmd_main ;;   # old name
  worker) cmd_worker "${2:-all}" ;;
  display) cmd_display "${2:-all}" ;;
  release) cmd_main && cmd_worker all && cmd_display all ;;
  selftest) cmd_selftest "${2:-}" ;;
  selftest-worker) cmd_selftest_worker "${2:-}" ;;
  emu) cmd_emu "${2:-}" ;;
  webtest) cmd_webtest ;;
  prototest) cmd_prototest ;;
  dispsim) cmd_dispsim ;;
  all) cmd_prototest && cmd_main && cmd_worker all && cmd_display all && cmd_dispsim && cmd_selftest && cmd_selftest_worker && cmd_webtest ;;
  *) sed -n '2,18p' "$0"; exit 1 ;;
esac
