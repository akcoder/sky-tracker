#pragma once
// UI-69 launch animation. When a rocket launch (UI-54, Launch Library) reaches T-0, a cartoon
// rocket lifts off from the lower left of the screen with a puff of smoke at the pad, arcs up
// across the screen leaving a fading smoke trail and leaves past the upper right, about 4.5 s.
// A banner at the bottom names the mission. Once per launch, a tap anywhere skips it. Off with
// the Launch Animation switch; the Play Launch Animation button shows it on demand.
// Everything sits on lv_layer_top and is deleted when it ends. The rocket is drawn once at
// init into two small ARGB8888 buffers (two flame lengths, swapped for the flicker) and shown
// with an lv_image rotated along the path; the smoke is a ring of round lv_objs.
// Compiled in sky_extra.cpp (SKY_IMPL), as sky_about.h.
#include <cstdint>

#ifndef SKY_IMPL
namespace sat {
namespace rocket {
void init(const lv_font_t *banner);
void set_enabled(bool on);
void play_now();  // the button: the next launch's name (or a generic one)
}  // namespace rocket
}  // namespace sat
#else
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sat {
namespace rocket {

constexpr float DUR_S = 4.5f;        // pad to off screen
constexpr float S = 22.0f;           // rocket scale (body half-width is 0.32 S)
constexpr int IW = 32, IH = 84;      // image buffer
constexpr int PX = 16, PY = 30;      // pivot (body centre) in the buffer
constexpr int PUFFS = 48, PADS = 3;
constexpr float PAD_X = 140, PAD_Y = 400;
constexpr double LATE_S = 120;       // a launch is "now" for 2 min after T-0 (refresh lag)

struct St {
  bool enabled = true;
  const lv_font_t *font = nullptr;
  lv_draw_buf_t *img_buf[2] = {};
  lv_obj_t *catcher = nullptr, *img = nullptr, *banner = nullptr, *label = nullptr;
  lv_obj_t *puff[PUFFS] = {}, *pad[PADS] = {};
  float puff_x[PUFFS] = {}, puff_y[PUFFS] = {}, puff_t[PUFFS] = {};
  int puff_next = 0;
  float last_emit = -1;
  uint32_t t0 = 0;
  lv_timer_t *anim = nullptr;
  uint32_t played_key = 0;  // the last launch shown (name + T-0)
};
inline St st;

#ifdef SAT_HOST_TEST
inline uint32_t now_ms() { return (uint32_t) (sat_host_now * 1000.0); }
#else
inline uint32_t now_ms() { return esphome::millis(); }
#endif

inline uint32_t key_of(const char *name, double net) {
  uint32_t h = 2166136261u;
  for (const char *p = name; *p; p++)
    h = (h ^ (uint8_t) *p) * 16777619u;
  return h ^ (uint32_t) (int64_t) net;
}

// ---- the rocket, rasterised once (local units: x right, y down, body centre at 0,0)
struct Pt {
  float x, y;
};
inline bool in_poly(const Pt *p, int n, float x, float y) {
  bool in = false;
  for (int i = 0, j = n - 1; i < n; j = i++)
    if (((p[i].y > y) != (p[j].y > y)) && (x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x))
      in = !in;
  return in;
}
// colour of one sample, 0 = transparent
inline uint32_t sample(float x, float y, float flame) {
  const Pt nose[3] = {{-0.32f, -0.5f}, {0, -1.25f}, {0.32f, -0.5f}};
  const Pt body[4] = {{-0.32f, -0.5f}, {0.32f, -0.5f}, {0.32f, 0.95f}, {-0.32f, 0.95f}};
  const Pt finl[3] = {{-0.32f, 0.35f}, {-0.62f, 1.0f}, {-0.32f, 0.85f}};
  const Pt finr[3] = {{0.32f, 0.35f}, {0.62f, 1.0f}, {0.32f, 0.85f}};
  const Pt fo[3] = {{-0.28f, 0.95f}, {0, 0.95f + 1.1f * flame}, {0.28f, 0.95f}};
  const Pt fi[3] = {{-0.15f, 0.95f}, {0, 0.95f + 0.6f * flame}, {0.15f, 0.95f}};
  const float wr = 0.16f, wd = std::sqrt(x * x + (y + 0.1f) * (y + 0.1f));
  if (wd <= wr)
    return wd > wr - 0.05f ? 0xFF283250 : 0xFF3C78DC;  // window, dark rim
  if (in_poly(nose, 3, x, y))
    return 0xFFD63C3C;
  if (in_poly(body, 4, x, y))
    return 0xFFEBEEF5;
  if (in_poly(finl, 3, x, y) || in_poly(finr, 3, x, y))
    return 0xFFD63C3C;
  if (in_poly(fi, 3, x, y))
    return 0xFFFFEB78;
  if (in_poly(fo, 3, x, y))
    return 0xFFFF8C1E;
  return 0;
}
inline lv_draw_buf_t *draw_rocket(float flame) {
  lv_draw_buf_t *db = lv_draw_buf_create(IW, IH, LV_COLOR_FORMAT_ARGB8888, 0);
  if (db == nullptr)
    return nullptr;
  const uint32_t stride = db->header.stride;
  for (int py = 0; py < IH; py++) {
    uint8_t *row = (uint8_t *) db->data + (size_t) py * stride;
    for (int px = 0; px < IW; px++) {
      uint32_t r = 0, g = 0, b = 0, a = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          const float x = (px + (sx + 0.5f) / 4 - PX) / S, y = (py + (sy + 0.5f) / 4 - PY) / S;
          const uint32_t c = sample(x, y, flame);
          if (c) {
            r += (c >> 16) & 255;
            g += (c >> 8) & 255;
            b += c & 255;
            a += 255;
          }
        }
      // ARGB8888 in memory: B, G, R, A; colour un-premultiplied over the covered samples
      const uint32_t n = a / 255;
      row[px * 4 + 0] = n ? b / n : 0;
      row[px * 4 + 1] = n ? g / n : 0;
      row[px * 4 + 2] = n ? r / n : 0;
      row[px * 4 + 3] = a / 16;
    }
  }
  return db;
}

// ---- the path (u = 0 at the pad, 1 off the upper right)
inline void path(float u, float &x, float &y, float &deg) {
  x = PAD_X + 380.0f * std::pow(u, 1.6f);
  y = PAD_Y - 470.0f * u + 110.0f * u * u;
  const float dx = 380.0f * 1.6f * std::pow(std::max(u, 1e-3f), 0.6f), dy = -470.0f + 220.0f * u;
  deg = std::atan2(dx, -dy) * 180.0f / (float) M_PI;  // clockwise from straight up
}

inline lv_obj_t *blob(uint32_t col) {
  lv_obj_t *o = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(o);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  return o;
}

inline void stop() {
  if (st.anim) {
    lv_timer_delete(st.anim);
    st.anim = nullptr;
  }
  lv_obj_t **all[] = {&st.catcher, &st.img, &st.banner};
  for (auto **o : all)
    if (*o) {
      lv_obj_delete(*o);
      *o = nullptr;
    }
  for (auto &o : st.puff)
    if (o) {
      lv_obj_delete(o);
      o = nullptr;
    }
  for (auto &o : st.pad)
    if (o) {
      lv_obj_delete(o);
      o = nullptr;
    }
  st.label = nullptr;
}
inline bool playing() { return st.anim != nullptr; }

inline void frame() {
  const float ts = (now_ms() - st.t0) / 1000.0f, u = ts / DUR_S;
  if (u > 1.25f) {  // the smoke has faded out
    stop();
    return;
  }
  float x, y, deg;
  path(std::min(u, 1.1f), x, y, deg);
  // rocket: pivot on the body centre, flame flickers between the two drawings
  const int fl = ((now_ms() / 80) & 1);
  if (st.img_buf[fl])
    lv_image_set_src(st.img, st.img_buf[fl]);
  lv_image_set_rotation(st.img, (int32_t) (deg * 10.0f));
  lv_obj_set_pos(st.img, (int32_t) std::lround(x) - PX, (int32_t) std::lround(y) - PY);
  // smoke: a new puff at the tail every DUR/PUFFS while climbing
  if (u <= 1.0f && (st.last_emit < 0 || ts - st.last_emit >= DUR_S / PUFFS)) {
    st.last_emit = ts;
    const float r = (float) (deg * M_PI / 180.0);
    const int i = st.puff_next++ % PUFFS;
    st.puff_x[i] = x - std::sin(r) * 1.1f * S;  // behind the nozzle
    st.puff_y[i] = y + std::cos(r) * 1.1f * S;
    st.puff_t[i] = ts;
  }
  for (int i = 0; i < PUFFS && i < st.puff_next; i++) {
    const float age = (ts - st.puff_t[i]) / DUR_S;
    const int a = (int) (150 - 260 * age);
    if (a <= 0) {
      lv_obj_add_flag(st.puff[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const int r = (int) (6 + 24 * age);
    lv_obj_set_size(st.puff[i], 2 * r, 2 * r);
    lv_obj_set_pos(st.puff[i], (int32_t) st.puff_x[i] - r, (int32_t) st.puff_y[i] - r);
    lv_obj_set_style_bg_opa(st.puff[i], (lv_opa_t) a, 0);
    lv_obj_remove_flag(st.puff[i], LV_OBJ_FLAG_HIDDEN);
  }
  // the cloud at the pad, spreading for the first moment
  for (int k = 0; k < PADS; k++) {
    if (u > 0.35f) {
      lv_obj_add_flag(st.pad[k], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const float g = std::min(u, 0.15f) / 0.15f;
    const int w = (int) (40 + (90 + 50 * k) * g), h = (int) (16 + (14 + 6 * k) * g);
    const int a = (int) (120 * (1.0f - std::max(0.0f, u - 0.15f) / 0.2f));
    lv_obj_set_size(st.pad[k], w, h);
    lv_obj_set_pos(st.pad[k], (int32_t) PAD_X - w / 2, (int32_t) PAD_Y + 6 - h / 2 - 4 * k);
    lv_obj_set_style_bg_opa(st.pad[k], (lv_opa_t) std::max(0, a), 0);
    lv_obj_remove_flag(st.pad[k], LV_OBJ_FLAG_HIDDEN);
  }
}

// "Falcon 9 | Starlink Group 12-5" -> "Falcon 9 · Starlink Group 12-5"
inline void banner_text(const char *name, char *b, size_t n) {
  size_t k = 0;
  for (const char *p = name; *p && k + 3 < n; p++) {
    if (*p == '|') {
      b[k++] = (char) 0xC2;
      b[k++] = (char) 0xB7;
    } else {
      b[k++] = *p;
    }
  }
  b[k] = 0;
}

inline void play(const char *name) {
  stop();
  if (!st.img_buf[0])
    return;
  // a transparent full-screen layer: any tap skips
  st.catcher = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(st.catcher);
  lv_obj_set_size(st.catcher, 480, 480);
  lv_obj_add_flag(st.catcher, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(st.catcher, [](lv_event_t *) { stop(); }, LV_EVENT_CLICKED, nullptr);
  for (auto &o : st.pad)
    o = blob(0xDCDCE1);
  for (auto &o : st.puff)
    o = blob(0xC8CDD7);
  st.img = lv_image_create(lv_layer_top());
  lv_image_set_src(st.img, st.img_buf[0]);
  lv_image_set_pivot(st.img, PX, PY);
  lv_obj_remove_flag(st.img, LV_OBJ_FLAG_CLICKABLE);
  // the banner: the mission, centred both ways in its box
  st.banner = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(st.banner);
  lv_obj_set_size(st.banner, 360, 46);
  lv_obj_set_pos(st.banner, 60, 424);
  lv_obj_set_style_radius(st.banner, 10, 0);
  lv_obj_set_style_bg_color(st.banner, lv_color_hex(0x0E1836), 0);
  lv_obj_set_style_bg_opa(st.banner, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(st.banner, lv_color_hex(0x2D5BD0), 0);
  lv_obj_set_style_border_width(st.banner, 2, 0);
  lv_obj_remove_flag(st.banner, LV_OBJ_FLAG_CLICKABLE);
  st.label = lv_label_create(st.banner);
  if (st.font)
    lv_obj_set_style_text_font(st.label, st.font, 0);
  lv_obj_set_style_text_color(st.label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_width(st.label, 336);
  lv_label_set_long_mode(st.label, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(st.label, LV_TEXT_ALIGN_CENTER, 0);
  char b[96];
  banner_text(name && *name ? name : "Rocket launch", b, sizeof(b));
  lv_label_set_text(st.label, b);
  // the label box is ascent + descent tall; caps and digits sit in the top part of it, so
  // centring the box leaves the text high. Drop it by a third of the descent.
  const int32_t drop = st.font ? std::max<int32_t>(1, st.font->base_line / 3) : 1;
  lv_obj_align(st.label, LV_ALIGN_CENTER, 0, drop);
  st.t0 = now_ms();
  st.puff_next = 0;
  st.last_emit = -1;
  st.anim = lv_timer_create([](lv_timer_t *) { frame(); }, 33, nullptr);
  frame();
  ESP_LOGI("rocket", "launch animation: %s", b);
}

// every second: a launch at T-0 that has not been shown
inline void check() {
  if (!st.enabled || playing() || !clock_valid())
    return;
  if (lv_obj_get_child_count(lv_layer_top()) > 0)  // a prompt is open (UI-66/68): wait
    return;
  const double t = clock_now();
  for (const auto &l : live.launches) {
    if (!launch_pending(l) || l.net > t || t - l.net > LATE_S)
      continue;
    const uint32_t k = key_of(l.name, l.net);
    if (k == st.played_key)
      continue;
    st.played_key = k;
    play(l.name);
    return;
  }
}

void init(const lv_font_t *banner) {
  st.font = banner;
  st.img_buf[0] = draw_rocket(1.0f);
  st.img_buf[1] = draw_rocket(1.25f);
  lv_timer_create([](lv_timer_t *) { check(); }, 1000, nullptr);
}
void set_enabled(bool on) {
  st.enabled = on;
  if (!on)
    stop();
}
void play_now() {
  const double t = clock_valid() ? clock_now() : 0;
  for (const auto &l : live.launches)
    if (launch_pending(l) && l.net > t - LATE_S) {
      play(l.name);
      return;
    }
  play("Rocket launch");
}

}  // namespace rocket
}  // namespace sat
#endif  // SKY_IMPL
