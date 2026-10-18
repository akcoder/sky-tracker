#pragma once
// UI-87 the eclipse view: a full-screen picture of the eclipse as it is right now, from the same Sun/Moon/shadow
// geometry the eclipse alert is worked out with (sky_events.h: solar_view, lunar_view), and a map of where on the
// Earth it is happening. Opened from the web page's Play Solar / Lunar Eclipse Animation buttons (a replay of the
// next eclipse seen from here, or an example when there is none, first to last contact in 30 s) and by the debug
// command "eclipse solar|annular|lunar". Live (no replay) it follows the real clock, redrawn every 2 s.
//  * solar: the Sun and the Moon's black disc, as seen from here (zenith up), the corona around the Sun only at
//    totality, the ring of fire at annularity; the map shows the Moon's shadow axis moving along its path;
//  * lunar: the Moon (its real picture when the Moon image is loaded) with the Earth's umbra and penumbra over it;
//    the map shows where the Moon is overhead, and the half of the Earth that can see it.
// Compiled in sky_extra.cpp (SKY_IMPL), as sky_octopus.h.
#include <cstdint>

#ifndef SKY_IMPL
namespace sat {
namespace eclipse {
bool open(int which, bool replay);  // which: 0 the next/current one, 1 solar, 2 lunar, 3 annular example; false: nothing to show
bool open_for(const ev::Eclipse &e, bool replay, double lat, double lon, bool example);  // one given, seen from lat/lon
void close();
bool showing();
}  // namespace eclipse
}  // namespace sat
#else
#include "sky_coast.h"
#include "sky_moon_full.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sat {
namespace eclipse {

constexpr int W = 480, H = 232;      // the picture (full width: the Moon's disc passes in from the side)
constexpr float R = 66.0f;           // the Sun's radius in it, px
constexpr float RL = 82.0f;          // the Moon's (lunar view)
constexpr int MW = 240, MH = 120;    // the map
constexpr int MX = 234, MY = 268;    // ... and where it sits
constexpr int N_PATH = 40;
constexpr float REPLAY_S = 30.0f;    // first to last contact, replayed
constexpr uint32_t LIVE_MS = 2000, REPLAY_MS = 350;
constexpr int CV_Y = 28;             // the picture's top

struct St {
  lv_obj_t *root = nullptr, *cv = nullptr, *map = nullptr, *title = nullptr, *line1 = nullptr, *line2 = nullptr, *loc = nullptr, *note = nullptr;
  lv_obj_t *bar = nullptr, *now = nullptr, *max = nullptr, *t_a = nullptr, *t_m = nullptr, *t_b = nullptr;
  lv_timer_t *timer = nullptr;
  uint16_t *px = nullptr, *mpx = nullptr, *moonpx = nullptr;
  ev::Eclipse e;
  bool replay = false, example = false;
  uint32_t replay_t0 = 0;
  double lat = 0, lon = 0, alt = 0;
  double t_from = 0, t_to = 0;  // the bar: first and last contact seen here (padded)
  ev::GeoPt path[N_PATH];       // solar: the shadow axis along the eclipse
  int n_path = 0;
};
inline St st;

inline uint16_t rgb(float r, float g, float b) {  // 0..1 each
  const int R5 = (int) (std::min(1.0f, std::max(0.0f, r)) * 31.0f + 0.5f), G6 = (int) (std::min(1.0f, std::max(0.0f, g)) * 63.0f + 0.5f),
            B5 = (int) (std::min(1.0f, std::max(0.0f, b)) * 31.0f + 0.5f);
  return (uint16_t) (R5 << 11 | G6 << 5 | B5);
}
inline float smooth(float a, float b, float x) {
  const float t = std::min(1.0f, std::max(0.0f, (x - a) / (b - a)));
  return t * t * (3 - 2 * t);
}
inline float edge(float d, float r) { return std::min(1.0f, std::max(0.0f, r - d + 0.5f)); }  // 1 inside, 0 outside, 1 px soft

// ---- solar: the Sun at the centre, the Moon's disc in front of it where it really is
// (the colour depends only on the distance from the Sun's centre, so it is worked out once per half pixel of radius into a
// table: the first version called expf three times a pixel and a step took 780 ms on the unit)
inline uint16_t lut_sky[2 * 290], lut_sun[2 * 290];
inline void render_solar(uint16_t *px, const ev::EclView &v) {
  const float k = R / (float) v.r1;                      // px per degree
  const float rm = (float) v.r2 * k, mx = (float) v.x * k, my = -(float) v.y * k;  // y down on the screen
  const bool big = rm >= R;                              // the Moon covers the Sun's disc (total) or not (annular)
  const float total = big ? smooth(0.985f, 0.9995f, (float) v.cover) : 0.0f;       // the corona shows only at totality
  const float ring = big ? 0.0f : smooth(0.93f, 0.985f, (float) v.cover);          // the ring of fire
  const float glare = 1.0f - smooth(0.4f, 0.97f, (float) v.cover) * 0.8f;          // the sky darkens as it goes
  const float cx = W / 2.0f, cy = H / 2.0f;
  const int nl = 2 * 290;
  for (int i = 0; i < nl; i++) {
    const float rs = (i + 0.5f) / 2.0f;
    // the sky: black, a faint glare round the Sun while it is bright, the corona at totality
    float r = 0.012f, g = 0.015f, b = 0.04f;
    if (rs > R - 2.0f) {
      const float d = std::max(0.0f, rs - R);
      const float vig = 1.0f - smooth(0.62f * cy, 0.98f * cy, rs);  // the glows fade out before the picture's edge
      const float gl = 0.5f * expf(-d / 18.0f) * glare * vig;
      r += gl * 1.0f;
      g += gl * 0.7f;
      b += gl * 0.35f;
      const float cor = total * vig * (0.9f * expf(-d / 26.0f) + 0.25f * expf(-d / 75.0f));
      r += cor * 0.95f;
      g += cor * 0.95f;
      b += cor * 1.0f;
    }
    lut_sky[i] = rgb(r, g, b);
    // the Sun's disc, darker toward the limb
    const float u = std::min(1.0f, rs / R), mu = sqrtf(std::max(0.0f, 1.0f - u * u));
    const float lum = (0.52f + 0.48f * mu) * (1.0f + 0.35f * ring);  // (the ring of fire is the bright part left)
    lut_sun[i] = rgb(std::min(1.0f, lum), std::min(1.0f, (0.62f + 0.33f * mu) * lum), std::min(1.0f, (0.20f + 0.40f * mu) * lum));
  }
  const int x0m = std::max(0, (int) floorf(cx + mx - rm - 2)), x1m = std::min(W - 1, (int) ceilf(cx + mx + rm + 2));
  const int y0m = std::max(0, (int) floorf(cy + my - rm - 2)), y1m = std::min(H - 1, (int) ceilf(cy + my + rm + 2));
  const uint16_t dark = rgb(0.018f, 0.020f, 0.034f);
  for (int y = 0; y < H; y++) {
    const float dy = y + 0.5f - cy;
    for (int x = 0; x < W; x++) {
      const float dx = x + 0.5f - cx;
      const float rs = sqrtf(dx * dx + dy * dy);
      const int i = std::min(nl - 1, (int) (rs * 2.0f));
      uint16_t c;
      if (rs < R - 1.0f)
        c = lut_sun[i];
      else if (rs > R + 1.0f)
        c = lut_sky[i];
      else {  // the Sun's edge, one pixel soft
        const float sun = edge(rs, R);
        const uint16_t a = lut_sun[i], b = lut_sky[i];
        const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
        c = (uint16_t) (((int) (ar * sun + br * (1 - sun)) << 11) | ((int) (ag * sun + bg * (1 - sun)) << 5) | (int) (ab * sun + bb * (1 - sun)));
      }
      // the Moon's disc, black with a hint of earthshine, over it all
      if (x >= x0m && x <= x1m && y >= y0m && y <= y1m) {
        const float rmoon = sqrtf((dx - mx) * (dx - mx) + (dy - my) * (dy - my));
        if (rmoon < rm - 1.0f)
          c = dark;
        else if (rmoon < rm + 1.0f) {
          const float mo = edge(rmoon, rm);
          const int cr = c >> 11, cg = (c >> 5) & 63, cb = c & 31, dr = dark >> 11, dg = (dark >> 5) & 63, db = dark & 31;
          c = (uint16_t) (((int) (cr * (1 - mo) + dr * mo) << 11) | ((int) (cg * (1 - mo) + dg * mo) << 5) | (int) (cb * (1 - mo) + db * mo));
        }
      }
      px[y * W + x] = c;
    }
  }
}

// ---- lunar: the Moon at the centre, the Earth's shadow over it where it really is
inline void render_lunar(uint16_t *px, const ev::EclView &v, bool flip, const uint16_t *moon) {
  const float k = RL / (float) v.r1;
  const float ru = (float) v.r2 * k, rp = (float) v.r3 * k;
  float sx = (float) v.x * k, sy = -(float) v.y * k;  // the shadow's centre from the Moon's (screen, y down)
  if (flip)  // the Moon's picture is the southern view (south up): the shadow goes the same way round
    sx = -sx, sy = -sy;
  const bool photo = moon != nullptr;  // the full Moon, 164 px across (the eclipse is at full Moon whatever the phase is today)
  const float cx = W / 2.0f, cy = H / 2.0f;
  const uint16_t sky = rgb(0.008f, 0.010f, 0.028f);
  const int x0 = (int) (cx - RL - 2), x1 = (int) (cx + RL + 2), y0 = (int) (cy - RL - 2), y1 = (int) (cy + RL + 2);
  for (int y = 0; y < H; y++) {
    uint16_t *row = px + y * W;
    if (y < y0 || y > y1) {
      for (int x = 0; x < W; x++)
        row[x] = sky;
      continue;
    }
    for (int x = 0; x < x0; x++)
      row[x] = sky;
    for (int x = x1 + 1; x < W; x++)
      row[x] = sky;
    for (int x = x0; x <= x1; x++) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      const float rm = sqrtf(dx * dx + dy * dy);
      const float mo = edge(rm, RL);
      if (mo <= 0) {
        row[x] = sky;
        continue;
      }
      float mr, mg, mb;
      if (photo) {
        int ix = std::min(moonfull::DIM - 1, std::max(0, (int) (dx + RL))), iy = std::min(moonfull::DIM - 1, std::max(0, (int) (dy + RL)));
        if (flip)  // seen from the south the Moon is the other way up
          ix = moonfull::DIM - 1 - ix, iy = moonfull::DIM - 1 - iy;
        const uint16_t p = moon[iy * moonfull::DIM + ix];
        mr = ((p >> 11) & 31) / 31.0f;
        mg = ((p >> 5) & 63) / 63.0f;
        mb = (p & 31) / 31.0f;
      } else {  // no picture yet: a plain disc, darker at the limb
        const float u = rm / RL, shade = 0.55f + 0.45f * sqrtf(std::max(0.0f, 1.0f - u * u));
        mr = 0.78f * shade, mg = 0.76f * shade, mb = 0.72f * shade;
      }
      // the shadow
      const float d = sqrtf((dx - sx) * (dx - sx) + (dy - sy) * (dy - sy));
      float fr = 1, fg = 1, fb = 1;
      if (d < rp) {  // penumbra: a slow darkening toward the umbra
        const float t = smooth(rp, ru, d);
        const float dim = 1.0f - 0.38f * t * t;
        fr = fg = fb = dim;
      }
      const float um = 1.0f - smooth(ru - 3.0f, ru + 3.0f, d);  // inside the umbra, with a soft edge
      if (um > 0) {
        const float s = std::min(1.0f, d / ru);  // 0 at the shadow's centre, 1 at its rim
        const float ur = 0.17f + 0.50f * s * s, ug = 0.034f + 0.19f * s * s, ub = 0.015f + 0.10f * s * s;  // deep red, brighter at the rim
        fr = fr * (1 - um) + ur * um;
        fg = fg * (1 - um) + ug * um;
        fb = fb * (1 - um) + ub * um;
      }
      mr *= fr, mg *= fg, mb *= fb;
      const float r = 0.008f * (1 - mo) + mr * mo, g = 0.010f * (1 - mo) + mg * mo, b = 0.028f * (1 - mo) + mb * mo;
      row[x] = rgb(r, g, b);
    }
  }
}

