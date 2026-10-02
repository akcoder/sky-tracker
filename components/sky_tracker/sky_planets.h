#pragma once
// UI-40 planets: Mercury, Venus, Mars, Jupiter, Saturn from JPL's approximate Keplerian
// elements (E. M. Standish, "Approximate Positions of the Planets", table 1, valid
// 1800-2050: a few arcminutes, far below a map pixel), with the Earth-Moon barycentre
// standing in for the Earth. Magnitudes from the Explanatory Supplement / Meeus 41
// phase laws, Saturn with its ring tilt (Meeus 45). Checked against PyEphem: within 0.08°
// and 0.2 mag.
#include <cmath>
#include "sky_math.h"

namespace sat {
namespace planets {

enum Id : uint8_t { MERCURY = 0, VENUS, MARS, JUPITER, SATURN, N_PLANETS };
inline const char *name(int p) {
  static const char *const N[N_PLANETS] = {"Mercury", "Venus", "Mars", "Jupiter", "Saturn"};
  return N[p];
}

struct Elem {
  double a, da, e, de, i, di, L, dL, w, dw, O, dO;  // J2000 values and rates per century
};
// index 0..4 = the planets above, 5 = Earth-Moon barycentre
static const Elem EL[6] = {
    {0.38709927, 0.00000037, 0.20563593, 0.00001906, 7.00497902, -0.00594749, 252.25032350, 149472.67411175,
     77.45779628, 0.16047689, 48.33076593, -0.12534081},
    {0.72333566, 0.00000390, 0.00677672, -0.00004107, 3.39467605, -0.00078890, 181.97909950, 58517.81538729,
     131.60246718, 0.00268329, 76.67984255, -0.27769418},
    {1.52371034, 0.00001847, 0.09339410, 0.00007882, 1.84969142, -0.00813131, -4.55343205, 19140.30268499,
     -23.94362959, 0.44441088, 49.55953891, -0.29257343},
    {5.20288700, -0.00011607, 0.04838624, -0.00013253, 1.30439695, -0.00183714, 34.39644051, 3034.74612775,
     14.72847983, 0.21252668, 100.47390909, 0.20469106},
    {9.53667594, -0.00125060, 0.05386179, -0.00050991, 2.48599187, 0.00193609, 49.95424423, 1222.49362201,
     92.59887831, -0.41897216, 113.66242448, -0.28867794},
    {1.00000261, 0.00000562, 0.01671123, -0.00004392, -0.00001531, -0.01294668, 100.46457166, 35999.37244981,
     102.93768193, 0.32327364, 0.0, 0.0},
};

// Heliocentric ecliptic (J2000) position, AU.
inline void helio(int k, double T, double out[3]) {
  const Elem &E = EL[k];
  const double a = E.a + E.da * T, e = E.e + E.de * T, I = astro::rad(E.i + E.di * T);
  const double L = E.L + E.dL * T, wb = E.w + E.dw * T, O = E.O + E.dO * T;
  const double w = astro::rad(wb - O), Om = astro::rad(O);
  double M = astro::rad(astro::wrap360(L - wb));
  if (M > M_PI)
    M -= 2 * M_PI;
  double Ea = M + e * sin(M);
  for (int n = 0; n < 8; n++)
    Ea -= (Ea - e * sin(Ea) - M) / (1 - e * cos(Ea));
  const double xp = a * (cos(Ea) - e), yp = a * sqrt(1 - e * e) * sin(Ea);
  const double cw = cos(w), sw = sin(w), cO = cos(Om), sO = sin(Om), cI = cos(I), sI = sin(I);
  out[0] = (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp;
  out[1] = (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp;
  out[2] = (sw * sI) * xp + (cw * sI) * yp;
}

struct Pos {
  astro::Equ equ;  // geocentric RA/Dec of date (radians)
  double dist_au;  // from the Earth
  float mag;       // apparent visual magnitude
  float elong;     // angle from the Sun, degrees
};

// UI-36b: Earth-Sun distance in AU (the Earth-Moon barycentre's, within 0.00003 AU)
inline double sun_dist_au(double jdv) {
  double E[3];
  helio(5, (jdv - 2451545.0) / 36525.0, E);
  return sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
}

inline Pos position(int p, double jdv) {
  const double T = (jdv - 2451545.0) / 36525.0;
  double P[3], E[3];
  helio(p, T, P);
  helio(5, T, E);
  const double x = P[0] - E[0], y = P[1] - E[1], z = P[2] - E[2];
  const double d = sqrt(x * x + y * y + z * z);
  const double r = sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
  const double R = sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
  // J2000 ecliptic -> ecliptic of date (general precession in longitude)
  const double lam = atan2(y, x) + astro::rad(1.3969713 * T), bet = asin(z / d);
  const double eps = astro::rad(23.439 - 0.0000004 * (jdv - 2451545.0));
  const double xe = cos(bet) * cos(lam);
  const double ye = cos(eps) * cos(bet) * sin(lam) - sin(eps) * sin(bet);
  const double ze = sin(eps) * cos(bet) * sin(lam) + cos(eps) * sin(bet);
  Pos o;
  o.equ.ra = atan2(ye, xe);
  o.equ.dec = asin(ze);
  o.equ.lon_ecl = astro::wrap360(lam * 180.0 / M_PI);
  o.dist_au = d;
  // phase angle (Sun-planet-Earth) and elongation (Sun-Earth-planet)
  const double ci = std::max(-1.0, std::min(1.0, (r * r + d * d - R * R) / (2 * r * d)));
  const double i = acos(ci) * 180.0 / M_PI;
  const double ce = std::max(-1.0, std::min(1.0, (R * R + d * d - r * r) / (2 * R * d)));
  o.elong = (float) (acos(ce) * 180.0 / M_PI);
  const double m5 = 5 * log10(r * d);
  double m = 0;
  switch (p) {
    case MERCURY: m = -0.42 + m5 + 0.0380 * i - 0.000273 * i * i + 0.000002 * i * i * i; break;
    case VENUS: m = -4.40 + m5 + 0.0009 * i + 0.000239 * i * i - 0.00000065 * i * i * i; break;
    case MARS: m = -1.52 + m5 + 0.016 * i; break;
    case JUPITER: m = -9.40 + m5 + 0.005 * i; break;
    default: {  // Saturn plus its rings (Meeus 45: ring tilt B towards the Earth)
      const double ir = astro::rad(28.075216 - 0.012998 * T), Or = astro::rad(169.508470 + 1.394681 * T);
      const double la0 = atan2(y, x), be0 = asin(z / d);
      const double sB = fabs(sin(ir) * cos(be0) * sin(la0 - Or) - cos(ir) * sin(be0));
      m = -8.88 + m5 + 0.044 * i - 2.60 * sB + 1.25 * sB * sB;
      break;
    }
  }
  o.mag = (float) m;
  return o;
}

// Where it is in the observer's sky (refracted), at unix time t.
inline geo::AzEl horizontal(int p, double t, double lat, double lon, Pos *pos = nullptr) {
  const double j = astro::jd(t);
  const Pos ps = position(p, j);
  if (pos)
    *pos = ps;
  const double lst = astro::rad(astro::wrap360(astro::gmst_deg(j) + lon));
  return astro::horizontal(ps.equ, lat, lst);
}

}  // namespace planets
}  // namespace sat
