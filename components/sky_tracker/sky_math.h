// sky_math.h — geometry, Sun and Moon for the Sky Tracker (shared by sat_net.h and
// sat_tracker.h). Plain maths: no LVGL, no ESPHome, safe from any task.
#pragma once
#include <algorithm>
#include <cmath>

namespace sat {

// ================================================================== geometry
namespace geo {

constexpr float DEG = (float) (M_PI / 180.0);
constexpr float RE_KM = 6378.137f;         // WGS84 equatorial radius
constexpr float E2 = 6.69437999014e-3f;    // WGS84 first eccentricity squared
constexpr float SHADOW_R_KM = 6371.0f;     // cylindrical shadow radius (DATA-7)

struct V3 {
  float x, y, z;
};
inline V3 v3(const float a[3]) { return {a[0], a[1], a[2]}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float norm(V3 a) { return sqrtf(dot(a, a)); }

// Geodetic (deg, deg, km) -> ECEF km.
inline V3 ecef(float lat_deg, float lon_deg, float h_km) {
  const float la = lat_deg * DEG, lo = lon_deg * DEG;
  const float sl = sinf(la), cl = cosf(la);
  const float n = RE_KM / sqrtf(1.0f - E2 * sl * sl);
  return {(n + h_km) * cl * cosf(lo), (n + h_km) * cl * sinf(lo), (n * (1.0f - E2) + h_km) * sl};
}

struct Observer {
  float lat = 0, lon = 0, alt_km = 0;
  V3 pos{0, 0, 0}, east{0, 0, 0}, north{0, 0, 0}, up{0, 0, 0};
  void set(float lat_deg, float lon_deg, float alt_m) {
    lat = lat_deg;
    lon = lon_deg;
    alt_km = alt_m / 1000.0f;
    pos = ecef(lat, lon, alt_km);
    const float la = lat * DEG, lo = lon * DEG;
    east = {-sinf(lo), cosf(lo), 0};
    north = {-sinf(la) * cosf(lo), -sinf(la) * sinf(lo), cosf(la)};
    up = {cosf(la) * cosf(lo), cosf(la) * sinf(lo), sinf(la)};
  }
};

struct AzEl {
  float az, el;  // degrees; az from north through east, 0..360
};

// MOTION-6: one path for every object.
inline AzEl azel(const Observer &o, V3 target_ecef) {
  const V3 d = sub(target_ecef, o.pos);
  const float e = dot(d, o.east), n = dot(d, o.north), u = dot(d, o.up);
  float az = atan2f(e, n) / DEG;
  if (az < 0)
    az += 360.0f;
  return {az, atan2f(u, sqrtf(e * e + n * n)) / DEG};
}

// Rodrigues rotation of unit vector p about unit axis k by angle a (MOTION-1).
inline V3 rotate(V3 p, V3 k, float a) {
  const float c = cosf(a), s = sinf(a);
  return add(add(mul(p, c), mul(cross(k, p), s)), mul(k, dot(k, p) * (1.0f - c)));
}

// Great-circle separation of two sky positions, degrees.
inline float separation(AzEl a, AzEl b) {
  const float c = sinf(a.el * DEG) * sinf(b.el * DEG) + cosf(a.el * DEG) * cosf(b.el * DEG) * cosf((a.az - b.az) * DEG);
  return acosf(std::max(-1.0f, std::min(1.0f, c))) / DEG;
}

}  // namespace geo

// ================================================================== Sun and Moon (DATA-7)
// Low-precision formulae from the Astronomical Almanac (sections C and D). Time
// arithmetic stays in double; everything after the angle reduction is float-safe.
namespace astro {

inline double jd(double unix_s) { return unix_s / 86400.0 + 2440587.5; }
inline double wrap360(double d) {
  d = fmod(d, 360.0);
  return d < 0 ? d + 360.0 : d;
}
inline double rad(double d) { return d * M_PI / 180.0; }

// Greenwich mean sidereal time, degrees.
inline double gmst_deg(double jdv) { return wrap360(280.46061837 + 360.98564736629 * (jdv - 2451545.0)); }

struct Equ {
  double ra, dec;  // radians
  double lon_ecl;  // apparent ecliptic longitude, degrees (for the phase)
};

inline Equ sun(double jdv) {
  const double n = jdv - 2451545.0;
  const double L = wrap360(280.460 + 0.9856474 * n);
  const double g = rad(wrap360(357.528 + 0.9856003 * n));
  const double lam = rad(L + 1.915 * sin(g) + 0.020 * sin(2 * g));
  const double eps = rad(23.439 - 0.0000004 * n);
  Equ e;
  e.ra = atan2(cos(eps) * sin(lam), cos(lam));
  e.dec = asin(sin(eps) * sin(lam));
  e.lon_ecl = lam * 180.0 / M_PI;
  return e;
}

// Geocentric Moon plus its distance in Earth radii.
inline Equ moon(double jdv, double *dist_er) {
  const double T = (jdv - 2451545.0) / 36525.0;
  const double lam = 218.32 + 481267.881 * T + 6.29 * sin(rad(135.0 + 477198.87 * T)) -
                     1.27 * sin(rad(259.3 - 413335.36 * T)) + 0.66 * sin(rad(235.7 + 890534.22 * T)) +
                     0.21 * sin(rad(269.9 + 954397.74 * T)) - 0.19 * sin(rad(357.5 + 35999.05 * T)) -
                     0.11 * sin(rad(186.5 + 966404.03 * T));
  const double bet = 5.13 * sin(rad(93.3 + 483202.02 * T)) + 0.28 * sin(rad(228.2 + 960400.89 * T)) -
                     0.28 * sin(rad(318.3 + 6003.15 * T)) - 0.17 * sin(rad(217.6 - 407332.21 * T));
  const double par = 0.9508 + 0.0518 * cos(rad(135.0 + 477198.87 * T)) + 0.0095 * cos(rad(259.3 - 413335.36 * T)) +
                     0.0078 * cos(rad(235.7 + 890534.22 * T)) + 0.0028 * cos(rad(269.9 + 954397.74 * T));
  const double l = rad(wrap360(lam)), b = rad(bet), eps = rad(23.439 - 0.0000004 * (jdv - 2451545.0));
  const double x = cos(b) * cos(l);
  const double y = cos(eps) * cos(b) * sin(l) - sin(eps) * sin(b);
  const double z = sin(eps) * cos(b) * sin(l) + cos(eps) * sin(b);
  Equ e;
  e.ra = atan2(y, x);
  e.dec = asin(z);
  e.lon_ecl = wrap360(lam);
  *dist_er = 1.0 / sin(rad(par));
  return e;
}

// Topocentric correction for a body at `dist_er` Earth radii (Moon parallax).
inline Equ topocentric(Equ g, double dist_er, double lat_deg, double alt_m, double lst_rad) {
  const double f = 1.0 / 298.257223563, la = rad(lat_deg);
  const double u = atan((1 - f) * tan(la));
  const double h = alt_m / 6378137.0;
  const double rho_c = cos(u) + h * cos(la), rho_s = (1 - f) * sin(u) + h * sin(la);
  const double x = dist_er * cos(g.dec) * cos(g.ra) - rho_c * cos(lst_rad);
  const double y = dist_er * cos(g.dec) * sin(g.ra) - rho_c * sin(lst_rad);
  const double z = dist_er * sin(g.dec) - rho_s;
  Equ t = g;
  t.ra = atan2(y, x);
  t.dec = atan2(z, sqrt(x * x + y * y));
  return t;
}

// Equatorial -> horizontal, refraction applied above -1° (Saemundsson).
inline geo::AzEl horizontal(Equ e, double lat_deg, double lst_rad) {
  const double la = rad(lat_deg), H = lst_rad - e.ra;
  const double sin_el = sin(la) * sin(e.dec) + cos(la) * cos(e.dec) * cos(H);
  double el = asin(std::max(-1.0, std::min(1.0, sin_el))) * 180.0 / M_PI;
  double az = atan2(-cos(e.dec) * sin(H), sin(e.dec) * cos(la) - cos(e.dec) * sin(la) * cos(H)) * 180.0 / M_PI;
  if (az < 0)
    az += 360.0;
  if (el > -1.0)
    el += 1.02 / tan(rad(el + 10.3 / (el + 5.11))) / 60.0;
  return {(float) az, (float) el};
}

struct SunMoon {
  geo::AzEl sun, moon;
  float moon_illum;     // 0 new .. 1 full
  float moon_elong;     // Moon minus Sun ecliptic longitude, 0..360 (< 180 waxing)
  geo::V3 sun_ecef_dir;  // unit vector towards the Sun, Earth-fixed frame
};

inline SunMoon compute(double unix_s, double lat_deg, double lon_deg, double alt_m) {
  const double j = jd(unix_s);
  const double gm = gmst_deg(j);
  const double lst = rad(wrap360(gm + lon_deg));
  SunMoon r;
  const Equ s = sun(j);
  r.sun = horizontal(s, lat_deg, lst);
  double dist;
  const Equ mg = moon(j, &dist);
  r.moon = horizontal(topocentric(mg, dist, lat_deg, alt_m, lst), lat_deg, lst);
  const double cos_psi = sin(s.dec) * sin(mg.dec) + cos(s.dec) * cos(mg.dec) * cos(s.ra - mg.ra);
  r.moon_illum = (float) ((1.0 - cos_psi) / 2.0);
  r.moon_elong = (float) wrap360(mg.lon_ecl - s.lon_ecl);
  const double lon_s = s.ra - rad(gm);
  r.sun_ecef_dir = {(float) (cos(s.dec) * cos(lon_s)), (float) (cos(s.dec) * sin(lon_s)), (float) sin(s.dec)};
  return r;
}

// UI-20 rise/set: the next time the refracted centre crosses -0.27° (the upper limb
// on the horizon; both discs are about 0.27° in radius), searched in 20-minute steps
// over the next 24 h and refined by bisection to about a second.
struct Event {
  double t = 0;       // UTC seconds; 0 = none in the window
  bool rise = false;  // true: rises at t; false: sets at t
  bool up = false;    // with t == 0: up (true) or down (false) the whole window
};
// One sweep for both bodies (the maths is the expensive part: ~100 evaluations).
inline void next_events(double t0, double lat, double lon, double alt_m, Event &sun_ev, Event &moon_ev) {
  constexpr double H0 = -0.27, STEP = 1200, SPAN = 86400;
  auto f = [&](bool m, const SunMoon &r) { return (double) (m ? r.moon.el : r.sun.el) - H0; };
  auto refine = [&](bool m, double lo, double hi, double flo) {
    for (int i = 0; i < 12; i++) {
      const double mid = (lo + hi) / 2, fm = f(m, compute(mid, lat, lon, alt_m));
      if ((fm > 0) == (flo > 0)) {
        lo = mid;
        flo = fm;
      } else {
        hi = mid;
      }
    }
    return (lo + hi) / 2;
  };
  sun_ev = Event();
  moon_ev = Event();
  Event *ev[2] = {&sun_ev, &moon_ev};
  SunMoon ra = compute(t0, lat, lon, alt_m);
  double fa[2] = {f(false, ra), f(true, ra)};
  bool done[2] = {false, false};
  for (double ta = t0; ta < t0 + SPAN && !(done[0] && done[1]); ta += STEP) {
    const SunMoon rb = compute(ta + STEP, lat, lon, alt_m);
    for (int k = 0; k < 2; k++) {
      if (done[k])
        continue;
      const double fb = f(k == 1, rb);
      if ((fa[k] > 0) != (fb > 0)) {
        ev[k]->t = refine(k == 1, ta, ta + STEP, fa[k]);
        ev[k]->rise = fb > 0;
        done[k] = true;
      }
      fa[k] = fb;
    }
  }
  for (int k = 0; k < 2; k++)
    if (!done[k])
      ev[k]->up = fa[k] > 0;
}

inline const char *phase_name(float illum, float elong) {
  const bool waxing = elong < 180.0f;
  if (illum < 0.03f)
    return "New Moon";
  if (illum > 0.97f)
    return "Full Moon";
  if (illum >= 0.47f && illum <= 0.53f)
    return waxing ? "First Quarter" : "Last Quarter";
  if (illum < 0.5f)
    return waxing ? "Waxing Crescent" : "Waning Crescent";
  return waxing ? "Waxing Gibbous" : "Waning Gibbous";
}

// ---------------------------------------------------------------- UI-36 Sun / Moon cards
// Times when the Sun's (or Moon's) refracted centre crosses `h` degrees in [t0, t1],
// found in `step`-second samples and refined by bisection (~1 s). Returns the count.
struct Crossing {
  double t;
  bool rising;
};
inline int crossings(double t0, double t1, double h, bool moon, double lat, double lon, double alt_m, Crossing *out,
                     int max, double step = 600) {
  auto el = [&](double t) {
    const SunMoon r = compute(t, lat, lon, alt_m);
    return (double) (moon ? r.moon.el : r.sun.el) - h;
  };
  int n = 0;
  double ta = t0, fa = el(ta);
  while (ta < t1 && n < max) {
    const double tb = std::min(ta + step, t1), fb = el(tb);
    if ((fa > 0) != (fb > 0)) {
      double lo = ta, hi = tb, flo = fa;
      for (int i = 0; i < 10; i++) {
        const double mid = (lo + hi) / 2, fm = el(mid);
        if ((fm > 0) == (flo > 0)) {
          lo = mid;
          flo = fm;
        } else {
          hi = mid;
        }
      }
      out[n++] = {(lo + hi) / 2, fb > 0};
    }
    ta = tb;
    fa = fb;
  }
  return n;
}

// Sun's highest point in [t0, t1] (solar noon), by sampling then golden-section search.
inline double sun_peak(double t0, double t1, double lat, double lon, double alt_m, float *peak_el) {
  auto el = [&](double t) { return (double) compute(t, lat, lon, alt_m).sun.el; };
  double best = t0, be = -99;
  for (double t = t0; t <= t1; t += 1800) {
    const double e = el(t);
    if (e > be) {
      be = e;
      best = t;
    }
  }
  double lo = best - 1800, hi = best + 1800;
  const double g = 0.3819660112501051;
  for (int i = 0; i < 20; i++) {
    const double a = lo + g * (hi - lo), b = hi - g * (hi - lo);
    if (el(a) < el(b))
      lo = a;
    else
      hi = b;
  }
  const double tp = (lo + hi) / 2;
  if (peak_el)
    *peak_el = (float) el(tp);
  return tp;
}

// Moon minus Sun geocentric ecliptic longitude, degrees 0..360 (0 new, 180 full).
inline double elongation(double t) {
  const double j = jd(t);
  double d;
  return wrap360(moon(j, &d).lon_ecl - sun(j).lon_ecl);
}
// Next (dir = +1) or previous (dir = -1) time the elongation passes `target` degrees
// (0 = new Moon, 180 = full), searched in 6 h steps and refined to about a minute.
inline double moon_phase_time(double t0, double target, int dir) {
  auto f = [&](double t) {
    double x = elongation(t) - target;
    while (x >= 180)
      x -= 360;
    while (x < -180)
      x += 360;
    return x;
  };
  const double step = 6 * 3600.0 * dir;
  double ta = t0, fa = f(ta);
  for (int i = 0; i < 4 * 32; i++) {
    const double tb = ta + step, fb = f(tb);
    // elongation grows ~12°/day, so f climbs through 0 forward in time (and the
    // wrap from +180 to -180 is not a crossing)
    const bool cross = dir > 0 ? (fa < 0 && fb >= 0 && fb - fa < 90) : (fa >= 0 && fb < 0 && fa - fb < 90);
    if (cross) {
      double lo = std::min(ta, tb), hi = std::max(ta, tb);
      for (int k = 0; k < 16; k++) {
        const double mid = (lo + hi) / 2;
        if (f(mid) < 0)
          lo = mid;
        else
          hi = mid;
      }
      return (lo + hi) / 2;
    }
    ta = tb;
    fa = fb;
  }
  return 0;
}
// Geocentric Earth-Moon distance, km.
inline double moon_distance_km(double t) {
  double d;
  moon(jd(t), &d);
  return d * 6378.137;
}

// Cylindrical Earth shadow.
inline bool sunlit(geo::V3 sat_ecef, geo::V3 sun_dir) {
  const float d = geo::dot(sat_ecef, sun_dir);
  if (d > 0)
    return true;
  return geo::norm(geo::sub(sat_ecef, geo::mul(sun_dir, d))) > geo::SHADOW_R_KM;
}

}  // namespace astro

}  // namespace sat