// ---- the map: coastlines, the path, where it is now and where you are
inline void map_put(uint16_t *px, int x, int y, uint16_t c) {
  if (x >= 0 && x < MW && y >= 0 && y < MH)
    px[y * MW + x] = c;
}
inline void map_xy(double lat, double lon, int &x, int &y) {
  x = (int) lroundf((float) ((lon + 180.0) / 360.0 * (MW - 1)));
  y = (int) lroundf((float) ((90.0 - lat) / 180.0 * (MH - 1)));
}
inline void map_line(uint16_t *px, int x0, int y0, int x1, int y1, uint16_t c) {
  const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int i = 0; i < 2000; i++) {
    map_put(px, x0, y0, c);
    if (x0 == x1 && y0 == y1)
      break;
    const int e2 = 2 * err;
    if (e2 >= dy)
      err += dy, x0 += sx;
    if (e2 <= dx)
      err += dx, y0 += sy;
  }
}
inline void map_dot(uint16_t *px, int cx, int cy, int r, uint16_t c) {
  for (int y = -r; y <= r; y++)
    for (int x = -r; x <= r; x++)
      if (x * x + y * y <= r * r + r / 2)
        map_put(px, cx + x, cy + y, c);
}
inline void map_ring(uint16_t *px, int cx, int cy, int r, uint16_t c) {
  for (int y = -r - 1; y <= r + 1; y++)
    for (int x = -r - 1; x <= r + 1; x++) {
      const int d2 = x * x + y * y;
      if (d2 <= (r + 1) * (r + 1) && d2 >= r * r - r)
        map_put(px, cx + x, cy + y, c);
    }
}
// lit: the sub-point of what the shading follows (the Sun's for a solar eclipse: the day side; the Moon's for a lunar one:
// the side that sees it); event: where the eclipse is now
inline void render_map(uint16_t *px, double lit_lat, double lit_lon, const ev::GeoPt &event, bool lunar) {
  const float la0 = (float) (lit_lat * M_PI / 180.0), sl0 = sinf(la0), cl0 = cosf(la0);
  static float cdl[MW];
  for (int x = 0; x < MW; x++)
    cdl[x] = cosf((float) ((x / (float) (MW - 1) * 360.0 - 180.0 - lit_lon) * M_PI / 180.0));
  for (int y = 0; y < MH; y++) {
    const float la = (float) ((90.0 - y / (float) (MH - 1) * 180.0) * M_PI / 180.0), s = sinf(la), c = cosf(la);
    for (int x = 0; x < MW; x++) {
      const float up = s * sl0 + c * cl0 * cdl[x];  // > 0: that place sees it
      const float lit = smooth(-0.12f, 0.12f, up);
      px[y * MW + x] = rgb(0.025f + 0.05f * lit, 0.04f + 0.08f * lit, 0.09f + 0.16f * lit);
    }
  }
  // graticule: every 30 degrees
  const uint16_t grid = rgb(0.10f, 0.14f, 0.26f);
  for (int lon = -180; lon <= 180; lon += 30) {
    int x, y;
    map_xy(0, lon, x, y);
    for (int yy = 0; yy < MH; yy++)
      map_put(px, x, yy, grid);
  }
  for (int lat = -60; lat <= 60; lat += 30) {
    int x, y;
    map_xy(lat, 0, x, y);
    for (int xx = 0; xx < MW; xx++)
      map_put(px, xx, y, grid);
  }
  // coastlines
  const uint16_t cc = rgb(0.52f, 0.64f, 0.82f);
  int px0 = 0, py0 = 0;
  bool pen = false;
  for (int i = 0; i < coast::N; i++) {
    const int16_t lo = coast::PTS[2 * i], la = coast::PTS[2 * i + 1];
    if (lo == 32767) {
      pen = false;
      continue;
    }
    int x, y;
    map_xy(la / 100.0, lo / 100.0, x, y);
    if (pen && std::abs(x - px0) < MW / 2)
      map_line(px, px0, py0, x, y, cc);
    px0 = x, py0 = y, pen = true;
  }
  // the solar path, then the eclipse now
  const uint16_t path = rgb(1.0f, 0.62f, 0.2f);
  if (!lunar) {
    int lx = 0, ly = 0;
    bool have = false;
    for (int i = 0; i < st.n_path; i++) {
      int x, y;
      map_xy(st.path[i].lat, st.path[i].lon, x, y);
      if (have && std::abs(x - lx) < MW / 2)
        map_line(px, lx, ly, x, y, path);
      lx = x, ly = y, have = true;
    }
  }
  int ex, ey, yx, yy;
  map_xy(event.lat, event.lon, ex, ey);
  map_xy(st.lat, st.lon, yx, yy);
  map_ring(px, ex, ey, 6, rgb(1.0f, 0.62f, 0.2f));
  map_dot(px, ex, ey, 3, rgb(1.0f, 0.85f, 0.4f));
  map_dot(px, yx, yy, 2, rgb(0.5f, 1.0f, 0.95f));
  map_ring(px, yx, yy, 4, rgb(0.5f, 1.0f, 0.95f));
}

