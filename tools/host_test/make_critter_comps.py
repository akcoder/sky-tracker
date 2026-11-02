#!/usr/bin/env python3
"""The axolotl and audrey comps (docs/comps): run `CRITTER_GIF=1 tools/host_test/build.sh run` first, it writes the frames to
tools/host_test/out/renders/axolotl_*.ppm and audrey_*.ppm; this puts them together (PNG strips, GIFs). Not checked in (VER-3)."""
import glob, os
from PIL import Image

R = os.path.join(os.path.dirname(__file__), "out", "renders") + "/"
C = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "comps") + "/"
os.makedirs(C, exist_ok=True)


def frames(prefix):
    return [Image.open(f) for f in sorted(glob.glob(R + prefix + "_[0-9][0-9][0-9].ppm"))]


def gif(fr, name, size=320, colors=64, ms=100):
    q = [f.resize((size, size), Image.LANCZOS).quantize(colors=colors, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for f in fr]
    q[0].save(C + name, save_all=True, append_images=q[1:], duration=ms, loop=0, optimize=True)


def grid(fr, picks, cols, size, name):
    rows = (len(picks) + cols - 1) // cols
    im = Image.new("RGB", (cols * size, rows * size))
    for k, i in enumerate(picks):
        im.paste(fr[min(i, len(fr) - 1)].resize((size, size)), ((k % cols) * size, (k // cols) * size))
    im.save(C + name)


# the axolotl: six poses and colours, and the planned visit on the real map
sheet = Image.new("RGB", (1440, 960))
for i, (v, k) in enumerate([(0, 0), (0, 1), (0, 3), (1, 2), (2, 1), (3, 0)]):
    sheet.paste(Image.open(R + f"axolotl_{v}_{k}.ppm"), ((i % 3) * 480, (i // 3) * 480))
sheet.save(C + "axolotl_strip.png")
sw = frames("axolotl_anim")
if sw:
    gif(sw, "axolotl.gif", ms=80)
v = frames("axolotl_visit")
if v:
    gif(v, "axolotl_visit.gif")
    grid(v, [int(len(v) * k / 16) for k in range(16)], 4, 320, "axolotl_visit_strip.png")

# Audrey: four poses, a close-up of the head, and the feeding (the tongue grabs a satellite)
p = Image.new("RGB", (1920, 480))
for i in range(4):
    p.paste(Image.open(R + f"audrey_{i}.ppm"), (i * 480, 0))
p.resize((1440, 360)).save(C + "audrey_strip.png")
cl = Image.new("RGB", (1800, 440))
for k, (f, box) in enumerate([("audrey_0.ppm", (190, 80, 490, 300)), ("audrey_1.ppm", (240, 40, 540, 260)), ("audrey_2.ppm", (260, 100, 560, 320))]):
    cl.paste(Image.open(R + f).crop(box).resize((600, 440)), (k * 600, 0))
cl.save(C + "audrey_neck_closeup.png")
a = frames("audrey_anim")
if a:
    gif(a, "audrey_anim.gif")
    grid(a, [3, 10, 16, 18, 20, 22, 24, 26, 30, 40], 5, 320, "audrey_anim_strip.png")
print("comps:", ", ".join(sorted(os.path.basename(f) for f in glob.glob(C + "axolotl*") + glob.glob(C + "audrey*"))))
