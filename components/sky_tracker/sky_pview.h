#pragma once
// UI-61: what a planet looks like tonight, drawn on the device (no network).
//  - phase: the Sun-planet-Earth angle and the position angle of the bright limb
//  - axis: the position angle of the planet's north pole and the latitude the Earth sees
//    (IAU pole directions), which give Saturn's ring tilt and the plane of Jupiter's moons
//  - Jupiter's four big moons: Meeus, Astronomical Algorithms ch. 44 (lower accuracy)
// Checked against PyEphem (host tests). Pictures are north up, east left, as the sky
// looks to the eye or in binoculars.
#include <cmath>
#include <cstdint>
#include <cstring>
#include "sky_math.h"
#include "sky_planets.h"

namespace sat {
namespace pview {

struct Moon {
  float x, y;     // planet radii; x positive west (right), y positive north (up)
  bool behind;    // hidden behind the disc
  bool in_front;  // crossing the disc
};
struct View {
  int planet = 0;
  float phase_deg = 0;   // Sun-planet-Earth
  float lit = 1;         // illuminated fraction
  float limb_pa = 0;     // position angle of the bright limb, degrees (N=0, E=90)
  float pole_pa = 0;     // position angle of the north pole
  float earth_lat = 0;   // planetocentric latitude of the Earth (Saturn: ring tilt B)
  float diam = 0;        // equatorial diameter, arcseconds
  double dist_au = 0;
  Moon moon[4];          // Jupiter: Io, Europa, Ganymede, Callisto
};

// IAU pole (J2000 RA, Dec, degrees) and equatorial diameter at 1 AU (arcsec)
struct Body {
  double ra0, dec0, d1au, flat;
};
inline const Body &body(int p) {
  static const Body B[planets::N_PLANETS] = {
      {281.0103, 61.4155, 6.74, 0.0},      // Mercury
      {272.76, 67.16, 16.92, 0.0},         // Venus
      {317.681, 52.887, 9.36, 0.0059},     // Mars
      {268.057, 64.495, 196.94, 0.0649},   // Jupiter
      {40.589, 83.537, 165.6, 0.0980},     // Saturn
  };
  return B[p];
}

// position angle of the direction to (ra1, dec1) as seen at (ra, dec), degrees
inline double pos_angle(double ra, double dec, double ra1, double dec1) {
  const double d = ra1 - ra;
  return astro::wrap360(atan2(cos(dec1) * sin(d), sin(dec1) * cos(dec) - cos(dec1) * sin(dec) * cos(d)) * 180.0 / M_PI);
}

inline void galilean(double jdv, float earth_lat_deg, Moon out[4]);

inline View compute(int p, double jdv) {
  View v;
  v.planet = p;
  const planets::Pos ps = planets::position(p, jdv);
  const astro::Equ su = astro::sun(jdv);
  const double T = (jdv - 2451545.0) / 36525.0;
  double P[3], E[3];
  planets::helio(p, T, P);
  planets::helio(5, T, E);
  const double r = sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
  const double R = sqrt(E[0] * E[0] + E[1] * E[1] + E[2] * E[2]);
  const double d = ps.dist_au;
  const double ci = std::max(-1.0, std::min(1.0, (r * r + d * d - R * R) / (2 * r * d)));
  v.phase_deg = (float) (acos(ci) * 180.0 / M_PI);
  v.lit = (float) ((1 + ci) / 2);
  v.limb_pa = (float) pos_angle(ps.equ.ra, ps.equ.dec, su.ra, su.dec);
  const Body &b = body(p);
  const double a0 = astro::rad(b.ra0), d0 = astro::rad(b.dec0);
  v.pole_pa = (float) pos_angle(ps.equ.ra, ps.equ.dec, a0, d0);
  const double sD = -sin(d0) * sin(ps.equ.dec) - cos(d0) * cos(ps.equ.dec) * cos(a0 - ps.equ.ra);
  v.earth_lat = (float) (asin(std::max(-1.0, std::min(1.0, sD))) * 180.0 / M_PI);
  v.diam = (float) (b.d1au / d);
  v.dist_au = d;
  if (p == planets::JUPITER)
    galilean(jdv, v.earth_lat, v.moon);
  return v;
}

// Meeus ch. 44 (lower accuracy), with the Earth's jovicentric latitude from the pole above
inline void galilean(double jdv, float earth_lat_deg, Moon out[4]) {
  auto rd = [](double x) { return x * M_PI / 180.0; };
  const double d = jdv - 2451545.0;
  const double V = 172.74 + 0.00111588 * d, M = 357.529 + 0.9856003 * d;
  const double N = 20.020 + 0.0830853 * d + 0.329 * sin(rd(V));
  const double J = 66.115 + 0.9025179 * d - 0.329 * sin(rd(V));
  const double A = 1.915 * sin(rd(M)) + 0.020 * sin(rd(2 * M));
  const double B = 5.555 * sin(rd(N)) + 0.168 * sin(rd(2 * N));
  const double K = J + A - B;
  const double R = 1.00014 - 0.01671 * cos(rd(M)) - 0.00014 * cos(rd(2 * M));
  const double r = 5.20872 - 0.25208 * cos(rd(N)) - 0.00611 * cos(rd(2 * N));
  const double dl = sqrt(r * r + R * R - 2 * r * R * cos(rd(K)));
  const double psi = asin(R / dl * sin(rd(K))) * 180.0 / M_PI;
  const double t = d - dl / 173.0;
  double u1 = 163.8069 + 203.4058646 * t + psi - B;
  double u2 = 358.4140 + 101.2916335 * t + psi - B;
  double u3 = 5.7176 + 50.2345180 * t + psi - B;
  double u4 = 224.8092 + 21.4879800 * t + psi - B;
  const double G = 331.18 + 50.310482 * t, H = 87.45 + 21.569231 * t;
  const double c1 = 0.473 * sin(rd(2 * (u1 - u2))), c2 = 1.065 * sin(rd(2 * (u2 - u3)));
  const double c3 = 0.165 * sin(rd(G)), c4 = 0.843 * sin(rd(H));
  const double r1 = 5.9057 - 0.0244 * cos(rd(2 * (u1 - u2))), r2 = 9.3966 - 0.0882 * cos(rd(2 * (u2 - u3)));
  const double r3 = 14.9883 - 0.0216 * cos(rd(G)), r4 = 26.3627 - 0.1939 * cos(rd(H));
  const double u[4] = {u1 + c1, u2 + c2, u3 + c3, u4 + c4}, rr[4] = {r1, r2, r3, r4};
  const double sDE = sin(rd(earth_lat_deg));
  for (int i = 0; i < 4; i++) {
    const double uu = rd(u[i]);
    Moon &m = out[i];
    m.x = (float) (rr[i] * sin(uu));
    m.y = (float) (-rr[i] * cos(uu) * sDE);
    const bool near = m.x * m.x + m.y * m.y < 1.0f;
    const bool far_side = cos(uu) < 0;  // u = 0 at inferior conjunction (in front; checked against PyEphem)
    m.behind = near && far_side;
    m.in_front = near && !far_side;
  }
}

// ---- drawing (RGB565, W x H, black sky). Returns where Jupiter's moons were drawn.
struct Marks {
  int n = 0;
  int x[4], y[4];
  bool shown[4];
  int strip_y = -1, strip_x0 = 0, strip_x1 = 0;  // Jupiter's moon strip (for the E / W labels)
};
struct Rgb {
  float r, g, b;
};
inline Rgb jupiter_band(float lat) {  // degrees, planetographic enough for a sketch
  const Rgb zone{232, 216, 186}, belt{176, 124, 88}, polar{176, 166, 152};
  const float a = fabsf(lat);
  auto mix = [](Rgb x, Rgb y, float k) { return Rgb{x.r + (y.r - x.r) * k, x.g + (y.g - x.g) * k, x.b + (y.b - x.b) * k}; };
  auto band = [&](float lo, float hi) {  // soft-edged belt between lo..hi
    const float e = 2.0f;
    if (lat < lo - e || lat > hi + e)
      return 0.0f;
    if (lat < lo)
      return (lat - (lo - e)) / e;
    if (lat > hi)
      return ((hi + e) - lat) / e;
    return 1.0f;
  };
  float k = std::max({band(7, 18), band(-21, -7), 0.6f * band(24, 30), 0.6f * band(-33, -27)});
  Rgb c = mix(zone, belt, k);
  if (a > 42)
    c = mix(c, polar, std::min(1.0f, (a - 42) / 12));
  return c;
}
inline void draw_strip(const View &v, uint16_t *buf, int W, int H, Marks &mk, float sy0);
inline void render(const View &v, uint16_t *buf, int W, int H, Marks &mk) {
  // UI-62b: drawn on the picture screen's own colour (0x070B18), like the masked photos
  const Rgb sky{7, 11, 24};
  const uint16_t sky565 = (uint16_t) ((7 >> 3) << 11 | (11 >> 2) << 5 | (24 >> 3));
  for (int k = 0; k < W * H; k++)
    buf[k] = sky565;
  const int p = v.planet;
  const Body &b = body(p);
  const bool jup = p == planets::JUPITER, sat_ = p == planets::SATURN;
  const float R = jup ? 0.27f * W : sat_ ? 0.2f * W : 0.33f * W;
  const float cx = W / 2.0f, cy = jup ? 0.34f * H : 0.5f * H;
  const float th = astro::rad(v.pole_pa), ch = astro::rad(v.limb_pa);
  const float ct = cosf(th), st = sinf(th);
  const float i = astro::rad(v.phase_deg), si = sinf(i), ci = cosf(i);
  const float sE = sinf(ch) * si, sN = cosf(ch) * si;  // Sun direction (E, N, towards us)
  const float D = astro::rad(v.earth_lat), sD = sinf(D), cD = cosf(D);
  const float flat = (float) b.flat;
  const float sB = sinf(D);  // Saturn: ring opening
  const Rgb base = p == planets::MERCURY ? Rgb{176, 166, 156}
                   : p == planets::VENUS ? Rgb{242, 232, 204}
                   : p == planets::MARS  ? Rgb{214, 112, 62}
                   : sat_                ? Rgb{222, 196, 140}
                                         : Rgb{232, 216, 186};
  auto shade = [&](float E, float N, Rgb &out) -> bool {  // one sample; false = empty sky
    // planet frame: x along the equator (west), y towards the north pole
    const float xp = -E * ct + N * st, yp = E * st + N * ct;
    const float yf = yp / (1 - flat);
    const float rr = xp * xp + yf * yf;
    // rings (Saturn): radius in the ring plane, near half in front of the disc
    float ring_a = 0;
    Rgb ring{0, 0, 0};
    bool ring_front = false;
    if (sat_ && fabsf(sB) > 0.005f) {
      const float ys = yp / sB;
      const float rho = sqrtf(xp * xp + ys * ys);
      if (rho >= 1.24f && rho <= 2.27f) {
        if (rho < 1.53f)
          ring = {150, 130, 100}, ring_a = 0.25f;  // C ring
        else if (rho < 1.95f)
          ring = {236, 220, 184}, ring_a = 1.0f;   // B ring
        else if (rho < 2.03f)
          ring_a = 0;                               // Cassini division
        else
          ring = {204, 188, 156}, ring_a = 0.9f;   // A ring
        ring_front = yp * sB < 0;
      }
    }
    Rgb c{0, 0, 0};
    bool any = false;
    if (rr <= 1.0f) {
      const float nz = sqrtf(std::max(0.0f, 1 - E * E - N * N));
      const float lam = E * sE + N * sN + nz * ci;  // Lambert
      // bright to near the terminator (atmospheres and rough surfaces), mild limb darkening
      float k = lam <= 0 ? 0.0f : powf(std::min(1.0f, lam * 1.4f), 0.45f) * (0.62f + 0.38f * nz);
      Rgb col = base;
      if (jup || sat_ || p == planets::MARS) {
        const float zf = sqrtf(std::max(0.0f, 1 - rr));
        const float lat = asinf(std::max(-1.0f, std::min(1.0f, yf * cD + zf * sD))) * 57.29578f;
        if (jup)
          col = jupiter_band(lat);
        else if (sat_)
          col = fabsf(lat) < 20 ? Rgb{230, 206, 150} : fabsf(lat) > 55 ? Rgb{190, 180, 150} : base;
        else if (fabsf(lat) > 76)
          col = {245, 240, 235};  // Mars: polar caps
      }
      c = {col.r * k, col.g * k, col.b * k};
      any = true;
    }
    if (ring_a > 0 && (ring_front || rr > 1.0f)) {
      c = {c.r + (ring.r - c.r) * ring_a, c.g + (ring.g - c.g) * ring_a, c.b + (ring.b - c.b) * ring_a};
      any = true;
    }
    out = c;
    return any;
  };
  const int y0 = std::max(0, (int) (cy - (sat_ ? 2.3f : 1.1f) * R)), y1 = std::min(H - 1, (int) (cy + (sat_ ? 2.3f : 1.1f) * R));
  const int x0 = std::max(0, (int) (cx - (sat_ ? 2.3f : 1.1f) * R)), x1 = std::min(W - 1, (int) (cx + (sat_ ? 2.3f : 1.1f) * R));
  for (int py = y0; py <= y1; py++)
    for (int px = x0; px <= x1; px++) {
      float r = 0, g = 0, bl = 0;
      int n = 0;
      for (int sy = 0; sy < 2; sy++)
        for (int sx = 0; sx < 2; sx++) {
          const float E = -((px + 0.25f + 0.5f * sx) - cx) / R, N = (cy - (py + 0.25f + 0.5f * sy)) / R;
          Rgb c;
          if (!shade(E, N, c))
            c = sky;
          r += std::max(c.r, sky.r), g += std::max(c.g, sky.g), bl += std::max(c.b, sky.b);
          n++;
        }
      r /= n, g /= n, bl /= n;
      buf[py * W + px] = (uint16_t) (((int) std::min(255.0f, r) >> 3) << 11 | ((int) std::min(255.0f, g) >> 2) << 5 |
                                     ((int) std::min(255.0f, bl) >> 3));
    }
  mk = Marks();
  if (jup)
    draw_strip(v, buf, W, H, mk, 0.80f * H);
}

// UI-61: Jupiter's four moons on a strip at row sy0: true offsets (north up, east left),
// Jupiter itself drawn to the same scale. `mk` gets where each moon went (for its letter).
inline void draw_strip(const View &v, uint16_t *buf, int W, int H, Marks &mk, float sy0) {
  mk = Marks();
  const float cx = W / 2.0f, s = (W / 2.0f - 12) / 27.5f;  // px per Jupiter radius
  const float th = astro::rad(v.pole_pa), ct = cosf(th), st = sinf(th);
  mk.strip_y = (int) sy0;
  mk.strip_x0 = 6;
  mk.strip_x1 = W - 6;
  auto dot = [&](float x, float y, float rad, Rgb c) {
    for (int py = (int) (y - rad - 1); py <= (int) (y + rad + 1); py++)
      for (int px = (int) (x - rad - 1); px <= (int) (x + rad + 1); px++) {
        if (px < 0 || py < 0 || px >= W || py >= H)
          continue;
        const float d = sqrtf((px + 0.5f - x) * (px + 0.5f - x) + (py + 0.5f - y) * (py + 0.5f - y));
        const float a = std::max(0.0f, std::min(1.0f, rad + 0.5f - d));
        if (a <= 0)
          continue;
        const uint16_t o = buf[py * W + px];
        const float orr = (o >> 11) << 3, og = ((o >> 5) & 63) << 2, ob = (o & 31) << 3;
        buf[py * W + px] = (uint16_t) (((int) (orr + (c.r - orr) * a) >> 3) << 11 | ((int) (og + (c.g - og) * a) >> 2) << 5 |
                                       ((int) (ob + (c.b - ob) * a) >> 3));
      }
  };
  dot(cx, sy0, s * 1.0f, {232, 216, 186});
  for (int k = 0; k < 4; k++) {
    const Moon &m = v.moon[k];
    // planet frame -> sky (E, N) -> screen (east left)
    const float E = -m.x * ct + m.y * st, N = m.x * st + m.y * ct;
    const float x = cx - E * s, y = sy0 - N * s;
    mk.x[k] = (int) x;
    mk.y[k] = (int) y;
    mk.shown[k] = !m.behind;
    if (!m.behind)
      dot(x, y, 2.6f, m.in_front ? Rgb{120, 110, 100} : Rgb{255, 250, 235});
  }
  mk.n = 4;
}

// UI-61a: tonight's phase on a real photo: the disc is found (brighter than the page),
// and each pixel is dimmed by how the Sun lights that point now (the terminator and the
// dark side), black sky lifted to the page colour. Photos are north up as published.
inline void shade_photo(const View &v, uint16_t *buf, int W, int H, bool phase) {
  int x0 = W, x1 = -1, y0 = H, y1 = -1;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const uint16_t p = buf[y * W + x];
      const int lum = ((p >> 11) << 3) + (((p >> 5) & 63) << 2) * 2 + ((p & 31) << 3);  // ~4x luma
      if (lum > 4 * 48) {
        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
      }
    }
  const float cx = (x0 + x1 + 1) / 2.0f, cy = (y0 + y1 + 1) / 2.0f;
  const float R = std::max(4.0f, std::max(x1 - x0 + 1, y1 - y0 + 1) / 2.0f);
  const float ch = astro::rad(v.limb_pa), i = astro::rad(v.phase_deg);
  const float sE = sinf(ch) * sinf(i), sN = cosf(ch) * sinf(i), ci = cosf(i);
  const bool shade = phase && x1 >= x0 && v.lit < 0.995f;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      uint16_t &p = buf[y * W + x];
      float r = (p >> 11) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
      if (shade) {
        const float E = -(x + 0.5f - cx) / R, N = (cy - (y + 0.5f)) / R, rr = E * E + N * N;
        if (rr <= 1.0f) {
          const float nz = sqrtf(1 - rr), lam = E * sE + N * sN + nz * ci;
          const float k = lam <= 0 ? 0.03f : 0.03f + 0.97f * powf(std::min(1.0f, lam * 1.4f), 0.45f);
          r *= k, g *= k, b *= k;
        }
      }
      r = std::max(r, 7.0f), g = std::max(g, 11.0f), b = std::max(b, 24.0f);
      p = (uint16_t) (((int) r >> 3) << 11 | ((int) g >> 2) << 5 | ((int) b >> 3));
    }
}

inline const char *moon_name(int i) {
  static const char *const N[4] = {"Io", "Europa", "Ganymede", "Callisto"};
  return N[i];
}

}  // namespace pview
}  // namespace sat
