# rev 4 test vectors: element sets (OMM CSV rows) + skyfield truth
import json, math, random
from datetime import datetime, timezone, timedelta
from sgp4.api import Satrec, WGS72
from skyfield.api import load, wgs84, EarthSatellite
ts = load.timescale(builtin=True)
LAT, LON, ALT = 61.5814, -149.4394, 100.0
obs = wgs84.latlon(LAT, LON, ALT)
T0 = datetime(2026, 9, 24, 6, 0, 0, tzinfo=timezone.utc)   # 22:00 AKDT, dark
t0u = T0.timestamp()
random.seed(4)
HDR = "OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY,EPHEMERIS_TYPE,CLASSIFICATION_TYPE,NORAD_CAT_ID,ELEMENT_SET_NO,REV_AT_EPOCH,BSTAR,MEAN_MOTION_DOT,MEAN_MOTION_DDOT"
def row(name, intl, epoch, n, e, i, raan, argp, ma, cat, bstar, ndot):
    return f"{name},{intl},{epoch.strftime('%Y-%m-%dT%H:%M:%S.%f')},{n:.8f},{e:.8f},{i:.4f},{raan:.4f},{argp:.4f},{ma:.4f},0,U,{cat},999,1000,{bstar:.8E},{ndot:.8E},0"
def satrec(epoch, n, e, i, raan, argp, ma, cat, bstar, ndot):
    s = Satrec()
    ep = (epoch - datetime(1949,12,31,tzinfo=timezone.utc)).total_seconds()/86400.0
    xp = 1440.0/(2*math.pi); d=math.pi/180
    s.sgp4init(WGS72, 'i', cat % 100000, ep, bstar, ndot/(xp*1440), 0.0, e, argp*d, i*d, ma*d, n/xp, raan*d)
    return s
objs = []
# real ISS row (CelesTrak 2026-09-24)
iss_ep = datetime(2026,9,24,3,24,21,452544,tzinfo=timezone.utc)
objs.append(dict(kind='iss', name='ISS (ZARYA)', intl='1998-067A', ep=iss_ep, n=15.49258637, e=.00046914, i=51.6318, raan=170.3464, argp=174.6338, ma=185.4701, cat=25544, bstar=.18115501E-3, ndot=.9634E-4))
objs.append(dict(kind='sat', name='ATLAS CENTAUR 2', intl='1963-047A', ep=datetime(2026,9,24,1,49,53,443200,tzinfo=timezone.utc), n=14.12725132, e=.05447003, i=30.3522, raan=166.2924, argp=29.7444, ma=333.3007, cat=694, bstar=.31026394E-3, ndot=.2623E-4))
for k in range(59):
    kind = random.random()
    if kind < 0.8:  n = random.uniform(13.0, 15.3); e = random.uniform(0.0005, 0.02)
    elif kind < 0.93: n = random.uniform(2.0, 2.2); e = random.uniform(0.0, 0.01)
    else: n = random.uniform(0.99, 1.01); e = random.uniform(0.0, 0.001)
    objs.append(dict(kind='sat', name=f'TEST SAT {k}', intl=f'2000-{k:03d}A', ep=T0 - timedelta(hours=random.uniform(2, 30)), n=n, e=e,
        i=random.choice([98.7, 82.5, 71.0, 65.0, 56.0, 28.5, 0.1]) + random.uniform(-1,1), raan=random.uniform(0,360), argp=random.uniform(0,360), ma=random.uniform(0,360), cat=40000+k, bstar=random.uniform(1e-5, 3e-4) if n>10 else 0.0, ndot=1e-5))
for k in range(600):
    inc, n = random.choice([(53.05, 15.06), (43.0, 15.25), (70.0, 15.05), (97.6, 15.08), (53.16, 15.33)])
    objs.append(dict(kind='starlink', name=f'STARLINK-{1000+k}', intl=f'2024-{k:03d}B', ep=T0 - timedelta(hours=random.uniform(1, 20)), n=n + random.uniform(-0.01, 0.01), e=random.uniform(0.0001, 0.0003),
        i=inc + random.uniform(-0.02, 0.02), raan=random.uniform(0,360), argp=random.uniform(0,360), ma=random.uniform(0,360), cat=60000+k, bstar=random.uniform(1e-5, 4e-4), ndot=random.uniform(1e-6, 3e-5)))
out = dict(T0=t0u, observer=dict(lat=LAT, lon=LON, alt=ALT), csv={}, truth=[], passes=[])
for grp in ('iss', 'sat', 'starlink'):
    out['csv'][grp] = HDR + "\r\n" + "\r\n".join(row(o['name'], o['intl'], o['ep'], o['n'], o['e'], o['i'], o['raan'], o['argp'], o['ma'], o['cat'], o['bstar'], o['ndot']) for o in objs if o['kind']==grp) + "\r\n"
offs = [0, 10, 30, 90, 600, 3600, 6*3600, 20*3600]
for o in objs:
    s = satrec(o['ep'], o['n'], o['e'], o['i'], o['raan'], o['argp'], o['ma'], o['cat'], o['bstar'], o['ndot'])
    es = EarthSatellite.from_satrec(s, ts)
    pts = []
    for dt in offs:
        t = ts.from_datetime(T0 + timedelta(seconds=dt))
        alt, az, dist = (es - obs).at(t).altaz()
        sp = wgs84.subpoint_of(es.at(t))
        pts.append(dict(dt=dt, az=az.degrees, el=alt.degrees, range=dist.km, h=sp.elevation.km))
    out['truth'].append(dict(id=o['cat'], kind=o['kind'], pts=pts))
    if o['kind']=='iss':
        s_iss = es
# ISS passes: rise/culminate/set at 0 deg, keep max>=10
tA = ts.from_datetime(T0); tB = ts.from_datetime(T0 + timedelta(days=4))
t, ev = s_iss.find_events(obs, tA, tB, altitude_degrees=0.0)
cur = {}
for ti, e in zip(t, ev):
    if e == 0: cur = dict(rise=ti.tt and ti.utc_datetime().timestamp())
    elif e == 1 and cur: cur['max'] = ti.utc_datetime().timestamp(); cur['max_el'] = (s_iss - obs).at(ti).altaz()[0].degrees
    elif e == 2 and 'max' in cur:
        cur['set'] = ti.utc_datetime().timestamp()
        if cur['max_el'] >= 10: out['passes'].append(cur)
        cur = {}
json.dump(out, open('vectors4.json','w'))
print(len(objs), len(out['passes']), out['passes'][:2])
