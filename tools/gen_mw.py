# Builds sky_mw.h: the Milky Way's brightness on a 1-degree RA/Dec grid, from d3-celestial
# 0.7.35 data/mw.json (BSD-3-Clause; five nested outlines ol1..ol5, faintest first).
# Each outline is filled by even-odd crossings along each meridian from the south pole
# (two of ol1's rings wrap all the way round the sky), the five fills are summed (0..5),
# smoothed (Gaussian, 0.8 degree) and scaled to 0..255.
import json, math
import numpy as np
from scipy.ndimage import gaussian_filter
d = json.load(open('package/data/mw.json'))
W, H = 360, 180
lons = -179.5 + np.arange(W)          # cell centres
lats = -89.5 + np.arange(H)
total = np.zeros((H, W))
for f in d['features']:
    cross = [[] for _ in range(W)]
    for ring in f['geometry']['coordinates'][0]:
        for (a, la), (b, lb) in zip(ring, ring[1:] + ring[:1]):
            dl = b - a
            if dl > 180: dl -= 360
            if dl < -180: dl += 360
            if dl == 0: continue
            lo, hi = (a, a + dl) if dl > 0 else (a + dl, a)
            # columns whose centre lies in [lo, hi) (mod 360)
            for k in range(int(math.floor(lo - 0.5)), int(math.ceil(hi + 0.5)) + 1):
                c = k + 0.5
                if lo <= c < hi:
                    t = (c - a) / dl
                    col = int((c + 180) % 360)
                    cross[col].append(la + t * (lb - la))
    fill = np.zeros((H, W))
    for col in range(W):
        cs = sorted(cross[col])
        for j in range(0, len(cs) - 1, 2):
            fill[(lats >= cs[j]) & (lats < cs[j + 1]), col] = 1
        if len(cs) % 2:
            print('odd crossings', f['id'], col, len(cs))
    total += fill
    print(f['id'], int(fill.sum()), 'cells')
sm = gaussian_filter(total, 0.8, mode=['nearest', 'wrap'])
v = np.clip(np.round(sm / 5 * 255), 0, 255).astype(np.uint8)
# RA order: column i = RA i+0.5 degrees (lon -> RA, negative lon + 360)
ra_cols = np.array([int(((i + 0.5) + 180) % 360 - 0.5 + 0.5) for i in range(W)])
out = np.zeros_like(v)
for i in range(W):
    ra = (lons[i]) % 360          # centre RA of this column
    out[:, int(ra - 0.5) % W] = v[:, i]
np.save('/tmp/claude-0/mw.npy', out)
with open('/home/claude/sky/sky_mw.h', 'w') as o:
    o.write('#pragma once\n// UI-64 Milky Way (gen_mw.py from d3-celestial 0.7.35 data/mw.json, BSD-3-Clause):\n'
            '// brightness 0..255 on a 1-degree grid, row = Dec -89.5 + r, column = RA 0.5 + c (degrees).\n'
            '#include <cstdint>\nnamespace sat {\nnamespace sky {\nconstexpr int MW_W = 360, MW_H = 180;\n'
            'static const uint8_t MW[MW_H * MW_W] = {\n')
    flat = out.flatten()
    for i in range(0, len(flat), 30):
        o.write('    ' + ','.join(str(x) for x in flat[i:i + 30]) + ',\n')
    o.write('};\n}  // namespace sky\n}  // namespace sat\n')
print('nonzero', int((out > 0).sum()), 'max', out.max())
