#!/usr/bin/env python3
# docs/renders/screens.png (the README's and the install page's picture): the sky map, Jupiter's picture and the
# About page, side by side. Run after make_comps.sh (needs docs/comps/about.png from render_settings.py and the
# renders in docs/renders); not part of make_comps.sh, so the picture changes only when you ask.
import os
from PIL import Image
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "../..")
r = os.path.join(root, "docs", "renders")
parts = [os.path.join(r, "r5_alert0.png"), os.path.join(r, "r10_planet3.png"), os.path.join(root, "docs", "comps", "about.png")]
sheet = Image.new("RGB", (480 * 3 + 40, 480), (17, 17, 17))
for i, f in enumerate(parts):
    sheet.paste(Image.open(f).convert("RGB"), (i * 500, 0))
sheet.save(os.path.join(r, "screens.png"), optimize=True)
print("docs/renders/screens.png written")
