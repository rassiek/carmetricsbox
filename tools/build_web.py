#!/usr/bin/env python3
"""Build the CarMetrics web app for the ESP32.

1. gzips every file in web/ into firmware/CarMetrics/data/www/<name>.gz
   (the ESP32 serves the .gz version directly - smaller and faster over Wi-Fi)
2. if mklittlefs is found, also writes build/littlefs.bin, which you can upload
   from the System page ("Web app") or flash over USB.

Usage:  python3 tools/build_web.py [--mklittlefs PATH]
"""
import argparse
import glob
import gzip
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "web")
DATA = os.path.join(ROOT, "firmware", "CarMetrics", "data")
OUT = os.path.join(DATA, "www")
BUILD = os.path.join(ROOT, "build")
FS_SIZE = 0x90000  # must match the spiffs partition in partitions.csv (0x350000)


def find_mklittlefs(explicit):
    if explicit:
        return explicit
    found = shutil.which("mklittlefs")
    if found:
        return found
    patterns = [
        os.path.expanduser("~/Arduino/hardware/espressif/esp32/tools/mklittlefs/mklittlefs*"),
        os.path.expanduser("~/.arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs*"),
        os.path.expanduser("~/Library/Arduino15/packages/esp32/tools/mklittlefs/*/mklittlefs*"),
        os.path.expandvars(r"%LOCALAPPDATA%\Arduino15\packages\esp32\tools\mklittlefs\*\mklittlefs.exe"),
        os.path.expanduser("~/.platformio/packages/tool-mklittlefs/mklittlefs*"),
    ]
    for p in patterns:
        hits = [h for h in glob.glob(p) if os.path.isfile(h)]
        if hits:
            return sorted(hits)[-1]
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mklittlefs", help="path to the mklittlefs tool")
    ap.add_argument("--line", default="", help="ignored since 5.3 (one version for every board)")
    ap.add_argument("--fs-size", default=hex(FS_SIZE), help="LittleFS partition size (5.1: 0x90000, 5.2: 0x1C0000)")
    ap.add_argument("--data", default=DATA, help="where the gzipped files go")
    ap.add_argument("--out", default=os.path.join(BUILD, "littlefs.bin"), help="LittleFS image to write")
    args = ap.parse_args()

    # The firmware's release label (Config.h FW_VERSION) is baked into the web
    # app, so the pages can warn when the firmware and web app don't match.
    import re
    cfg = open(os.path.join(ROOT, "firmware", "CarMetrics", "Config.h"), encoding="utf-8").read()
    version = re.search(r'#define FW_VERSION "([^"]+)"', cfg).group(1)
    data_dir, out_dir = args.data, os.path.join(args.data, "www")
    print(f"release {version}")

    shutil.rmtree(data_dir, ignore_errors=True)
    os.makedirs(out_dir)
    total_in = total_out = 0
    for name in sorted(os.listdir(SRC)):
        src = os.path.join(SRC, name)
        if not os.path.isfile(src) or name.startswith("."):
            continue
        raw = open(src, "rb").read().replace(b"__CM_VERSION__", version.encode())
        packed = gzip.compress(raw, compresslevel=9, mtime=0)
        with open(os.path.join(out_dir, name + ".gz"), "wb") as f:
            f.write(packed)
        total_in += len(raw)
        total_out += len(packed)
        print(f"  {name:22s} {len(raw):8d} -> {len(packed):7d}")
    # Presets stay plain JSON: the firmware reads them itself (POST /api/v1/presets/apply).
    pdir = os.path.join(SRC, "presets")
    if os.path.isdir(pdir):
        os.makedirs(os.path.join(out_dir, "presets"))
        for name in sorted(os.listdir(pdir)):
            if name.endswith(".json"):
                shutil.copy(os.path.join(pdir, name), os.path.join(out_dir, "presets", name))
                total_in += os.path.getsize(os.path.join(pdir, name)); total_out += os.path.getsize(os.path.join(pdir, name))
    print(f"web app: {total_in} bytes -> {total_out} bytes gzipped")

    tool = find_mklittlefs(args.mklittlefs)
    if not tool:
        print("mklittlefs not found - skipped littlefs.bin (data/ folder is ready for the IDE upload plugin)")
        return 0
    os.makedirs(BUILD, exist_ok=True)
    image = args.out
    os.makedirs(os.path.dirname(image), exist_ok=True)
    # LittleFS never relies on unused blocks being erased (it erases a block before
    # using it), so a Wi-Fi update may leave the rest of the partition as it is.
    subprocess.check_call([tool, "-c", data_dir, "-b", "4096", "-p", "256", "-s", str(int(args.fs_size, 0)), image],
                          stdout=subprocess.DEVNULL)
    # The whole image goes into the full-flash file (it wipes the partition clean);
    # the web app file for Wi-Fi updates is cut after the last block that holds data.
    data = open(image, "rb").read()
    end = (len(data.rstrip(b"\xff")) + 4095) // 4096 * 4096
    trimmed = image[:-4] + "-trimmed.bin"
    with open(trimmed, "wb") as f:
        f.write(data[:end])
    print(f"filesystem image: {image} ({len(data)} bytes), web app file {trimmed} ({end} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
