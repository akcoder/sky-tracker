# Independent reference data (VER-1): skyfield/SGP4 for satellites, pyephem for Sun/Moon.
import json, math, random
import ephem
from sgp4.api import Satrec, WGS72
from skyfield.api import load, wgs84, EarthSatellite
ts = load.timescale(builtin=True)
LAT, LON, ALT = 61.5814, -149.4394, 100.0
obs = wgs84.latlon(LAT, LON, elevation_m=ALT)
T0 = 1790100000.0  # 2026-09-22 ~ 17:20 UTC
def t_of(u): return ts.from_datetime(__import__('datetime').datetime.fromtimestamp(u, __import__('datetime').timezone.utc))
epoch_days = T0 / 86400.0 + 7305.0
def mk(satnum, inc, raan, ecc, argp, ma, mm):
    s = Satrec()
    s.sgp4init(WGS72, 'i', satnum, epoch_days, 0.0, 0.0, 0.0, ecc, math.radians(argp), math.radians(inc),
               math.radians(ma), mm * 2 * math.pi / 1440.0, math.radians(raan))
    return EarthSatellite.from_satrec(s, ts)
def state(sat, u):
    t = t_of(u)
    g = sat.at(t)
    sp = wgs84.geographic_position_of(g)
    alt, az, _ = (sat - obs).at(t).altaz()
    return dict(lat=sp.latitude.degrees, lon=sp.longitude.degrees, alt_km=sp.elevation.km, az=az.degrees, el=alt.degrees)

# find an ISS-like orbit that passes high over the observer within 2 h of T0
best = None
for raan in range(0, 360, 3):
    iss = mk(25544, 51.64, raan, 0.0005, 90, 0, 15.50)
    for k in range(0, 21600, 60):
        s = state(iss, T0 + k)
        if s['el'] > 5 and (best is None or s['el'] > best[2]):
            best = (raan, k, s['el'])
    if best and best[2] > 16: break
raan, kpeak, _ = best
iss = mk(25544, 51.64, raan, 0.0005, 90, 0, 15.50)
tpk = T0 + kpeak
start = tpk - 300
# fixes every 15 s (ISS cadence) and 90 s (/above cadence), truth every 2 s
truth = [dict(t=start + i*2, **state(iss, start + i*2)) for i in range(0, 331)]
out = dict(observer=dict(lat=LAT, lon=LON, alt=ALT), iss_truth=truth, peak=tpk)

# Sun/Moon from pyephem at many times (refraction on: pressure default, temp 10C)
o = ephem.Observer(); o.lat, o.lon, o.elevation = str(LAT), str(LON), ALT
o.pressure = 1010; o.temp = 10
sm = []
for i in range(400):
    u = T0 + i * 86400 * 0.37 + i * 3131
    o.date = ephem.Date(__import__('datetime').datetime.utcfromtimestamp(u))
    s = ephem.Sun(o); m = ephem.Moon(o)
    sm.append(dict(t=u, sun_az=math.degrees(s.az), sun_el=math.degrees(s.alt), moon_az=math.degrees(m.az),
                   moon_el=math.degrees(m.alt), moon_phase=m.phase/100.0))
out['sunmoon'] = sm

# eclipse truth for the ISS: skyfield is_sunlit needs an ephemeris; use geometry with ephem Sun
# (cylindrical shadow, same as device) is not independent, so use skyfield's is_sunlit if de421 loads.
from sgp4.exporter import export_tle
import datetime as _dt
l1, l2 = export_tle(iss.model)
es = ephem.readtle('ISS', l1, l2)
ecl = []
for i in range(0, 5400*2, 60):
    u = T0 + kpeak - 5400 + i
    es.compute(ephem.Date(_dt.datetime.fromtimestamp(u, _dt.timezone.utc).replace(tzinfo=None)))
    st = state(iss, u)
    ecl.append(dict(t=u, lat=st['lat'], lon=st['lon'], alt_km=st['alt_km'], sunlit=not bool(es.eclipsed)))
out['eclipse'] = ecl
# a sky full of objects for the render test: random LEO/MEO/GEO + Starlink-like shells
random.seed(7)
sky, sl = [], []
n = 0
while len(sky) < 30 and n < 5000:
    n += 1
    kind = random.random()
    mm = 15.2 if kind < .7 else (2.0 if kind < .85 else 1.0027)
    inc = random.choice([53, 70, 82, 97.6, 55, 0.05 if mm < 1.1 else 63])
    sat = mk(40000+n, inc, random.uniform(0,360), 0.001, 0, random.uniform(0,360), mm)
    a = state(sat, T0); b = state(sat, T0+90)
    if a['el'] > 5 and b['el'] > 5:
        sky.append(dict(id=40000+n, name=random.choice(['COSMOS','NOAA','SL-16 R/B','METEOR','GPS BIIF','INTELSAT','TERRA','AQUA','IRIDIUM'])+' %d'%n, a=a, b=b,
                        c=state(sat, T0+180)))
n = 0
while len(sl) < 20 and n < 5000:
    n += 1
    sat = mk(50000+n, random.choice([53, 70, 97.6]), random.uniform(0,360), 0.0001, 0, random.uniform(0,360), 15.06)
    a = state(sat, T0); b = state(sat, T0+90)
    if a['el'] > 30 and b['el'] > 25:
        sl.append(dict(id=50000+n, name='STARLINK-%d'%(1000+n), a=a, b=b, c=state(sat, T0+180)))
out['sky'] = sky; out['starlink'] = sl; out['T0'] = T0
json.dump(out, open('vectors.json','w'))
print('iss peak el %.1f at +%ds, sky %d, starlink %d, sunmoon %d' % (best[2], kpeak, len(sky), len(sl), len(sm)), 'eclipse' in out)
