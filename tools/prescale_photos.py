#!/usr/bin/env python3
"""components/sky_tracker/sky_photos.h: the five planet photos scaled to 5/6 (the Planets tab shows a 300 px picture,
the photos were made for 360): decoded, resampled with Lanczos, re-encoded as baseline JPEG, and get() updated.
Run once on the 360 px originals (it refuses if the sizes are not the original ones: the originals are in git).
  tools/prescale_photos.py"""
import io, os, re, sys
from PIL import Image

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "components", "sky_tracker", "sky_photos.h")
ORIG = {"MERCURY": (360, 360), "VENUS": (360, 360), "MARS": (360, 360), "JUPITER": (260, 260), "SATURN": (360, 184)}
NUM, DEN = 5, 6
src = open(PATH).read()
for name, (w, h) in ORIG.items():
    if f"{{{name}, sizeof({name}), {w}, {h}}}" not in src:
        sys.exit(f"{name} is not {w}x{h} in get(): already scaled?")
out = src
for name, (w, h) in ORIG.items():
    m = re.search(rf"static const uint8_t {name}\[\] = \{{(.*?)\}};", out, re.S)
    data = bytes(int(x) for x in re.findall(r"\d+", m.group(1)))
    im = Image.open(io.BytesIO(data)).convert("RGB")
    assert im.size == (w, h), (name, im.size)
    nw, nh = round(w * NUM / DEN), round(h * NUM / DEN)
    im = im.resize((nw, nh), Image.LANCZOS)
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=92, subsampling=0, optimize=False, progressive=False)
    b = buf.getvalue()
    print(name, f"{w}x{h} ({len(data)} B) -> {nw}x{nh} ({len(b)} B)")
    rows = "\n".join(",".join(str(x) for x in b[i:i + 40]) + "," for i in range(0, len(b), 40))
    out = out[:m.start()] + f"static const uint8_t {name}[] = {{\n{rows}\n}};" + out[m.end():]
    out = out.replace(f"{{{name}, sizeof({name}), {w}, {h}}}", f"{{{name}, sizeof({name}), {nw}, {nh}}}")
out = out.replace("// Commons, fetched once and scaled in the browser on 2026-09-29;", "// Commons, fetched once and scaled in the browser on 2026-09-29, then scaled to 5/6 for the Planets tab's 300 px\n// picture by tools/prescale_photos.py;")
open(PATH, "w").write(out)
