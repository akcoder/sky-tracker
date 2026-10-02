#pragma once
// sky_events.h — sky events for the Sky Tracker (UI-47..UI-51): a fitted precise Moon,
// eclipses, meteor showers, full-Moon names, Moon conjunctions and the named bright
// stars. Plain maths (no LVGL); generated in part by gen_events.py.
#include <cmath>
#include <cstdint>
#include "sky_math.h"
#include "sky_planets.h"

namespace sat {
namespace ev {

// Moon series fitted by fitmoon.py to PyEphem (ELP-based) positions, 2024-2050:
// ecliptic of date, lon/lat rms 0.002 deg (max 0.007), distance rms 6 km.
struct MoonTerm { int8_t d, m, mp, f; float a; };
constexpr int MOON_L_N = 57;
const MoonTerm MOON_L[MOON_L_N] = {
  {0,0,0,2,-0.114323f},
  {0,0,1,-2,0.010953f},
  {0,0,1,0,6.288751f},
  {0,0,1,2,-0.012522f},
  {0,0,2,-2,-0.000377f},
  {0,0,2,0,0.213620f},
  {0,0,2,2,-0.001109f},
  {0,0,3,0,0.010035f},
  {0,0,4,0,0.000537f},
  {0,1,-2,0,-0.002682f},
  {0,1,-1,0,-0.040858f},
  {0,1,0,0,-0.184924f},
  {0,1,1,0,-0.030328f},
  {0,1,2,0,-0.002123f},
  {0,2,-1,0,-0.000706f},
  {0,2,0,0,-0.002067f},
  {0,2,1,0,-0.000322f},
  {1,0,-2,0,-0.000492f},
  {1,0,-1,0,-0.005157f},
  {1,0,0,0,-0.034729f},
  {1,0,1,0,-0.002342f},
  {1,1,-1,0,0.000573f},
  {1,1,0,0,0.004987f},
  {1,1,1,0,0.000353f},
  {2,-2,-1,0,0.002050f},
  {2,-2,0,0,0.002244f},
  {2,-1,-2,0,0.002388f},
  {2,-1,-1,0,0.057008f},
  {2,-1,0,-2,0.000600f},
  {2,-1,0,0,0.045711f},
  {2,-1,1,0,0.004031f},
  {2,-1,2,0,0.000326f},
  {2,0,-3,0,0.003669f},
  {2,0,-2,0,0.058801f},
  {2,0,-1,0,1.274023f},
  {2,0,-1,2,-0.002595f},
  {2,0,0,-2,0.015327f},
  {2,0,0,0,0.658312f},
  {2,0,0,2,-0.001593f},
  {2,0,1,-2,-0.001775f},
  {2,0,1,0,0.053322f},
  {2,0,2,0,0.003994f},
  {2,1,-2,0,0.000696f},
  {2,1,-1,0,-0.007859f},
  {2,1,0,-2,-0.000397f},
  {2,1,0,0,-0.006749f},
  {2,1,1,0,-0.000807f},
  {2,2,-1,0,-0.000698f},
  {2,2,0,-2,-0.000508f},
  {3,0,-2,0,-0.000334f},
  {3,0,-1,0,-0.000891f},
  {4,-1,-1,0,0.001218f},
  {4,-1,0,0,0.000521f},
  {4,0,-2,0,0.008555f},
  {4,0,-1,0,0.010676f},
  {4,0,0,0,0.003861f},
  {4,0,1,0,0.000550f},
};
constexpr int MOON_B_N = 44;
const MoonTerm MOON_B[MOON_B_N] = {
  {0,0,0,1,5.127885f},
  {0,0,0,3,-0.001745f},
  {0,0,1,-3,0.000769f},
  {0,0,1,-1,0.277676f},
  {0,0,1,1,0.280587f},
  {0,0,2,-1,0.008690f},
  {0,0,2,1,0.017195f},
  {0,0,3,-1,0.000428f},
  {0,0,3,1,0.001103f},
  {0,1,-1,-1,-0.001857f},
  {0,1,-1,1,-0.001565f},
  {0,1,0,-1,-0.001317f},
  {0,1,0,1,-0.001767f},
  {0,1,1,-1,-0.001396f},
  {0,1,1,1,-0.001468f},
  {1,0,0,-1,-0.001331f},
  {1,0,0,1,-0.001498f},
  {1,1,-1,-1,-0.000455f},
  {2,-2,0,-1,0.000309f},
  {2,-1,-1,-1,0.002065f},
  {2,-1,-1,1,0.002455f},
  {2,-1,0,-1,0.008206f},
  {2,-1,0,1,0.002217f},
  {2,-1,1,-1,0.000493f},
  {2,-1,1,1,0.000313f},
  {2,0,-3,-1,0.000418f},
  {2,0,-2,-1,0.004323f},
  {2,0,-2,1,-0.000467f},
  {2,0,-1,-1,0.046269f},
  {2,0,-1,1,0.055414f},
  {2,0,0,-3,0.000609f},
  {2,0,0,-1,0.173242f},
  {2,0,0,1,0.032572f},
  {2,0,1,-1,0.009264f},
  {2,0,1,1,0.004198f},
  {2,0,2,-1,0.000596f},
  {2,0,2,1,0.000422f},
  {2,1,-1,1,-0.000365f},
  {2,1,0,-1,-0.003353f},
  {2,1,0,1,-0.000346f},
  {4,0,-1,-1,0.001828f},
  {4,0,-1,1,0.000836f},
  {4,0,0,-1,0.001029f},
  {4,0,0,1,0.000332f},
};
constexpr int MOON_R_N = 40;
const MoonTerm MOON_R[MOON_R_N] = {
  {0,0,0,2,-3.17f},
  {0,0,1,-2,79.58f},
  {0,0,1,0,-20905.29f},
  {0,0,2,-2,-4.39f},
  {0,0,2,0,-569.90f},
  {0,0,3,0,-23.21f},
  {0,1,-2,0,-6.98f},
  {0,1,-1,0,-129.41f},
  {0,1,0,0,48.85f},
  {0,1,1,0,104.57f},
  {0,1,2,0,5.75f},
  {0,2,-1,0,-2.10f},
  {1,0,-1,0,-8.37f},
  {1,0,0,0,108.78f},
  {1,0,1,0,6.28f},
  {1,1,0,0,-16.69f},
  {2,-2,-1,0,-4.96f},
  {2,-2,0,0,-9.92f},
  {2,-1,-2,0,10.07f},
  {2,-1,-1,0,-151.99f},
  {2,-1,0,0,-204.36f},
  {2,-1,1,0,-12.81f},
  {2,0,-3,0,14.40f},
  {2,0,-2,0,246.18f},
  {2,0,-1,-2,8.80f},
  {2,0,-1,0,-3699.13f},
  {2,0,0,-2,10.37f},
  {2,0,0,0,-2955.99f},
  {2,0,1,-2,4.12f},
  {2,0,1,0,-170.74f},
  {2,0,2,0,-10.45f},
  {2,1,-1,0,24.11f},
  {2,1,0,0,30.73f},
  {2,1,1,0,2.60f},
  {2,2,-1,0,2.36f},
  {3,0,-1,0,3.27f},
  {4,-1,-1,0,-3.97f},
  {4,0,-2,0,-21.66f},
  {4,0,-1,0,-34.80f},
  {4,0,0,0,-11.67f},
};
constexpr double MOON_L0[2] = {0.009832323, 0.011416640};
constexpr double MOON_LOM[2] = {0.001789494, -0.000586765};
constexpr double MOON_B0[2] = {0.000082525, -0.000152299};
constexpr double MOON_R0[2] = {384999.8985, 1.711524};

// ================================================================== precise Moon
struct MoonPos {
  double lon, lat;  // ecliptic of date, degrees
  double dist_km;
};
inline MoonPos moon_precise(double jdv) {
  const double T = (jdv - 2451545.0) / 36525.0;
  auto red = [](double deg) { return (float) (astro::wrap360(deg) * M_PI / 180.0); };
  const float D = red(297.8501921 + 445267.1114034 * T), M = red(357.5291092 + 35999.0502909 * T),
              Mp = red(134.9633964 + 477198.8675055 * T), F = red(93.2720950 + 483202.0175233 * T),
              Om = red(125.04452 - 1934.136261 * T);
  auto arg = [&](const MoonTerm &k) { return k.d * D + k.m * M + k.mp * Mp + k.f * F; };
  double l = 0, b = 0, r = 0;
  for (int i = 0; i < MOON_L_N; i++)
    l += MOON_L[i].a * sinf(arg(MOON_L[i]));
  for (int i = 0; i < MOON_B_N; i++)
    b += MOON_B[i].a * sinf(arg(MOON_B[i]));
  for (int i = 0; i < MOON_R_N; i++)
    r += MOON_R[i].a * cosf(arg(MOON_R[i]));
  MoonPos p;
  p.lon = astro::wrap360(218.3164477 + 481267.88123421 * T + MOON_L0[0] + MOON_L0[1] * T + l +
                         MOON_LOM[0] * sinf(Om) + MOON_LOM[1] * sinf(2 * Om));
  p.lat = MOON_B0[0] + MOON_B0[1] * T + b;
  p.dist_km = MOON_R0[0] + MOON_R0[1] * T + r;
  return p;
}
inline astro::Equ ecl_to_equ(double lon_deg, double lat_deg, double jdv) {
  const double l = astro::rad(lon_deg), b = astro::rad(lat_deg), eps = astro::rad(23.439 - 0.0000004 * (jdv - 2451545.0));
  astro::Equ e;
  e.ra = atan2(cos(eps) * cos(b) * sin(l) - sin(eps) * sin(b), cos(b) * cos(l));
  e.dec = asin(sin(eps) * cos(b) * sin(l) + cos(eps) * sin(b));
  e.lon_ecl = lon_deg;
  return e;
}
// angle between two (lon, lat) points, degrees
inline double sep_deg(double l1, double b1, double l2, double b2) {
  const double c = sin(astro::rad(b1)) * sin(astro::rad(b2)) + cos(astro::rad(b1)) * cos(astro::rad(b2)) * cos(astro::rad(l1 - l2));
  return acos(std::max(-1.0, std::min(1.0, c))) * 180.0 / M_PI;
}
inline double equ_sep_deg(const astro::Equ &a, const astro::Equ &b) {
  const double c = sin(a.dec) * sin(b.dec) + cos(a.dec) * cos(b.dec) * cos(a.ra - b.ra);
  return acos(std::max(-1.0, std::min(1.0, c))) * 180.0 / M_PI;
}
// topocentric Moon (RA/Dec) and its distance from the observer, km
inline astro::Equ moon_topo(double t, double lat, double lon, double alt_m, double *dist_km = nullptr,
                            double *el = nullptr) {
  const double j = astro::jd(t);
  const MoonPos m = moon_precise(j);
  const astro::Equ g = ecl_to_equ(m.lon, m.lat, j);
  const double er = m.dist_km / 6378.137;
  const double lst = astro::rad(astro::wrap360(astro::gmst_deg(j) + lon));
  const astro::Equ tp = astro::topocentric(g, er, lat, alt_m, lst);
  if (dist_km) {
    // distance from the observer: close enough from the elevation (parallax in el)
    const geo::AzEl h = astro::horizontal(tp, lat, lst);
    *dist_km = m.dist_km - 6378.137 * sin(astro::rad(std::max(-90.0f, h.el)));
  }
  if (el)
    *el = astro::horizontal(tp, lat, lst).el;
  return tp;
}

// ================================================================== UI-51 eclipses
enum EclType : uint8_t { ECL_NONE, ECL_PENUMBRAL, ECL_PARTIAL_LUNAR, ECL_TOTAL_LUNAR, ECL_PARTIAL_SOLAR, ECL_ANNULAR, ECL_TOTAL_SOLAR };
struct Eclipse {
  uint8_t type = ECL_NONE;
  double t_max = 0;         // greatest eclipse (lunar: geocentric; solar: as seen here)
  double t0 = 0, t1 = 0;    // what can be seen from here: start and end (Moon/Sun up)
  double c0 = 0, c1 = 0;    // lunar: total phase (U2-U3); solar: central phase here
  float mag = 0;            // lunar: umbral (penumbral for ECL_PENUMBRAL); solar: fraction of the Sun's diameter
  float el = 0;             // elevation of the Moon/Sun at greatest (visible) eclipse
};
inline bool is_lunar(uint8_t k) { return k >= ECL_PENUMBRAL && k <= ECL_TOTAL_LUNAR; }

// golden-section minimum of f on [a, b]
template <class Fn> inline double gmin(Fn f, double a, double b, int it = 40) {
  const double g = 0.3819660112501051;
  double x1 = a + g * (b - a), x2 = b - g * (b - a), f1 = f(x1), f2 = f(x2);
  for (int i = 0; i < it; i++) {
    if (f1 < f2) {
      b = x2;
      x2 = x1;
      f2 = f1;
      x1 = a + g * (b - a);
      f1 = f(x1);
    } else {
      a = x1;
      x1 = x2;
      f1 = f2;
      x2 = b - g * (b - a);
      f2 = f(x2);
    }
  }
  return (a + b) / 2;
}
// first t in [a, b] where f changes sign (f(a) and f(b) differ), to ~1 s
template <class Fn> inline double bisect(Fn f, double a, double b) {
  const bool sa = f(a) > 0;
  for (int i = 0; i < 16; i++) {
    const double m = (a + b) / 2;
    if ((f(m) > 0) == sa)
      a = m;
    else
      b = m;
  }
  return (a + b) / 2;
}

// Moon's ecliptic latitude at a syzygy: no eclipse of either kind beyond ~1.6°
inline bool eclipse_possible(double t) { return fabs(moon_precise(astro::jd(t)).lat) < 1.6; }

// Lunar eclipse at the full Moon near t_full. Shadow radii after Meeus (Astronomical
// Algorithms ch. 54, with the 1.02 atmosphere enlargement).
inline bool lunar_eclipse(double t_full, double lat, double lon, double alt_m, Eclipse &out) {
  struct G {
    double d, pu, pp, sm;
  };
  auto geom = [](double t) {
    const double j = astro::jd(t);
    const MoonPos m = moon_precise(j);
    const double sl = astro::sun(j).lon_ecl, R = planets::sun_dist_au(j);
    G g;
    g.d = sep_deg(m.lon, m.lat, astro::wrap360(sl + 180.0), 0.0);
    const double pim = asin(6378.14 / m.dist_km) * 180.0 / M_PI, pis = 8.794 / 3600.0 / R, ss = 959.63 / 3600.0 / R;
    g.pu = 1.02 * (0.998340 * pim - ss + pis);
    g.pp = 1.02 * (0.998340 * pim + ss + pis);
    g.sm = asin(1737.4 / m.dist_km) * 180.0 / M_PI;
    return g;
  };
  const double tm = gmin([&](double t) { return geom(t).d; }, t_full - 5 * 3600.0, t_full + 5 * 3600.0);
  const G g = geom(tm);
  out = Eclipse();
  if (g.d >= g.pp + g.sm)
    return false;
  out.t_max = tm;
  out.type = g.d <= g.pu - g.sm ? ECL_TOTAL_LUNAR : g.d < g.pu + g.sm ? ECL_PARTIAL_LUNAR : ECL_PENUMBRAL;
  out.mag = (float) ((out.type == ECL_PENUMBRAL ? g.pp : g.pu) + g.sm - g.d) / (2 * g.sm);
  // the phase worth watching: umbral for partial/total, penumbral otherwise
  auto edge = [&](double t) {
    const G h = geom(t);
    return h.d - ((out.type == ECL_PENUMBRAL ? h.pp : h.pu) + h.sm);
  };
  const double a = bisect(edge, tm - 5 * 3600.0, tm), b = bisect(edge, tm, tm + 5 * 3600.0);
  if (out.type == ECL_TOTAL_LUNAR) {
    auto tot = [&](double t) {
      const G h = geom(t);
      return h.d - (h.pu - h.sm);
    };
    out.c0 = bisect(tot, a, tm);
    out.c1 = bisect(tot, tm, b);
  }
  // seen from here: the part of [a, b] with the Moon above the horizon
  out.t0 = out.t1 = 0;
  float best = -90;
  for (double t = a; t <= b + 1; t += 300) {
    const float el = astro::compute(std::min(t, b), lat, lon, alt_m).moon.el;
    if (el > 0) {
      if (out.t0 == 0)
        out.t0 = std::min(t, b);
      out.t1 = std::min(t, b);
    }
    if (fabs(t - tm) < 300)
      best = el;
  }
  out.el = best > -90 ? best : astro::compute(tm, lat, lon, alt_m).moon.el;
  return true;
}

// Solar eclipse seen from here at the new Moon near t_new: topocentric Sun-Moon
// separation against the sum of the two radii; only the part with the Sun up counts.
inline bool solar_eclipse(double t_new, double lat, double lon, double alt_m, Eclipse &out) {
  struct G {
    double s, rs, rm, sun_el;
  };
  auto geom = [&](double t) {
    const double j = astro::jd(t);
    double dk;
    const astro::Equ m = moon_topo(t, lat, lon, alt_m, &dk);
    const astro::Equ s = astro::sun(j);
    const double lst = astro::rad(astro::wrap360(astro::gmst_deg(j) + lon));
    G g;
    g.s = equ_sep_deg(m, s);
    g.rs = 0.26656 / planets::sun_dist_au(j);
    g.rm = asin(1737.4 / dk) * 180.0 / M_PI;
    g.sun_el = astro::horizontal(s, lat, lst).el;
    return g;
  };
  // coarse 10-minute scan, then refine the closest approach
  double tb = 0, sb = 1e9;
  for (double t = t_new - 6 * 3600.0; t <= t_new + 6 * 3600.0; t += 600) {
    const double s = geom(t).s;
    if (s < sb) {
      sb = s;
      tb = t;
    }
  }
  if (sb > 1.5)
    return false;
  const double tm = gmin([&](double t) { return geom(t).s; }, tb - 600, tb + 600, 30);
  const G g = geom(tm);
  out = Eclipse();
  if (g.s >= g.rs + g.rm)
    return false;  // not seen from here at all
  auto edge = [&](double t) {
    const G h = geom(t);
    return h.s - (h.rs + h.rm);
  };
  const double a = bisect(edge, tm - 4 * 3600.0, tm), b = bisect(edge, tm, tm + 4 * 3600.0);
  // visible part: Sun above the horizon
  double v0 = 0, v1 = 0, vbest = 0;
  double sbest = 1e9;
  for (double t = a; t <= b + 1; t += 120) {
    const double tt = std::min(t, b);
    const G h = geom(tt);
    if (h.sun_el > 0) {
      if (v0 == 0)
        v0 = tt;
      v1 = tt;
      if (h.s < sbest) {
        sbest = h.s;
        vbest = tt;
      }
    }
  }
  if (v0 == 0)
    return false;  // below the horizon throughout
  const double tv = (vbest > 0 && (tm < v0 || tm > v1)) ? vbest : tm;
  const G h = geom(tv);
  out.t_max = tv;
  out.t0 = v0;
  out.t1 = v1;
  out.mag = (float) ((h.rs + h.rm - h.s) / (2 * h.rs));
  out.el = (float) h.sun_el;
  out.type = ECL_PARTIAL_SOLAR;
  if (g.s < fabs(g.rm - g.rs) && tm >= v0 && tm <= v1) {
    out.type = g.rm > g.rs ? ECL_TOTAL_SOLAR : ECL_ANNULAR;
    auto cen = [&](double t) {
      const G k = geom(t);
      return k.s - fabs(k.rm - k.rs);
    };
    out.c0 = bisect(cen, tm - 900, tm);
    out.c1 = bisect(cen, tm, tm + 900);
  }
  return true;
}
inline const char *eclipse_name(uint8_t k) {
  switch (k) {
    case ECL_PENUMBRAL: return "Penumbral lunar eclipse";
    case ECL_PARTIAL_LUNAR: return "Partial lunar eclipse";
    case ECL_TOTAL_LUNAR: return "Total lunar eclipse";
    case ECL_PARTIAL_SOLAR: return "Partial solar eclipse";
    case ECL_ANNULAR: return "Annular solar eclipse";
    case ECL_TOTAL_SOLAR: return "Total solar eclipse";
    default: return "Eclipse";
  }
}

// ================================================================== Sun longitude events
// next time (after t) the Sun's apparent ecliptic longitude reaches lam (degrees)
inline double sun_lon_time(double t, double lam) {
  auto lon = [](double tt) { return astro::wrap360(astro::sun(astro::jd(tt)).lon_ecl); };
  double d0 = astro::wrap360(lam - lon(t));
  double te = t + d0 / 0.98561 * 86400.0;
  for (int i = 0; i < 4; i++) {
    double d = lon(te) - lam;
    d -= 360.0 * floor((d + 180.0) / 360.0);
    te -= d / 0.98561 * 86400.0;
  }
  return te;
}

// ================================================================== UI-47 meteor showers
// IMO working list: peak solar longitude (J2000), ZHR, radiant at the peak, active span
// in days either side of the peak, parent body.
struct Shower {
  const char *name;
  float lam;        // peak, solar longitude degrees
  int zhr;
  float ra, dec;    // radiant, degrees
  int8_t before, after;  // active days around the peak
  const char *parent;
};
constexpr int N_SHOWERS = 9;
const Shower SHOWERS[N_SHOWERS] = {
    {"Quadrantids", 283.15f, 110, 230.0f, 49.0f, 6, 8, "asteroid 2003 EH1"},
    {"Lyrids", 32.32f, 18, 271.0f, 34.0f, 8, 8, "comet Thatcher"},
    {"Eta Aquariids", 45.5f, 50, 338.0f, -1.0f, 17, 22, "Halley's Comet"},
    {"Perseids", 140.0f, 100, 48.0f, 58.0f, 26, 12, "comet Swift-Tuttle"},
    {"Draconids", 195.4f, 10, 262.0f, 54.0f, 2, 2, "comet Giacobini-Zinner"},
    {"Orionids", 208.0f, 20, 95.0f, 16.0f, 19, 17, "Halley's Comet"},
    {"Leonids", 235.27f, 15, 152.0f, 22.0f, 11, 13, "comet Tempel-Tuttle"},
    {"Geminids", 262.2f, 150, 112.0f, 33.0f, 10, 6, "asteroid 3200 Phaethon"},
    {"Ursids", 270.7f, 10, 217.0f, 76.0f, 5, 4, "comet Tuttle"},
};

// ================================================================== UI-48 full Moon names
inline const char *full_moon_name(int month) {  // month 1..12 (northern names)
  static const char *const N[12] = {"Wolf Moon",   "Snow Moon", "Worm Moon",     "Pink Moon",
                                    "Flower Moon", "Strawberry Moon", "Buck Moon", "Sturgeon Moon",
                                    "Corn Moon",   "Hunter's Moon",   "Beaver Moon", "Cold Moon"};
  return N[(month + 11) % 12];
}

// ================================================================== UI-49/50 bright stars
struct BrightStar {
  const char *name;
  float ra, dec, mag;  // J2000 degrees; magnitude
  const char *dist;    // light years, as text
  const char *where;   // constellation
  const char *fact;
};
constexpr int N_BRIGHT = 20;
const BrightStar BRIGHT[N_BRIGHT] = {
    {"Sirius", 101.287f, -16.716f, -1.46f, "8.6", "Canis Major",
     "The brightest star in the night sky, with a white dwarf companion the size of the Earth."},
    {"Arcturus", 213.915f, 19.182f, -0.05f, "37", "Bootes",
     "An orange giant: follow the arc of the Big Dipper's handle to find it."},
    {"Vega", 279.235f, 38.784f, 0.03f, "25", "Lyra",
     "The pole star 14,000 years ago and again in 12,000 years; a corner of the Summer Triangle."},
    {"Capella", 79.172f, 45.998f, 0.08f, "43", "Auriga",
     "Two yellow giants orbiting each other; the sixth-brightest star in the sky."},
    {"Rigel", 78.634f, -8.202f, 0.13f, "860", "Orion",
     "A blue supergiant at Orion's foot, some 100,000 times as luminous as the Sun."},
    {"Procyon", 114.825f, 5.225f, 0.34f, "11.5", "Canis Minor",
     "Its name means 'before the dog': it rises just ahead of Sirius."},
    {"Betelgeuse", 88.793f, 7.407f, 0.50f, "550", "Orion",
     "A red supergiant that would reach past the orbit of Mars; it will end as a supernova."},
    {"Altair", 297.696f, 8.868f, 0.76f, "17", "Aquila",
     "Spins once every 9 hours, so fast that it bulges at the equator."},
    {"Aldebaran", 68.980f, 16.509f, 0.86f, "65", "Taurus",
     "The orange eye of the Bull, in front of (not part of) the Hyades cluster."},
    {"Spica", 201.298f, -11.161f, 0.97f, "250", "Virgo",
     "Two hot blue stars so close they are egg-shaped; the ear of wheat in Virgo's hand."},
    {"Antares", 247.352f, -26.432f, 1.0f, "550", "Scorpius",
     "'Rival of Mars' for its red colour; the heart of the Scorpion."},
    {"Pollux", 116.329f, 28.026f, 1.14f, "34", "Gemini",
     "The brighter Gemini twin, an orange giant with a planet of its own."},
    {"Deneb", 310.358f, 45.280f, 1.25f, "2,600", "Cygnus",
     "One of the most luminous stars known: seen from this far, it is still first rank."},
    {"Regulus", 152.093f, 11.967f, 1.35f, "79", "Leo",
     "The Lion's heart; it spins in under 16 hours and is flattened by it."},
    {"Castor", 113.650f, 31.888f, 1.58f, "51", "Gemini",
     "Six stars in one: three pairs, all orbiting each other."},
    {"Bellatrix", 81.283f, 6.350f, 1.64f, "250", "Orion",
     "'Female warrior', Orion's western shoulder."},
    {"Alnilam", 84.053f, -1.202f, 1.69f, "1,300", "Orion",
     "The middle star of Orion's Belt, a blue supergiant."},
    {"Polaris", 37.955f, 89.264f, 1.98f, "430", "Ursa Minor",
     "The North Star, under 1 degree from the pole: its height above your horizon equals your latitude."},
    {"Algol", 47.042f, 40.956f, 2.1f, "90", "Perseus",
     "The Demon Star: it fades for a few hours every 2.87 days as its companion passes in front."},
    {"Mizar", 200.981f, 54.925f, 2.2f, "83", "Ursa Major",
     "In the Big Dipper's handle; with faint Alcor beside it, an old test of eyesight."},
};

// ================================================================== UI-56 deep-sky objects
struct DeepSky {
  const char *name, *cat;  // "Andromeda Galaxy", "M31"
  float ra, dec, mag;      // J2000 degrees
  const char *kind, *dist, *where, *fact;  // distance in light years, as text
};
constexpr int N_DSO = 12;
const DeepSky DSO[N_DSO] = {
    {"Andromeda Galaxy", "M31", 10.685f, 41.269f, 3.4f, "Spiral galaxy", "2.5 million", "Andromeda",
     "The farthest thing you can see with the eye alone: a trillion stars, headed our way."},
    {"Triangulum Galaxy", "M33", 23.462f, 30.660f, 5.7f, "Spiral galaxy", "2.7 million", "Triangulum",
     "A face-on spiral, the test of a truly dark sky."},
    {"Pleiades", "M45", 56.750f, 24.117f, 1.6f, "Open cluster", "444", "Taurus",
     "The Seven Sisters: young blue stars still wrapped in the dust they pass through."},
    {"Orion Nebula", "M42", 83.822f, -5.391f, 4.0f, "Nebula", "1,340", "Orion",
     "A stellar nursery below Orion's Belt, lit by the four Trapezium stars."},
    {"Double Cluster", "NGC 869/884", 34.750f, 57.133f, 3.7f, "Open clusters", "7,500", "Perseus",
     "Two star clusters side by side, between Perseus and Cassiopeia."},
    {"Beehive Cluster", "M44", 130.100f, 19.667f, 3.7f, "Open cluster", "577", "Cancer",
     "A swarm of stars that Galileo was first to see as separate points."},
    {"Hercules Cluster", "M13", 250.423f, 36.461f, 5.8f, "Globular cluster", "22,200", "Hercules",
     "Several hundred thousand stars in a ball; the 1974 Arecibo message was sent its way."},
    {"Bode's Galaxy", "M81", 148.888f, 69.065f, 6.9f, "Spiral galaxy", "12 million", "Ursa Major",
     "A bright spiral near the Big Dipper, with the Cigar Galaxy (M82) beside it."},
    {"Whirlpool Galaxy", "M51", 202.470f, 47.195f, 8.4f, "Spiral galaxy", "23 million", "Canes Venatici",
     "The first galaxy seen to be a spiral, by Lord Rosse in 1845."},
    {"Ring Nebula", "M57", 283.396f, 33.029f, 8.8f, "Planetary nebula", "2,570", "Lyra",
     "The shell of gas a dying Sun-like star threw off; a telescope shows the ring."},
    {"Dumbbell Nebula", "M27", 299.901f, 22.721f, 7.5f, "Planetary nebula", "1,360", "Vulpecula",
     "The brightest nebula of its kind, shaped like an apple core."},
    {"Lagoon Nebula", "M8", 270.904f, -24.387f, 6.0f, "Nebula", "4,100", "Sagittarius",
     "A star-forming cloud in Sagittarius, seen by eye from dark sites."},
};

}  // namespace ev
}  // namespace sat
