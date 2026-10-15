#pragma once
// UI-85 the octopus: a mimic octopus visits the map two or three times an hour, about 11 s. It jets in from
// the right, says something, imitates a lionfish or a banded sea snake, then the ISS (arms out like the
// station's solar arrays), squirts ink and jets off to the left. The words are drawn at random from a pool
// for each beat. A tap ends it. Off with the Octopus switch; the Play Octopus button shows it on demand.
//
// How it stays fast (what sky_rocket.h learned, UI-69o): the screen as it is, snapshotted, is the opaque
// backdrop (rocket::back_take), so a dirty box costs one image blit; one full-screen object on the top layer
// draws the octopus (lines with round ends for the arms, rounded rectangles, arcs: nothing scaled), the ink and
// the speech bubble in a single draw callback. The octopus is a few dozen primitives a frame, no sprite data.
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

constexpr float DUR_S = 11.0f;
constexpr int N = 12;  // segments an arm is drawn in
constexpr uint32_t MIN_GAP_S = 15 * 60, MAX_GAP_S = 35 * 60, FIRST_MIN_S = 8 * 60, FIRST_MAX_S = 15 * 60;
constexpr uint32_t MODEL_MS = 60;  // the model moves at ~16 fps: the octopus is slow, and each redraw is the whole octopus
constexpr uint32_t C_BODY = 0x80583C, C_BAND = 0xEEE0C0, C_DARK = 0x3C2820, C_ISS_BODY = 0x3E5CA0, C_ISS_BAND = 0xDECC82,
                   C_LION = 0xA04E34, C_SNAKE = 0x2C2630, C_SNAKE_BAND = 0xF4F0E8;

// ---- the words: one line a beat, drawn at random per visit
inline const char *const SAY_ARRIVE[] = {"Mimic octopus.\nIt's a lifestyle.", "Three hearts.\nNine brains.", "Blue blood.\nVery fancy.",
                                         "I can be a\nsea snake.", "Just passing\nthrough."};
inline const char *const SAY_LION[] = {"Lionfish.\nBe afraid.", "I'm a lionfish.\nVenomous.", "Lionfish.\nDon't touch."};
inline const char *const SAY_SNAKE[] = {"Sea snake.\nSsss.", "Banded sea snake.\nVery toxic.", "Sea snake.\nRespect."};
inline const char *const SAY_ISS[] = {"ISS.\nObviously.", "Space station\nmode.", "Look up! The\nISS!", "Orbiting.\nPlease hold."};
inline const char *const SAY_INK[] = {"Ink!", "Oops.", "Bye!", "Poof."};
constexpr int N_BEATS = 4;
constexpr float BEAT_A[N_BEATS] = {2.5f, 5.7f, 7.8f, 9.4f}, BEAT_B[N_BEATS] = {4.9f, 7.1f, 9.3f, 10.4f};

// ---- the timeline (pure: the host test checks it, the draw callback and the box use it)
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
  } else if (t < 9.4f) {  // hovers
    x = 260 + 10 * std::sin((t - 2.2f) * 0.9f);
    y = 262 + 9 * std::sin((t - 2.2f) * 1.7f);
  } else {  // jets off to the left
    const float k = seg(t, 9.4f, 10.9f);
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
  if (t > 9.4f)
    return std::pow(std::max(0.0f, std::sin(2 * (float) M_PI * 2.6f * (t - 9.4f))), 2.0f);
  return 0.25f * (0.5f + 0.5f * std::sin(t * 2.2f));
}
inline float w_imp(float t) { return seg(t, 5.4f, 5.9f) * (1 - seg(t, 7.0f, 7.4f)); }
inline float w_iss(float t) { return seg(t, 7.5f, 8.0f) * (1 - seg(t, 9.0f, 9.35f)); }
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
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // everything it covers (the box to redraw)
};

