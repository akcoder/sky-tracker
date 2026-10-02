#pragma once
// UI-69 launch animation. When a rocket launch (UI-54, Launch Library) reaches T-0, a cartoon
// rocket lifts off from the lower left of the screen with a puff of smoke at the pad, arcs up
// across the screen leaving a fading smoke trail and leaves past the upper right, about 4.5 s.
// A banner at the bottom names the mission. Once per launch, a tap anywhere skips it. Off with
// the Launch Animation switch; the Play Launch Animation button shows it on demand.
// Everything sits on lv_layer_top and is deleted when it ends. The rocket is drawn once at
// init into FLAMES small ARGB8888 buffers (flames of different length, width and lean, swapped
// in an irregular order for the flicker, UI-69c) and shown
// with an lv_image rotated along the path; the smoke is a ring of round lv_objs.
// Compiled in sky_extra.cpp (SKY_IMPL), as sky_about.h.
#include <cstdint>

#ifndef SKY_IMPL
namespace sat {
namespace rocket {
void init(const lv_font_t *banner);
void set_enabled(bool on);
void play_now();  // the button: the next launch's name (or a generic one)
bool active();    // playing now (sat::tick pauses, UI-69b)
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
constexpr int PUFFS = 32, PADS = 6;
// UI-69a: smoke is drawn from soft round images made at init (radius PR_MIN + i * PR_STEP), so a
// puff is a plain image blend instead of an anti-aliased rounded-rectangle mask
constexpr int PR_MIN = 4, PR_STEP = 3, PR_N = 12;
constexpr int FLAMES = 5;
// UI-69c: flame shapes (length and width scale, sideways lean of the tip, in body widths)
struct Flame {
  float len, wid, lean;
};
constexpr Flame FLAME[FLAMES] = {
    {1.00f, 1.00f, 0.00f}, {1.30f, 0.92f, 0.05f}, {0.80f, 1.10f, -0.03f}, {1.18f, 1.04f, -0.06f}, {0.92f, 0.96f, 0.04f},
};
// the order they show in, one every FLAME_MS: irregular so it reads as flicker, not a blink
constexpr uint8_t FLAME_SEQ[] = {0, 1, 3, 2, 4, 1, 0, 3, 2, 1, 4, 3, 0, 2, 1, 4};
constexpr uint32_t FLAME_MS = 45;
constexpr float AGE_STEP = 0.04f;  // puff growth/fade step, in fractions of DUR_S (0.18 s)
constexpr float PAD_X = 140, PAD_Y = 400;
// ground cloud billows: final offset from the pad (dx, dy), radius, peak alpha, colour
struct PadBillow {
  float dx, dy, r;
  int a;
  uint32_t col;
};
constexpr PadBillow PAD_BILLOWS[] = {
    {0, -6, 22, 170, 0xF2D9BE},   {-34, 0, 19, 155, 0xE6D6C8}, {34, 0, 19, 155, 0xE6D6C8},
    {-68, 4, 15, 135, 0xD2D4DC},  {68, 4, 15, 135, 0xD2D4DC},  {0, -26, 15, 120, 0xE8DCD0},
};
constexpr float PAD_STEP = 0.025f, PAD_PHASE = 0.004f, PAD_END = 0.7f;
constexpr double LATE_S = 120;       // a launch is "now" for 2 min after T-0 (refresh lag)

struct St {
  bool enabled = true;
  const lv_font_t *font = nullptr;
  lv_draw_buf_t *img_buf[FLAMES] = {};
  lv_draw_buf_t *smoke[PR_N] = {};
  int flame_i = 0;
  uint32_t flame_at = 0;
  lv_obj_t *catcher = nullptr, *img = nullptr, *banner = nullptr, *label = nullptr;
  lv_obj_t *puff[PUFFS] = {}, *pad[PADS] = {};
  float puff_x[PUFFS] = {}, puff_y[PUFFS] = {}, puff_t[PUFFS] = {};
  int8_t puff_step[PUFFS] = {};  // the age step last drawn (-1 hidden): a puff only changes on a new step
  int8_t pad_step[PADS] = {};
  int puff_next = 0;
  float last_emit = -1;
  uint32_t t0 = 0;
  lv_timer_t *anim = nullptr;  // fallback driver; frames normally come from the display refresh
  uint32_t last_frame = 0;
  uint32_t frames = 0, max_gap = 0, last_refr = 0;  // UI-69a: logged when it ends
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
inline uint32_t sample(float x, float y, const Flame &f) {
  const Pt nose[3] = {{-0.32f, -0.5f}, {0, -1.25f}, {0.32f, -0.5f}};
  const Pt body[4] = {{-0.32f, -0.5f}, {0.32f, -0.5f}, {0.32f, 0.95f}, {-0.32f, 0.95f}};
  const Pt finl[3] = {{-0.32f, 0.35f}, {-0.62f, 1.0f}, {-0.32f, 0.85f}};
  const Pt finr[3] = {{0.32f, 0.35f}, {0.62f, 1.0f}, {0.32f, 0.85f}};
  const float w = f.wid;
  const Pt fo[3] = {{-0.28f * w, 0.95f}, {f.lean, 0.95f + 1.1f * f.len}, {0.28f * w, 0.95f}};
  const Pt fi[3] = {{-0.15f * w, 0.95f}, {f.lean * 0.6f, 0.95f + 0.6f * f.len}, {0.15f * w, 0.95f}};
  const Pt fc[3] = {{-0.07f * w, 0.95f}, {f.lean * 0.3f, 0.95f + 0.28f * f.len}, {0.07f * w, 0.95f}};
  const float wr = 0.16f, wd = std::sqrt(x * x + (y + 0.1f) * (y + 0.1f));
  if (wd <= wr)
    return wd > wr - 0.05f ? 0xFF283250 : 0xFF3C78DC;  // window, dark rim
  if (in_poly(nose, 3, x, y))
    return 0xFFD63C3C;
  if (in_poly(body, 4, x, y))
    return 0xFFEBEEF5;
  if (in_poly(finl, 3, x, y) || in_poly(finr, 3, x, y))
    return 0xFFD63C3C;
  if (in_poly(fc, 3, x, y))
    return 0xFFFFFDF0;  // white-hot core
  if (in_poly(fi, 3, x, y))
    return 0xFFFFEB78;
  if (in_poly(fo, 3, x, y))
    return 0xFFFF8C1E;
  return 0;
}
inline lv_draw_buf_t *draw_rocket(const Flame &flame) {
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

// a soft round puff: opaque core fading to nothing at the rim (smoothstep over the outer half)
inline lv_draw_buf_t *draw_smoke(int r) {
  const int d = 2 * r;
  lv_draw_buf_t *db = lv_draw_buf_create(d, d, LV_COLOR_FORMAT_ARGB8888, 0);
  if (db == nullptr)
    return nullptr;
  for (int py = 0; py < d; py++) {
    uint8_t *row = (uint8_t *) db->data + (size_t) py * db->header.stride;
    for (int px = 0; px < d; px++) {
      const float dx = px + 0.5f - r, dy = py + 0.5f - r;
      float t = (r - std::sqrt(dx * dx + dy * dy)) / (0.5f * r);
      t = std::min(1.0f, std::max(0.0f, t));
      row[px * 4 + 0] = 0xD7;  // B, G, R of 0xC8CDD7
      row[px * 4 + 1] = 0xCD;
      row[px * 4 + 2] = 0xC8;
      row[px * 4 + 3] = (uint8_t) (255.0f * t * t * (3 - 2 * t));
    }
  }
  return db;
}
// the smoke image nearest radius r; returns the radius it has
inline int smoke_pick(int r, lv_draw_buf_t *&img) {
  const int i = std::min(PR_N - 1, std::max(0, (r - PR_MIN + PR_STEP / 2) / PR_STEP));
  img = st.smoke[i];
  return PR_MIN + i * PR_STEP;
}
inline lv_obj_t *blob(uint32_t warm) {
  lv_obj_t *o = lv_image_create(lv_layer_top());
  if (warm) {  // the billows nearest the flame: tinted
    lv_obj_set_style_image_recolor(o, lv_color_hex(warm), 0);
    lv_obj_set_style_image_recolor_opa(o, LV_OPA_20, 0);
  }
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  return o;
}
inline void smoke_at(lv_obj_t *o, int x, int y, int r, int opa) {
  lv_draw_buf_t *img = nullptr;
  r = smoke_pick(r, img);
  if (img == nullptr)
    return;
  lv_image_set_src(o, img);
  lv_obj_set_pos(o, x - r, y - r);
  lv_obj_set_style_image_opa(o, (lv_opa_t) std::min(255, std::max(0, opa)), 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

inline void on_refr(lv_event_t *);
inline void stop() {
  if (st.anim && st.frames > 1)
    ESP_LOGI("rocket", "animation: %u frames in %u ms (%.1f fps), longest gap %u ms", (unsigned) st.frames,
             (unsigned) (st.last_refr - st.t0), st.frames * 1000.0f / std::max<uint32_t>(1, st.last_refr - st.t0),
             (unsigned) st.max_gap);
  if (st.anim) {
    lv_timer_delete(st.anim);
    st.anim = nullptr;
    if (lv_display_t *d = lv_display_get_default())
      lv_display_remove_event_cb_with_user_data(d, on_refr, nullptr);
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
bool active() { return playing(); }

inline void frame() {
  st.last_frame = now_ms();
  const float ts = (now_ms() - st.t0) / 1000.0f, u = ts / DUR_S;
  if (u > 1.25f) {  // the smoke has faded out
    stop();
    return;
  }
  float x, y, deg;
  path(std::min(u, 1.1f), x, y, deg);
  // rocket: pivot on the body centre; the flame steps through FLAME_SEQ (UI-69c)
  if (now_ms() - st.flame_at >= FLAME_MS) {
    st.flame_at = now_ms();
    st.flame_i = (st.flame_i + 1) % (int) sizeof(FLAME_SEQ);
    if (lv_draw_buf_t *b = st.img_buf[FLAME_SEQ[st.flame_i]])
      lv_image_set_src(st.img, b);
  }
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
    st.puff_step[i] = -1;
  }
  // A puff grows and fades in steps of AGE_STEP rather than every frame: each changed object
  // makes LVGL redraw the sky under it, and past 32 dirty areas a frame redraws the whole
  // screen. Puffs are born at different times, so only a few step on any one frame.
  for (int i = 0; i < PUFFS && i < st.puff_next; i++) {
    const int step = (int) ((ts - st.puff_t[i]) / DUR_S / AGE_STEP);
    if (step == st.puff_step[i])
      continue;
    st.puff_step[i] = (int8_t) std::min(step, 127);
    const float age = step * AGE_STEP;
    const int a = (int) (150 - 260 * age);
    if (a <= 0) {
      lv_obj_add_flag(st.puff[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    smoke_at(st.puff[i], (int) st.puff_x[i], (int) st.puff_y[i], (int) (10 + 28 * age), a + 40);
  }
  // The ground cloud: round billows that roll out sideways from the pad along the ground
  // (exhaust deflected by the flame trench), the inner ones bigger and lit warm by the flame,
  // then hang and thin out. Each billow steps on its own phase so they don't all change on
  // the same frame.
  for (int k = 0; k < PADS; k++) {
    const float uk = u - k * PAD_PHASE;
    const int ps = uk < 0 ? -1 : uk > PAD_END ? 99 : (int) (uk / PAD_STEP);
    if (ps == st.pad_step[k])
      continue;
    st.pad_step[k] = (int8_t) ps;
    if (ps < 0 || ps == 99) {
      lv_obj_add_flag(st.pad[k], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const PadBillow &b = PAD_BILLOWS[k];
    const float uq = ps * PAD_STEP + k * PAD_PHASE;
    float g = std::min(uq / 0.22f, 1.0f);
    g = 1.0f - (1.0f - g) * (1.0f - g);  // ease out: fast burst, then slow drift
    const float drift = std::max(0.0f, uq - 0.22f) * 40.0f;  // keeps rolling outward a little
    const int r = (int) (b.r * (0.35f + 0.65f * g) + drift * 0.3f);
    const int x = (int) (PAD_X + b.dx * g + (b.dx > 0 ? drift : b.dx < 0 ? -drift : 0));
    const int y = (int) (PAD_Y + 8 + b.dy * g - drift * 0.15f);
    const int a = (int) (b.a * (1.0f - std::max(0.0f, uq - 0.25f) / (PAD_END - 0.25f)));
    smoke_at(st.pad[k], x, y, (int) (r * 1.2f), a + 50);
  }
}

inline void on_refr(lv_event_t *) {
  if (!playing())
    return;
  const uint32_t t = now_ms();
  if (st.frames++ > 0)
    st.max_gap = std::max(st.max_gap, t - st.last_refr);
  st.last_refr = t;
  frame();
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
  for (int k = 0; k < PADS; k++)
    st.pad[k] = blob(k < 3 ? 0xFFB070 : 0);
  for (auto &o : st.puff)
    o = blob(0);
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
  st.frames = st.max_gap = 0;
  st.last_emit = -1;
  for (auto &p : st.pad_step)
    p = -1;
  // Move everything at the start of each display refresh, from the clock at that moment, so
  // every frame drawn shows a fresh position (a separate 33 ms timer beat against the 16 ms
  // refresh and dropped or doubled steps). The objects it moves re-arm the next refresh. The
  // timer only covers a stretch where nothing moved and no refresh came.
  if (lv_display_t *d = lv_display_get_default())
    lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_START, nullptr);
  st.anim = lv_timer_create(
      [](lv_timer_t *) {
        if (now_ms() - st.last_frame >= 40)
          frame();
      },
      40, nullptr);
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
  for (int i = 0; i < FLAMES; i++)
    st.img_buf[i] = draw_rocket(FLAME[i]);
  for (int i = 0; i < PR_N; i++)
    st.smoke[i] = draw_smoke(PR_MIN + i * PR_STEP);
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
