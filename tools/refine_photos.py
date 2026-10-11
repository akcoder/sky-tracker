#!/usr/bin/env python3
"""components/sky_tracker/sky_photos.h: Mars, Jupiter and Saturn made as large as the other planets on the Planets tab
(Mercury and Venus fill ~275 px of the 300 px frame; Mars filled 250, Jupiter 201, Saturn's rings 245).
Mars and Saturn are cropped tight from the original 360 px photos (git cb0e1db: no upscaling); Jupiter from Commons' 960 px
Hubble picture. Baseline JPEG, re-encoded; get() updated.  tools/refine_photos.py   (needs the network and git)"""
import io, os, re, subprocess, urllib.request
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PATH = os.path.join(ROOT, "components", "sky_tracker", "sky_photos.h")
UA = "sky-tracker/1.0 (https://github.com/akcoder/sky-tracker)"
JUP = "https://upload.wikimedia.org/wikipedia/commons/thumb/2/2b/Jupiter_and_its_shrunken_Great_Red_Spot.jpg/960px-Jupiter_and_its_shrunken_Great_Red_Spot.jpg"


def original(name):
    src = subprocess.check_output(["git", "-C", ROOT, "show", "cb0e1db:components/sky_tracker/sky_photos.h"], text=True)
    m = re.search(rf"static const uint8_t {name}\[\] = \{{(.*?)\}};", src, re.S)
    return Image.open(io.BytesIO(bytes(int(x) for x in re.findall(r"\d+", m.group(1))))).convert("RGB")


def bbox(im, t=40):
    return im.convert("L").point(lambda v: 255 if v > t else 0).getbbox()


def square(im, frame, margin):
    x0, y0, x1, y1 = bbox(im)
    cx, cy, r = (x0 + x1) / 2, (y0 + y1) / 2, max(x1 - x0, y1 - y0) / 2
    h = r + margin
    return im.crop((round(cx - h), round(cy - h), round(cx + h), round(cy + h))).resize((frame, frame), Image.LANCZOS)


def wide(im, width, margin):
    x0, y0, x1, y1 = bbox(im)
    c = im.crop((max(0, x0 - margin), max(0, y0 - margin), min(im.size[0], x1 + margin), min(im.size[1], y1 + margin)))
    return c.resize((width, round(c.size[1] * width / c.size[0])), Image.LANCZOS)


jup = Image.open(io.BytesIO(urllib.request.urlopen(urllib.request.Request(JUP, headers={"User-Agent": UA}), timeout=60).read())).convert("RGB")
PICS = {"MARS": square(original("MARS"), 300, 3), "JUPITER": square(jup, 250, 8), "SATURN": wide(original("SATURN"), 298, 2)}
out = open(PATH).read()
for name, im in PICS.items():
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=92, subsampling=0, optimize=False, progressive=False)
    b = buf.getvalue()
    print(name, im.size, len(b), "bytes")
    m = re.search(rf"static const uint8_t {name}\[\] = \{{(.*?)\}};", out, re.S)
    rows = "\n".join(",".join(str(x) for x in b[i:i + 40]) + "," for i in range(0, len(b), 40))
    out = out[:m.start()] + f"static const uint8_t {name}[] = {{\n{rows}\n}};" + out[m.end():]
    out = re.sub(rf"\{{{name}, sizeof\({name}\), \d+, \d+\}}", f"{{{name}, sizeof({name}), {im.size[0]}, {im.size[1]}}}", out)
open(PATH, "w").write(out)
