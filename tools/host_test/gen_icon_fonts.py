#!/usr/bin/env python3
# Regenerates the harness icon fonts from the device YAML's glyph lists, so the harness draws
# every icon the device can: mdi40.c from mdi_card40, mdi20.c from mdi_card20 (plus whatever
# mdi20.c already held). Needs node (npx lv_font_conv).
import os, re, subprocess, urllib.request
HERE = os.path.dirname(os.path.abspath(__file__))
y = open(os.path.join(HERE, "../../sky-tracker.yaml"), encoding="utf-8").read()

def glyphs(font_id):
    i = y.index("id: " + font_id)
    url = re.findall(r"url: (\S+\.ttf)", y[:i])[-1]
    return url, re.findall(r"U000(F[0-9A-F]{4})", re.search(r"glyphs: \[(.*?)\]", y[i:]).group(1))

def build(name, size, cps, url):
    ttf = os.path.join(HERE, "deps", os.path.basename(url))
    os.makedirs(os.path.dirname(ttf), exist_ok=True)
    if not os.path.exists(ttf):
        urllib.request.urlretrieve(url, ttf)
    out = os.path.join(HERE, name + ".c")
    subprocess.run(["npx", "--yes", "lv_font_conv", "--font", ttf, "-r", ",".join("0x" + c for c in cps), "--size", str(size),
                    "--bpp", "4", "--format", "lvgl", "--no-compress", "--lv-font-name", name, "-o", out], check=True)
    src = open(out).read()
    open(out, "w").write(re.sub(r"--font \S*/deps/", "--font deps/", src))
    print(name, len(cps), "glyphs")

url, g40 = glyphs("mdi_card40")
build("mdi40", 40, g40, url)
url, g20 = glyphs("mdi_card20")
old = re.findall(r"0x(F[0-9A-F]{4})", open(os.path.join(HERE, "mdi20.c")).read().split("\n")[3])
build("mdi20", 20, sorted(set(g20) | set(old)), url)
