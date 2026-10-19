#!/usr/bin/env python3
"""UI-77b: find a public-domain JPEG on Wikimedia Commons for each "On this day" event (sky_history.h order) from
tools/hist_queries.txt, check that the 330 px thumbnail is a baseline JPEG the device's decoder can read, and write
tools/hist_pics.json (one record per event: title, thumb path, licence, credit) for review and for sky_histpic.h.
  tools/gen_hist_pics.py            search every event not yet in hist_pics.json
  tools/gen_hist_pics.py 12 15      redo events 12 and 15 (ignores what was found)"""
import io, json, os, re, sys, time, urllib.parse, urllib.request
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
UA = "sky-tracker-dev/1.0 (https://github.com/akcoder/sky-tracker)"
API = "https://commons.wikimedia.org/w/api.php"
OK_LIC = re.compile(r"^(public domain|pd[- ]|cc0|cc zero)", re.I)
WIDTH = 330


def get(url, binary=False):
    for k in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": UA}), timeout=30) as r:
                d = r.read()
            return d if binary else json.loads(d)
        except Exception as e:
            time.sleep(2 + 3 * k)
    return None


def candidates(q):
    p = {"action": "query", "generator": "search", "gsrnamespace": 6, "gsrsearch": q + " filetype:bitmap", "gsrlimit": 20,
         "prop": "imageinfo", "iiprop": "url|mime|size|extmetadata", "iiurlwidth": WIDTH, "format": "json"}
    d = get(API + "?" + urllib.parse.urlencode(p))
    if not d or "query" not in d:
        return []
    out = []
    for pg in sorted(d["query"]["pages"].values(), key=lambda x: x.get("index", 99)):
        ii = (pg.get("imageinfo") or [{}])[0]
        m = ii.get("extmetadata", {})
        lic = m.get("LicenseShortName", {}).get("value", "")
        if ii.get("mime") != "image/jpeg" or not OK_LIC.match(lic) or ii.get("width", 0) < WIDTH or ii.get("height", 0) < 200:
            continue
        ar = ii["width"] / ii["height"]
        if not 0.5 <= ar <= 2.2:
            continue
        credit = re.sub(r"<[^>]+>", "", m.get("Artist", {}).get("value", "") or m.get("Credit", {}).get("value", ""))[:60]
        out.append({"title": pg["title"], "thumb": ii["thumburl"].split("?")[0], "lic": lic, "credit": credit.strip(),
                    "w": ii["width"], "h": ii["height"]})
    return out


def usable(c):
    d = get(c["thumb"], True)
    if not d:
        return None
    try:
        im = Image.open(io.BytesIO(d))
        if im.format != "JPEG" or im.info.get("progressive") or im.info.get("progression"):
            return None
        if im.mode not in ("RGB", "L"):
            return None
        c["bytes"] = len(d)
        c["tw"], c["th"] = im.size
        if len(d) > 150000:
            return None
        return d
    except Exception:
        return None


def main():
    qs = [l.strip() for l in open(os.path.join(HERE, "hist_queries.txt")) if l.strip()]
    ov = os.path.join(HERE, "hist_overrides.txt")  # "idx query": a better search for an event (a wrong or missing picture)
    if os.path.exists(ov):
        for l in open(ov):
            if l.strip():
                k, q = l.split(None, 1)
                qs[int(k)] = q.strip()
    path = os.path.join(HERE, "hist_pics.json")
    res = json.load(open(path)) if os.path.exists(path) else {}
    todo = [int(a) for a in sys.argv[1:]] or [i for i in range(len(qs)) if str(i) not in res]
    os.makedirs(os.path.join(HERE, "out", "hist"), exist_ok=True)
    for i in todo:
        pick = None
        for c in candidates(qs[i]):
            time.sleep(0.4)
            d = usable(c)
            if d:
                pick = c
                open(os.path.join(HERE, "out", "hist", "%03d.jpg" % i), "wb").write(d)
                break
        res[str(i)] = pick
        print(i, qs[i], "->", pick and (pick["title"], pick["lic"], pick["bytes"]), flush=True)
        json.dump(res, open(path, "w"), indent=1)
        time.sleep(0.4)


main()