// ---- the screen
inline lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int x, int y, int w, lv_text_align_t al) {
  lv_obj_t *l = lv_label_create(p);
  if (f)
    lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_style_text_align(l, al, 0);
  lv_obj_set_width(l, w);
  lv_obj_set_pos(l, x, y);
  lv_label_set_text(l, "");
  return l;
}
inline void hm(double t, char *b, size_t n) {
  if (t > 0)
    local_hm((int64_t) t, b, n);
  else
    snprintf(b, n, "--:--");
}
inline void in_words(double dt, char *b, size_t n) {  // "in 12m", "in 2h 05m"
  const long s = (long) fabs(dt), m = s / 60, h = m / 60;
  if (h > 0)
    snprintf(b, n, "in %ldh %02ldm", h, m % 60);
  else
    snprintf(b, n, "in %ldm", std::max(1L, m));
}
inline void ll_text(double lat, double lon, char *b, size_t n) {
  snprintf(b, n, "%.1f\xC2\xB0%c %.1f\xC2\xB0%c", fabs(lat), lat >= 0 ? 'N' : 'S', fabs(lon), lon >= 0 ? 'E' : 'W');
}

inline double view_time() {
  if (!st.replay)
    return clock_now();
  const float f = std::min(1.0f, (lv_tick_get() - st.replay_t0) / (REPLAY_S * 1000.0f));
  return st.t_from + (st.t_to - st.t_from) * f;
}

