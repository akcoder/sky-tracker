#!/usr/bin/env python3
"""The dinosaur's head-down poses (a hunting walk bites its target): DIP_HALF and DIP_OPEN, made from the STAND body
with the head of ROAR_HALF / ROAR_OPEN turned down about the neck. Reads and rewrites components/sky_tracker/sky_dino_data.h
(it keeps POSE0..POSE6 as they are, replaces POSE7/8 and the POSES table). `--preview out.png` writes the poses on a sheet."""
import re, sys
from PIL import Image

W, H = 144, 96
PIVOT = (113, 32)      # the neck: the head turns about this point
SHIFT = (-11, 2)       # and moves back a little (the sprite ends at 144)
ANGLE = 36             # degrees, nose down
CUT_Y, CUT_X = 34, 98  # the head is what is above CUT_Y and right of CUT_X
path = "components/sky_tracker/sky_dino_data.h"
src = open(path).read()
head_end = src.index("inline const uint8_t *const POSES[")
tail_start = src.index("\n", head_end) + 1


def parse(i):
    m = re.search(r"POSE%d\[41472\] = \{(.*?)\};" % i, src, re.S)
    b = bytes(int(x) for x in re.findall(r"\d+", m.group(1)))
    im = Image.new("RGBA", (W, H))
    px = []
    for k in range(W * H):
        v = b[2 * k] | (b[2 * k + 1] << 8)
        px.append((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, b[W * H * 2 + k]))
    im.putdata(px)
    return im


def head_of(im):
    h = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    h.paste(im.crop((CUT_X, 0, W, CUT_Y + 10)), (CUT_X, 0))  # (a little of the neck too: it overlaps the body, so there is no seam)
    return h


def dip(body, head_src, angle=ANGLE):
    base = body.copy()
    px = base.load()
    for y in range(CUT_Y):
        for x in range(CUT_X, W):
            px[x, y] = (0, 0, 0, 0)
    head = head_of(head_src).rotate(-angle, resample=Image.BICUBIC, center=PIVOT, translate=SHIFT)  # PIL turns anticlockwise: minus is nose down
    out = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out.alpha_composite(base)
    out.alpha_composite(head)
    return out


def encode(im):
    rgb, a = bytearray(), bytearray()
    for r, g, b, al in im.getdata():
        if al == 0:
            r = g = b = 0
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb += bytes((v & 255, v >> 8))
        a.append(al)
    return bytes(rgb + a)


def arr(name, data):
    lines = [", ".join(str(x) for x in data[i:i + 24]) + "," for i in range(0, len(data), 24)]
    return "inline constexpr uint8_t %s[41472] = {\n  %s\n};\n\n" % (name, "\n  ".join(lines))


stand, half, opn = parse(4), parse(5), parse(6)
poses = [dip(stand, half), dip(stand, opn)]
if "--preview" in sys.argv:
    out = sys.argv[sys.argv.index("--preview") + 1]
    sheet = Image.new("RGBA", (W * 2 * 3, H * 2), (40, 60, 90, 255))
    for i, im in enumerate([stand] + poses):
        big = im.resize((W * 2, H * 2), Image.NEAREST)
        sheet.paste(big, (i * W * 2, 0), big)
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
    sheet.save(out)
    sys.exit(0)
body = src[:head_end]
body = re.sub(r"inline constexpr uint8_t POSE[78]\[41472\] = \{.*?\};\n\n", "", body, flags=re.S)
body += arr("POSE7", encode(poses[0])) + arr("POSE8", encode(poses[1]))
body += "inline const uint8_t *const POSES[9] = {POSE0, POSE1, POSE2, POSE3, POSE4, POSE5, POSE6, POSE7, POSE8};\n"
open(path, "w").write(body + src[tail_start:])
print("wrote POSE7 (DIP_HALF), POSE8 (DIP_OPEN)")
