#!/usr/bin/env python3
# Synthetic stand-ins for GOES SUVI 304 frames (the real ones were never kept): 1280 px 8-bit RGB
# PNGs. suvi_good.png: a bright noisy solar disc (> 150 KB, SUVI_MIN_BYTES); suvi_dark.png: a
# frame from Earth's shadow, nearly black and small (< 150 KB, skipped unread).
import os, random, struct, zlib

def png(path, w, h, px):
    raw = b"".join(b"\0" + px(y) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

def good_row(y, n=1280, r=440):
    rnd = random.Random(y)
    out = bytearray()
    for x in range(n):
        d2 = (x - n / 2) ** 2 + (y - n / 2) ** 2
        if d2 < r * r:
            v = 200 + rnd.randrange(56)
            out += bytes((v, int(v * 0.55), int(v * 0.15)))
        else:
            out += bytes((rnd.randrange(12), rnd.randrange(6), 0))
    return bytes(out)

os.makedirs("fixtures", exist_ok=True)
png("fixtures/suvi_good.png", 1280, 1280, good_row)
png("fixtures/suvi_dark.png", 1280, 1280, lambda y: bytes(1280 * 3))
for f in ("suvi_good.png", "suvi_dark.png"):
    print(f, os.path.getsize("fixtures/" + f), "bytes")

# JPEG stand-ins (baseline, as the device's TJpgDec needs) for the other picture sources: a disc
# of the right colour on black at each source's real size. Needs Pillow.
from PIL import Image, ImageDraw, ImageFilter

def disc_jpg(name, size, col, r_frac=0.46, rings=False, land=False):
    im = Image.new("RGB", (size, size), (0, 0, 0))
    d = ImageDraw.Draw(im)
    c, r = size / 2, size * r_frac
    if rings:
        d.ellipse((c - 2.1 * r, c - 0.45 * r, c + 2.1 * r, c + 0.45 * r), outline=(200, 180, 140), width=max(3, size // 60))
    d.ellipse((c - r, c - r, c + r, c + r), fill=col)
    rnd = random.Random(size)
    for _ in range(40 if land else 25):  # craters / clouds / bands, so it isn't flat
        x, y, s = c + rnd.uniform(-0.7, 0.7) * r, c + rnd.uniform(-0.7, 0.7) * r, rnd.uniform(0.03, 0.12) * r
        shade = tuple(max(0, min(255, v + rnd.randint(-40, 40))) for v in col)
        d.ellipse((x - s, y - s, x + s, y + s), fill=shade)
    im.filter(ImageFilter.GaussianBlur(1)).save("fixtures/" + name, "JPEG", quality=85, progressive=False)
    print(name, os.path.getsize("fixtures/" + name), "bytes")

disc_jpg("moon_test.jpg", 730, (170, 170, 165))
disc_jpg("earth_test.jpg", 678, (40, 90, 170), r_frac=0.49, land=True)
disc_jpg("region_test.jpg", 500, (60, 100, 70), r_frac=0.7, land=True)
disc_jpg("venus_test.jpg", 500, (225, 205, 160))
disc_jpg("jupiter_test.jpg", 500, (200, 170, 130))
disc_jpg("saturn_test.jpg", 500, (215, 190, 140), r_frac=0.22, rings=True)

# The real pictures (and JPL's comet list), when fetch_images.py has cached them, replace the
# stand-ins above: the tests and renders then show the actual Sun, Moon, Earth and planets.
import shutil
real = os.path.join("cache", "images")
for name in ("suvi_good.png", "moon_test.jpg", "earth_test.jpg", "region_test.jpg", "venus_test.jpg",
             "jupiter_test.jpg", "saturn_test.jpg", "comet_real.json"):
    if os.path.exists(os.path.join(real, name)):
        shutil.copyfile(os.path.join(real, name), os.path.join("fixtures", name))
        print(name, "real")
