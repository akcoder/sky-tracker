#!/usr/bin/env python3
# Caches real pictures (and the JPL comet list) for the harness, from the sources the firmware
# uses, under cache/images/ (not committed: NOAA/NASA/Wikimedia publish them). gen_fixtures.py
# uses these in place of its drawn stand-ins. Fetched again when older than a day (--force: now).
import json, os, sys, time, urllib.parse, urllib.request
from datetime import datetime, timezone

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache", "images")
UA = {"User-Agent": "sky-tracker-host-test (https://github.com/akcoder/sky-tracker)"}
WIKI = "https://upload.wikimedia.org/wikipedia/commons/"

def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return r.read()

def suvi():  # the newest frame in NOAA's SUVI 304 list
    lst = json.loads(get("https://services.swpc.noaa.gov/products/animations/suvi-primary-304.json"))
    return get("https://services.swpc.noaa.gov" + lst[-1]["url"])

MOON_AT = "2026-09-25T05:00"  # the hour the harness's Moon view asks for (./t4 prints it)

def moon():  # NASA SVS Dial-A-Moon at that hour, so the picture matches the test's phase
    info = json.loads(get(f"https://svs.gsfc.nasa.gov/api/dialamoon/{MOON_AT}"))
    return get(info["image"]["url"])

def comets():  # the firmware's JPL query (sat_net.h do_comets)
    jd = time.time() / 86400.0 + 2440587.5
    cdata = json.dumps({"AND": ["q|LT|4", "M1|DF", f"tp|RG|{jd - 300:.0f}|{jd + 500:.0f}"]}, separators=(",", ":"))
    return get("https://ssd-api.jpl.nasa.gov/sbdb_query.api?fields=full_name,e,q,tp,om,w,i,M1,K1&sb-kind=c&sb-cdata="
               + urllib.parse.quote(cdata, safe=""))

FILES = {
    "suvi_good.png": suvi,
    "moon_test.jpg": moon,
    "earth_test.jpg": lambda: get("https://cdn.star.nesdis.noaa.gov/GOES18/ABI/FD/GEOCOLOR/678x678.jpg"),
    "region_test.jpg": lambda: get("https://cdn.star.nesdis.noaa.gov/GOES18/ABI/SECTOR/ak/GEOCOLOR/500x500.jpg"),
    "venus_test.jpg": lambda: get(WIKI + "e/e5/Venus-real_color.jpg"),
    "jupiter_test.jpg": lambda: get(WIKI + "thumb/2/2b/Jupiter_and_its_shrunken_Great_Red_Spot.jpg/"
                                           "500px-Jupiter_and_its_shrunken_Great_Red_Spot.jpg"),
    "saturn_test.jpg": lambda: get(WIKI + "thumb/e/e3/Saturn_from_Cassini_Orbiter_%282004-10-06%29.jpg/"
                                          "500px-Saturn_from_Cassini_Orbiter_%282004-10-06%29.jpg"),
    "comet_real.json": comets,
}

def main():
    force = "--force" in sys.argv
    os.makedirs(DIR, exist_ok=True)
    for name, fetch in FILES.items():
        dst = os.path.join(DIR, name)
        if os.path.exists(dst) and not force and time.time() - os.path.getmtime(dst) < 86400:
            print(f"{name:18s} cached")
            continue
        try:
            data = fetch()
        except Exception as e:
            print(f"{name:18s} FAILED ({e})")
            continue
        open(dst + ".tmp", "wb").write(data)
        os.replace(dst + ".tmp", dst)
        print(f"{name:18s} {len(data):8d} bytes")

if __name__ == "__main__":
    main()
