#pragma once
// UI-85 the octopus: a mimic octopus visits the map two or three times an hour, about 10 s. It jets in from
// the right, says something, morphs into ONE thing picked at random for the visit (a lionfish, a banded sea
// snake or the ISS, with a line to match), squirts ink and jets off to the left. The words are drawn at random
// from a pool for each beat. A tap ends it. Off with the Octopus Visits switch; the Play Octopus Animation
// button shows it on demand.
//
// How it stays fast and does not tear (the panel has one framebuffer that is scanned out while LVGL writes it):
//  * the screen as it is, snapshotted, is the backdrop (rocket::back_take, UI-69o); a second RGB565 buffer, the
//    composite, starts as a copy of it and is what the backdrop blits. Each step (~11 fps) restores the box
//    the octopus covers from the snapshot, paints the octopus onto the composite, and invalidates the box:
//    the screen then gets one opaque image blit of the box (a few ms), never the arms drawn strip by strip.
//  * the octopus is painted by a small rasteriser of its own (discs and spans into the RGB565 buffer): the
//    first version drew it with lv_draw_line / arc / rect and painting cost 192 ms a frame on the unit. Only
//    the speech bubble goes through LVGL (a canvas layer, only while a line is up).
// It waits for a quiet map: the sky page showing, the top layer empty, no touch for 20 s, no launch or ISS
// docking within 15 minutes, and no other animation playing.
// Compiled in sky_extra.cpp (SKY_IMPL), as sky_rocket.h.
#include <cstdint>

