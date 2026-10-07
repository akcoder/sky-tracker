#!/usr/bin/env python3
# Caches the CelesTrak files the firmware downloads (sat_net.h) under cache/celestrak/, for
# REAL_SKY=1 ./t4 (a render of the real sky now) and celestrak_server.py. CelesTrak updates every 2 hours and answers
# repeat downloads of an unchanged file with 403, so a file is fetched again only when it is
# more than 2 hours old. The cache is not committed (the data is CelesTrak's to publish).
#   ./fetch_celestrak.py          refresh what is stale
#   ./fetch_celestrak.py --force  refresh everything
import os, sys, time, urllib.request

BASE = "https://celestrak.org"
FILES = {  # cache name: path (as sat_net.h asks for it)
    "iss.csv": "/NORAD/elements/gp.php?CATNR=25544&FORMAT=csv",
    "css.csv": "/NORAD/elements/gp.php?CATNR=48274&FORMAT=csv",
    "visual.csv": "/NORAD/elements/gp.php?GROUP=visual&FORMAT=csv",
    "starlink.csv": "/NORAD/elements/gp.php?GROUP=starlink&FORMAT=csv",
    "gps-ops.csv": "/NORAD/elements/gp.php?GROUP=gps-ops&FORMAT=csv",
    "galileo.csv": "/NORAD/elements/gp.php?GROUP=galileo&FORMAT=csv",
    "glo-ops.csv": "/NORAD/elements/gp.php?GROUP=glo-ops&FORMAT=csv",
    "beidou.csv": "/NORAD/elements/gp.php?GROUP=beidou&FORMAT=csv",
    "geo.csv": "/NORAD/elements/gp.php?GROUP=geo&FORMAT=csv",
    "satcat_visual.json": "/satcat/records.php?GROUP=visual&FORMAT=JSON",
}
MAX_AGE_S = 2 * 3600
# The harness's own made-up lists (csv4_*.csv, test data) stand in when CelesTrak refuses (403)
# or can't be reached and nothing real is cached; a ".fake" marker makes the next run try again.
FAKE = {"iss.csv": "csv4_iss.csv", "css.csv": "csv4_css.csv", "visual.csv": "csv4_sat.csv",
        "starlink.csv": "csv4_starlink.csv", "gps-ops.csv": "csv4_gps.csv", "galileo.csv": "csv4_gal.csv",
        "glo-ops.csv": "csv4_glo.csv", "beidou.csv": "csv4_bds.csv", "geo.csv": "csv4_geo.csv",
        "satcat_visual.json": None}
HERE = os.path.dirname(os.path.abspath(__file__))

def use_fake(name, dst, why):
    src = FAKE.get(name)
    data = open(os.path.join(HERE, src), "rb").read() if src else b"[]"
    open(dst, "wb").write(data)
    open(dst + ".fake", "w").write(why)
    print(f"{name:20s} fake data ({why})")
DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache", "celestrak")

def main():
    force = "--force" in sys.argv
    os.makedirs(DIR, exist_ok=True)
    for name, path in FILES.items():
        dst = os.path.join(DIR, name)
        fake = os.path.exists(dst + ".fake")
        age = time.time() - os.path.getmtime(dst) if os.path.exists(dst) and not fake else None
        if age is not None and age < MAX_AGE_S and not force:
            print(f"{name:20s} cached ({age / 60:.0f} min old)")
            continue
        req = urllib.request.Request(BASE + path, headers={"User-Agent": "sky-tracker-host-test"})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
        except Exception as e:  # keep a real copy (403 = fetched too recently), else the fake list
            if age is not None:
                print(f"{name:20s} FAILED ({e}), keeping the cached copy")
            else:
                use_fake(name, dst, str(e))
            continue
        if len(data) < 100 or data.lstrip()[:1] == b"<":
            if age is not None:
                print(f"{name:20s} FAILED (unexpected answer), keeping the cached copy")
            else:
                use_fake(name, dst, f"unexpected answer: {data[:40]!r}")
            continue
        tmp = dst + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dst)
        if os.path.exists(dst + ".fake"):
            os.remove(dst + ".fake")
        print(f"{name:20s} {len(data):8d} bytes")
    sys.exit(0)

if __name__ == "__main__":
    main()