inline void step() {
  if (st.root == nullptr || st.px == nullptr)
    return;
  const int64_t t_step = ui_us();
  const double t = view_time();
  const ev::Eclipse &e = st.e;
  const bool lunar = ev::is_lunar(e.type);
  const ev::EclView v = lunar ? ev::lunar_view(t) : ev::solar_view(t, st.lat, st.lon, st.alt);
  if (lunar)
    render_lunar(st.px, v, st.lat < 0, st.moonpx);
  else
    render_solar(st.px, v);
  lv_obj_invalidate(st.cv);
  // the map: the Moon overhead (lunar) or the shadow's axis (solar), shaded by who can see it
  const ev::GeoPt ev_pt = lunar ? ev::moon_subpoint(t) : ev::shadow_axis_point(t);
  if (st.mpx) {
    if (lunar)
      render_map(st.mpx, ev_pt.lat, ev_pt.lon, ev_pt, true);
    else {
      const ev::GeoPt sun = ev::moon_subpoint(t);  // (the Sun is nearly in line: the day side is round the axis' point)
      render_map(st.mpx, sun.lat, sun.lon, ev_pt, false);
    }
    lv_obj_invalidate(st.map);
  }
  // the words
  char a[16], m[16], b2[16], buf[200], w[24], p1[40], p2[40];
  hm(e.t0, a, sizeof(a));
  hm(e.t_max, m, sizeof(m));
  hm(e.t1, b2, sizeof(b2));
  const bool central = e.c0 > 0 && e.c1 > 0;
  const char *phase;
  if (t < e.t0)
    phase = "Not started";
  else if (t > e.t1)
    phase = "Over";
  else if (central && t >= e.c0 && t <= e.c1)
    phase = e.type == ev::ECL_ANNULAR ? "Ring of fire" : "Totality";
  else
    phase = e.type == ev::ECL_PENUMBRAL ? "Penumbral phase" : "Partial phase";
  snprintf(buf, sizeof(buf), "%s%s", ev::eclipse_name(e.type), st.example ? " (example)" : "");
  lv_label_set_text(st.title, buf);
  if (lunar) {
    const double r = std::max(0.0, (v.r2 + v.r1 - hypot(v.x, v.y)) / (2 * v.r1));  // umbral magnitude now
    snprintf(buf, sizeof(buf), "Umbral mag %.2f", std::min(r, 2.0));
  } else {
    snprintf(buf, sizeof(buf), "%.0f%% covered", v.cover * 100.0);
  }
  lv_label_set_text(st.line1, buf);
  if (t < e.t0) {
    in_words(e.t0 - t, w, sizeof(w));
    snprintf(buf, sizeof(buf), "%s: starts %s (%s)", phase, a, w);
  } else if (t > e.t1) {
    snprintf(buf, sizeof(buf), "%s: ended %s", phase, b2);
  } else {
    snprintf(buf, sizeof(buf), "%s", phase);
  }
  lv_label_set_text(st.line2, buf);
  // where
  ll_text(ev_pt.lat, ev_pt.lon, p1, sizeof(p1));
  ll_text(st.lat, st.lon, p2, sizeof(p2));
  const double km = ev::great_circle_km(st.lat, st.lon, ev_pt.lat, ev_pt.lon);
  char dd[40];
  snprintf(dd, sizeof(dd), "%ld %s from you", lroundf(dist((float) km)), dist_unit());
  if (lunar)
    snprintf(buf, sizeof(buf), "Moon overhead\n%s\nYou: %s\n%s", p1, p2, km < 9000 ? "Moon is up here" : "Moon is down here");
  else
    snprintf(buf, sizeof(buf), "%s\n%s\nYou: %s\n%s", ev_pt.hit ? "Shadow axis" : "Shadow passes by", p1, p2, dd);
  lv_label_set_text(st.loc, buf);
  // the bar
  const double span = std::max(1.0, st.t_to - st.t_from);
  const int bx = 30, bw = 420;
  auto at = [&](double tt) { return bx + (int) (bw * std::min(1.0, std::max(0.0, (tt - st.t_from) / span))); };
  lv_obj_set_x(st.now, at(t) - 2);
  lv_obj_set_x(st.max, at(e.t_max) - 1);
  lv_label_set_text(st.t_a, a);
  lv_label_set_text(st.t_m, m);
  lv_label_set_text(st.t_b, b2);
  lv_obj_set_x(st.t_m, at(e.t_max) - 30);
  if (st.replay) {  // (the clock in the words is the replayed one)
    char c[16];
    hm(t, c, sizeof(c));
    snprintf(buf, sizeof(buf), "Replay %s. Tap to close", c);
  } else {
    snprintf(buf, sizeof(buf), "%s", lunar ? "Tap to close. Safe to watch with the naked eye." : "Tap to close. Never look at the Sun without eclipse glasses.");
  }
  lv_label_set_text(st.note, buf);
  static int steps = 0;
  if (steps++ % 40 == 0)
    ESP_LOGI(UI_TAG, "eclipse view: a step took %lld ms (picture, map and words)", (long long) ((ui_us() - t_step) / 1000));
}