struct St {
  bool enabled = true, running = false;
  uint32_t t0 = 0, last_model = 0, last_frame = 0, frames = 0, max_gap = 0, next_ms = 0, rng = 0x9E3779B9u;
  int kind = 0;  // 0 lionfish, 1 sea snake
  const char *say[N_BEATS] = {};
  lv_obj_t *catcher = nullptr, *draw = nullptr;
  lv_timer_t *timer = nullptr;
  Model m;
  lv_area_t last_box = {0, 0, -1, -1};
  float t = 0;
  // the ink: a fixed set of blobs (velocity, size, strength), the same every visit
  float ivx[26], ivy[26], ir[26], ia[26];
  float bx = 0, by = 0, bw = 0, bh = 0;  // the bubble now (bw 0: none)
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
  m.my = m.cy - 46;
  m.p = jet(t);
  m.wl = st.kind == 0 ? w_imp(t) : 0;
  m.ws = st.kind == 1 ? w_imp(t) : 0;
  m.wi = w_iss(t);
  const float wn = 1 - m.wl - m.wi - m.ws;
  m.body = mix3(mix3(mix3(C_BODY, C_LION, m.wl), C_ISS_BODY, m.wi), C_SNAKE, m.ws);
  m.band = mix3(mix3(C_BAND, C_ISS_BAND, m.wi), C_SNAKE_BAND, m.ws);
  m.rx = 56 * (1 - 0.10f * m.p);
  m.ry = 66 * (1 + 0.08f * m.p);
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
  }
  m.blink = 1;
  for (float bt : {3.4f, 6.2f, 8.4f, 10.1f})
    if (std::fabs(t - bt) < 0.09f)
      m.blink = 0.12f;
  m.look = t < 2.3f ? -4.0f : 3 * std::sin(t * 1.3f);
  // the box: every arm point, the mantle, a margin for the line widths
  float x0 = m.cx - m.rx - 6, x1 = m.cx + m.rx + 6, y0 = m.my - m.ry - 6, y1 = m.cy + 20;
  for (int k = 0; k < 8; k++)
    for (int i = 0; i <= N; i++) {
      x0 = std::min(x0, m.ax[k][i] - 22), x1 = std::max(x1, m.ax[k][i] + 22);
      y0 = std::min(y0, m.ay[k][i] - 22), y1 = std::max(y1, m.ay[k][i] + 22);
    }
  m.x0 = x0, m.y0 = y0, m.x1 = x1, m.y1 = y1;
}

inline float arm_r(const Model &m, int k, float u) {
  return (12.5f * std::pow(1 - u, 0.85f) + 2.6f) * (1 - 0.25f * m.wi) * ((k == 3 || k == 4) ? 1 + 0.35f * m.ws : 1.0f);
}

// the speech bubble now: its box (the same maths for the draw callback and the redraw box)
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
  by = beat == 3 ? std::max(46.0f, cy - 140) : std::max(46.0f, cy - 46 - 66 - bh - 22);
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

