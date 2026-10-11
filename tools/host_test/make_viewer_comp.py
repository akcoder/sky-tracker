#!/usr/bin/env python3
# Design comps for the picture viewer (not firmware renders): docs/comps/viewer_swipe.png (swipe between
# pictures, page dots, title chip) and docs/comps/viewer_planets.png (a strip of the planets, Earth among them).
# Built from the checked-in viewer renders, so the pictures are the firmware's own.
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
R = os.path.join(HERE, "../../docs/renders")
OUT = os.path.join(HERE, "../../docs/comps")
os.makedirs(OUT, exist_ok=True)
MONO = os.path.join(HERE, "deps/gfonts/RobotoMono-400.ttf")
ROBO = os.path.join(HERE, "deps/gfonts/Roboto-300.ttf")
BG = (0, 8, 24)
F = lambda p, s: ImageFont.truetype(p, s)
ORANGE, BLUE, CHIP, DIM, WHITE = (255, 138, 31), (45, 91, 208), (26, 37, 71), (126, 139, 179), (255, 255, 255)


def img(n):
    return Image.open(os.path.join(R, n + ".png")).convert("RGB")


def rbox(d, xy, fill, r=10, outline=None):
    d.rounded_rectangle(xy, radius=r, fill=fill, outline=outline)


def text(d, xy, t, font, fill, anchor="la"):
    d.text(xy, t, font=font, fill=fill, anchor=anchor)


def picon(n, size):
    """The firmware's own 20 px planet picture (sky_picons.h BIG_PX, ARGB), enlarged."""
    import re
    t = open(os.path.join(HERE, "../../components/sky_tracker/sky_picons.h")).read()
    body = t[t.index("BIG_PX"):]
    rows = re.findall(r"\{([^{}]*)\}", body)[:5]
    px = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{8}", rows[n])][:400]
    im = Image.new("RGBA", (20, 20))
    im.putdata([((v >> 16) & 255, (v >> 8) & 255, v & 255, (v >> 24) & 255) for v in px])
    return im.resize((size, size), Image.LANCZOS)