inline void close_now() {
  if (st.timer) {
    lv_timer_delete(st.timer);
    st.timer = nullptr;
  }
  if (st.root) {
    lv_obj_delete(st.root);
    page_restore_under();
  }
  if (st.px) {
    heap_caps_free(st.px);
    st.px = nullptr;
  }
  if (st.mpx) {
    heap_caps_free(st.mpx);
    st.mpx = nullptr;
  }
  if (st.moonpx) {
    heap_caps_free(st.moonpx);
    st.moonpx = nullptr;
  }
  st.root = st.cv = st.map = nullptr;
}
bool showing() { return st.root != nullptr; }
void close() { close_now(); }

inline bool build(const ev::Eclipse &e, bool replay) {
  if (st.root)
    close_now();
  st.px = (uint16_t *) heap_caps_malloc((size_t) W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  st.mpx = (uint16_t *) heap_caps_malloc((size_t) MW * MH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (st.px == nullptr || st.mpx == nullptr) {
    close_now();
    return false;
  }
  if (ev::is_lunar(e.type)) {  // the full Moon's picture; a plain disc if it cannot be had
    st.moonpx = (uint16_t *) heap_caps_malloc((size_t) moonfull::DIM * moonfull::DIM * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (st.moonpx && tracker::jpg::decode_copy(moonfull::DATA, moonfull::LEN, st.moonpx, moonfull::DIM, moonfull::DIM, moonfull::DIM, nullptr, ui_jpg_pool)) {
      heap_caps_free(st.moonpx);
      st.moonpx = nullptr;
    }
  }
  st.e = e;
  st.lat = config().lat;
  st.lon = config().lon;
  st.alt = config().alt_m;
  st.replay = replay;
  st.example = false;
  st.replay_t0 = lv_tick_get();
  const double pad = std::max(600.0, (e.t1 - e.t0) * 0.1);
  st.t_from = e.t0 - pad;
  st.t_to = e.t1 + pad;
  page_hide_under();
  lv_obj_t *root = lv_obj_create(lv_layer_top());
  st.root = root;
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_set_style_bg_color(root, lv_color_hex(0x04060F), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(root, [](lv_event_t *) { close_now(); }, LV_EVENT_CLICKED, nullptr);
  const auto &w = ui.w;
  st.title = label(root, w.card_title_font, 0xFFB070, 20, 4, 440, LV_TEXT_ALIGN_CENTER);
  st.cv = lv_canvas_create(root);
  lv_canvas_set_buffer(st.cv, st.px, W, H, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(st.cv, 0, CV_Y);
  st.map = lv_canvas_create(root);
  lv_canvas_set_buffer(st.map, st.mpx, MW, MH, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(st.map, MX, MY);
  st.line1 = label(root, w.card_title_font, 0xFFFFFF, 14, 266, 216, LV_TEXT_ALIGN_LEFT);
  st.line2 = label(root, w.card_font, 0xC9D3F2, 14, 292, 216, LV_TEXT_ALIGN_LEFT);
  st.loc = label(root, w.label_font, 0x9AA8D0, 14, 326, 216, LV_TEXT_ALIGN_LEFT);
  // the timeline: first to last contact seen here, the maximum, and now
  st.bar = lv_obj_create(root);
  lv_obj_remove_style_all(st.bar);
  lv_obj_set_size(st.bar, 420, 4);
  lv_obj_set_pos(st.bar, 30, 416);
  lv_obj_set_style_bg_color(st.bar, lv_color_hex(0x24356A), 0);
  lv_obj_set_style_bg_opa(st.bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(st.bar, 2, 0);
  st.max = lv_obj_create(root);
  lv_obj_remove_style_all(st.max);
  lv_obj_set_size(st.max, 2, 14);
  lv_obj_set_y(st.max, 411);
  lv_obj_set_style_bg_color(st.max, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(st.max, LV_OPA_COVER, 0);
  st.now = lv_obj_create(root);
  lv_obj_remove_style_all(st.now);
  lv_obj_set_size(st.now, 5, 14);
  lv_obj_set_y(st.now, 411);
  lv_obj_set_style_bg_color(st.now, lv_color_hex(0xFF8A1F), 0);
  lv_obj_set_style_bg_opa(st.now, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(st.now, 2, 0);
  const lv_font_t *sf = w.label_font;
  auto small = [&](int x, int wd, lv_text_align_t al) { return label(root, sf, 0xC9D3F2, x, 430, wd, al); };
  st.t_a = small(20, 80, LV_TEXT_ALIGN_LEFT);
  st.t_m = small(200, 60, LV_TEXT_ALIGN_CENTER);
  st.t_b = small(380, 80, LV_TEXT_ALIGN_RIGHT);
  st.note = label(root, sf, 0x7E8BB3, 20, 456, 440, LV_TEXT_ALIGN_CENTER);
  // the shadow's path over the whole event (solar)
  st.n_path = 0;
  if (!ev::is_lunar(e.type))
    for (double t = e.t_max - 150 * 60.0; t <= e.t_max + 150 * 60.0 && st.n_path < N_PATH; t += 8 * 60.0) {
      const ev::GeoPt p = ev::shadow_axis_point(t);
      if (p.hit)
        st.path[st.n_path++] = p;
    }
  step();
  st.timer = lv_timer_create([](lv_timer_t *) { step(); }, replay ? REPLAY_MS : LIVE_MS, nullptr);
  return true;
}

// the eclipses used when none is coming: a real total, annular and lunar one, each seen from a place that has it
struct Example {
  int kind;  // 1 solar total, 3 solar annular, 2 lunar
  double t, lat, lon;
};
inline const Example EXAMPLES[] = {{1, 1786556220.0, 42.34, -3.70},   // total solar, 12 Aug 2026, Burgos
                                   {3, 1801929420.0, -43.0, -69.0},   // annular solar, 6 Feb 2027, Argentina
                                   {2, 1757268660.0, 13.0, 80.2}};    // total lunar, 7 Sep 2025, India

// which: 0 the current or next one, 1 the next solar one, 2 the next lunar one, 3 the annular example
inline bool open_view(int which, bool replay) {
  const double t = clock_now();
  if (which != 3) {
    const ev::Eclipse *pick = nullptr;
    for (const auto &e : sev.ecl) {
      if (e.t1 < t - 3600.0)
        continue;
      if (which == 1 && ev::is_lunar(e.type))
        continue;
      if (which == 2 && !ev::is_lunar(e.type))
        continue;
      if (pick == nullptr || e.t_max < pick->t_max)
        pick = &e;
    }
    if (pick != nullptr && (replay || which != 0 || (pick->t0 - t) <= SKY_ALERT_S)) {
      return build(*pick, replay);
    }
    if (which == 0)
      return false;
  }
  // none coming: an example, played the same way
  const Example *ex = nullptr;
  for (const auto &x : EXAMPLES)
    if (x.kind == (which == 0 ? 1 : which))
      ex = &x;
  if (ex == nullptr)
    return false;
  ev::Eclipse e;
  const bool ok = ex->kind == 2 ? ev::lunar_eclipse(ex->t, ex->lat, ex->lon, 10, e) : ev::solar_eclipse(ex->t, ex->lat, ex->lon, 100, e);
  if (!ok)
    return false;
  if (!build(e, true))
    return false;
  st.example = true;
  st.lat = ex->lat;
  st.lon = ex->lon;
  step();
  return true;
}
bool open(int which, bool replay) { return open_view(which, replay); }
bool open_for(const ev::Eclipse &e, bool replay, double lat, double lon, bool example) {
  const bool ok = build(e, replay);
  if (ok) {
    st.lat = lat;
    st.lon = lon;
    st.example = example;
    step();
  }
  return ok;
}
}  // namespace eclipse
}  // namespace sat
#endif  // SKY_IMPL
