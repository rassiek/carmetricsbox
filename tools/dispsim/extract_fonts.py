#!/usr/bin/env python3
"""Copy the U8g2 fonts the display UI uses out of the installed U8g2 library into
<out>/fonts.c / fonts.h for the PC screen simulator (not committed: generated)."""
import re, sys, os
lib, out = sys.argv[1], sys.argv[2]
names = ["logisoso58_tn", "logisoso46_tn", "logisoso38_tn", "logisoso32_tn", "logisoso26_tn", "logisoso20_tn", "logisoso16_tn",
         "helvB24_tf", "helvB18_tf", "helvB14_tf", "helvB12_tf", "helvB10_tf", "helvB08_tf", "helvR08_tf"]
src = open(os.path.join(lib, "src/clib/u8g2_fonts.c"), encoding="latin-1").read()
c = ['/* Fonts used by the display UI, copied from U8g2 for the PC simulator. */', '#include <stdint.h>']
h = ['#pragma once', '#include <stdint.h>']
for n in names:
    m = re.search(r'const uint8_t u8g2_font_%s\[\d+\] U8G2_FONT_SECTION\("u8g2_font_%s"\) =\s*(.*?);\n' % (n, n), src, re.S)
    if not m: sys.exit("font not found: " + n)
    c.append('const uint8_t u8g2_font_%s[] = %s;' % (n, m.group(1)))
    h.append('extern const uint8_t u8g2_font_%s[];' % n)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "fonts.c"), "w", encoding="latin-1").write("\n".join(c) + "\n")
open(os.path.join(out, "fonts.h"), "w").write("\n".join(h) + "\n")
