#!/usr/bin/env python3
# Caches the CelesTrak files the firmware downloads (sat_net.h) under cache/celestrak/, for
# REAL_SKY=1 ./t4 (a render of the real sky now). CelesTrak updates every 2 hours and answers
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
DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cache", "celestrak")

def main():
    force = "--force" in sys.argv
    os.makedirs(DIR, exist_ok=True)
    failed = 0
    for name, path in FILES.items():
        dst = os.path.join(DIR, name)
        age = time.time() - os.path.getmtime(dst) if os.path.exists(dst) else None
        if age is not None and age < MAX_AGE_S and not force:
            print(f"{name:20s} cached ({age / 60:.0f} min old)")
            continue
        req = urllib.request.Request(BASE + path, headers={"User-Agent": "sky-tracker-host-test"})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                data = r.read()
        except Exception as e:  # keep the old copy (403 = fetched too recently)
            print(f"{name:20s} FAILED ({e}){', keeping the cached copy' if age is not None else ''}")
            failed += age is None
            continue
        if len(data) < 100 or data.lstrip()[:1] == b"<":
            print(f"{name:20s} FAILED (unexpected answer: {data[:60]!r})")
            failed += age is None
            continue
        tmp = dst + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, dst)
        print(f"{name:20s} {len(data):8d} bytes")
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