inline void draw_cb(lv_event_t *e) {
  if (!st.running)
    return;
  lv_layer_t *layer = lv_event_get_layer(e);
  const lv_area_t &clip = layer->_clip_area;
  const Model &m = st.m;
  if (m.x1 + 30 < clip.x1 || m.x0 - 30 > clip.x2 || m.y1 + 30 < clip.y1 || m.y0 - 30 > clip.y2) {
    // (the octopus is out of this strip; the ink and bubble may not be)
  } else {
    // arms: all the outlines, then all the fills (neighbours do not outline each other)
    for (int pass = 0; pass < 2; pass++)
      for (int k = 0; k < 8; k++)
        for (int i = 1; i <= N; i++) {
          const float u = (i - 0.5f) / N, r = arm_r(m, k, u);
          const float xa = m.ax[k][i - 1], ya = m.ay[k][i - 1], xb = m.ax[k][i], yb = m.ay[k][i];
          const float pad = r + 4;
          if (std::max(xa, xb) + pad < clip.x1 || std::min(xa, xb) - pad > clip.x2 || std::max(ya, yb) + pad < clip.y1 ||
              std::min(ya, yb) - pad > clip.y2)
            continue;
          lv_draw_line_dsc_t d;
          lv_draw_line_dsc_init(&d);
          d.p1.x = xa, d.p1.y = ya, d.p2.x = xb, d.p2.y = yb;
          d.width = (int32_t) std::lround(2 * r + (pass == 0 ? 4.4f : 0.0f));
          d.color = lv_color_hex(pass == 0 ? C_DARK : (i % 2 ? m.body : m.band));
          d.round_start = d.round_end = 1;
          lv_draw_line(layer, &d);
        }
    // the mantle, with its bands
    rect(layer, m.cx - m.rx, m.my - m.ry, m.cx + m.rx, m.my + m.ry, m.body, LV_RADIUS_CIRCLE, 255, C_DARK, 2);
    const float half = m.ry - m.rx;  // the straight part of the stadium
    for (int j = 0; j < 5; j++) {
      const float y = m.my - m.ry + 14 + j * 26, dy = std::fabs(y - m.my);
      const float w = dy <= half ? m.rx : std::sqrt(std::max(0.0f, m.rx * m.rx - (dy - half) * (dy - half)));
      if (w > 6)
        rect(layer, m.cx - w + 3, y - 4, m.cx + w - 3, y + 4, m.band, 4);
    }
    rect(layer, m.cx - 26, m.my - 52, m.cx - 4, m.my - 38, mix3(m.body, 0xFFFFFF, 0.35f), LV_RADIUS_CIRCLE);
    // the eyes: cream, with a horizontal slit pupil
    const float ey = m.my + 30, er = 15;
    for (int sg = -1; sg <= 1; sg += 2) {
      const float ex = m.cx + sg * 26;
      rect(layer, ex - er - 2, ey - er - 2, ex + er + 2, ey + er + 2, C_DARK, LV_RADIUS_CIRCLE);
      rect(layer, ex - er, ey - er * m.blink, ex + er, ey + er * m.blink, 0xFAF4E2, LV_RADIUS_CIRCLE);
      if (m.blink > 0.5f)
        rect(layer, ex + m.look - 10, ey - 3.6f, ex + m.look + 10, ey + 3.6f, 0x140E16, 3);
      lv_draw_arc_dsc_t a;
      lv_draw_arc_dsc_init(&a);
      a.center.x = (int32_t) std::lround(ex);
      a.center.y = (int32_t) std::lround(ey);
      a.radius = (uint16_t) er;
      a.start_angle = 205;
      a.end_angle = 335;
      a.width = (uint16_t) er;  // a filled wedge: the lid
      a.color = lv_color_hex(m.body);
      lv_draw_arc(layer, &a);
      a.width = 3;
      a.color = lv_color_hex(C_DARK);
      lv_draw_arc(layer, &a);
    }
  }
  // the ink
  if (st.t >= 9.4f) {
    float ox, oy;
    pos(9.4f, ox, oy);
    const float age = st.t - 9.4f;
    for (int i = 0; i < 26; i++) {
      const float x = ox + 10 + st.ivx[i] * age * (0.6f + i / 40.0f), y = oy + 20 + st.ivy[i] * age * 0.6f;
      const float r = st.ir[i] * (0.5f + 1.3f * std::min(1.0f, age / 0.8f));
      const float a = st.ia[i] * std::max(0.0f, 1 - age / 2.3f);
      if (a <= 0.02f || x + r < clip.x1 || x - r > clip.x2 || y + r < clip.y1 || y - r > clip.y2)
        continue;
      rect(layer, x - r, y - r, x + r, y + r, 0x0A0418, LV_RADIUS_CIRCLE, (uint8_t) std::min(255.0f, a * 1.4f * 255));
    }
  }
  // the speech bubble
  if (st.bw > 0 && rocket::st.font) {
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
  if (lv_display_t *d = lv_display_get_default())
    lv_display_remove_event_cb_with_user_data(d, on_refr, nullptr);
  const uint32_t ms = rocket::now_ms() - st.t0;
  if (st.frames > 1 && ms) {
    rocket::set_last_fps(st.frames * 1000.0f / ms);
    ESP_LOGI("octo", "visit: %u frames in %u ms (%.1f fps), longest gap %u ms", (unsigned) st.frames, (unsigned) ms,
             st.frames * 1000.0f / ms, (unsigned) st.max_gap);
  }
  for (lv_obj_t **o : {&st.draw, &st.catcher})
    if (*o) {
      lv_obj_delete(*o);
      *o = nullptr;
    }
  rocket::stop();  // the backdrop (and its snapshot)
  st.running = false;
}

inline void inval(const lv_area_t &a) {
  if (st.draw && a.x2 >= a.x1 && a.y2 >= a.y1)
    lv_obj_invalidate_area(st.draw, &a);
}

inline void frame() {
  if (!st.running)
    return;
  const uint32_t now = rocket::now_ms();
  if (st.last_frame) {
    const uint32_t gap = now - st.last_frame;
    if (gap > st.max_gap)
      st.max_gap = gap;
  }
  st.last_frame = now;
  const float t = (now - st.t0) / 1000.0f;
  if (t >= DUR_S || (rocket::st.back && lv_obj_get_parent(rocket::st.back) != lv_screen_active())) {
    end_visit();
    return;
  }
  if (st.frames > 0 && now - st.last_model < MODEL_MS)
    return;
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
  if (t >= 9.4f) {  // the cloud
    bx1 = std::min<int32_t>(bx1, 160), bx2 = 479, by1 = std::min<int32_t>(by1, 120), by2 = std::max<int32_t>(by2, 440);
  }
  lv_area_t box = {std::max<int32_t>(0, bx1), std::max<int32_t>(0, by1), std::min<int32_t>(479, bx2), std::min<int32_t>(479, by2)};
  inval(box);
  inval(st.last_box);  // what it covered before
  st.last_box = box;
}
inline void on_refr(lv_event_t *) { frame(); }

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
  st.kind = (int) (rnd() % 2);
  st.say[0] = SAY_ARRIVE[rnd() % (sizeof(SAY_ARRIVE) / sizeof(*SAY_ARRIVE))];
  st.say[1] = st.kind == 0 ? SAY_LION[rnd() % (sizeof(SAY_LION) / sizeof(*SAY_LION))]
                           : SAY_SNAKE[rnd() % (sizeof(SAY_SNAKE) / sizeof(*SAY_SNAKE))];
  st.say[2] = SAY_ISS[rnd() % (sizeof(SAY_ISS) / sizeof(*SAY_ISS))];
  st.say[3] = SAY_INK[rnd() % (sizeof(SAY_INK) / sizeof(*SAY_INK))];
  uint32_t keep = st.rng;
  st.rng = 0x1234567u;  // the ink is the same every visit
  for (int i = 0; i < 26; i++) {
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
  st.draw = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(st.draw);
  lv_obj_set_size(st.draw, 480, 480);
  lv_obj_remove_flag(st.draw, (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_event_cb(st.draw, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  st.frames = st.max_gap = st.last_frame = 0;
  st.last_box = {0, 0, -1, -1};
  st.bw = 0;
  st.t = 0;
  st.t0 = rocket::now_ms();
  st.running = true;
  if (lv_display_t *d = lv_display_get_default())
    lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_START, nullptr);
  st.timer = lv_timer_create(
      [](lv_timer_t *) {
        if (rocket::now_ms() - st.last_frame >= 150)
          frame();
      },
      40, nullptr);
  frame();
  ESP_LOGI("octo", "visit: %s, \"%s\" / \"%s\"", st.kind == 0 ? "lionfish" : "sea snake", st.say[1], st.say[2]);
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