#ifndef SKY_IMPL
namespace sat {
namespace octo {
void set_enabled(bool on);
bool play_now();  // the button
bool playing();
void stop();
void check();  // every second (sky_rocket.h's timer): a visit that is due
}  // namespace octo
}  // namespace sat
#else
#include "sky_rocket.h"  // (esphome.h includes the headers alphabetically: this one comes first)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sat {
namespace octo {

constexpr float DUR_S = 10.0f;
constexpr float SC = 0.75f;  // its size: the screen blit costs ~0.8 us a pixel, so the box it covers (and its fps) is set by this
constexpr int N = 12;  // segments an arm is kept in
constexpr uint32_t MIN_GAP_S = 15 * 60, MAX_GAP_S = 35 * 60, FIRST_MIN_S = 8 * 60, FIRST_MAX_S = 15 * 60;
constexpr uint32_t MODEL_MS = 70;  // the model steps at up to ~14 fps (a step is ~45 ms to paint and ~30 ms to refresh)
constexpr uint32_t C_BODY = 0x80583C, C_BAND = 0xEEE0C0, C_DARK = 0x3C2820, C_ISS_BODY = 0x3E5CA0, C_ISS_BAND = 0xDECC82,
                   C_LION = 0xA04E34, C_SNAKE = 0x2C2630, C_SNAKE_BAND = 0xF4F0E8;

// ---- the words: one line a beat, drawn at random per visit
enum Kind { K_LION = 0, K_SNAKE = 1, K_ISS = 2 };
inline const char *const SAY_ARRIVE[] = {"Mimic octopus.\nIt's a lifestyle.", "Three hearts.\nNine brains.", "Blue blood.\nVery fancy.",
                                         "I can be anything.\nWatch.", "Just passing\nthrough."};
inline const char *const SAY_LION[] = {"Lionfish.\nBe afraid.", "I'm a lionfish.\nVenomous.", "Lionfish.\nDon't touch."};
inline const char *const SAY_SNAKE[] = {"Sea snake.\nSsss.", "Banded sea snake.\nVery toxic.", "Sea snake.\nRespect."};
inline const char *const SAY_ISS[] = {"ISS.\nObviously.", "Space station\nmode.", "Look up! The\nISS!", "Orbiting.\nPlease hold."};
inline const char *const SAY_INK[] = {"Ink!", "Oops.", "Bye!", "Poof."};
constexpr int N_BEATS = 3;  // arrival, the morph, the ink
constexpr float BEAT_A[N_BEATS] = {2.2f, 5.5f, 8.4f}, BEAT_B[N_BEATS] = {4.5f, 7.4f, 9.4f};
constexpr float EXIT_T = 8.4f;  // the ink, then off to the left

// ---- the timeline (pure: the host test checks it, the painter and the box use it)
inline float ease(float x) {
  x = std::min(1.0f, std::max(0.0f, x));
  return x * x * (3 - 2 * x);
}
inline float seg(float t, float a, float b) { return ease((t - a) / (b - a)); }
inline void pos(float t, float &x, float &y) {
  if (t < 1.8f) {  // in from the right edge, jetting
    const float k = seg(t, 0, 1.8f);
    x = 430 - 170 * k;
    y = 262 + 6 * std::sin(t * 5);
  } else if (t < EXIT_T) {  // hovers
    x = 260 + 10 * std::sin((t - 2.2f) * 0.9f);
    y = 262 + 9 * std::sin((t - 2.2f) * 1.7f);
  } else {  // jets off to the left
    const float k = seg(t, EXIT_T, EXIT_T + 1.5f);
    x = 260 - 480 * std::pow(k, 1.3f);
    y = 262 - 40 * k;
  }
}
inline float vel(float t) {
  float x0, y0, x1, y1;
  pos(t - 0.05f, x0, y0);
  pos(t + 0.05f, x1, y1);
  return (x1 - x0) / 0.1f;
}
inline float jet(float t) {
  if (t < 1.8f)
    return std::pow(std::max(0.0f, std::sin(2 * (float) M_PI * 1.5f * t)), 2.0f);
  if (t > EXIT_T)
    return std::pow(std::max(0.0f, std::sin(2 * (float) M_PI * 2.6f * (t - EXIT_T))), 2.0f);
  return 0.25f * (0.5f + 0.5f * std::sin(t * 2.2f));
}
inline float w_morph(float t) { return seg(t, 4.8f, 5.3f) * (1 - seg(t, 7.6f, 8.1f)); }  // 0 octopus .. 1 the thing
inline int beat_at(float t) {
  for (int i = 0; i < N_BEATS; i++)
    if (t >= BEAT_A[i] && t <= BEAT_B[i])
      return i;
  return -1;
}
inline uint32_t mix3(uint32_t a, uint32_t b, float k) {  // k 0..1
  auto ch = [&](int sh) { return (uint32_t) ((float) ((a >> sh) & 255) + ((float) ((b >> sh) & 255) - (float) ((a >> sh) & 255)) * k); };
  return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

struct Model {
  float cx = 0, cy = 0, my = 0, rx = 0, ry = 0, p = 0, wl = 0, wi = 0, ws = 0, blink = 1, look = 0;
  uint32_t body = C_BODY, band = C_BAND;
  float ax[8][N + 1], ay[8][N + 1];
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // everything it covers (the box to redo)
};

struct St {
  bool enabled = true, running = false;
  uint32_t t0 = 0, last_model = 0, last_frame = 0, frames = 0, max_gap = 0, next_ms = 0, rng = 0x9E3779B9u;
  int kind = K_LION;  // what it morphs into this visit
  const char *say[N_BEATS] = {};
  lv_obj_t *catcher = nullptr, *cv = nullptr;  // the tap catcher; the (hidden) canvas the bubble is drawn through
  lv_timer_t *timer = nullptr;
  lv_draw_buf_t *clean = nullptr, *comp = nullptr;  // the screen as it was; that, with the octopus painted on it
  Model m;
  lv_area_t last_box = {0, 0, -1, -1};
  float t = 0;
  float ivx[26], ivy[26], ir[26], ia[26];  // the ink: velocity, size, strength per blob
  float bx = 0, by = 0, bw = 0, bh = 0;    // the bubble now (bw 0: none)
  uint32_t paint_us = 0, paint_max = 0, px = 0;  // what painting cost (the visit's log)
  uint32_t exit_us = 0, exit_n = 0, exit_px = 0;  // ...of which in the ink and exit (t >= EXIT_T)
};
inline St st;
inline uint32_t min_idle_ms = 20000;  // no touch for this long before a visit starts

inline uint32_t rnd() {
  st.rng ^= st.rng << 13, st.rng ^= st.rng >> 17, st.rng ^= st.rng << 5;
  return st.rng;
}
inline float rnd01() { return (rnd() % 10000) / 10000.0f; }

inline void build(float t, Model &m) {
  pos(t, m.cx, m.cy);
  m.my = m.cy - 46 * SC;
  m.p = jet(t);
  const float w = w_morph(t);
  m.wl = st.kind == K_LION ? w : 0;
  m.ws = st.kind == K_SNAKE ? w : 0;
  m.wi = st.kind == K_ISS ? w : 0;
  const float wn = 1 - m.wl - m.wi - m.ws;
  m.body = mix3(mix3(mix3(C_BODY, C_LION, m.wl), C_ISS_BODY, m.wi), C_SNAKE, m.ws);
  m.band = mix3(mix3(C_BAND, C_ISS_BAND, m.wi), C_SNAKE_BAND, m.ws);
  m.rx = 56 * SC * (1 - 0.10f * m.p);
  m.ry = 66 * SC * (1 + 0.08f * m.p);
  const float trail = std::max(-1.2f, std::min(1.2f, -vel(t) / 260.0f));
  for (int k = 0; k < 8; k++) {
    const bool left = k < 4;
    const float sx_n = m.cx + (k - 3.5f) * 13, sy_n = m.cy + 4;
    const float sx_l = m.cx + (k - 3.5f) * 8, sy_l = m.cy - 6;
    const float sx_i = m.cx + (left ? -34 : 34), sy_i = m.cy - 30 + (k % 4) * 17;
    float sx_s = sx_n, sy_s = sy_n, L_s = 34;
    if (k == 3)
      sx_s = m.cx - 22, sy_s = m.cy - 8, L_s = 190;
    else if (k == 4)
      sx_s = m.cx + 22, sy_s = m.cy - 8, L_s = 190;
    float x = sx_n * wn + sx_l * m.wl + sx_i * m.wi + sx_s * m.ws;
    float y = sy_n * wn + sy_l * m.wl + sy_i * m.wi + sy_s * m.ws;
    const float L = (150 * (1 - 0.25f * m.p) * wn + 128 * m.wl + 150 * m.wi + L_s * m.ws);
    m.ax[k][0] = x;
    m.ay[k][0] = y;
    for (int i = 1; i <= N; i++) {
      const float u = (float) i / N;
      const float amp = 0.55f * (0.25f + u) * (1 - 0.6f * m.p);
      const float ph = 2 * (float) M_PI * (1.0f * u - 0.55f * t) + k * 0.9f;
      float phi_n = (k - 3.5f) * 0.20f * (1 + u) + amp * std::sin(ph) + trail * u * 1.25f;
      phi_n += (k > 3 ? 1.0f : -1.0f) * 0.9f * u * u * (1 - std::fabs(trail));
      const float phi_l = (k - 3.5f) * 0.50f + 0.06f * std::sin(2 * (float) M_PI * (u - 0.8f * t) + k);
      const float phi_i = (left ? -1.5708f : 1.5708f) + 0.03f * std::sin(2 * (float) M_PI * (u - 0.5f * t) + k);
      float phi_s = phi_n;
      if (k == 3 || k == 4)
        phi_s = (k == 3 ? -1.5708f : 1.5708f) +
                (k == 3 ? 1.0f : -1.0f) * 0.85f * (0.3f + u) * std::sin(2 * (float) M_PI * (1.3f * u - 0.9f * t));
      const float phi = phi_n * wn + phi_l * m.wl + phi_i * m.wi + phi_s * m.ws;
      x += std::sin(phi) * L / N;
      y += std::cos(phi) * L / N;
      m.ax[k][i] = x;
      m.ay[k][i] = y;
    }
    for (int i = 0; i <= N; i++) {  // the whole octopus at SC about its centre
      m.ax[k][i] = m.cx + (m.ax[k][i] - m.cx) * SC;
      m.ay[k][i] = m.cy + (m.ay[k][i] - m.cy) * SC;
    }
  }
  m.blink = 1;
  for (float bt : {3.0f, 6.2f, 8.9f})
    if (std::fabs(t - bt) < 0.09f)
      m.blink = 0.12f;
  m.look = t < 2.3f ? -4.0f : 3 * std::sin(t * 1.3f);
  // the box: every arm point, the mantle, a margin for the arm widths
  float x0 = m.cx - m.rx - 6, x1 = m.cx + m.rx + 6, y0 = m.my - m.ry - 6, y1 = m.cy + 14;
  for (int k = 0; k < 8; k++)
    for (int i = 0; i <= N; i++) {
      x0 = std::min(x0, m.ax[k][i] - 17), x1 = std::max(x1, m.ax[k][i] + 17);
      y0 = std::min(y0, m.ay[k][i] - 17), y1 = std::max(y1, m.ay[k][i] + 17);
    }
  m.x0 = x0, m.y0 = y0, m.x1 = x1, m.y1 = y1;
}

inline float arm_r(const Model &m, int k, float u) {
  return SC * (12.5f * std::pow(1 - u, 0.85f) + 2.6f) * (1 - 0.25f * m.wi) * ((k == 3 || k == 4) ? 1 + 0.35f * m.ws : 1.0f);
}

// ---- a small rasteriser: discs, ellipses and spans straight into an RGB565 buffer, with an anti-aliased edge
struct Rz {
  uint8_t *base;
  size_t stride;
  int cx1, cy1, cx2, cy2;  // clip, inclusive
};
inline uint16_t c565(uint32_t rgb) { return (uint16_t) (((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x1F)); }
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint32_t a5) {  // a5: 0..32 of fg
  const uint32_t f = (fg | ((uint32_t) fg << 16)) & 0x07E0F81Fu, b = (bg | ((uint32_t) bg << 16)) & 0x07E0F81Fu;
  const uint32_t r = ((((f - b) * a5) >> 5) + b) & 0x07E0F81Fu;
  return (uint16_t) (r | (r >> 16));
}
inline void px_a(const Rz &r, int x, int y, uint16_t c, uint32_t a5) {  // one pixel at a5/32, clipped
  if (x < r.cx1 || x > r.cx2 || y < r.cy1 || y > r.cy2 || a5 == 0)
    return;
  uint16_t *p = (uint16_t *) (r.base + (size_t) y * r.stride) + x;
  *p = a5 >= 32 ? c : blend565(c, *p, a5);
}
// the row y of a shape whose edges on that row are xl..xr: solid inside, a blended pixel at each end
// (alpha 0..32 for the whole shape, so a translucent shape works too)
inline void row(const Rz &r, int y, float xl, float xr, uint16_t c, uint32_t alpha = 32) {
  if (y < r.cy1 || y > r.cy2 || xr <= xl)
    return;
  const int x1 = (int) std::ceil(xl), x2 = (int) std::floor(xr);  // pixel centres at integers
  if (x1 > x2) {  // narrower than a pixel
    px_a(r, (int) std::floor(xl), y, c, (uint32_t) ((xr - xl) * alpha));
    return;
  }
  if (x1 - xl > 0.02f)
    px_a(r, x1 - 1, y, c, (uint32_t) ((x1 - xl) * alpha));
  if (xr - x2 > 0.02f)
    px_a(r, x2 + 1, y, c, (uint32_t) ((xr - x2) * alpha));
  const int a = std::max(x1, r.cx1), b = std::min(x2, r.cx2);
  if (a > b)
    return;
  uint16_t *p = (uint16_t *) (r.base + (size_t) y * r.stride);
  if (alpha >= 32) {
    for (int x = a; x <= b; x++)
      p[x] = c;
  } else {
    for (int x = a; x <= b; x++)
      p[x] = blend565(c, p[x], alpha);
  }
}
inline void ellipse(const Rz &r, float cx, float cy, float rx, float ry, uint16_t c, uint32_t alpha = 32) {
  if (cx + rx < r.cx1 || cx - rx > r.cx2)
    return;
  const int y0 = std::max(r.cy1, (int) std::ceil(cy - ry)), y1 = std::min(r.cy2, (int) std::floor(cy + ry));
  for (int y = y0; y <= y1; y++) {
    const float d = (y - cy) / ry, q = 1 - d * d;
    if (q <= 0)
      continue;
    const float hw = rx * std::sqrt(q);
    row(r, y, cx - hw, cx + hw, c, alpha);
  }
}
inline void disc(const Rz &r, float cx, float cy, float rad, uint16_t c, uint32_t alpha = 32) { ellipse(r, cx, cy, rad, rad, c, alpha); }

// the octopus, onto the composite (everything inside the clip)
inline void paint_octopus(const Rz &r, const Model &m) {
  const uint16_t dark = c565(C_DARK), body = c565(m.body), band = c565(m.band), sucker = c565(0xF4E8D0);
  // arms: all the outlines, then all the fills (neighbours do not outline each other); a stamp every ~3 px
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < 8; k++)
      for (int i = 1; i <= N; i++) {
        const float xa = m.ax[k][i - 1], ya = m.ay[k][i - 1], xb = m.ax[k][i], yb = m.ay[k][i];
        const float ra = arm_r(m, k, (float) (i - 1) / N), rb = arm_r(m, k, (float) i / N);
        const float pad = std::max(ra, rb) + 4;
        if (std::max(xa, xb) + pad < r.cx1 || std::min(xa, xb) - pad > r.cx2 || std::max(ya, yb) + pad < r.cy1 ||
            std::min(ya, yb) - pad > r.cy2)
          continue;
        const uint16_t c = pass == 0 ? dark : (i % 2 ? body : band);
        for (int s = 0; s < 4; s++) {
          const float f = s * 0.25f;
          disc(r, xa + (xb - xa) * f, ya + (yb - ya) * f, ra + (rb - ra) * f + (pass == 0 ? 1.8f : 0.0f), c);
        }
        if (i == N)
          disc(r, xb, yb, rb + (pass == 0 ? 1.8f : 0.0f), c);
      }
  for (int k = 0; k < 8; k++)  // suckers
    for (int i = 3; i < N; i += 2)
      disc(r, m.ax[k][i], m.ay[k][i] + 1.5f, arm_r(m, k, (float) i / N) * 0.30f, sucker);
  // the mantle, with its bands
  ellipse(r, m.cx, m.my, m.rx + 1.8f, m.ry + 1.8f, dark);
  ellipse(r, m.cx, m.my, m.rx, m.ry, body);
  for (int j = 0; j < 5; j++) {
    const float yb = m.my - m.ry + (14 + j * 26) * SC;
    for (int y = (int) std::ceil(yb - 3); y <= (int) std::floor(yb + 3); y++) {
      const float d = (y - m.my) / m.ry, q = 1 - d * d;
      if (q <= 0)
        continue;
      const float hw = m.rx * std::sqrt(q) - 2;
      if (hw > 2)
        row(r, y, m.cx - hw, m.cx + hw, band);
    }
  }
  ellipse(r, m.cx - 15 * SC, m.my - 45 * SC, 11 * SC, 7 * SC, c565(mix3(m.body, 0xFFFFFF, 0.35f)));
  // the eyes: cream, a horizontal slit pupil, a lid
  const uint16_t cream = c565(0xFAF4E2), slit = c565(0x140E16);
  const float ey = m.my + 30 * SC, er = 15 * SC;
  for (int sg = -1; sg <= 1; sg += 2) {
    const float ex = m.cx + sg * 26 * SC;
    disc(r, ex, ey, er + 1.5f, dark);
    ellipse(r, ex, ey, er, er * m.blink, cream);
    if (m.blink > 0.5f)
      for (int y = (int) std::ceil(ey - 3.6f * SC); y <= (int) std::floor(ey + 3.6f * SC); y++)
        row(r, y, ex + (m.look - 10) * SC, ex + (m.look + 10) * SC, slit);
    // the lid: the wedge of the eye between 205 and 335 degrees (above the middle, 2.14 wide for every 1 up)
    for (int y = (int) std::ceil(ey - er); y < (int) ey; y++) {
      const float dy = ey - y, q = er * er - dy * dy;
      if (q <= 0)
        continue;
      const float hw = std::min(std::sqrt(q), dy * 2.14f);
      row(r, y, ex - hw, ex + hw, body);
      px_a(r, (int) std::lround(ex - hw), y, dark, 32);  // its lower edges
      px_a(r, (int) std::lround(ex + hw), y, dark, 32);
    }
  }
}

// The ink: NI opaque dark blobs that spread from the octopus and shrink away (opaque discs are plain span writes; 26
// translucent ones, each blended with what is under it, made the exit the slowest part of a visit).
constexpr int NI = 16;
inline bool ink_blob(int i, float t, float &x, float &y, float &rad) {
  if (t < EXIT_T)
    return false;
  float ox, oy;
  pos(EXIT_T, ox, oy);
  const float age = t - EXIT_T;
  x = ox + 10 + st.ivx[i] * age * (0.6f + i / 26.0f);
  y = oy + 20 + st.ivy[i] * age * 0.6f;
  const float grow = 0.5f + 1.1f * std::min(1.0f, age / 0.8f);
  const float fade = 1 - seg(age, 0.9f, 2.0f + st.ia[i] * 0.3f);  // gone by ~2.2 s
  rad = st.ir[i] * SC * grow * fade;
  return rad >= 1.5f;
}
inline void paint_ink(const Rz &r, float t) {
  const uint16_t c = c565(0x0A0418);
  for (int i = 0; i < NI; i++) {
    float x, y, rad;
    if (!ink_blob(i, t, x, y, rad) || x + rad < r.cx1 || x - rad > r.cx2 || y + rad < r.cy1 || y - rad > r.cy2)
      continue;
    disc(r, x, y, rad, c);
  }
}
// what the cloud covers now (the redraw box): false when there is none
inline bool ink_box(float t, int &x1, int &y1, int &x2, int &y2) {
  bool any = false;
  x1 = y1 = 100000, x2 = y2 = -100000;
  for (int i = 0; i < NI; i++) {
    float x, y, rad;
    if (!ink_blob(i, t, x, y, rad))
      continue;
    any = true;
    x1 = std::min(x1, (int) (x - rad) - 2), x2 = std::max(x2, (int) (x + rad) + 2);
    y1 = std::min(y1, (int) (y - rad) - 2), y2 = std::max(y2, (int) (y + rad) + 2);
  }
  return any;
}

// ---- the speech bubble now: its box (the same maths for the painter and the redraw box)
inline void bubble_geom(float t, int beat, float &bx, float &by, float &bw, float &bh) {
  bw = bh = 0;
  bx = by = 0;
  const lv_font_t *f = rocket::st.font;
  if (beat < 0 || st.say[beat] == nullptr || f == nullptr)
    return;
  lv_point_t sz;
  lv_text_get_size(&sz, st.say[beat], f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  bw = (float) sz.x + 28;
  bh = (float) sz.y + 18;
  float cx, cy;
  pos(t, cx, cy);
  bx = std::max(10.0f, std::min(480 - bw - 10, cx - bw / 2 + 30));
  by = beat == N_BEATS - 1 ? std::max(46.0f, cy - 120) : std::max(46.0f, cy - (46 + 66) * SC - bh - 22);
  const float pop = ease((t - BEAT_A[beat]) / 0.18f);
  by += (1 - pop) * 10;
}

inline void rect(lv_layer_t *l, float x0, float y0, float x1, float y1, uint32_t col, int radius, uint8_t opa = 255,
                 uint32_t border = 0, int bw = 0) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = radius;
  d.bg_color = lv_color_hex(col);
  d.bg_opa = opa;
  if (bw) {
    d.border_width = bw;
    d.border_color = lv_color_hex(border);
    d.border_opa = LV_OPA_COVER;
  }
  const lv_area_t a = {(int32_t) std::lround(x0), (int32_t) std::lround(y0), (int32_t) std::lround(x1) - 1, (int32_t) std::lround(y1) - 1};
  lv_draw_rect(l, &d, &a);
}

// the bubble, through a canvas layer (a few primitives and one label; only while a line is up)
inline void paint_bubble(lv_layer_t *layer) {
  if (st.bw <= 0 || rocket::st.font == nullptr)
    return;
  const float bx = st.bx, by = st.by, bw = st.bw, bh = st.bh;
  rect(layer, bx, by, bx + bw, by + bh, 0x0E1836, 14, 255, 0x3A4E86, 2);
  float cx, cy;
  pos(st.t, cx, cy);
  const float tx = std::min(cx, 440.0f);
  lv_draw_triangle_dsc_t tri;
  lv_draw_triangle_dsc_init(&tri);
  tri.color = lv_color_hex(0x3A4E86);
  tri.p[0].x = tx - 8, tri.p[0].y = by + bh - 3;
  tri.p[1].x = tx + 20, tri.p[1].y = by + bh - 3;
  tri.p[2].x = tx + 2, tri.p[2].y = by + bh + 20;
  lv_draw_triangle(layer, &tri);
  tri.color = lv_color_hex(0x0E1836);
  tri.p[0].x = tx - 4, tri.p[0].y = by + bh - 3;
  tri.p[1].x = tx + 16, tri.p[1].y = by + bh - 3;
  tri.p[2].x = tx + 2, tri.p[2].y = by + bh + 14;
  lv_draw_triangle(layer, &tri);
  rect(layer, tx - 4, by + bh - 4, tx + 16, by + bh, 0x0E1836, 0);  // the seam
  lv_draw_label_dsc_t l;
  lv_draw_label_dsc_init(&l);
  l.font = rocket::st.font;
  l.color = lv_color_hex(0xEEF2FF);
  l.text = st.say[beat_at(st.t)];
  const lv_area_t a = {(int32_t) (bx + 14), (int32_t) (by + 9), (int32_t) (bx + bw + 40), (int32_t) (by + bh)};
  lv_draw_label(layer, &l, &a);
}

inline void on_refr(lv_event_t *);
inline void frame();

inline void end_visit() {
  if (!st.running)
    return;
  if (st.timer) {
    lv_timer_delete(st.timer);
    st.timer = nullptr;
  }
  if (lv_display_t *d = lv_display_get_default()) {
    lv_display_remove_event_cb_with_user_data(d, on_refr, nullptr);
    lv_display_remove_event_cb_with_user_data(d, rocket::on_prof, nullptr);
  }
  const auto &pf = rocket::st.prof;
  if (pf.n > 0)  // what the screen's refreshes cost while it played (the same counters as the launch animation's)
    ESP_LOGI("octo", "%u refreshes: render %.1f/%.1f ms (avg/max), flush %.1f/%.1f ms in %.1f chunks, %u/%u px (avg/max)", (unsigned) pf.n,
             pf.render / 1000.0f / pf.n, pf.render_max / 1000.0f, pf.flush / 1000.0f / pf.n, pf.flush_max / 1000.0f,
             (float) pf.chunks / pf.n, (unsigned) (pf.px / pf.n), (unsigned) pf.px_max);
  const uint32_t ms = rocket::now_ms() - st.t0;
  if (st.exit_n > 0)
    ESP_LOGI("octo", "ink and exit: %u frames, painting %.1f ms a frame, %u px a frame; before: %.1f ms", (unsigned) st.exit_n,
             st.exit_us / 1000.0f / st.exit_n, (unsigned) (st.exit_px / st.exit_n),
             (st.paint_us - st.exit_us) / 1000.0f / std::max<uint32_t>(1, st.frames - st.exit_n));
  if (st.frames > 1 && ms) {
    rocket::set_last_fps(st.frames * 1000.0f / ms);
    ESP_LOGI("octo", "visit: %u frames in %u ms (%.1f fps), longest gap %u ms; painting %.1f ms a frame (longest %.1f ms), %u px a frame",
             (unsigned) st.frames, (unsigned) ms, st.frames * 1000.0f / ms, (unsigned) st.max_gap, st.paint_us / 1000.0f / st.frames,
             st.paint_max / 1000.0f, (unsigned) (st.px / st.frames));
  }
  for (lv_obj_t **o : {&st.cv, &st.catcher})
    if (*o) {
      lv_obj_delete(*o);
      *o = nullptr;
    }
  rocket::st.snap = st.clean;  // the backdrop shows the composite meanwhile; rocket::stop() frees the clean one
  if (st.comp)
    lv_draw_buf_destroy(st.comp);
  st.comp = st.clean = nullptr;
  rocket::stop();  // the backdrop (and its snapshot)
  st.running = false;
}

inline void inval(const lv_area_t &a) {
  if (rocket::st.back && a.x2 >= a.x1 && a.y2 >= a.y1)
    lv_obj_invalidate_area(rocket::st.back, &a);
}

inline void frame() {
  if (!st.running)
    return;
  const uint32_t now = rocket::now_ms();
  const float t = (now - st.t0) / 1000.0f;
  if (t >= DUR_S || (rocket::st.back && lv_obj_get_parent(rocket::st.back) != lv_screen_active())) {
    end_visit();
    return;
  }
  if (st.frames > 0 && now - st.last_model < MODEL_MS)
    return;
  if (st.last_frame) {
    const uint32_t gap = now - st.last_frame;
    if (gap > st.max_gap)
      st.max_gap = gap;
  }
  st.last_frame = now;
  st.last_model = now;
  st.frames++;
  st.t = t;
  build(t, st.m);
  const int beat = beat_at(t);
  bubble_geom(t, beat, st.bx, st.by, st.bw, st.bh);
  int32_t bx1 = (int32_t) st.m.x0, by1 = (int32_t) st.m.y0, bx2 = (int32_t) st.m.x1, by2 = (int32_t) st.m.y1;
  if (st.bw > 0) {  // the bubble and its tail
    bx1 = std::min(bx1, (int32_t) st.bx - 4), by1 = std::min(by1, (int32_t) st.by - 4);
    bx2 = std::max(bx2, (int32_t) (st.bx + st.bw + 6)), by2 = std::max(by2, (int32_t) (st.by + st.bh + 24));
  }
  {  // the cloud
    int cx1, cy1, cx2, cy2;
    if (ink_box(t, cx1, cy1, cx2, cy2))
      bx1 = std::min<int32_t>(bx1, cx1), by1 = std::min<int32_t>(by1, cy1), bx2 = std::max<int32_t>(bx2, cx2), by2 = std::max<int32_t>(by2, cy2);
  }
  lv_area_t box = {std::max<int32_t>(0, bx1), std::max<int32_t>(0, by1), std::min<int32_t>(479, bx2), std::min<int32_t>(479, by2)};
  // What is drawn on the screen is one RGB565 image (the composite), blitted over the dirty box in a few
  // milliseconds. The paint happens here, off the scan, into the composite.
  lv_area_t reg = box;
  if (st.last_box.x2 >= st.last_box.x1) {
    reg.x1 = std::min(reg.x1, st.last_box.x1), reg.y1 = std::min(reg.y1, st.last_box.y1);
    reg.x2 = std::max(reg.x2, st.last_box.x2), reg.y2 = std::max(reg.y2, st.last_box.y2);
  }
  const uint32_t t_paint = rocket::now_us();
  for (int32_t y = reg.y1; y <= reg.y2; y++)  // the screen as it was, under the box
    memcpy((uint8_t *) st.comp->data + (size_t) y * st.comp->header.stride + (size_t) reg.x1 * 2,
           (const uint8_t *) st.clean->data + (size_t) y * st.clean->header.stride + (size_t) reg.x1 * 2, (size_t) (reg.x2 - reg.x1 + 1) * 2);
  const Rz rz = {(uint8_t *) st.comp->data, st.comp->header.stride, reg.x1, reg.y1, reg.x2, reg.y2};
  paint_octopus(rz, st.m);
  paint_ink(rz, t);
  if (st.bw > 0) {
    lv_layer_t layer;
    lv_canvas_init_layer(st.cv, &layer);
    layer._clip_area = reg;
    paint_bubble(&layer);
    lv_canvas_finish_layer(st.cv, &layer);
  }
  const uint32_t us = rocket::now_us() - t_paint;
  st.paint_us += us;
  st.paint_max = std::max(st.paint_max, us);
  st.px += (uint32_t) ((reg.x2 - reg.x1 + 1) * (reg.y2 - reg.y1 + 1));
  if (t >= EXIT_T)
    st.exit_us += us, st.exit_n++, st.exit_px += (uint32_t) ((reg.x2 - reg.x1 + 1) * (reg.y2 - reg.y1 + 1));
  inval(reg);
  st.last_box = box;
}
inline void on_refr(lv_event_t *) {
  frame();
  rocket::st.prof.start = rocket::now_us();  // the refresh that follows: its render time, from here to REFR_READY
}

// start a visit now; false: no memory for the backdrop, or something else is playing
inline bool begin(uint32_t seed) {
  if (st.running || rocket::playing() || rocket::st.font == nullptr)
    return false;
  st.rng = seed ? seed : 0x9E3779B9u;
  rocket::stop();
  rocket::back_take();
  if (rocket::st.snap == nullptr) {
    rocket::stop();
    return false;
  }
  // the composite: a copy of the screen the visit paints on; the backdrop (rocket::back_draw) shows it
  st.clean = rocket::st.snap;
  st.comp = lv_draw_buf_create(480, 480, LV_COLOR_FORMAT_RGB565, 0);
  if (st.comp == nullptr) {
    ESP_LOGW("octo", "no memory for the composite: no visit");
    st.clean = nullptr;
    rocket::stop();
    return false;
  }
  for (int32_t y = 0; y < 480; y++)
    memcpy((uint8_t *) st.comp->data + (size_t) y * st.comp->header.stride, (const uint8_t *) st.clean->data + (size_t) y * st.clean->header.stride, 480 * 2);
  rocket::st.snap = st.comp;
  // one thing to morph into, and a line for each beat
  st.kind = (int) (rnd() % 3);
  st.say[0] = SAY_ARRIVE[rnd() % (sizeof(SAY_ARRIVE) / sizeof(*SAY_ARRIVE))];
  st.say[1] = st.kind == K_LION    ? SAY_LION[rnd() % (sizeof(SAY_LION) / sizeof(*SAY_LION))]
              : st.kind == K_SNAKE ? SAY_SNAKE[rnd() % (sizeof(SAY_SNAKE) / sizeof(*SAY_SNAKE))]
                                   : SAY_ISS[rnd() % (sizeof(SAY_ISS) / sizeof(*SAY_ISS))];
  st.say[2] = SAY_INK[rnd() % (sizeof(SAY_INK) / sizeof(*SAY_INK))];
  const uint32_t keep = st.rng;
  st.rng = 0x1234567u;  // the ink is the same every visit
  for (int i = 0; i < 26; i++) {  // (NI of them are used)
    st.ivx[i] = -30 + 100 * rnd01();
    st.ivy[i] = -40 + 100 * rnd01();
    st.ir[i] = 14 + 16 * rnd01();
    st.ia[i] = 0.55f + 0.35f * rnd01();
  }
  st.rng = keep;
  // a transparent full-screen layer: any tap ends it
  st.catcher = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(st.catcher);
  lv_obj_set_size(st.catcher, 480, 480);
  lv_obj_add_flag(st.catcher, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(st.catcher, [](lv_event_t *) { end_visit(); }, LV_EVENT_CLICKED, nullptr);
  st.cv = lv_canvas_create(lv_layer_top());  // holds the composite; never shown itself
  lv_obj_add_flag(st.cv, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_draw_buf(st.cv, st.comp);
  st.frames = st.max_gap = st.last_frame = 0;
  st.paint_us = st.paint_max = st.px = st.exit_us = st.exit_n = st.exit_px = 0;
  st.last_box = {0, 0, -1, -1};
  st.bw = 0;
  st.t = 0;
  st.t0 = rocket::now_ms();
  st.running = true;
  rocket::st.prof = rocket::St::Prof();
  if (lv_display_t *d = lv_display_get_default()) {
    lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_START, nullptr);
    for (lv_event_code_t c : {LV_EVENT_REFR_READY, LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH, LV_EVENT_INVALIDATE_AREA})
      lv_display_add_event_cb(d, rocket::on_prof, c, nullptr);
  }
  st.timer = lv_timer_create(
      [](lv_timer_t *) {  // the clock: a step skipped by the throttle invalidates nothing, so no refresh would come to wake it
        if (rocket::now_ms() - st.last_model >= MODEL_MS)
          frame();
      },
      20, nullptr);
  frame();
  ESP_LOGI("octo", "visit: %s, \"%s\" / \"%s\"", st.kind == K_LION ? "lionfish" : st.kind == K_SNAKE ? "sea snake" : "ISS", st.say[1], st.say[2]);
  return true;
}

inline void schedule(bool first) {
  const uint32_t lo = first ? FIRST_MIN_S : MIN_GAP_S, hi = first ? FIRST_MAX_S : MAX_GAP_S;
  st.next_ms = rocket::now_ms() + (lo + rnd() % (hi - lo + 1)) * 1000u;
}

// a launch or an ISS docking/undocking close by wants the map to itself
inline bool busy_soon() {
  const double t = clock_now();
  for (const auto &l : live.launches)
    if (launch_pending(l) && l.net - t < 900 && t - l.net < rocket::LATE_S + 60)
      return true;
  for (const auto &e : live.events)
    if (e.iss && e.exact && std::fabs(e.t - t) < 900)
      return true;
  return false;
}

}  // namespace octo

namespace octo {
void set_enabled(bool on) {
  st.enabled = on;
  if (!on)
    end_visit();
  else if (st.next_ms == 0)
    schedule(true);
}
bool playing() { return st.running; }
void stop() { end_visit(); }
bool play_now() { return begin(rocket::now_us() * 2654435761u + 12345u); }
void check() {
  if (!st.enabled || st.running || rocket::playing() || !clock_valid() || !ui.ready || ui.ota)
    return;
  if (st.next_ms == 0) {
    st.rng ^= rocket::now_us();
    schedule(true);
    return;
  }
  if ((int32_t) (rocket::now_ms() - st.next_ms) < 0)
    return;
  // due: wait for a quiet map (it plays the moment one comes)
  if (lv_obj_get_child_count(lv_layer_top()) > 0 || ui.w.sky == nullptr || lv_obj_get_parent(ui.w.sky) != lv_screen_active() ||
      lv_display_get_inactive_time(nullptr) < min_idle_ms || busy_soon())
    return;
  if (begin(st.rng ^ rocket::now_us()))
    schedule(false);
}
}  // namespace octo
}  // namespace sat
#endif  // SKY_IMPL
