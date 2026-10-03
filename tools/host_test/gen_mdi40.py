#!/usr/bin/env python3
# Regenerates mdi40.c (the details-card icon font) from mdi_card40's glyph list in the device
# YAML, so the harness draws every icon the device can. Needs node (npx lv_font_conv).
import os, re, subprocess, urllib.request
HERE = os.path.dirname(os.path.abspath(__file__))
y = open(os.path.join(HERE, "../../sky-tracker.yaml"), encoding="utf-8").read()
i = y.index("id: mdi_card40")
url = re.findall(r"url: (\S+\.ttf)", y[:i])[-1]
glyphs = re.search(r"glyphs: \[(.*?)\]", y[i:]).group(1)
cps = ",".join("0x" + g for g in re.findall(r"U000(F[0-9A-F]{4})", glyphs))
ttf = os.path.join(HERE, "deps", os.path.basename(url))
os.makedirs(os.path.dirname(ttf), exist_ok=True)
if not os.path.exists(ttf):
    urllib.request.urlretrieve(url, ttf)
subprocess.run(["npx", "--yes", "lv_font_conv", "--font", ttf, "-r", cps, "--size", "40", "--bpp", "4", "--format", "lvgl",
                "--no-compress", "--lv-font-name", "mdi40", "-o", os.path.join(HERE, "mdi40.c")], check=True)
print("mdi40.c:", cps)
