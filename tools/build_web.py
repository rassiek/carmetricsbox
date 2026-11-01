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
    args = ap.parse_args()

    # The firmware's release label (Config.h FW_VERSION) is baked into the web
    # app, so the pages can warn when the firmware and web app don't match.
    import re
    cfg = open(os.path.join(ROOT, "firmware", "CarMetrics", "Config.h"), encoding="utf-8").read()
    version = re.search(r'#define FW_VERSION "([^"]+)"', cfg).group(1)
    print(f"release {version}")

    shutil.rmtree(DATA, ignore_errors=True)
    os.makedirs(OUT)
    total_in = total_out = 0
    for name in sorted(os.listdir(SRC)):
        src = os.path.join(SRC, name)
        if not os.path.isfile(src) or name.startswith("."):
            continue
        raw = open(src, "rb").read().replace(b"__CM_VERSION__", version.encode())
        packed = gzip.compress(raw, compresslevel=9, mtime=0)
        with open(os.path.join(OUT, name + ".gz"), "wb") as f:
            f.write(packed)
        total_in += len(raw)
        total_out += len(packed)
        print(f"  {name:22s} {len(raw):8d} -> {len(packed):7d}")
    print(f"web app: {total_in} bytes -> {total_out} bytes gzipped")

    tool = find_mklittlefs(args.mklittlefs)
    if not tool:
        print("mklittlefs not found - skipped littlefs.bin (data/ folder is ready for the IDE upload plugin)")
        return 0
    os.makedirs(BUILD, exist_ok=True)
    image = os.path.join(BUILD, "littlefs.bin")
    subprocess.check_call([tool, "-c", DATA, "-b", "4096", "-p", "256", "-s", str(FS_SIZE), image],
                          stdout=subprocess.DEVNULL)
    print(f"filesystem image: {image} ({os.path.getsize(image)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
