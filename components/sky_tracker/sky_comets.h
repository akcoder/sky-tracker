#pragma once
// UI-63 comets: orbital elements from the JPL Small-Body Database query API (heliocentric,
// J2000 ecliptic: q, e, time of perihelion, i, node, argument of perihelion; total
// magnitude parameters M1, K1). Positions by two-body motion in universal variables, so
// ellipses, near-parabolas and hyperbolas all take the same path (Danby, "Fundamentals of
// Celestial Mechanics" 6.9; Laguerre-Conway iteration). Magnitude m = M1 + 5 log D + K1 log r
// (JPL's total-magnitude law; comets often stray a magnitude or two from it).
#include <cmath>
#include <cstring>
#include "sky_math.h"
#include "sky_planets.h"

namespace sat {
namespace comets {

struct El {
  char name[36];  // JPL full name, trimmed: "C/2023 A3 (Tsuchinshan-ATLAS)", "12P/Pons-Brooks"
  char tag[24];   // what the map shows (short_tag)
  double q, e, tp, i, om, w;  // AU, -, JD (TDB), degrees
  float M1, K1;
};

// UI-63: the name for the map. Survey names (PANSTARRS, ATLAS, ...) are shared by hundreds of
// comets, so those keep their designation; a discoverer's name is how people know a comet.
inline void short_tag(const char *full, char *out, size_t n) {
  static const char *const SURVEY[] = {"PANSTARRS", "PanSTARRS", "ATLAS",    "LINEAR",  "NEAT",   "LONEOS", "Catalina",
                                       "ZTF",       "MAPS",      "Spacewatch", "NEOWISE-survey", "Siding Spring", "SOHO",
                                       "WISE",      "Bok"};
  const char *lp = strchr(full, '('), *rp = lp ? strchr(lp, ')') : nullptr;
  if (lp && rp && rp > lp + 1) {  // "C/2023 A3 (Tsuchinshan-ATLAS)"
    char nm[40];
    const size_t k = std::min((size_t) (rp - lp - 1), sizeof(nm) - 1);
    memcpy(nm, lp + 1, k);
    nm[k] = 0;
    bool survey = false;
    for (const char *s : SURVEY)
      survey |= strcmp(nm, s) == 0;
    if (!survey) {
      snprintf(out, n, "%s", nm);
      return;
    }
    size_t d = (size_t) (lp - full);  // the designation before " ("
    while (d > 0 && full[d - 1] == ' ')
      d--;
    snprintf(out, n, "%.*s", (int) d, full);
    return;
  }
  const char *sl = strchr(full, '/');  // "12P/Pons-Brooks" -> "Pons-Brooks"; "P/2026 N2" stays
  if (sl && sl > full && full[0] >= '0' && full[0] <= '9' && sl[1]) {
    bool survey = false;
    for (const char *s : SURVEY)
      survey |= strncmp(sl + 1, s, strlen(s)) == 0;
    if (!survey) {
      snprintf(out, n, "%s", sl + 1);
      return;
    }
    snprintf(out, n, "%.*s", (int) (sl - full + 1), full);  // "240P/"
    const size_t k = strlen(out);
    if (k && out[k - 1] == '/')
      out[k - 1] = 0;
    return;
  }
  snprintf(out, n, "%s", full);
}

inline void stumpff(double z, double &C, double &S) {
  if (z > 1e-3) {
    const double s = sqrt(z);
    C = (1 - cos(s)) / z;
    S = (s - sin(s)) / (z * s);
  } else if (z < -1e-3) {
    const double s = sqrt(-z);
    C = (cosh(s) - 1) / -z;
    S = (sinh(s) - s) / (-z * s);
  } else {
    C = 0.5 - z / 24 + z * z / 720 - z * z * z / 40320;
    S = 1.0 / 6 - z / 120 + z * z / 5040 - z * z * z / 362880;
  }
}

// Heliocentric J2000 ecliptic position (AU) at Julian date jdv. False if it did not converge.
inline bool helio(const El &c, double jdv, double out[3]) {
  constexpr double K = 0.01720209895;  // Gaussian gravitational constant, AU^1.5 / day
  const double q = c.q, e = c.e, alpha = (1 - e) / q, dt = jdv - c.tp;
  const double v0 = K * sqrt((1 + e) / q);
  // solve  e chi^3 S(z) + q chi = K dt,  z = alpha chi^2
  double chi = K * dt / q;
  if (alpha > 0) {  // an ellipse: start near the answer for a circle of that period
    const double lim = 2 * M_PI / sqrt(alpha);
    if (fabs(chi) > lim)
      chi = chi > 0 ? lim : -lim;
  }
  if (fabs(chi) > 5.0)
    chi = std::cbrt(6 * K * dt / std::max(e, 1e-6));  // far from perihelion: parabola-like start
  bool ok = false;
  double C = 0.5, S = 1.0 / 6;
  for (int it = 0; it < 60; it++) {
    const double z = alpha * chi * chi;
    stumpff(z, C, S);
    const double F = e * chi * chi * chi * S + q * chi - K * dt;
    const double F1 = e * chi * chi * C + q;              // r
    const double F2 = e * chi * (1 - z * S);              // dr/dchi
    const double disc = sqrt(fabs(16 * F1 * F1 - 20 * F * F2));
    const double den = F1 + (F1 >= 0 ? disc : -disc);
    const double d = den != 0 ? 5 * F / den : F / F1;
    chi -= d;
    if (fabs(d) <= 1e-12 * std::max(1.0, fabs(chi))) {
      ok = true;
      break;
    }
  }
  const double z = alpha * chi * chi;
  stumpff(z, C, S);
  const double f = 1 - chi * chi * C / q, g = dt - chi * chi * chi * S / K;
  const double xp = f * q, yp = g * v0;
  const double w = astro::rad(c.w), Om = astro::rad(c.om), I = astro::rad(c.i);
  const double cw = cos(w), sw = sin(w), cO = cos(Om), sO = sin(Om), cI = cos(I), sI = sin(I);
  out[0] = (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp;
  out[1] = (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp;
  out[2] = (sw * sI) * xp + (cw * sI) * yp;
  return ok && std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

struct Pos {
  astro::Equ equ;       // geocentric RA/Dec of date (radians)
  astro::Equ tail;      // a point 0.1 AU down the tail (straight away from the Sun), for its direction
  double r, d;          // from the Sun, from the Earth (AU)
  float mag;            // predicted total magnitude
  float elong;          // from the Sun, degrees
  bool ok;
};

// geocentric J2000 ecliptic vector -> RA/Dec of date (as planets::position)
inline astro::Equ equ_of(double x, double y, double z, double jdv) {
  const double T = (jdv - 2451545.0) / 36525.0, d = sqrt(x * x + y * y + z * z);
  const double lam = atan2(y, x) + astro::rad(1.3969713 * T), bet = asin(z / d);
  const double eps = astro::rad(23.439 - 0.0000004 * (jdv - 2451545.0));
  const double xe = cos(bet) * cos(lam);
  const double ye = cos(eps) * cos(bet) * sin(lam) - sin(eps) * sin(bet);
  const double ze = sin(eps) * cos(bet) * sin(lam) + cos(eps) * sin(bet);
  astro::Equ o;
  o.ra = atan2(ye, xe);
  o.dec = asin(ze);
  o.lon_ecl = astro::wrap360(lam * 180.0 / M_PI);
  return o;
}

inline Pos position(const El &c, double jdv) {
  Pos o{};
  double P[3], E[3];
  o.ok = helio(c, jdv, P);
  if (!o.ok)
    return o;
  planets::helio(5, (jdv - 2451545.0) / 36525.0, E);
  const double x = P[0] - E[0], y = P[1] - E[1], z = P[2] - E[2];
  o.r = sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
  o.d = sqrt(x * x + y * y + z * z);
  o.equ = equ_of(x, y, z, jdv);
  const double s = 0.1 / o.r;
  o.tail = equ_of(x + P[0] * s, y + P[1] * s, z + P[2] * s, jdv);
  const double R = sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
  const double ce = std::max(-1.0, std::min(1.0, (R * R + o.d * o.d - o.r * o.r) / (2 * R * o.d)));
  o.elong = (float) (acos(ce) * 180.0 / M_PI);
  o.mag = (float) (c.M1 + 5 * log10(o.d) + c.K1 * log10(o.r));
  return o;
}

}  // namespace comets
}  // namespace sat
