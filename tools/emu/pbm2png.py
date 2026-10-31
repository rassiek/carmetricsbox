#!/usr/bin/env python3
"""Extract OLED frames (PBM dumps between <<<PBM and >>>PBM) from an emulator log into PNGs."""
import os
import re
import sys

from PIL import Image

log_path, out_dir = sys.argv[1], sys.argv[2]
os.makedirs(out_dir, exist_ok=True)
log = open(log_path, "rb").read().decode("latin1")
frames = re.findall(r"<<<PBM(.*?)>>>PBM", log, re.S)
for n, f in enumerate(frames):
    f = re.sub(r"\[(test|cfg|oled|hb|net)\][^\n]*\n?", "", f)   # other tasks print in between
    m = re.search(r"P1\s+(\d+)\s+(\d+)\s+([01\s]+)", f)
    if not m:
        continue
    w, h = int(m.group(1)), int(m.group(2))
    bits = re.sub(r"\s", "", m.group(3))[: w * h]
    im = Image.new("L", (w, h))
    im.putdata([255 if b == "1" else 0 for b in bits] + [0] * (w * h - len(bits)))
    path = os.path.join(out_dir, f"oled-{n}-{w}x{h}.png")
    im.resize((w * 4, h * 4), Image.NEAREST).save(path)
    print("OLED frame:", path)
