#!/usr/bin/env python3
"""Checks every On this day picture (components/sky_tracker/sky_hist_pics.h) against what the unit can decode:
a baseline JPEG, colour (the ROM's TJpgDec) or greyscale (sky_gray, the newer TJpgDec). Run: python3 tools/check_hist_pics.py
Needs curl (python's own TLS has no certificates on a stock macOS install)."""
import re, subprocess, sys, tempfile, os

src = open(os.path.join(os.path.dirname(__file__), "..", "components", "sky_tracker", "sky_hist_pics.h")).read()
items = re.findall(r'^\s*"([^"]+)",\s*//\s*(\d+) ', src, re.M)


def sof(d):  # "base comps=3 330x272" from the first start-of-frame marker
    i = 2
    while i + 9 < len(d):
        if d[i] != 0xFF:
            i += 1
            continue
        m = d[i + 1]
        if m == 0xFF:
            i += 1
            continue
        if m == 0 or m == 1 or 0xD0 <= m <= 0xD8:
            i += 2
            continue
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            return "%s comps=%d %dx%d" % ("prog" if m == 0xC2 else "base", d[i + 9], d[i + 7] << 8 | d[i + 8], d[i + 5] << 8 | d[i + 6])
        i += 2 + (d[i + 2] << 8 | d[i + 3])
    return "no SOF"


bad = 0
with tempfile.NamedTemporaryFile() as f:
    for path, idx in items:
        r = subprocess.run(["curl", "-s", "-m", "20", "-A", "sky-tracker-check", "-r", "0-4095", "-o", f.name,
                            "https://upload.wikimedia.org/wikipedia/commons/thumb/" + path])
        res = sof(open(f.name, "rb").read()) if r.returncode == 0 else "fetch failed"
        if not (res.startswith("base comps=3") or res.startswith("base comps=1")):
            bad += 1
            print("%3s  %-22s %s" % (idx, res, path[:70]))
print("%d pictures, %d the unit cannot show" % (len(items), bad))
sys.exit(1 if bad else 0)
