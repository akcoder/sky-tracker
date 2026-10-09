#!/usr/bin/env bash
# Renders the design comps into docs/comps (PNG, not checked in): the settings tabs (render_settings.py) and, when
# ./t4 has written them, the boot-screen launch and the test suite's screen renders (out/renders,
# into docs/renders, which is checked in), all drawn on the cached CelesTrak lists (RENDER_REAL=1). Needs Pillow.
set -euo pipefail
cd "$(dirname "$0")"
./fetch_images.py >/dev/null || true   # the real Sun, Moon, Earth, planets (and comets)
python3 gen_fixtures.py >/dev/null
./build.sh >/dev/null
./fetch_celestrak.py >/dev/null || true   # the real orbital lists (fake data where CelesTrak refuses)
./t4 > out/t4.log 2>&1 || { tail -20 out/t4.log; exit 1; }   # the checks, on the test data
rm -rf out/renders/* out/bgif out/sgif
RENDER_REAL=1 BOOT_GIF=1 SPLASH_GIF=1 ./t4 > out/t4_real.log 2>&1 || true   # the renders, on the cached CelesTrak lists
./render_settings.py   # the settings tabs, from the firmware's own widget code (docs/comps/settings_*.png)
python3 - <<'PY'
from PIL import Image
import glob, os
out = "../../docs/comps"
fs = sorted(glob.glob("out/bgif/f*.ppm"))
if fs:
    pick = [fs[int(len(fs) * k)] for k in (0.1, 0.35, 0.6, 0.85)]
    sheet = Image.new("RGB", (480 * 4 + 30, 480), (30, 30, 30))
    for i, f in enumerate(pick):
        sheet.paste(Image.open(f), (i * 490, 0))
    sheet.save(f"{out}/boot_launch_strip.png")
    fr = [Image.open(f).convert("P", palette=Image.ADAPTIVE, colors=128) for f in fs[::2]]
    fr[0].save(f"{out}/boot_launch.gif", save_all=True, append_images=fr[1:], duration=66, loop=0, optimize=True)
fs = sorted(glob.glob("out/sgif/f*.ppm"))  # UI-69m the splashdown
if fs:
    pick = [fs[int(len(fs) * k)] for k in (0.2, 0.5, 0.62, 0.85)]
    sheet = Image.new("RGB", (480 * 4 + 30, 480), (30, 30, 30))
    for i, f in enumerate(pick):
        sheet.paste(Image.open(f), (i * 490, 0))
    sheet.save(f"{out}/splashdown_strip.png")
    fr = [Image.open(f).convert("P", palette=Image.ADAPTIVE, colors=128) for f in fs[::2]]
    fr[0].save(f"{out}/splashdown.gif", save_all=True, append_images=fr[1:], duration=66, loop=0, optimize=True)
rdir = "../../docs/renders"
os.makedirs(rdir, exist_ok=True)
n = 0
for f in sorted(glob.glob("out/renders/*.ppm")):  # the test suite's screens (./t4)
    Image.open(f).save(f"{rdir}/" + os.path.basename(f)[:-4] + ".png", optimize=True)
    n += 1
print("comps:", sorted(os.listdir(out)), f"+ {n} renders")
PY
