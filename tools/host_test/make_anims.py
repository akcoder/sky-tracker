#!/usr/bin/env python3
# docs/renders: every device animation as a GIF and a four-frame strip, from the frames ./t4 writes
# when run with the animation flags (ROCKET_GIF SPLASH_GIF UNDOCK_GIF BOOT_GIF ABOUT_GIF REENTRY_GIF):
#   ROCKET_GIF=1 SPLASH_GIF=1 UNDOCK_GIF=1 BOOT_GIF=1 ABOUT_GIF=1 REENTRY_GIF=1 ./t4 && ./make_anims.py
import glob, os
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
R = os.path.join(HERE, "../../docs/renders")


def pal(f, colors, box):
    return Image.open(f).convert("RGB").crop(box).convert("P", palette=Image.ADAPTIVE, colors=colors)


# name, frame folder, every n-th frame, ms per frame, colours, strip positions, hold first/last (ms), crop box
FULL = (0, 0, 480, 480)
ANIMS = [
    ("launch", "rgif", 2, 60, 96, (0.15, 0.35, 0.55, 0.8), None, FULL),          # UI-69: lift-off over the map
    ("splashdown", "sgif", 2, 60, 128, (0.2, 0.5, 0.62, 0.85), None, FULL),      # UI-69m
    ("undocking", "ugif", 3, 60, 128, (0.15, 0.4, 0.65, 0.9), (990, 790), FULL),  # UI-69f
    ("boot_launch", "bgif", 2, 66, 128, (0.1, 0.35, 0.6, 0.85), None, FULL),     # UI-69k: a launch over the boot screen
    ("about_meteor", "agif", 3, 100, 96, (0.1, 0.3, 0.55, 0.8), None, (0, 40, 270, 170)),      # UI-67a: the shooting star over the logo
    ("reentry", "fgif", 1, 40, 128, (0.15, 0.4, 0.6, 0.85), None, FULL),          # UI-79: the re-entry fireball
]
for name, src, step, ms, colors, picks, hold, box in ANIMS:
    fs = sorted(glob.glob(os.path.join(OUT, src, "f*.ppm")))
    if not fs:
        print(name, ": no frames in out/" + src)
        continue
    w, h = box[2] - box[0], box[3] - box[1]
    sheet = Image.new("RGB", (w * 4 + 30, h), (30, 30, 30))
    for i, f in enumerate([fs[int(len(fs) * k)] for k in picks]):
        sheet.paste(Image.open(f).convert("RGB").crop(box), (i * (w + 10), 0))
    sheet.save(os.path.join(R, name + "_strip.png"))
    fr = [pal(f, colors, box) for f in fs[::step]]
    d = [ms] * len(fr)
    if hold:
        d[0], d[-1] = hold
    fr[0].save(os.path.join(R, name + ".gif"), save_all=True, append_images=fr[1:], duration=d, loop=0, optimize=True)
    print(name, len(fr), "frames,", os.path.getsize(os.path.join(R, name + ".gif")) // 1024, "KB")
