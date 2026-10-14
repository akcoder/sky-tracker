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
rm -rf out/renders/* out/rgif out/bgif out/sgif out/ugif out/dgif out/agif out/fgif
RENDER_REAL=1 ROCKET_GIF=1 BOOT_GIF=1 SPLASH_GIF=1 UNDOCK_GIF=1 DOCK_GIF=1 ABOUT_GIF=1 REENTRY_GIF=1 ./t4 > out/t4_real.log 2>&1 || true   # the renders, on the cached CelesTrak lists
./make_anims.py   # every animation: docs/renders/<name>.gif and <name>_strip.png
./render_settings.py   # the settings tabs, from the firmware's own widget code (docs/comps/settings_*.png)
python3 - <<'PY'
from PIL import Image
import glob, os
out = "../../docs/comps"
rdir = "../../docs/renders"   # checked in
os.makedirs(rdir, exist_ok=True)
n = 0
for f in sorted(glob.glob("out/renders/*.ppm")):  # the test suite's screens (./t4)
    Image.open(f).save(f"{rdir}/" + os.path.basename(f)[:-4] + ".png", optimize=True)
    n += 1
print("comps:", sorted(os.listdir(out)), f"+ {n} renders")
PY
