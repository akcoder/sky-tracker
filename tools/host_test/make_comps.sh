#!/usr/bin/env bash
# Renders the design comps into docs/comps (PNG): the settings tabs (setcomp.cpp) and, when
# ./t4 has written them, the boot-screen launch (BOOT_GIF=1 ./t4) and the test suite's screen
# renders (out/renders, into docs/comps/renders). Needs Pillow.
set -euo pipefail
cd "$(dirname "$0")"
./fetch_images.py >/dev/null || true   # the real Sun, Moon, Earth, planets (and comets)
python3 gen_fixtures.py >/dev/null
./build.sh >/dev/null
BOOT_GIF=1 ./t4 > out/t4.log 2>&1 || { tail -20 out/t4.log; exit 1; }   # the renders and the boot launch
CF="-O2 -w -DLV_CONF_INCLUDE_SIMPLE -I. -Ideps -Ideps/lvgl -I../../components/sky_tracker"
for f in mono12 mono15; do
  [ -f build/$f.c ] || npx --yes lv_font_conv --font deps/RobotoMono.ttf -r 0x20-0x7E,0xB0 --size ${f#mono} --bpp 4 \
    --format lvgl --no-compress --lv-font-name $f -o build/$f.c >/dev/null
  [ -f build/$f.o ] || cc $CF -c build/$f.c -o build/$f.o
done
cc $CF -c mdi20.c -o build/mdi20.o
c++ -std=gnu++20 $CF setcomp.cpp build/mdi20.o build/mono12.o build/mono15.o build/mono16.o build/liblvgl.a -lm -o setcomp
for t in satellites celestial alerts; do ./setcomp C $t >/dev/null; done
python3 - <<'PY'
from PIL import Image
import glob, os
out = "../../docs/comps"
for t in ("satellites", "celestial", "alerts"):
    Image.open(f"out/setcomp_C_{t}.ppm").save(f"{out}/settings_{t}.png")
fs = sorted(glob.glob("out/bgif/f*.ppm"))
if fs:
    pick = [fs[int(len(fs) * k)] for k in (0.1, 0.35, 0.6, 0.85)]
    sheet = Image.new("RGB", (480 * 4 + 30, 480), (30, 30, 30))
    for i, f in enumerate(pick):
        sheet.paste(Image.open(f), (i * 490, 0))
    sheet.save(f"{out}/boot_launch_strip.png")
    fr = [Image.open(f).convert("P", palette=Image.ADAPTIVE, colors=128) for f in fs[::2]]
    fr[0].save(f"{out}/boot_launch.gif", save_all=True, append_images=fr[1:], duration=66, loop=0, optimize=True)
os.makedirs(f"{out}/renders", exist_ok=True)
n = 0
for f in sorted(glob.glob("out/renders/*.ppm")):  # the test suite's screens (./t4)
    Image.open(f).save(f"{out}/renders/" + os.path.basename(f)[:-4] + ".png", optimize=True)
    n += 1
print("comps:", sorted(os.listdir(out)), f"+ {n} renders")
PY