def disc_icon(src, box, size, bg=(14, 20, 44)):
    c = Image.new("RGB", (size, size), bg)
    p = src.crop(box)
    p.thumbnail((size - 6, size - 6))
    c.paste(p, ((size - p.width) // 2, (size - p.height) // 2))
    m = Image.new("L", (size * 4, size * 4), 0)
    ImageDraw.Draw(m).ellipse((0, 0, size * 4 - 1, size * 4 - 1), fill=255)
    return c, m.resize((size, size), Image.LANCZOS)


# ---- 1: swipe between pictures, page dots, a title chip ------------------------------------
def swipe():
    im = Image.new("RGB", (480, 480), BG)
    earth = img("r10_earth").crop((56, 50, 424, 410))      # the globe, 368 x 360
    im.paste(earth, (56, 46))
    d = ImageDraw.Draw(im, "RGBA")
    rbox(d, (10, 8, 126, 44), (26, 37, 71, 255), 18)                          # title chip: what is showing
    text(d, (68, 26), "Earth", F(MONO, 18), ORANGE, "mm")
    rbox(d, (394, 8, 470, 44), (26, 37, 71, 255), 18)                         # close
    text(d, (432, 26), "Close", F(MONO, 16), WHITE, "mm")
    # edge hints: the neighbours, faint
    text(d, (8, 226), "‹", F(ROBO, 44), (126, 139, 179, 150), "lm")
    text(d, (472, 226), "›", F(ROBO, 44), (126, 139, 179, 150), "rm")
    text(d, (8, 262), "Moon", F(MONO, 12), (126, 139, 179, 150), "lm")
    text(d, (472, 262), "UAF", F(MONO, 12), (126, 139, 179, 150), "rm")
    # sub-views of this picture, then the caption, then the page dots
    rbox(d, (130, 414, 238, 442), BLUE, 10)
    text(d, (184, 428), "Globe", F(MONO, 14), ORANGE, "mm")
    rbox(d, (244, 414, 352, 442), (26, 37, 71, 255), 10)
    text(d, (298, 428), "Alaska", F(MONO, 14), WHITE, "mm")
    text(d, (240, 454), "GOES-18 05:30, 16m ago", F(MONO, 14), (201, 211, 242), "mm")
    names = ["Sun", "Moon", "Earth", "Planets", "UAF"]
    x0 = 240 - 2 * 26
    for i, n in enumerate(names):
        cx = x0 + i * 26
        if i == 2:
            rbox(d, (cx - 11, 468, cx + 11, 476), ORANGE, 4)
        else:
            d.ellipse((cx - 4, 468, cx + 4, 476), fill=(90, 104, 150, 255))
    im.save(os.path.join(OUT, "viewer_swipe.png"))


# ---- 4: the planets as a strip under the picture -------------------------------------------
def planets():
    im = Image.new("RGB", (480, 480), BG)
    d = ImageDraw.Draw(im, "RGBA")
    tabs = ["Sun", "Moon", "Planets", "UAF"]
    x = 8
    for i, t in enumerate(tabs):
        w = 96 if i != 2 else 112
        rbox(d, (x, 4, x + w, 40), BLUE if i == 2 else (26, 37, 71, 255), 10)
        text(d, (x + w // 2, 22), t, F(MONO, 16), ORANGE if i == 2 else WHITE, "mm")
        x += w + 6
    rbox(d, (x, 4, 472, 40), (26, 37, 71, 255), 10)
    text(d, ((x + 472) // 2, 22), "×", F(ROBO, 28), WHITE, "mm")
    jup = img("r10_planet3_jupiter").crop((60, 52, 420, 372))     # Jupiter and its moons
    jup = jup.resize((int(360 * 0.86), int(320 * 0.86)), Image.LANCZOS)
    im.paste(jup, (240 - jup.width // 2, 46))
    text(d, (240, 336), "Jupiter: 32.9\" across  I Io  E Europa  G Ganymede  C Callisto", F(MONO, 12), (201, 211, 242), "mm")
    # the strip: the planets above the horizon now, as small discs with their phase
    srcs = [("Mercury", img("r10_planet0_mercury"), (150, 60, 410, 390)), ("Venus", img("r10_planet1_venus"), (170, 52, 372, 330)),
            ("Earth", img("r10_earth"), (62, 52, 420, 408)), ("Mars", None, None),
            ("Jupiter", img("r10_planet3_jupiter"), (118, 62, 362, 302)), ("Saturn", img("r10_planet4_saturn"), (90, 168, 390, 292))]
    w, gap = 72, 6
    x0 = 240 - (len(srcs) * w + (len(srcs) - 1) * gap) // 2
    for i, (n, s, box) in enumerate(srcs):
        x = x0 + i * (w + gap)
        sel = n == "Jupiter"
        rbox(d, (x, 372, x + w, 450), BLUE if sel else (26, 37, 71, 255), 12, outline=ORANGE if sel else None)
        if s is None:                                   # Mars: the firmware's own small picture
            pi = picon(2, 44)
            icon = Image.new("RGB", (44, 44), (14, 20, 44))
            icon.paste(pi, (0, 0), pi)
            mask = Image.new("L", (44, 44), 0)
            ImageDraw.Draw(mask).ellipse((0, 0, 43, 43), fill=255)
        else:
            icon, mask = disc_icon(s, box, 44)
        im.paste(icon, (x + (w - 44) // 2, 380), mask)
        d = ImageDraw.Draw(im, "RGBA")
        text(d, (x + w // 2, 435), n, F(MONO, 12), ORANGE if sel else WHITE, "mm")
    im.save(os.path.join(OUT, "viewer_planets.png"))


swipe()
planets()
print("docs/comps/viewer_swipe.png, viewer_planets.png")
