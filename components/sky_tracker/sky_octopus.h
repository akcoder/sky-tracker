#pragma once
// EASTER EGG (the first rule of fight club applies): never exposed in the on-screen UI (no on-screen
// button, setting or text; it only turns up on the map by itself). Its switch and Play button are web UI /
// Home Assistant only.
// Never discuss it outside this file, its web UI entities and its requirement in docs/requirements.md: no README,
// release notes, changelog or marketing.
// UI-85 the octopus: a mimic octopus visits the map two or three times an hour, about 10 s. It jets in from
// the right, says something, morphs into ONE thing picked at random for the visit (a lionfish, a banded sea
// snake or the ISS, with a line to match), squirts ink and jets off to the left. The words are drawn at random
// from a pool for each beat. A tap ends it. Off with the Octopus Visits switch; the Play Octopus Animation
// button shows it on demand.
//
// UI-85d: a third of the visits it is a meal instead: it hovers over a constellation on the map that is up and
// is something it eats (Cancer: crab; Pisces, Piscis Austrinus, Volans, Dorado: fish), says it likes it, eats the
// figure (gone from the map for the rest of the visit) and says it was good. About 1 visit in 100 the meal is Taurus.
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
bool play_critter(int who);  // the debug command: 1 UFO, 2 cat, 3 axolotl, 4 Audrey
bool play_eat(int meal);  // the debug command: a meal (0 crab, 1 fish, 2 bull) if one is up on the map
bool play_dino();  // the web button: a dinosaur walk as a random visit has it (hunting, three in four, when something is up)
bool playing();
const char *last_log();  // what the last visit logged (the API's log stream starves while a visit runs)
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
constexpr uint32_t MODEL_MS_SLOW = 50;  // the model steps at up to ~14 fps (a step is ~45 ms to paint and ~30 ms to refresh)
inline int critter_id();
inline uint32_t model_ms() { return critter_id() >= 3 ? 25u : MODEL_MS_SLOW; }  // (the axolotl, CR_AXO, paints in ~17 ms and refreshes in ~20: a step every 25 ms at most)
constexpr uint32_t C_BODY = 0x80583C, C_BAND = 0xEEE0C0, C_DARK = 0x3C2820, C_ISS_BODY = 0x3E5CA0, C_ISS_BAND = 0xDECC82,
                   C_LION = 0xA04E34, C_SNAKE = 0x2C2630, C_SNAKE_BAND = 0xF4F0E8;

// ---- the words: one line a beat, drawn at random per visit
enum Kind { K_LION = 0, K_SNAKE = 1, K_ISS = 2, K_MEAL = 3 };
inline const char *const SAY_ARRIVE[] = {
    "Mimic octopus.\nIt's a lifestyle.", "Three hearts.\nNine brains.", "Blue blood.\nVery fancy.", "I can be anything.\nWatch.",
    "Just passing\nthrough.", "Don't mind me.", "Eight arms.\nZero bones.", "I'm not an\nalien. Probably.", "Oh, hello.\nNice sky.",
    "Squeezed through\na tiny gap.", "Is this the\nright ocean?", "Nice stars.\nI'll take them.", "Octopus.\nNot a squid.", "Hold my\ntentacles.",
    "Excuse me,\ncoming through.", "I live in\nthe sky now.", "Guess who's\nback?", "No bones about it.", "Ink supply:\nfull.",
    "Mind the arms.", "Breaking and\nentering.", "Escaped the\naquarium.", "Spotted me?\nBad camouflage.", "Greetings,\nland dwellers."};
inline const char *const SAY_LION[] = {"Lionfish.\nBe afraid.", "I'm a lionfish.\nVenomous.", "Lionfish.\nDon't touch.", "Spiky.\nSo spiky.",
                                       "Fancy fins.\nNasty sting.", "Lionfish mode.", "Look, don't touch."};
inline const char *const SAY_SNAKE[] = {"Sea snake.\nSsss.", "Banded sea snake.\nVery toxic.", "Sea snake.\nRespect.", "Ssssnake.", "Stripes.\nVenom.\nHi.",
                                        "Slithering\nthrough space."};
inline const char *const SAY_ISS[] = {"ISS.\nObviously.", "Space station\nmode.", "Look up! The\nISS!", "Orbiting.\nPlease hold.", "Solar panels\nout.",
                                      "Houston?\nIt's me.", "Sixteen sunrises\na day."};
inline const char *const SAY_INK[] = {"Ink!", "Oops.", "Bye!", "Poof.", "Smoke bomb!", "Ta-ta!", "See ya!", "Ink and\nrun.", "Splat."};
// UI-85d meals: the line before (it likes it) and the line after (it was good), by what it eats
enum Meal { M_NONE = -1, M_CRAB = 0, M_FISH = 1, M_BULL = 2 };
inline const char *const MEAL_BEFORE[3][6] = {
    {"Ooh, a crab!\nI love crab.", "Crab!\nMy favorite.", "Is that a crab?\nYum.", "Crab dinner!", "Cancer?\nI'd say crab.", "Crab. Finally."},
    {"Fish!\nMy favorite.", "Ooh, fish!\nDinner.", "Is that a fish?\nYes please.", "Sushi night!", "Fishies!", "I do love\na fish."},
    {"A bull?\nOkay, why not.", "Surf and\nturf!", "Taurus.\nBull on the menu.", "Big snack.", "", ""}};
inline const char *const MEAL_AFTER[3][6] = {
    {"Crab.\nPerfectly crunchy.", "Delicious\ncrab!", "Crab.\nAlways crab.", "Mmm.\nCrab.", "Eight arms,\nno regrets.", "Needs butter."},
    {"Delicious\nfish.", "Fish.\nVery fresh.", "Mmm.\nSeafood.", "Sashimi.", "No bones.\nPerfect.", "Catch of\nthe day."},
    {"All bull.\nNo shit.", "", "", "", "", ""}};
constexpr int N_BEATS = 3;  // arrival, the morph, the ink
constexpr float BEAT_A[N_BEATS] = {2.2f, 5.5f, 8.4f}, BEAT_B[N_BEATS] = {4.5f, 7.4f, 9.4f};
constexpr float EXIT_T = 8.4f, EAT_T = 6.1f;  // EAT_T: the gulp (a meal: the figure has been pulled into the beak)
constexpr float REACH0 = 3.9f, REACH1 = 4.8f, PULL0 = 4.9f;  // a meal: the arms reach the figure, grab it, pull it in
constexpr float MEAL_A[3] = {2.2f, 6.3f, 8.4f}, MEAL_B[3] = {4.4f, 8.1f, 9.4f};  // a meal's lines: before, after, the ink  // the ink, then off to the left

// ---- the timeline (pure: the host test checks it, the painter and the box use it)
inline float ease(float x) {
  x = std::min(1.0f, std::max(0.0f, x));
  return x * x * (3 - 2 * x);
}
inline float seg(float t, float a, float b) { return ease((t - a) / (b - a)); }
// ---- UI-85d a meal: the constellation's lines, on the screen, which the arms take hold of and pull into the beak
struct FigSeg {
  float x0, y0, x1, y1;
  bool major;
};
struct Fig {
  FigSeg s[48];
  int n = 0;
  float px[6], py[6];  // the points the arms hold (screen)
  int arm[6];          // ...and the arm that holds each
  int n_anch = 0;
  float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
};
inline Fig fig;
// where a point of the figure is at time t: lifted off the map and grown (held), then drawn in to the beak (mx, my)
inline void fig_pt(float t, float x, float y, float mx, float my, float &ox, float &oy) {
  const float held = seg(t, REACH0, PULL0), k = seg(t, PULL0, EAT_T), gx = (fig.bx0 + fig.bx1) / 2, gy = (fig.by0 + fig.by1) / 2, sc = 1 + 1.2f * held;
  const float x1 = gx + (x - gx) * sc, y1 = gy + (y - gy) * sc;
  ox = x1 + (mx - x1) * k;
  oy = y1 + (my - y1) * k;
}
inline float hover_x = 260, hover_y = 262;  // where it hovers (a meal: over the constellation)
inline bool is_meal = false;
// which way it comes in and goes out (-1, 0, 1 on each axis: a side, the top or bottom, or a corner), chosen per visit
inline int in_dx = 1, in_dy = 0, out_dx = -1, out_dy = 0;
inline float edge_x(int d, float mid) { return d < 0 ? -220.0f : d > 0 ? 700.0f : mid; }  // far enough off for the arms
inline float edge_y(int d, float mid) { return d < 0 ? -210.0f : d > 0 ? 650.0f : mid; }
inline void pos(float t, float &x, float &y) {
  if (t < 1.8f) {  // in from beyond the edge, jetting: fast, then slowing as it arrives (the dinosaur's entrance)
    const float u = 1 - t / 1.8f, k = 1 - u * u * u;
    const float sx = edge_x(in_dx, hover_x), sy = edge_y(in_dy, hover_y);
    x = sx + (hover_x - sx) * k;
    y = sy + (hover_y - sy) * k + 6 * std::sin(t * 5);
  } else if (t < EXIT_T) {  // hovers
    x = hover_x + 10 * std::sin((t - 2.2f) * 0.9f);
    y = hover_y + 9 * std::sin((t - 2.2f) * 1.7f);
  } else {  // jets off
    const float k = std::pow(seg(t, EXIT_T, EXIT_T + 1.5f), 1.3f);
    x = hover_x + (edge_x(out_dx, hover_x) - hover_x) * k;
    y = hover_y + (edge_y(out_dy, hover_y - 40) - hover_y) * k;
  }
}
inline float vel(float t) {
  float x0, y0, x1, y1;
  pos(t - 0.05f, x0, y0);
  pos(t + 0.05f, x1, y1);
  return (x1 - x0) / 0.1f;
}
inline float vel_y(float t) {
  float x0, y0, x1, y1;
  pos(t - 0.05f, x0, y0);
  pos(t + 0.05f, x1, y1);
  return (y1 - y0) / 0.1f;
}
inline float jet(float t) {
  if (t < 1.8f)
    return std::pow(std::max(0.0f, std::sin(2 * (float) M_PI * 1.5f * t)), 2.0f);
  if (t > EXIT_T)
    return std::pow(std::max(0.0f, std::sin(2 * (float) M_PI * 2.6f * (t - EXIT_T))), 2.0f);
  if (is_meal && t > EAT_T - 0.3f && t < EAT_T + 0.5f)  // the gulp: it squeezes, once
    return std::pow(std::sin((t - (EAT_T - 0.3f)) / 0.8f * (float) M_PI), 2.0f);
  return 0.25f * (0.5f + 0.5f * std::sin(t * 2.2f));
}
inline float w_morph(float t) { return seg(t, 4.8f, 5.3f) * (1 - seg(t, 7.6f, 8.1f)); }  // 0 octopus .. 1 the thing
inline float cat_beat(int i, bool end);  // (the cat's lines: before the big shot, and on leaving)
inline bool cat_on();
inline float beat_a(int i) { return cat_on() ? cat_beat(i, false) : is_meal ? MEAL_A[i] : BEAT_A[i]; }
inline float beat_b(int i) { return cat_on() ? cat_beat(i, true) : is_meal ? MEAL_B[i] : BEAT_B[i]; }
inline int beat_at(float t) {
  for (int i = 0; i < N_BEATS; i++)
    if (t >= beat_a(i) && t <= beat_b(i))
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
  int kind = K_LION;  // what it morphs into this visit (K_MEAL: nothing: it eats)
  int meal = M_NONE, eat_name = -1;  // UI-85d: what it eats; the constellation (sky::NAMES index)
  bool ate = false;
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
struct Pb {  // a second of the visit: frames, what painting cost, the longest gap between frames (PERF: where the lag is)
  uint16_t n = 0;
  uint32_t sum = 0, mx = 0, gap = 0;
};
inline Pb pb[20];
inline Pb &pb_at(float t) { return pb[std::max(0, std::min(19, (int) t))]; }
inline char vlog[700];  // the last visit's events: read back by the octolog debug command
inline void vlogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
inline void vlogf(const char *fmt, ...) {
  const size_t n = strlen(vlog);
  if (n + 4 >= sizeof(vlog))
    return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(vlog + n, sizeof(vlog) - n - 1, fmt, ap);
  va_end(ap);
  strncat(vlog, "\n", sizeof(vlog) - strlen(vlog) - 1);
}
inline uint32_t min_idle_ms = 20000;  // no touch for this long before a visit starts

inline uint32_t rnd() {
  st.rng ^= st.rng << 13, st.rng ^= st.rng >> 17, st.rng ^= st.rng << 5;
  return st.rng;
}
inline float rnd01() { return (rnd() % 10000) / 10000.0f; }
// A line from a pool that is not the one said last (per pool: `last`), so a line does not repeat in a row.
inline const char *pick_line(const char *const *a, uint32_t n, int &last) {
  uint32_t i = rnd() % n;
  if (n > 1 && (int) i == last)
    i = (i + 1 + rnd() % (n - 1)) % n;
  last = (int) i;
  return a[i];
}
inline int last_arrive = -1, last_lion = -1, last_snake = -1, last_iss = -1, last_ink = -1, last_before[3] = {-1, -1, -1}, last_after[3] = {-1, -1, -1};

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
  // moving down the arms are swept back up and out (fanned); moving up they stream straight down behind it (drawn in a little)
  const float vy = std::max(-1.2f, std::min(1.2f, vel_y(t) / 260.0f)), fan = vy > 0 ? vy * 1.0f : vy * 0.5f;
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
      phi_n += (k > 3 ? 1.0f : -1.0f) * fan * u * (0.4f + u);
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
  if (is_meal && fig.n_anch > 0) {  // UI-85d: arms reach down to the figure's points, hold them and pull them in to the beak
    const float wr = seg(t, REACH0, REACH1) * (1 - seg(t, EAT_T, EAT_T + 0.5f));
    if (wr > 0) {
      const float mx = m.cx, my = m.cy + 6 * SC;
      for (int j = 0; j < fig.n_anch; j++) {
        float tx, ty;
        fig_pt(t, fig.px[j], fig.py[j], mx, my, tx, ty);
        const int a = fig.arm[j];
        const float bx = m.ax[a][0], by = m.ay[a][0], dx = tx - bx, dy = ty - by, len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
        for (int i = 1; i <= N; i++) {
          const float u = (float) i / N, wob = std::sin((float) M_PI * u) * 7.0f * std::sin(t * 7 + a);
          const float sx = bx + dx * u - dy / len * wob, sy = by + dy * u + dx / len * wob;
          m.ax[a][i] += (sx - m.ax[a][i]) * wr;
          m.ay[a][i] += (sy - m.ay[a][i]) * wr;
        }
      }
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
// floor/ceil straight to an int: one FPU instruction on the S3 (std::floor/ceil are ~60-instruction library calls)
#ifdef __XTENSA__
inline int ifloor(float x) { int r; __asm__("floor.s %0, %1, 0" : "=a"(r) : "f"(x)); return r; }
inline int iceil(float x) { int r; __asm__("ceil.s %0, %1, 0" : "=a"(r) : "f"(x)); return r; }
#else
inline int ifloor(float x) { return (int) std::floor(x); }
inline int iceil(float x) { return (int) std::ceil(x); }
#endif
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
  const int x1 = iceil(xl), x2 = ifloor(xr);  // pixel centres at integers
  if (x1 > x2) {  // narrower than a pixel
    px_a(r, ifloor(xl), y, c, (uint32_t) ((xr - xl) * alpha));
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
  const int y0 = std::max(r.cy1, iceil(cy - ry)), y1 = std::min(r.cy2, ifloor(cy + ry));
  const float inv = 1.0f / ry;  // one division, not one a row
  for (int y = y0; y <= y1; y++) {
    const float d = (y - cy) * inv, q = 1 - d * d;
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
  float AR[8][N + 1];  // arm_r once per joint (it calls pow): both passes, both ends of a segment and the suckers share it
  for (int k = 0; k < 8; k++)
    for (int i = 0; i <= N; i++)
      AR[k][i] = arm_r(m, k, (float) i / N);
  // arms: all the outlines, then all the fills (neighbours do not outline each other); stamps along each segment
  for (int pass = 0; pass < 2; pass++)
    for (int k = 0; k < 8; k++)
      for (int i = 1; i <= N; i++) {
        const float xa = m.ax[k][i - 1], ya = m.ay[k][i - 1], xb = m.ax[k][i], yb = m.ay[k][i];
        const float ra = AR[k][i - 1], rb = AR[k][i];
        const float pad = std::max(ra, rb) + 4;
        if (std::max(xa, xb) + pad < r.cx1 || std::min(xa, xb) - pad > r.cx2 || std::max(ya, yb) + pad < r.cy1 ||
            std::min(ya, yb) - pad > r.cy2)
          continue;
        const uint16_t c = pass == 0 ? dark : (i % 2 ? body : band);
        // a stamp every ~r/2 px along the segment: two where the arm is thick, three or four toward the tip
        const int ns = rb >= 5.0f ? 2 : rb >= 3.0f ? 3 : 4;
        for (int s = 0; s < ns; s++) {
          const float f = (float) s / ns;
          disc(r, xa + (xb - xa) * f, ya + (yb - ya) * f, ra + (rb - ra) * f + (pass == 0 ? 1.8f : 0.0f), c);
        }
        if (i == N)
          disc(r, xb, yb, rb + (pass == 0 ? 1.8f : 0.0f), c);
      }
  for (int k = 0; k < 8; k++)  // suckers
    for (int i = 3; i < N; i += 2)
      disc(r, m.ax[k][i], m.ay[k][i] + 1.5f, AR[k][i] * 0.30f, sucker);
  // the mantle, with its bands
  ellipse(r, m.cx, m.my, m.rx + 1.8f, m.ry + 1.8f, dark);
  ellipse(r, m.cx, m.my, m.rx, m.ry, body);
  for (int j = 0; j < 5; j++) {
    const float yb = m.my - m.ry + (14 + j * 26) * SC;
    for (int y = iceil(yb - 3); y <= ifloor(yb + 3); y++) {
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
      for (int y = iceil(ey - 3.6f * SC); y <= ifloor(ey + 3.6f * SC); y++)
        row(r, y, ex + (m.look - 10) * SC, ex + (m.look + 10) * SC, slit);
    // the lid: the wedge of the eye between 205 and 335 degrees (above the middle, 2.14 wide for every 1 up)
    for (int y = iceil(ey - er); y < (int) ey; y++) {
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

// ---- the other critters (they share this file's timeline, backdrop, composite and rasteriser): a UFO that hovers and
// beams down, and a cat riding a comet across the map
enum { CR_OCTO = 0, CR_UFO = 1, CR_CAT = 2, CR_AXO = 3, CR_AUDREY = 4 };
inline int critter = CR_OCTO;
inline int critter_id() { return critter; }
// The cat's visit is a story (UI-90): it comes in on its comet and hovers, fires 2 to 4 laser shots, each vaporising a Sun,
// Moon, planet or satellite that is up on the map; moves out of the way, charges, and blows up the whole sky disc (flash,
// shock ring, debris, dark); two seconds later the disc fades back, with everything that was shot.
constexpr float CAT_K = 4.0f;  // CAT_K: the cat's size, in units of its 15 px body half-length
struct CatTgt {
  float x, y, r;     // on the screen
  lv_obj_t *o[2];    // the map's own objects to hide when it is gone (the picture, its label)
  bool gone;
  uint8_t kind = 0;  // 0 the Sun, 1 the Moon, 2 a planet, 3 the ISS, 4 another satellite
  int8_t pl = 0;     // which planet (kind 2)
};
inline CatTgt ctg[4];
inline int n_ctg = 0;
// The map round each target, as it is without the target (bare) and with it, taken when the visit starts: a shot that lands, or the disc
// coming back, copies a patch into the backdrop (no retake of the whole screen, which stopped the cat for 155 ms a time).
struct Patch {
  lv_area_t a;
  uint16_t *bare = nullptr, *with = nullptr;
};
inline Patch pt[4];
inline bool pt_ok = false;
inline void patches_free() {
  for (Patch &q : pt) {
    heap_caps_free(q.bare);
    heap_caps_free(q.with);
    q.bare = q.with = nullptr;
  }
  pt_ok = false;
}
struct CatPlan {
  float shot0 = 1.8f, dt = 0.9f, t_move = 0, t_charge = 0, t_fire = 0, t_hit = 0, t_leave = 0, t_rest = 0, t_end = 0;  // t_leave: the cat flies off; t_rest: the disc starts to come back
};
inline CatPlan cp;
// The axolotl's visit (UI-91): it swims in on a smooth path, stalks each of 2 to 4 small things on the map (satellites, the ISS, bright stars),
// lunges, opens wide and gulps it down (the thing is sucked into its mouth), puffs its cheeks and blows bubbles; when it has eaten them all
// they regrow on the map, with a sparkle, and it swims off.
constexpr float AXO_K = 1.6f, AXO_MOUTH = 49.0f;  // its size (px a unit); the mouth's tip, in units ahead of its middle
struct AxoSeg {
  float t0, t1, x0, y0, x1, y1, bow, f0, f1, l0, len;  // a smooth arc from (x0, y0) to (x1, y1); f: which way it faces (a change: it turns round); l0, len: path length
  bool burst;
};
struct AxoPlan {
  AxoSeg s[24];
  int n = 0;
  float pull[4] = {}, regrow[4] = {}, t_stalk = 0, t_regrow = 0, t_exit = 0, t_end = 0;  // (pull, regrow: by target)
  bool back[4] = {};
};
inline AxoPlan ap;
// Audrey's visit (UI-92): see audrey_plan below
constexpr float AUD_K = 1.7f, AUD_DT = 110.0f, AUD_RH = 38.0f;  // its size (px a unit); how far its tongue reaches (px)
struct AudreySeg {
  float t0, t1, hx0, hy0, hx1, hy1, tl0, tl1, f0, f1;  // the head's hinge moves from (hx0, hy0) to (hx1, hy1), tilting from tl0 to tl1; f: which way it faces (a change: it turns round)
};
struct AudreyPlan {
  AudreySeg s[14];
  int n = 0;
  float bx = 240, by = 412;  // the pot
  float t_open[4] = {}, t_out[4] = {}, pull[4] = {}, regrow[4] = {}, t_stalk = 0, t_regrow = 0, t_sink = 0, t_end = 0;  // (by target)
  bool back[4] = {};
};
inline AudreyPlan pl;
inline float disc_ox = 0, disc_oy = 0;  // where the sky disc's square sits on the screen (ui.cx, ui.cy and the stars are relative to it)
inline void disc_origin() {
  lv_area_t a;
  if (ui.w.sky) {
    lv_obj_get_coords(ui.w.sky, &a);
    disc_ox = (float) a.x1, disc_oy = (float) a.y1;
  } else
    disc_ox = disc_oy = 0;
}
inline float cat_x1 = 370, cat_y1 = 270, cat_x2 = 390, cat_y2 = 300;  // where it hovers to shoot, and where it goes to fire at the disc
inline bool cat_restored = false;
inline void cat_spots() {  // where it hovers and where it fires the big shot: they depend only on the way it comes in
  auto cl = [](float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); };
  cat_x1 = cl(240 + in_dx * 110, 130, 340), cat_y1 = cl(270 + in_dy * 100, 200, 400);
  const float dn = std::max(1.0f, std::hypot((float) out_dx, (float) out_dy));  // the big shot is fired from well outside the disc, out by the edge or corner it leaves by (not the one it came in on)
  cat_x2 = cl(240 + out_dx / dn * 235, 40, 440), cat_y2 = cl(246 + out_dy / dn * 235, 70, 440);
}
// is (x, y) under the cat where it hovers or fires (its body and comet would hide a blast there)
inline bool cat_under(float x, float y, float r) {
  if (critter != CR_CAT)
    return false;
  auto u = [&](float hx, float hy) { return std::fabs(x - hx) < 120 + r && y > hy - 160 - r && y < hy + 80 + r; };
  return u(cat_x1, cat_y1) || u(cat_x2, cat_y2);
}
inline void cat_plan() {
  cat_spots();
  cp.t_move = cp.shot0 + cp.dt * std::max(n_ctg, 1);
  cp.t_charge = cp.t_move + 1.0f, cp.t_fire = cp.t_charge + 1.0f, cp.t_hit = cp.t_fire + 0.35f, cp.t_leave = cp.t_hit + 1.7f;
  cp.t_rest = cp.t_leave + 1.5f + 2.0f, cp.t_end = cp.t_rest + 2.0f;  // the disc stays dark until two seconds after the cat is off the screen
  cat_restored = false;
  for (int i = 0; i < n_ctg; i++)
    ctg[i].gone = false;
}
inline const char *const SAY_CAT_BEFORE[] = {"Hold my catnip.", "Knocked it out\nof the sky.", "Is that\nAlderaan?", "Time for the\nbig one."};
inline const char *const SAY_CAT_LEAVE[] = {"Nailed it.\nNap time.", "Nine lives.\nZero regrets.", "Sky's cleaner now.", "Meow.",
                                            "Millions of voices\ncried out in terror,\nthen silence. Meow."};
inline int last_cat_before = -1, last_cat_leave = -1;
inline void cat_lines() {
  st.say[0] = nullptr;
  st.say[1] = pick_line(SAY_CAT_BEFORE, sizeof(SAY_CAT_BEFORE) / sizeof(*SAY_CAT_BEFORE), last_cat_before);
  st.say[2] = pick_line(SAY_CAT_LEAVE, sizeof(SAY_CAT_LEAVE) / sizeof(*SAY_CAT_LEAVE), last_cat_leave);
}
inline bool cat_on() { return critter == CR_CAT || critter == CR_AXO || critter == CR_AUDREY; }
inline float cat_beat(int i, bool end) {  // beat 1: from while it backs away to just before the big shot; beat 2: after the blast, till it flies off
  if (critter == CR_AUDREY) {  // Audrey's: as it reaches for the first thing, and as they regrow
    if (i == 1)
      return end ? pl.t_stalk + 2.2f : pl.t_stalk + 0.2f;
    if (i == 2)
      return end ? std::max(pl.t_regrow + 1.0f, pl.t_sink - 0.1f) : pl.t_regrow + 0.2f;
    return -10.0f;
  }
  if (critter == CR_AXO) {  // the axolotl's: while it stalks the first thing, and as the things regrow
    if (i == 1)
      return end ? ap.t_stalk + 1.9f : ap.t_stalk - 0.1f;
    if (i == 2)
      return end ? std::max(ap.t_regrow + 1.0f, ap.t_end - 0.2f) : ap.t_regrow + 0.2f;
    return -10.0f;
  }
  if (i == 1)
    return end ? cp.t_fire + 0.3f : cp.t_move + 0.3f;
  if (i == 2)
    return end ? cp.t_hit + 3.0f : cp.t_hit + 0.7f;
  return -10.0f;
}
inline float ufo_delay = 0;  // a cow is there first: the UFO comes in this long after the start (its own clock is t - ufo_delay)
inline float dur_s() { return critter == CR_AUDREY ? pl.t_end : critter == CR_AXO ? ap.t_end : critter == CR_CAT ? cp.t_end : critter == CR_UFO ? DUR_S + ufo_delay : DUR_S; }
inline float shot_hit(int i) { return cp.shot0 + cp.dt * i + 0.4f; }  // when shot i's beam arrives
// the comet's head (where the cat rides): in from its edge, hovering, over to where it fires at the disc, out by the other edge
inline void cat_xy(float t, float &x, float &y, bool calm = false) {  // calm: without the weave and the sway (where the bubble goes)
  auto ex = [](int d, float m) { return d < 0 ? -230.0f : d > 0 ? 710.0f : m; };
  const float e0x = ex(in_dx, cat_x1), e0y = ex(in_dy, cat_y1), e1x = ex(out_dx, cat_x2), e1y = ex(out_dy, cat_y2);
  const float side = (in_dx + 2 * in_dy) >= 0 ? 1.0f : -1.0f;  // which way the swoops bow
  // a weave: the run from (ax, ay) to (bx, by) at progress k, bobbing and weaving across it (`bow` wide, `n` half-waves; still at both ends)
  auto swoop = [&](float ax, float ay, float bx, float by, float k, float bow, float n_w) {
    const float dx = bx - ax, dy = by - ay, n = std::max(1.0f, std::hypot(dx, dy)), o = calm ? 0.0f : bow * std::sin(k * (float) M_PI) * std::sin(k * (float) M_PI * n_w);
    x = ax + dx * k - dy / n * o, y = ay + dy * k + dx / n * o;
  };
  if (t < 1.6f) {
    const float u = 1 - t / 1.6f;
    swoop(e0x, e0y, cat_x1, cat_y1, 1 - u * u * u, 28 * side, 3);
  } else if (t < cp.t_move) {
    x = cat_x1, y = cat_y1;
  } else if (t < cp.t_charge) {
    swoop(cat_x1, cat_y1, cat_x2, cat_y2, seg(t, cp.t_move, cp.t_charge), 20 * side, 2);
  } else if (t < cp.t_leave) {
    x = cat_x2, y = cat_y2;
  } else {
    const float u = std::min(1.0f, (t - cp.t_leave) / 1.5f);
    swoop(cat_x2, cat_y2, e1x, e1y, u * u, 35 * side, 3);
  }
  if (!calm)
    x += 5 * std::sin(t * 2.1f) + 8 * std::sin(t * 1.3f + 1.0f), y += 6 * std::sin(t * 2.3f);  // (it never holds still: a gentle sway and bob)
}
// how big the cat and its comet are: it backs away, shrinking to a third, as it moves out of the way of the big shot, and stays small
inline float cat_scale(float t) { return t < cp.t_move ? 1.0f : 1.0f - 0.65f * seg(t, cp.t_move, cp.t_charge); }
// what it aims at now, and which way it faces: ax, ay (the target or the disc's middle), f (-1 left, 1 right); aim false: just flying
inline bool cat_aim(float t, float x, float &ax, float &ay, float &f) {
  const bool fin = t >= cp.t_move;
  if (fin && t < cp.t_leave) {
    ax = disc_ox + ui.cx, ay = disc_oy + ui.cy, f = ax < x ? -1.0f : 1.0f;
    return t >= cp.t_fire && t < cp.t_hit + 0.2f;
  }
  if (!fin && n_ctg > 0 && t >= cp.shot0 - 0.4f) {
    const int i = std::min(n_ctg - 1, std::max(0, (int) ((t - cp.shot0 + 0.4f) / cp.dt)));
    ax = ctg[i].x, ay = ctg[i].y, f = ax < x ? -1.0f : 1.0f;
    const float w = t - (cp.shot0 + cp.dt * i);  // the charge, then the beam (0.3 to 0.55 s into its slot)
    return w >= 0.3f && w < 0.55f;
  }
  float e0 = in_dx < 0 ? -230.0f : in_dx > 0 ? 710.0f : cat_x1;
  f = t < 1.6f ? (cat_x1 >= e0 ? 1.0f : -1.0f) : (out_dx != 0 ? (float) out_dx : (cat_x2 >= x ? 1.0f : -1.0f));
  ax = x + f * 100, ay = 0;
  return false;
}
constexpr float UFO_K = 1.6f;                  // the saucer's size, in units of its 48 px half-width
constexpr float BEAM_TOP = 14 * UFO_K;
inline float beam_a(float t) { return seg(t, 2.6f, 3.2f) * (1 - seg(t, 6.8f, 7.4f)); }
// What it beams up (UI-90): something that is really on the map (the Sun, the Moon, a planet, the ISS or another satellite, a bright
// star, a constellation's figure) or a cow standing at the bottom of the screen. It hovers right over it, the beam comes down on it, and
// as the beam takes hold it comes off the map and rises into the saucer.
enum { IT_NONE = 0, IT_SUN, IT_MOON, IT_PLANET, IT_SAT, IT_STAR, IT_CONST, IT_COW };
inline int ufo_item = IT_NONE, ufo_planet = 0, ufo_var = 0;
inline float ufo_bh = 150;        // how far down its beam reaches (to the object)
constexpr float UFO_LIFT = 3.3f;  // when the beam takes hold: the object comes off the map
inline char ufo_a[48], ufo_c[40];
inline const char *const SAY_PLANET_B[5] = {"Small one.\nBeam it up.", "Handle with\noven mitts.", "Beam me up,\nScotty!", "Bring the\nbig crane.", "Rings and all.\nBeam it up."};
inline const char *const SAY_UFO_EXIT[] = {"Nothing to see\nhere.", "Take me to your\nleader.", "We were never\nhere.", "Thanks for\nthe snack.",
                                           "Back to the\nmothership.", "Greetings,\nearthlings.", "Taking this\nto go.", "Don't tell\nanyone."};
inline int last_ufo_exit = -1;
inline bool ufo_iss = false;
inline void ufo_lines() {  // a line as the beam comes on, one as it lifts the thing, one on leaving
  const char *a = "Scanning...\nNothing good.", *b = "Maybe next time.";
  if (ufo_item == IT_SUN)
    a = "Just the Sun.\nThey have more.", b = "Hot! Hot! Hot!\nBeam it up.";
  else if (ufo_item == IT_MOON)
    a = "The Moon.\nI'll take it.", b = "That's no moon.\nOh wait, it is.";
  else if (ufo_item == IT_PLANET) {
    snprintf(ufo_a, sizeof(ufo_a), "Target locked:\n%s.", planets::name(ufo_planet));
    a = ufo_a, b = SAY_PLANET_B[ufo_planet % 5];
  } else if (ufo_item == IT_SAT)
    a = ufo_iss ? "The ISS.\nNobody will notice." : "Free satellite.\nNo tracking fee.", b = "Beam me up,\nSat... err, Scotty.";
  else if (ufo_item == IT_STAR)
    a = ufo_var ? "Twinkle,\ntwinkle..." : "Just one star.\nThey won't miss it.", b = ufo_var ? "...gotcha." : "Make a wish.\nThen beam it.";
  else if (ufo_item == IT_CONST) {
    snprintf(ufo_a, sizeof(ufo_a), "Take all of\n%s.", ufo_c);
    a = ufo_a, b = "Connect the dots.\nBeam up.";
  } else if (ufo_item == IT_COW)
    a = ufo_var ? "Moo-ve over,\nEarth." : "Cow.\nClassic.", b = ufo_var ? "Moo?! Put me\ndown!" : "Moooooo???";
  st.say[0] = a, st.say[1] = b;
  st.say[2] = pick_line(SAY_UFO_EXIT, sizeof(SAY_UFO_EXIT) / sizeof(*SAY_UFO_EXIT), last_ufo_exit);
}
// the cow: it rises up from below the bottom edge of the screen as if standing on a hill, grazes there with its head down, and is then taken by the beam
inline float cow_dir = 1;  // which way the cow faces: towards the middle of the screen
inline float cow_x(float) {
  cow_dir = ctg[0].x < 240 ? 1.0f : -1.0f;
  return ctg[0].x;
}
inline float cow_y(float t) { return ctg[0].y + 110 * (1 - seg(t, 0.3f, 1.3f)); }
// the grassy hill the cow stands on: it rises from below the edge with him and sinks back after the UFO has gone
inline float hill_off(float t) { return 110 * (1 - seg(t, 0.3f, 1.3f)) + 110 * seg(t, ufo_delay + 6.9f, ufo_delay + 8.0f); }
inline void ufo_setup() {  // after ufo_pick(): it hovers over what it has and reaches down to it
  const CatTgt &g = ctg[0];
  ufo_item = n_ctg == 0 ? IT_NONE : g.kind == 0 ? IT_SUN : g.kind == 1 ? IT_MOON : g.kind == 2 ? IT_PLANET : g.kind <= 4 ? IT_SAT : g.kind == 5 ? IT_STAR : g.kind == 6 ? IT_CONST : IT_COW;
  ufo_iss = n_ctg > 0 && g.kind == 3, ufo_planet = n_ctg > 0 ? g.pl : 0, ufo_var = (int) (rnd() % 2), ufo_bh = 130, ufo_delay = ufo_item == IT_COW ? 2.0f : 0.0f;
  if (n_ctg > 0) {
    hover_x = std::max(40.0f, std::min(440.0f, g.x)), hover_y = std::max(70.0f, std::min(330.0f, g.y - 150));
    ufo_bh = std::max(40.0f, g.y - (hover_y + BEAM_TOP) + 6);
  }
  ufo_lines();
}
// the box to redo
inline void axo_box(float t, int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2);
inline void audrey_box(float t, int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2);
inline void critter_box(float t, int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2) {
  if (critter == CR_AXO) {
    axo_box(t, x1, y1, x2, y2);
    return;
  }
  if (critter == CR_AUDREY) {
    audrey_box(t, x1, y1, x2, y2);
    return;
  }
  if (critter == CR_UFO) {
    const float tu = t - ufo_delay;  // the UFO's own clock
    float cx = 240, cy = 0;
    if (tu >= 0)
      pos(tu, cx, cy);
    x1 = (int32_t) (cx - 52 * UFO_K), x2 = (int32_t) (cx + 52 * UFO_K), y1 = (int32_t) (cy - 28 * UFO_K), y2 = (int32_t) (cy + (beam_a(tu) > 0 ? BEAM_TOP + ufo_bh + 40 : 14 * UFO_K));
    if (tu < 0)
      x1 = x2 = y1 = y2 = 0;
    if (ufo_item == IT_COW && n_ctg > 0) {  // (the cow at the bottom of the screen, from the start)
      const int32_t cxx = (int32_t) cow_x(t);
      x1 = tu < 0 ? cxx - 140 : std::min(x1, cxx - 140), x2 = tu < 0 ? cxx + 140 : std::max(x2, cxx + 140), y2 = std::max(y2, (int32_t) (ctg[0].y + 120)), y1 = tu < 0 ? (int32_t) (ctg[0].y - 80) : y1;
    }
  } else {  // the cat: the comet and its tails, the cat above, the beam, what it is shooting; the whole disc once it blows up
    float x, y, ax, ay, f;
    cat_xy(t, x, y);
    const bool beam = cat_aim(t, x, ax, ay, f);
    const float S = cat_scale(t);  // the comet's glow, the cat above it (ears 178 px up at full size), its head and tail to either side
    x1 = (int32_t) (x - 125 * S), x2 = (int32_t) (x + 125 * S), y1 = (int32_t) (y - 190 * S), y2 = (int32_t) (y + 70 * S);
    if (beam) {  // (the beam runs from its eyes to what it hits: all of that, not just its ends)
      const float ey = y - 74 * S;
      x1 = std::min(x1, (int32_t) std::min(ax, x) - 60), x2 = std::max(x2, (int32_t) std::max(ax, x) + 60);
      y1 = std::min(y1, (int32_t) std::min(ay, ey) - 60), y2 = std::max(y2, (int32_t) std::max(ay, ey) + 60);
    }
    for (int i = 0; i < n_ctg; i++)
      if (t >= shot_hit(i) - 0.05f && t < shot_hit(i) + 1.2f)
        x1 = std::min(x1, (int32_t) (ctg[i].x - 160)), x2 = std::max(x2, (int32_t) (ctg[i].x + 160)), y1 = std::min(y1, (int32_t) (ctg[i].y - 160)),
        y2 = std::max(y2, (int32_t) (ctg[i].y + 160));  // (the sparks fly 145 px)
    if (t >= cp.t_hit && (t < cp.t_hit + 1.6f || t >= cp.t_rest) && t < cp.t_end) {  // the blast, from the middle out; then the dark needs nothing till the disc fades back
      const float R = (float) ui.radius + 26, h = t < cp.t_hit + 0.9f ? std::min(R, ((float) ui.radius + 10) * 1.15f * ease((t - cp.t_hit) / 0.9f) + 16) : R;
      const float mx = disc_ox + ui.cx, my = disc_oy + ui.cy;
      x1 = std::min(x1, (int32_t) (mx - h)), x2 = std::max(x2, (int32_t) (mx + h)), y1 = std::min(y1, (int32_t) (my - h)), y2 = std::max(y2, (int32_t) (my + h));
    }
  }
}
inline void fig_line(const Rz &r, float x0, float y0, float x1, float y1, uint16_t c, bool thick);
inline void tapered(const Rz &r, float x0, float y0, float r0, float x1, float y1, float r1, uint16_t c, float a0, float a1);
// what it has in the beam, at (x, y), s: its scale (it shrinks as it rises)
inline void paint_item(const Rz &r, float x, float y, float s, float t) {
  const float R = 13 * s;
  switch (ufo_item) {
    case IT_SUN:
      disc(r, x, y, R * 1.9f, c565(0xFFE070), 8);
      disc(r, x, y, R, c565(0xFFD84A));
      break;
    case IT_MOON:
      disc(r, x, y, R, c565(0xD8D8E0));
      disc(r, x + R * 0.3f, y - R * 0.25f, R * 0.2f, c565(0xB0B0BC));
      disc(r, x - R * 0.35f, y + R * 0.3f, R * 0.25f, c565(0xB0B0BC));
      break;
    case IT_PLANET: {  // Mercury, Venus, Mars, Jupiter, Saturn (the map's five)
      static const uint32_t COL[5] = {0xA8A8A8, 0xF2E0B0, 0xC1523A, 0xD8B48A, 0xE6D29A};
      const int pl = ufo_planet % 5;
      const float Rp = R * 1.2f;
      auto ring = [&](bool front) {  // the band between two flat ellipses, row by row: the back half goes behind the planet, the front over it
        const float ro = Rp * 2.1f, ao = Rp * 0.45f, ri = Rp * 1.5f, ai = Rp * 0.3f;
        for (int yy = (int) (y - ao); yy <= (int) (y + ao) + 1; yy++) {
          const float dy = yy - y;
          if ((dy >= 0) != front || std::fabs(dy) > ao)
            continue;
          const float xo = ro * std::sqrt(1 - (dy / ao) * (dy / ao)), xi = std::fabs(dy) < ai ? ri * std::sqrt(1 - (dy / ai) * (dy / ai)) : 0.0f;
          const uint16_t rc = c565(xi > 0 ? 0xCDB98A : 0xB8A472);
          row(r, yy, x - xo, x - xi, rc);
          row(r, yy, x + xi, x + xo, rc);
        }
      };
      if (pl == 4)
        ring(false);
      disc(r, x, y, Rp, c565(COL[pl]));
      if (pl == 3)  // Jupiter's bands
        for (int k = -2; k <= 2; k++) {
          const float by = y + k * Rp * 0.36f, hw = std::sqrt(std::max(0.0f, Rp * Rp - (k * Rp * 0.36f) * (k * Rp * 0.36f)));
          row(r, (int) by, x - hw, x + hw, c565(k % 2 ? 0xA87B52 : 0xE8D0A8));
        }
      if (pl == 2)
        disc(r, x - Rp * 0.3f, y - Rp * 0.2f, Rp * 0.22f, c565(0x8E3A28));
      if (pl == 4)
        ring(true);
      break;
    }
    case IT_SAT: {  // a little satellite: gold body, two blue panels, a dish
      const float sc = s * 0.9f, w = 11 * sc, h = 9 * sc;
      for (int yy = (int) (y - h); yy <= (int) (y + h); yy++) {
        row(r, yy, x - w, x + w, c565(0xD8B04C));
        row(r, yy, x - w - 6 * sc - 26 * sc, x - w - 6 * sc, c565((yy & 3) ? 0x3A5FB0 : 0x8CAEF0));
        row(r, yy, x + w + 6 * sc, x + w + 6 * sc + 26 * sc, c565((yy & 3) ? 0x3A5FB0 : 0x8CAEF0));
      }
      row(r, (int) y, x - w - 6 * sc, x - w, c565(0x9AA4B8));
      row(r, (int) y, x + w, x + w + 6 * sc, c565(0x9AA4B8));
      disc(r, x, y - h - 4 * sc, 5 * sc, c565(0xE6E6E6));
      break;
    }
    case IT_STAR:
      disc(r, x, y, 20 * s, c565(0xFFF2A0), 6);
      ellipse(r, x, y, 18 * s, 1.8f * s, c565(0xFFF8D0));
      ellipse(r, x, y, 1.8f * s, 18 * s, c565(0xFFF8D0));
      disc(r, x, y, 4.5f * s, c565(0xFFFFFF));
      break;
    case IT_CONST:  // the figure's lines, as they were on the map, drawn in about the item's place and size
      for (int i = 0; i < fig.n; i++)
        fig_line(r, x + (fig.s[i].x0 - (fig.bx0 + fig.bx1) / 2) * s, y + (fig.s[i].y0 - (fig.by0 + fig.by1) / 2) * s, x + (fig.s[i].x1 - (fig.bx0 + fig.bx1) / 2) * s,
                 y + (fig.s[i].y1 - (fig.by0 + fig.by1) / 2) * s, c565(0x9FB6F0), false);
      break;
    case IT_COW: {  // a cow: a boxy body with a flat back and rump, straight legs with dark hooves, an udder, a tail with a tuft, a long head with a horn
      const uint16_t white = c565(0xF4F4F4), spot = c565(0x2A2A2A), pink = c565(0xF0A8A8), horn = c565(0xE8DCC0);
      const float k = 1.7f * s, d = cow_dir;  // d: +1 faces right, -1 left (the way it walks in)
      const bool graze = t >= 1.3f && t < UFO_LIFT + ufo_delay;  // head down in the grass, chewing
      const float ph = t * 10.0f, hd = graze ? 8.0f + 0.5f * std::sin(ph) : 0.0f;  // the head's drop, in k
      const bool walking = false;
      x -= d * 6 * k;  // (the head sticks out in front: the whole cow is centred)
      auto X = [&](float u) { return x + d * u * k; };
      auto rrect = [&](float u0, float y0, float u1, float y1, float rad, uint16_t c) {  // a rectangle with rounded corners, by rows (u: along the cow, in k)
        const float xa = std::min(X(u0), X(u1)), xb = std::max(X(u0), X(u1)), ya = y + y0 * k, yb = y + y1 * k, rd = rad * k;
        for (int yy = (int) std::ceil(ya); yy <= (int) yb; yy++) {
          const float top = yy - ya, bot = yb - yy, e = std::min(top, bot);
          const float in = e < rd ? rd - std::sqrt(std::max(0.0f, rd * rd - (rd - e) * (rd - e))) : 0.0f;
          row(r, yy, xa + in, xb - in, c);
        }
      };
      int li = 0;
      for (float lx : {-11.0f, -6.0f, 5.0f, 10.0f}) {  // four legs: diagonal pairs swing together while it walks
        const float sw = walking ? std::sin(ph + (li++ % 2 ? 3.14159f : 0.0f)) : 0.0f;
        const float fx = lx + sw * 2.2f, lift = walking ? std::max(0.0f, sw) * 1.6f : 0.0f;
        tapered(r, X(lx), y + 5 * k, 1.5f * k, X(fx), y + (16.5f - lift) * k, 1.4f * k, white, 32, 32);
        disc(r, X(fx), y + (17 - lift) * k, 1.5f * k, spot);
      }
      tapered(r, X(-13), y - 4 * k, 0.6f * k, X(-15 + (walking ? std::sin(ph) * 1.5f : 0.0f)), y + 8 * k, 0.5f * k, white, 32, 32);  // the tail swings
      disc(r, X(-15 + (walking ? std::sin(ph) * 1.5f : 0.0f)), y + 9 * k, 1.5f * k, spot);
      rrect(-13, -7, 13, 7, 2.6f, white);
      rrect(-11, -6, -4, 1, 1.8f, spot);
      rrect(1, -1, 8, 5.5f, 1.8f, spot);
      rrect(-12, 2.5f, -8, 6, 1.2f, spot);
      ellipse(r, X(-4), y + 8 * k, 2.2f * k, 1.6f * k, pink);
      if (graze)
        rrect(10, -7, 16, hd - 3, 1.2f, white);  // the neck down to the head
      y += hd * k;  // (the head, lowered in the grass)
      rrect(12, -8, 22, 3, 2, white);
      rrect(12, -8, 16, -2, 1.5f, spot);
      rrect(19.5f, -1.5f, 23.5f, 3.5f, 1.4f, pink);
      ellipse(r, X(17), y - 9 * k, 1.1f * k, 2.2f * k, horn);
      ellipse(r, X(13.5f), y - 7 * k, 3 * k, 1.3f * k, white);
      disc(r, X(18.5f), y - 5 * k, 0.8f * k, spot);
      break;
    }
    default:
      break;
  }
}
// the hill's mound (behind the cow) and the grass blades (in front of his legs), swaying a little
inline float hash01(uint32_t i);
inline void paint_hill(const Rz &r, float t, bool front) {
  const float cx = cow_x(t), top = ctg[0].y + 34 + hill_off(t);  // the mound's top: a little above the cow's hooves
  if (!front) {
    ellipse(r, cx, top + 36, 130, 36, c565(0x2A6030));
    ellipse(r, cx, top + 40, 112, 30, c565(0x347A3A));
    return;
  }
  for (int i = 0; i < 44; i++) {
    const float u = (i + hash01(i * 3 + 1) * 0.8f) / 44.0f, bx = cx - 118 + u * 236, edge = 1 - std::pow(std::fabs(u * 2 - 1), 3.0f);
    const float by = top + 12 + (1 - edge) * 20, h = (7 + 9 * hash01(i * 7 + 5)) * (0.5f + 0.5f * edge), lean = 2.2f * std::sin(t * 2.0f + i * 0.7f);
    tapered(r, bx, by, 1.5f, bx + lean, by - h, 0.4f, c565(i % 3 == 0 ? 0x56A84A : i % 3 == 1 ? 0x3C8C3C : 0x2F7A33), 32, 32);
  }
}
inline void paint_ufo(const Rz &r, float t) {
  const float tu = t - ufo_delay;  // the UFO's own clock: a cow is there before it comes
  if (ufo_item == IT_COW && n_ctg > 0) {  // the hill and the cow, risen from below the bottom edge and grazing
    paint_hill(r, t, false);
    if (tu < UFO_LIFT)
      paint_item(r, cow_x(t), cow_y(t), 1.5f, t);
    paint_hill(r, t, true);
  }
  if (tu < 0)
    return;
  float cx, cy;
  pos(tu, cx, cy);
  const float a = beam_a(tu), K = UFO_K;
  if (a > 0) {  // the tractor beam: a pale green cone, fading towards the ground
    for (int y = (int) (cy + BEAM_TOP); y <= (int) (cy + BEAM_TOP + ufo_bh); y++) {
      const float f = (y - cy - BEAM_TOP) / ufo_bh, hw = (14 + 40 * f) * K * std::min(1.0f, ufo_bh / 150.0f + 0.4f);
      row(r, y, cx - hw, cx + hw, c565(0xB8FFB0), (uint32_t) (a * (11 - 7 * f)));
    }
    if (tu >= UFO_LIFT && ufo_item != IT_NONE && n_ctg > 0) {  // the thing rises from where it was up the beam, small as it goes up, and is gone into the saucer
      const float k = seg(tu, UFO_LIFT + 0.1f, 6.6f), y0 = ctg[0].y, y1 = cy + BEAM_TOP + 6, x0 = ufo_item == IT_COW ? cow_x(t) : ctg[0].x;
      paint_item(r, x0 + (cx - x0) * k + 2 * std::sin(tu * 4), y0 + (y1 - y0) * k, (ufo_item == IT_COW ? 1.5f : 1.0f) * (1.0f - 0.6f * k), tu);
    }
  }
  ellipse(r, cx, cy + 5 * K, 30 * K, 7 * K, c565(0x6C7690));      // the underside
  ellipse(r, cx, cy, 48 * K, 11 * K, c565(0x9AA4B8));              // the saucer
  ellipse(r, cx, cy - 9 * K, 20 * K, 13 * K, c565(0xA8DCF4));      // the dome
  disc(r, cx - 7 * K, cy - 14 * K, 3.5f * K, c565(0xF0FAFF));
  for (int i = 0; i < 5; i++)                                      // the lights chase round the rim
    disc(r, cx + (i - 2) * 18 * K, cy + (3 + (i == 2 ? 3 : 0)) * K, 2.6f * K, ((int) (tu * 6) + i) % 3 == 0 ? c565(0xFFE060) : c565(0x4A5068));
}
// a straight tapered streak from (x0, y0) of radius r0 to (x1, y1) of radius r1, strength a0..a1 (of 32) along it, soft at the edge:
// every pixel is worked out from its distance to the line (no chain of discs, so no beads)
inline void tapered(const Rz &r, float x0, float y0, float r0, float x1, float y1, float r1, uint16_t c, float a0, float a1) {
  const float rm = std::max(r0, r1) + 1;
  const int ya = std::max(r.cy1, ifloor(std::min(y0, y1) - rm)), yb = std::min(r.cy2, iceil(std::max(y0, y1) + rm));
  const int xa = std::max(r.cx1, ifloor(std::min(x0, x1) - rm)), xb = std::min(r.cx2, iceil(std::max(x0, x1) + rm));
  const float dx = x1 - x0, dy = y1 - y0, l2 = std::max(1.0f, dx * dx + dy * dy), il2 = 1.0f / l2;  // (the FPU has no divide: one reciprocal, not one division a pixel)
  for (int y = ya; y <= yb; y++) {
    uint16_t *row_p = (uint16_t *) (r.base + (size_t) y * r.stride);
    for (int x = xa; x <= xb; x++) {
      const float px = x - x0, py = y - y0;
      const float u = std::min(1.0f, std::max(0.0f, (px * dx + py * dy) * il2));
      const float qx = px - dx * u, qy = py - dy * u, rad = r0 + (r1 - r0) * u, d2 = qx * qx + qy * qy;
      if (d2 >= (rad + 0.5f) * (rad + 0.5f))
        continue;
      const float rin = rad - 0.5f;  // (inside it the edge is not needed: no square root, a software call on the S3)
      const float edge = rin > 0 && d2 <= rin * rin ? 1.0f : std::min(1.0f, std::max(0.0f, rad + 0.5f - std::sqrt(d2)));  // one pixel of soft edge
      const uint32_t a5 = (uint32_t) ((a0 + (a1 - a0) * u) * edge);
      if (a5)
        row_p[x] = a5 >= 32 ? c : blend565(c, row_p[x], a5);
    }
  }
}
// the band between two circles, row by row
inline void ring_fill(const Rz &r, float cx, float cy, float ro, float ri, uint16_t c, uint32_t alpha) {
  for (int y = std::max(r.cy1, ifloor(cy - ro)); y <= std::min(r.cy2, iceil(cy + ro)); y++) {
    const float dy = y - cy, o2 = ro * ro - dy * dy;
    if (o2 <= 0)
      continue;
    const float xo = std::sqrt(o2), xi = std::fabs(dy) < ri ? std::sqrt(ri * ri - dy * dy) : 0.0f;
    row(r, y, cx - xo, cx - xi, c, alpha);
    row(r, y, cx + xi, cx + xo, c, alpha);
  }
}
inline float hash01(uint32_t i) {  // a repeatable number 0..1 per index (the particles of every shot fly the same way)
  i ^= i >> 16, i *= 0x7FEB352Du, i ^= i >> 15, i *= 0x846CA68Bu, i ^= i >> 16;
  return (i & 0xFFFF) / 65535.0f;
}
// a shot's target vaporising: a flash, a ring and sparks, va seconds after the hit
inline void paint_zap(const Rz &r, const CatTgt &g, float va, int seed) {
  if (va < 0 || va > 1.2f)
    return;
  const float R = std::max(10.0f, g.r);
  if (va < 0.3f)
    disc(r, g.x, g.y, R * (1.0f + 1.6f * va / 0.3f), c565(0xFFFFFF), (uint32_t) (32 * (1 - va / 0.3f)));
  ring_fill(r, g.x, g.y, R * (1 + 5 * va) + 3, R * (1 + 5 * va) - 2, c565(0xFFE8B0), (uint32_t) (22 * (1 - va / 1.2f)));
  for (int i = 0; i < 14; i++) {
    const float an = hash01(seed * 31 + i) * 6.2832f, sp = 40 + 80 * hash01(seed * 57 + i + 100);
    disc(r, g.x + std::cos(an) * sp * va, g.y + std::sin(an) * sp * va, 3.2f * (1 - va / 1.2f) + 0.6f, c565(i % 3 ? 0xFFD070 : 0xFF8040), (uint32_t) (32 * (1 - va / 1.2f)));
  }
}
// the sky disc destroyed: a white flash, a shock ring racing out, everything inside it gone dark, debris; the dark fades once t_rest is past
inline void paint_boom(const Rz &r, float t) {
  const float u = t - cp.t_hit;
  if (u < 0)
    return;
  const float R = (float) ui.radius + 10, cxd = disc_ox + ui.cx, cyd = disc_oy + ui.cy;  // (+10: the rim ring round the disc goes too)
  const float flash = u < 0.12f ? u / 0.12f : std::max(0.0f, 1 - (u - 0.12f) / 0.45f);
  const float Rs = R * 1.15f * ease(u / 0.9f);
  const float gone = t < cp.t_rest ? 1.0f : 1 - seg(t, cp.t_rest, cp.t_end);
  for (int y = std::max(r.cy1, (int) (cyd - R)); y <= std::min(r.cy2, (int) (cyd + R)); y++) {
    const float dy = y - cyd, hw = std::sqrt(std::max(0.0f, R * R - dy * dy));
    if (hw <= 0)
      continue;
    const float gw = Rs > std::fabs(dy) ? std::min(hw, std::sqrt(Rs * Rs - dy * dy)) : 0.0f;
    if (gw > 0 && gone > 0)
      row(r, y, cxd - gw, cxd + gw, c565(0x000000), (uint32_t) (32 * gone));  // (black, like the screen behind the disc)
    if (flash > 0 && gw > 0)  // (the flash runs out with the shock, inside it)
      row(r, y, cxd - gw, cxd + gw, c565(0xFFFFFF), (uint32_t) (26 * flash));
  }
  if (u < 1.3f) {  // the shock ring (clipped to the disc's square: it runs out over the rim)
    const float ro = Rs + 8, ri = std::max(0.0f, Rs - 4);
    ring_fill(r, cxd, cyd, ro, ri, c565(0xFFE8B0), (uint32_t) (24 * (1 - u / 1.3f)));
  }
  if (u < 1.6f)
    for (int i = 0; i < 44; i++) {  // debris
      const float an = hash01(i * 7 + 1) * 6.2832f, sp = 90 + 300 * hash01(i * 13 + 5), f = 1 - u / 1.6f;
      disc(r, cxd + std::cos(an) * sp * u, cyd + std::sin(an) * sp * u, (2.0f + 4.0f * hash01(i * 3 + 9)) * f + 0.8f, c565(i % 4 ? 0xFFB050 : 0x8C8C94), (uint32_t) (32 * f));
    }
}
// the comet the cat rides: a lumpy icy nucleus in a soft blue-white glow (no tail)
inline void paint_comet(const Rz &r, float hx, float hy, float S) {
  disc(r, hx, hy, 62 * S, c565(0xBFD8FF), 4);
  disc(r, hx, hy, 48 * S, c565(0xBFD8FF), 7);
  disc(r, hx, hy, 36 * S, c565(0xDDEBFF), 10);
  const uint16_t ice = c565(0x8E98AC), lit = c565(0xC4CEE0), pit = c565(0x646E82);
  disc(r, hx, hy, 24 * S, ice);  // an irregular lump: four overlapping blobs
  disc(r, hx - 15 * S, hy + 6 * S, 16 * S, ice);
  disc(r, hx + 14 * S, hy + 7 * S, 15 * S, ice);
  disc(r, hx + 2 * S, hy - 12 * S, 14 * S, ice);
  disc(r, hx - 8 * S, hy - 9 * S, 9 * S, lit);  // the sunlit side
  disc(r, hx + 12 * S, hy + 8 * S, 4.5f * S, pit);  // craters
  disc(r, hx - 14 * S, hy + 9 * S, 3.5f * S, pit);
  disc(r, hx + 3 * S, hy + 15 * S, 3 * S, pit);
}
// a filled triangle, row by row
inline void tri_fill(const Rz &r, float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
  const float X[3] = {x0, x1, x2}, Y[3] = {y0, y1, y2};
  const int ya = std::max(r.cy1, iceil(std::min(y0, std::min(y1, y2)))), yb = std::min(r.cy2, ifloor(std::max(y0, std::max(y1, y2))));
  for (int y = ya; y <= yb; y++) {
    float lo = 1e9f, hi = -1e9f;
    for (int e = 0; e < 3; e++) {
      const int a = e, b = (e + 1) % 3;
      if ((Y[a] <= y && Y[b] > y) || (Y[b] <= y && Y[a] > y)) {
        const float x = X[a] + (X[b] - X[a]) * (y - Y[a]) / (Y[b] - Y[a]);
        lo = std::min(lo, x), hi = std::max(hi, x);
      }
    }
    if (hi > lo)
      row(r, y, lo, hi, c);
  }
}
// the layer counts on the right (SAT, SL, GEO) crossfade to zero as the disc is destroyed and back to their numbers as it comes back
inline bool counts_active(float t) { return t >= cp.t_hit + 0.9f && t < cp.t_rest + 0.9f; }
inline float counts_u(float t) {  // 0: the numbers, 1: zeros
  if (t < cp.t_rest)
    return seg(t, cp.t_hit + 0.9f, cp.t_hit + 1.4f);
  return 1 - seg(t, cp.t_rest + 0.4f, cp.t_rest + 0.9f);
}
inline void paint_counts_bg(const Rz &r, float t) {  // the numbers as the page has them are covered (the screen's own background beside them)
  if (!counts_active(t) || st.clean == nullptr)
    return;
  for (int i = 0; i < COUNT_ROWS; i++) {
    if (count_num[i] == nullptr || lv_obj_has_flag(count_num[i], LV_OBJ_FLAG_HIDDEN))
      continue;
    lv_area_t a;
    lv_obj_get_coords(count_num[i], &a);
    const int32_t ya = std::max<int32_t>(0, a.y1 - 1), yb = std::min<int32_t>(479, a.y2 + 1);
    if (ya > yb)
      continue;
    const uint16_t bg = ((const uint16_t *) ((const uint8_t *) st.clean->data + (size_t) ya * st.clean->header.stride))[477];
    for (int32_t y = ya; y <= yb; y++)
      row(r, y, (float) (a.x2 - 40), 478.0f, bg);
  }
}
inline void paint_counts(lv_layer_t *layer, float t) {
  if (!counts_active(t))
    return;
  const float u = counts_u(t);
  for (int i = 0; i < COUNT_ROWS; i++) {
    if (count_num[i] == nullptr || lv_obj_has_flag(count_num[i], LV_OBJ_FLAG_HIDDEN))
      continue;
    lv_area_t a;
    lv_obj_get_coords(count_num[i], &a);
    const lv_area_t da = {a.x2 - 40, a.y1, a.x2, a.y2};
    for (int k = 0; k < 2; k++) {  // the number fading out, the zero fading in
      const float w = k ? u : 1 - u;
      if (w <= 0.0f)
        continue;
      lv_draw_label_dsc_t l;
      lv_draw_label_dsc_init(&l);
      l.font = lv_obj_get_style_text_font(count_num[i], LV_PART_MAIN);
      l.color = lv_color_hex(C_DIM);
      l.opa = (lv_opa_t) (255 * w);
      l.align = LV_TEXT_ALIGN_RIGHT;
      l.text = k ? "0" : lv_label_get_text(count_num[i]);
      lv_draw_label(layer, &l, &da);
    }
  }
}
inline void paint_cat(const Rz &r, float t) {
  float hx, hy, ax, ay, f;
  cat_xy(t, hx, hy);
  const bool beam = cat_aim(t, hx, ax, ay, f);
  paint_boom(r, t);  // (under everything else)
  paint_counts_bg(r, t);
  for (int i = 0; i < n_ctg; i++)
    paint_zap(r, ctg[i], t - shot_hit(i), i);
  const float S = cat_scale(t);
  paint_comet(r, hx, hy, S);
  const float K = CAT_K * S;
  const float cx = hx, cy = hy - 74 * S;  // it stands on the nucleus
  const uint16_t fur = c565(0xE8903A), dark = c565(0xB8661E);
  {  // the tail: a short one, from the rump back and up and curling over, its tip swaying a little (six streaks end to end)
    float px = cx - f * K * 12, py = cy - K * 1;
    for (int i = 1; i <= 6; i++) {
      const float u = i / 6.0f, th = 0.6f + 1.7f * u + 0.07f * u * std::sin(t * 4 + u * 3);
      const float qx = px - f * std::cos(th) * K * 2.6f, qy = py - std::sin(th) * K * 2.6f;
      tapered(r, px, py, K * (3.6f - 1.5f * (u - 0.17f)), qx, qy, K * (3.6f - 1.5f * u), i == 6 ? dark : fur, 32, 32);
      px = qx, py = qy;
    }
  }
  ellipse(r, cx + f * K * 8, cy + K * 8, K * 3, K * 4.5f, fur);
  ellipse(r, cx - f * K * 8, cy + K * 8, K * 3, K * 4.5f, fur);
  ellipse(r, cx, cy, K * 15, K * 9, fur);
  ellipse(r, cx, cy + K * 3, K * 9, K * 5, c565(0xF6D7A8));  // the pale belly
  ellipse(r, cx - K * 3, cy - K * 2, K * 2, K * 6, dark);
  ellipse(r, cx + K * 4.5f, cy - K * 2, K * 2, K * 6, dark);
  const uint16_t pink = c565(0xF0A0A0);
  disc(r, cx + f * K * 15, cy - K * 8, K * 8, fur);
  auto ear = [&](float xc, float lean) {  // pointed, with its base sunk into the head's curve so it joins the head; pink inside
    auto top = [](float x) { return -8.0f - std::sqrt(std::max(0.0f, 64.0f - (x - 15) * (x - 15))) + 1.2f; };  // the head's top edge at x, in units of K
    const float hw = 2.6f, xl = xc - hw, xr = xc + hw, ym = top(xc);
    tri_fill(r, cx + f * K * xl, cy + K * top(xl), cx + f * K * xr, cy + K * top(xr), cx + f * K * (xc + lean), cy + K * (ym - 9.6f), fur);
    const float hi = hw * 0.55f;
    tri_fill(r, cx + f * K * (xc - hi), cy + K * (top(xc - hi) - 0.4f), cx + f * K * (xc + hi), cy + K * (top(xc + hi) - 0.4f), cx + f * K * (xc + lean * 0.8f), cy + K * (ym - 6.6f), pink);
  };
  ear(10.5f, -0.9f);
  ear(19.5f, 0.5f);

  ellipse(r, cx + f * K * 17, cy - K * 4.5f, K * 4.2f, K * 3, c565(0xF6D7A8));  // the muzzle
  const float ey = cy - K * 9;
  const float charge = t >= cp.t_charge ? seg(t, cp.t_charge, cp.t_fire) * (t < cp.t_hit + 0.2f ? 1.0f : 0.0f) : 0.0f;
  for (int e = 0; e < 2; e++) {  // the eyes: red when they fire, and the beam straight at what it aims at
    const float ex = cx + f * K * (e ? 19.0f : 12.0f), eyy = ey + (e ? 0 : K);
    if (beam) {
      const bool fin = t >= cp.t_move;
      const float w0 = fin ? 9.0f : 5.0f;
      tapered(r, ex, eyy, w0 * 1.6f, ax, ay, w0 * 1.1f, c565(0xFF3030), 10, 10);
      tapered(r, ex, eyy, w0, ax, ay, w0 * 0.8f, c565(0xFF5040), 32, 32);
      tapered(r, ex, eyy, w0 * 0.4f, ax, ay, w0 * 0.3f, c565(0xFFF0E8), 32, 32);
    }
    disc(r, ex, eyy, K * 1.8f, beam || charge > 0 ? c565(0xFF2020) : c565(0x202020));
  }
  {  // the nose, the mouth (open when it fires) and whiskers
    const float nx = cx + f * K * 17, ny = cy - K * 5.6f;
    ellipse(r, nx, ny, K * 1.3f, K * 0.9f, pink);
    const uint16_t brown = c565(0x5A2E14);
    if (beam || charge > 0)
      ellipse(r, nx, ny + K * 2.4f, K * 1.4f, K * 1.2f, c565(0x401818));
    else {
      tapered(r, nx, ny + K * 0.9f, K * 0.28f, nx, ny + K * 2.2f, K * 0.28f, brown, 32, 32);  // a cat's mouth: down from the nose, then a smile either way
      tapered(r, nx, ny + K * 2.2f, K * 0.28f, nx - K * 2.0f, ny + K * 1.8f, K * 0.28f, brown, 32, 32);
      tapered(r, nx, ny + K * 2.2f, K * 0.28f, nx + K * 2.0f, ny + K * 1.8f, K * 0.28f, brown, 32, 32);
    }
    for (int w = -1; w <= 1; w++) {  // three whiskers on the front side
      tapered(r, nx + f * K * 1.5f, ny + K * 0.4f + w * K * 0.7f, K * 0.18f, nx + f * K * 6.5f, ny + K * 0.4f + w * K * 2.0f, K * 0.18f, c565(0xFFF4E0), 26, 26);
      tapered(r, nx - f * K * 1.5f, ny + K * 0.4f + w * K * 0.7f, K * 0.18f, nx - f * K * 4.5f, ny + K * 0.4f + w * K * 1.8f, K * 0.18f, c565(0xFFF4E0), 26, 26);
    }
  }
  if (charge > 0) {  // the big shot gathers at its eyes
    const float ex = cx + f * K * 16, rad = 6 + 30 * charge + 3 * std::sin(t * 30);
    disc(r, ex, ey, rad * 1.6f, c565(0xFF6040), 8);
    disc(r, ex, ey, rad, c565(0xFF4030));
    disc(r, ex, ey, rad * 0.5f, c565(0xFFF0E0));
  }
}

// a thin line, two pixels wide, no soft edge (the axolotl's gill filaments: sixty of them a frame)
inline void thin_line(const Rz &r, float x0, float y0, float x1, float y1, uint16_t c) {
  const int n = std::max(1, (int) std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)));
  for (int i = 0; i <= n; i++) {
    const int x = ifloor(x0 + (x1 - x0) * i / n), y = ifloor(y0 + (y1 - y0) * i / n);
    for (int dy = 0; dy < 2; dy++) {
      if (y + dy < r.cy1 || y + dy > r.cy2)
        continue;
      uint16_t *row_p = (uint16_t *) (r.base + (size_t) (y + dy) * r.stride);
      for (int dx = 0; dx < 2; dx++)
        if (x + dx >= r.cx1 && x + dx <= r.cx2)
          row_p[x + dx] = c;
    }
  }
}
// ---- the axolotl (UI-91): seen from the side; (cx, cy) the middle of its body, K px a unit (it is about 150 units long), fx: +1 faces right, -1 left, and
// in between it is turning round (its length foreshortened); pitch: nose down in radians; ph: the tail's wave; amp: how hard it swims;
// pal: 0 leucistic pink, 1 wild (dark olive), 2 golden, 3 mosaic (pink, dark patches); open: the mouth gaping; puff: the cheeks full
struct AxoPal {
  uint32_t body, belly, gill, gill2, fin, eye, patch;
};
inline const AxoPal AXO_PAL[4] = {{0xF2B3C6, 0xFBD6E0, 0xE5506F, 0xC23E5C, 0xF7C6D4, 0x181018, 0xF2B3C6},
                                  {0x4C4A3A, 0x7A745C, 0xA85A58, 0x7E3E40, 0x625E48, 0x0E0E0A, 0x4C4A3A},
                                  {0xF2C45A, 0xFBE3A0, 0xE98A4A, 0xC4683A, 0xF7D88A, 0x181008, 0xF2C45A},
                                  {0xF2B3C6, 0xFBD6E0, 0xE5506F, 0xC23E5C, 0xF7C6D4, 0x181018, 0x5A5648}};
// The body is a spine of nodes (a bendable curve, 6 units apart, the middle one the path's point): each node has an angle (nose down, radians), and
// a point of the body (lx units ahead of the middle, ly below) is put on the screen through the spine, so it bends instead of turning stiffly.
constexpr int AXO_N = 28, AXO_MID = 18;
constexpr float AXO_X0 = -108.0f, AXO_DX = 6.0f;  // node k is at local x AXO_X0 + AXO_DX * k
struct AxoSp {
  float x[AXO_N], y[AXO_N], a[AXO_N], K, fx;
};
inline void axo_sp_make(AxoSp &sp, float cx, float cy, float K, float fx, const float *bend, float pitch) {
  sp.K = K, sp.fx = fx;
  for (int k = 0; k < AXO_N; k++)
    sp.a[k] = bend ? bend[k] : pitch;
  sp.x[AXO_MID] = cx, sp.y[AXO_MID] = cy;
  for (int k = AXO_MID; k + 1 < AXO_N; k++) {
    const float a = (sp.a[k] + sp.a[k + 1]) / 2;
    sp.x[k + 1] = sp.x[k] + K * AXO_DX * fx * std::cos(a), sp.y[k + 1] = sp.y[k] + K * AXO_DX * std::sin(a);
  }
  for (int k = AXO_MID; k > 0; k--) {
    const float a = (sp.a[k] + sp.a[k - 1]) / 2;
    sp.x[k - 1] = sp.x[k] - K * AXO_DX * fx * std::cos(a), sp.y[k - 1] = sp.y[k] - K * AXO_DX * std::sin(a);
  }
}
inline void axo_wp(const AxoSp &sp, float lx, float ly, float &x, float &y) {
  const float f = std::min((float) AXO_N - 1.001f, std::max(0.0f, (lx - AXO_X0) / AXO_DX));
  const int k = (int) f;
  const float fr = f - (float) k, a = sp.a[k] + (sp.a[k + 1] - sp.a[k]) * fr;
  x = sp.x[k] + (sp.x[k + 1] - sp.x[k]) * fr + sp.K * sp.fx * (-ly * std::sin(a));
  y = sp.y[k] + (sp.y[k + 1] - sp.y[k]) * fr + sp.K * ly * std::cos(a);
}
inline void paint_axolotl_sp(const Rz &r, const AxoSp &sp, float ph, int pal, float open, float puff, float amp) {
  const float K = sp.K;
  const AxoPal &P = AXO_PAL[pal & 3];
  auto shade = [](uint32_t c) { return (std::max(0, (int) ((c >> 16) & 255) - 21) << 16) | (std::max(0, (int) ((c >> 8) & 255) - 21) << 8) | std::max(0, (int) (c & 255) - 21); };
  const uint16_t body = c565(P.body), belly = c565(P.belly), gill = c565(P.gill), gill2 = c565(P.gill2), fin = c565(P.fin), eye = c565(P.eye), patch = c565(P.patch), dk = c565(shade(P.body));
  auto W = [&](float lx, float ly, float &x, float &y) { axo_wp(sp, lx, ly, x, y); };
  // the tail: eight segments from the rump, a wave running down it, with a tall thin fin above and below
  float lxs[9], lys[9], trk[9], tx[9], ty[9];
  lxs[0] = -10, lys[0] = 0, trk[0] = 11.5f;  // (it starts inside the body at the body's full width, so there is no step where they join)
  for (int i = 1; i < 9; i++) {
    const float u = i / 8.0f;
    lxs[i] = lxs[i - 1] - 11.0f, trk[i] = 11.5f - 9.0f * u;
    lys[i] = 7.0f * amp * u * std::sin(ph - u * 3.4f);
  }
  for (int i = 0; i < 9; i++)
    W(lxs[i], lys[i], tx[i], ty[i]);
  for (int i = 0; i < 8; i++) {  // the fin first (behind the tail)
    const float h0 = 3 + 11 * std::sin(i * 0.38f + 0.25f), h1 = 3 + 11 * std::sin((i + 1) * 0.38f + 0.25f);
    for (int sgn = -1; sgn <= 1; sgn += 2) {
      const float k0 = sgn < 0 ? 0.8f : 0.8f;
      float ax, ay, bx, by, cx2, cy2, dx, dy;
      W(lxs[i], lys[i] + sgn * (trk[i] + h0 * k0), ax, ay);
      W(lxs[i + 1], lys[i + 1] + sgn * (trk[i + 1] + h1 * k0), bx, by);
      W(lxs[i], lys[i] + sgn * trk[i] * 0.5f, cx2, cy2);
      W(lxs[i + 1], lys[i + 1] + sgn * trk[i + 1] * 0.5f, dx, dy);
      tri_fill(r, ax, ay, bx, by, cx2, cy2, fin);
      tri_fill(r, bx, by, dx, dy, cx2, cy2, fin);
    }
  }
  for (int i = 0; i < 8; i++) {  // the tail: a strip (each step is two triangles) with a round tip
    float ax, ay, bx, by, cx2, cy2, dx, dy;
    W(lxs[i], lys[i] - trk[i], ax, ay);
    W(lxs[i + 1], lys[i + 1] - trk[i + 1], bx, by);
    W(lxs[i], lys[i] + trk[i], cx2, cy2);
    W(lxs[i + 1], lys[i + 1] + trk[i + 1], dx, dy);
    tri_fill(r, ax, ay, bx, by, cx2, cy2, body);
    tri_fill(r, bx, by, dx, dy, cx2, cy2, body);
  }
  disc(r, tx[8], ty[8], K * trk[8], body);
  // the legs: stubby, angled down and back, with three toes; the far pair a shade darker and behind the body
  auto leg = [&](float lx, float ly, float sw, uint16_t c) {
    const float exl = lx - (4 + 1.5f * sw), eyl = ly + 8;
    float x0, y0, x1, y1;
    W(lx, ly, x0, y0);
    W(exl, eyl, x1, y1);
    tapered(r, x0, y0, K * 4.8f, x1, y1, K * 3.0f, c, 32, 32);
    for (int k = -1; k <= 1; k++) {
      float x2, y2;
      W(exl + (k * 1.2f - 1.0f), eyl + 3.6f + 0.3f * (k == 0), x2, y2);
      tapered(r, x1, y1, K * 1.5f, x2, y2, K * 1.1f, c, 32, 32);
    }
  };
  leg(12, 6, std::sin(ph + 3.0f), dk);
  leg(-10, 6, std::sin(ph), dk);
  // the body, a paler belly, a crest along the back (running into the tail's fin), and (mosaic) dark patches
  float ax, ay, bx, by;
  W(-22, -11, ax, ay);
  W(8, -11, bx, by);
  tapered(r, ax, ay, K * 3.2f, bx, by, K * 3.2f, fin, 32, 32);
  W(-22, 0, ax, ay);
  W(0, 0, bx, by);
  tapered(r, ax, ay, K * 11.5f, bx, by, K * 11.5f, body, 32, 32);
  W(22, 0, ax, ay);
  tapered(r, bx, by, K * 11.5f, ax, ay, K * 11.5f, body, 32, 32);
  W(-20, 5.5f, ax, ay);
  W(0, 5.5f, bx, by);
  tapered(r, ax, ay, K * 6.0f, bx, by, K * 6.0f, belly, 26, 26);
  W(18, 5.5f, ax, ay);
  tapered(r, bx, by, K * 6.0f, ax, ay, K * 6.0f, belly, 26, 26);
  if (pal == 3)
    for (int i = 0; i < 7; i++) {
      W((i - 3) * 8.5f + (i * 5) % 3, -(4 + (i * 3) % 4), ax, ay);
      disc(r, ax, ay, K * (3.5f + (i % 3)), patch, 26);
    }
  leg(14, 7, std::sin(ph + 4.6f), body);
  leg(-12, 7, std::sin(ph + 1.6f), body);
  // the head: broad and flat, with a small blunt snout
  W(17, -1, ax, ay);
  W(36, 0.5f, bx, by);
  tapered(r, ax, ay, K * 13.5f, bx, by, K * 11.0f, body, 32, 32);
  W(20, 5, ax, ay);
  W(34, 5.5f, bx, by);
  tapered(r, ax, ay, K * 5.5f, bx, by, K * 5.0f, belly, 26, 26);
  if (puff > 0.0f) {  // the cheeks puffed out after a gulp
    W(22, 3, ax, ay);
    W(34, 4, bx, by);
    tapered(r, ax, ay, K * (9 + 5 * puff), bx, by, K * (8 + 5 * puff), body, 32, 32);
    tapered(r, ax, ay + K * 3, K * (4 + 3 * puff), bx, by + K * 3, K * (3.5f + 2.5f * puff), belly, 26, 26);
  }
  if (pal == 3) {
    W(24, -7, ax, ay);
    disc(r, ax, ay, K * 4, patch, 26);
  }
  // the gills: three feathered stalks on each side of the head (the far three darker, behind it), fanned back and up, fluttering
  for (int side = 0; side < 2; side++)
    for (int k = 0; k < 3; k++) {
      const float blx = 29 - (6 + 3.5f * k) + (side ? 4 : 0), bly = -1 - (7 + 0.8f * k) - (side ? 2 : 0);
      const float ang = 0.1f - 0.72f * k + 0.1f * std::sin(ph * 2.0f + k * 1.3f + side) - (side ? 0.12f : 0.0f), len = 29 - 3.0f * k;
      float sx0, sy0, sx1, sy1;
      W(blx, bly, sx0, sy0);
      W(blx - len * std::cos(ang), bly + len * std::sin(ang), sx1, sy1);
      const uint16_t gc = side ? gill2 : gill;
      for (int q = 0; q < 3; q++)  // (in three pieces: the box of a long slanted stroke is mostly empty)
        tapered(r, sx0 + (sx1 - sx0) * q / 3, sy0 + (sy1 - sy0) * q / 3, K * (2.0f - 0.37f * q), sx0 + (sx1 - sx0) * (q + 1) / 3, sy0 + (sy1 - sy0) * (q + 1) / 3,
                K * (2.0f - 0.37f * (q + 1)), gc, 32, 32);
      const float nx = -(sy1 - sy0), ny = sx1 - sx0, nl = std::max(1.0f, std::hypot(nx, ny));
      for (int q = 1; q <= 5; q++) {  // the filaments, in pairs along the stalk
        const float u = q / 6.0f, px = sx0 + (sx1 - sx0) * u, py = sy0 + (sy1 - sy0) * u, fl = K * (7.5f - 5.0f * u);
        for (int sgn = -1; sgn <= 1; sgn += 2)
          thin_line(r, px, py, px + sgn * nx / nl * fl + (sx1 - sx0) * 0.08f, py + sgn * ny / nl * fl + (sy1 - sy0) * 0.08f, gc);
      }
    }
  // the face: the eye (small and dark), the blush, and the smile every axolotl has (or the mouth wide open)
  W(40, -7, ax, ay);
  disc(r, ax, ay, K * 2.4f, eye);
  W(40.5f, -7.7f, bx, by);
  disc(r, bx, by, K * 0.65f, c565(0xFFFFFF));
  W(34, 0.5f, ax, ay);
  ellipse(r, ax, ay, K * 3.4f, K * 2.1f, c565(0xF08BA2), 14);
  if (open > 0.02f) {
    W(44, 2.5f, ax, ay);
    ellipse(r, ax, ay, K * (3 + 6 * open), K * (1.5f + 6.5f * open), c565(0x7A2038));
    W(44, 3.5f + 3 * open, ax, ay);
    ellipse(r, ax, ay, K * (2 + 3 * open), K * (1 + 2.5f * open), c565(0xE87A90));  // the tongue
  } else {
    for (int i = 0; i <= 12; i++) {
      const float u = i / 12.0f;
      W(49 - 17 * u, 1 + 4.0f * u - 7.5f * u * u, ax, ay);
      disc(r, ax, ay, K * 0.7f, eye);
    }
  }
}

// (a stiff pose, for the preview sheet: the whole body at one pitch)
inline void paint_axolotl(const Rz &r, float cx, float cy, float K, float fx, float ph, int pal = 0, float open = 0.0f, float puff = 0.0f, float pitch = 0.0f, float amp = 1.0f) {
  AxoSp sp;
  axo_sp_make(sp, cx, cy, K, fx, nullptr, pitch);
  paint_axolotl_sp(r, sp, ph, pal, open, puff, amp);
}

// ---- where it is, when: the plan's segments (a smooth arc each, slowing at either end) -> its middle, which way it faces, how fast it moves
inline void axo_pos(float t, float &x, float &y, float &fx, float &dist, bool calm = false) {
  int j = 0;
  for (int i = 0; i < ap.n; i++)
    if (t >= ap.s[i].t0)
      j = i;
  const AxoSeg &g = ap.s[j];
  const float u = std::min(1.0f, std::max(0.0f, (t - g.t0) / std::max(0.01f, g.t1 - g.t0)));
  const float k = g.burst ? 1 - (1 - u) * (1 - u) * (1 - u) : u * u * (3 - 2 * u);  // (a burst starts fast and glides to a stop)
  const float dx = g.x1 - g.x0, dy = g.y1 - g.y0, len = std::max(1.0f, std::hypot(dx, dy));
  const float mx = (g.x0 + g.x1) / 2 - dy / len * g.bow, my = (g.y0 + g.y1) / 2 + dx / len * g.bow;
  x = (1 - k) * (1 - k) * g.x0 + 2 * (1 - k) * k * mx + k * k * g.x1;
  y = (1 - k) * (1 - k) * g.y0 + 2 * (1 - k) * k * my + k * k * g.y1;
  if (!calm)
    x += 2 * std::sin(t * 1.3f + 1.0f), y += 3 * std::sin(t * 2.1f);  // (it never holds quite still)
  if (g.f0 != g.f1)
    fx = g.f0 * std::cos(3.14159f * ease(std::min(1.0f, std::max(0.0f, (t - g.t0) / 0.8f))));  // (it turns round over its first 0.8 s)
  else
    fx = g.f1;
  dist = g.l0 + g.len * k;
}
// how fast it moves (px/s) and the pitch of its body (nose down, radians) from the way it heads
inline void axo_motion(float t, float &pitch, float &amp) {
  float x0, y0, x1, y1, f0, f1, d0, d1;
  axo_pos(t - 0.08f, x0, y0, f0, d0, true);
  axo_pos(t + 0.08f, x1, y1, f1, d1, true);
  const float vx = (x1 - x0) / 0.16f, vy = (y1 - y0) / 0.16f, sp = std::hypot(vx, vy);
  float fx, d;
  axo_pos(t, x0, y0, fx, d, true);
  const float sgn = (vx * (fx >= 0 ? 1.0f : -1.0f)) >= 0 ? 1.0f : -1.0f;  // (backing up tips it the other way)
  pitch = std::max(-0.5f, std::min(0.5f, std::atan2(vy, std::fabs(vx) + 1.0f) * sgn)) * std::min(1.0f, sp / 50.0f);
  amp = 0.45f + 0.6f * std::min(1.0f, sp / 120.0f);
}
// the whole pose at time t: the spine bends along the course the head took: each part of the body is at the heading the head had a moment earlier
// (the tail's is the oldest), with a little wave through it as it swims
inline void axo_pose(float t, AxoSp &sp, float &ph, float &amp) {
  float x, y, fx, d, pitch;
  axo_pos(t, x, y, fx, d);
  axo_motion(t, pitch, amp);
  ph = d / (AXO_K * 4.0f) + t * 2.0f;
  float bend[AXO_N];
  for (int k = 0; k < AXO_N; k++) {
    const float lx = AXO_X0 + AXO_DX * k, lag = lx < 0 ? -lx * AXO_K / 160.0f : -lx * 0.004f;  // (the head looks ahead, the tail trails)
    float p, a2;
    axo_motion(t - lag, p, a2);
    bend[k] = p + 0.07f * amp * std::sin(ph - lx * 0.07f);
  }
  axo_sp_make(sp, x, y, AXO_K, fx, bend, 0);
}
// the mouth's tip on the screen at time t
inline void axo_mouth(float t, float &mx, float &my) {
  AxoSp sp;
  float ph, amp;
  axo_pose(t, sp, ph, amp);
  axo_wp(sp, AXO_MOUTH, 1.0f, mx, my);
}
inline void axo_plan() {
  const float K = AXO_K, MO = K * AXO_MOUTH;
  auto edge = [](int d, float other) { return d < 0 ? -270.0f : d > 0 ? 750.0f : other; };
  const float ex0 = edge(in_dx, 140 + (float) (rnd() % 200)), ey0 = in_dy < 0 ? -200.0f : in_dy > 0 ? 680.0f : 150 + (float) (rnd() % 180);
  const float ex1 = edge(out_dx, 140 + (float) (rnd() % 200)), ey1 = out_dy < 0 ? -200.0f : out_dy > 0 ? 680.0f : 150 + (float) (rnd() % 180);
  float cx = ex0, cy = ey0, f = 1, t = 0, L = 0;
  int done[4] = {}, order[4], n = 0;
  float px = cx, py = cy;
  for (int k = 0; k < n_ctg; k++) {  // nearest first
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < n_ctg; i++)
      if (!done[i] && std::hypot(ctg[i].x - px, ctg[i].y - py) < bd)
        bd = std::hypot(ctg[i].x - px, ctg[i].y - py), best = i;
    done[best] = 1, order[n++] = best, px = ctg[best].x, py = ctg[best].y;
  }
  ap.n = 0;
  auto add = [&](float dur, float nx, float ny, float nf, float bow, bool burst) {
    AxoSeg &g = ap.s[ap.n++];
    const float ch = std::hypot(nx - cx, ny - cy);
    g = {t, t + dur, cx, cy, nx, ny, bow, f, nf, L, ch * (1 + 2 * (bow / std::max(ch, 1.0f)) * (bow / std::max(ch, 1.0f))), burst};
    L += g.len, t += dur, cx = nx, cy = ny, f = nf;
  };
  ap.t_stalk = 1.5f;
  f = n > 0 ? (ctg[order[0]].x > ex0 ? 1.0f : -1.0f) : (ex1 > ex0 ? 1.0f : -1.0f);
  for (int k = 0; k < n; k++) {
    const CatTgt &g = ctg[order[k]];
    float bf = 1, bc = 1e9f, ax = 0;
    for (int d = -1; d <= 1; d += 2) {  // which side to come at it from: the nearer approach, without turning round if it can
      const float a = g.x - d * (MO + 70), c = std::hypot(a - cx, g.y - K - cy) + (d != (int) f ? 140.0f : 0.0f) + (a < -120 || a > 600 ? 300.0f : 0.0f);
      if (c < bc)
        bc = c, bf = (float) d, ax = a;
    }
    const float ay = g.y - K, dist = std::hypot(ax - cx, ay - cy);
    add(std::min(3.4f, std::max(0.9f, dist / 140)), ax, ay, bf, (k % 2 ? 1 : -1) * 0.16f * dist, false);
    if (k == 0)
      ap.t_stalk = t;
    add(0.5f, ax, ay, bf, 0, false);  // it stalks it
    ap.pull[order[k]] = t + 0.2f;
    add(0.32f, g.x - bf * (MO + 14), ay, bf, 0, true);  // the lunge
    add(0.8f, g.x - bf * (MO + 14), ay, bf, 0, false);  // the gulp, the cheeks
  }
  ap.t_regrow = t + 0.4f;
  for (int k = 0; k < n; k++)
    ap.regrow[order[k]] = ap.t_regrow + 0.35f * k;
  ap.t_exit = t;
  const float dist = std::hypot(ex1 - cx, ey1 - cy);
  add(std::min(3.4f, std::max(1.8f, dist / 170)), ex1, ey1, ex1 > cx ? 1.0f : -1.0f, (n % 2 ? 1 : -1) * 0.14f * dist, false);
  ap.t_end = std::max(t, ap.t_regrow + 0.35f * n + 0.8f);
  for (int i = 0; i < 4; i++)
    ap.back[i] = false;
}
inline void patch_apply(int i, bool gone);
inline void axo_events(float t) {  // a target that is pulled in comes off the map; it is back (with a sparkle) when it regrows
  for (int i = 0; i < n_ctg; i++) {
    if (!ctg[i].gone && t >= ap.pull[i]) {
      ctg[i].gone = true;
      vlogf("axolotl t %.1f: gulps target %d (kind %d)", t, i, (int) ctg[i].kind);
      if (pt_ok)
        patch_apply(i, true);
    } else if (!ctg[i].gone && ctg[i].o[0]) {  // (a satellite moves: the mouth was aimed at where it was)
      lv_area_t a;
      lv_obj_get_coords(ctg[i].o[0], &a);
      ctg[i].x = (a.x1 + a.x2) / 2.0f, ctg[i].y = (a.y1 + a.y2) / 2.0f;
    }
    if (ctg[i].gone && !ap.back[i] && t >= ap.regrow[i]) {
      ap.back[i] = true;
      if (pt_ok)
        patch_apply(i, false);
    }
  }
}
inline void axo_box(float t, int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2) {
  AxoSp sp;
  float ph, amp;
  axo_pose(t, sp, ph, amp);
  static const float PT[8][2] = {{56, -22}, {56, 24}, {-104, -34}, {-104, 34}, {30, -42}, {-6, -42}, {0, 28}, {-40, 34}};
  float x0 = 1e9f, y0 = 1e9f, xe = -1e9f, ye = -1e9f;
  for (const auto &p : PT) {
    float wx, wy;
    axo_wp(sp, p[0], p[1], wx, wy);
    x0 = std::min(x0, wx), xe = std::max(xe, wx), y0 = std::min(y0, wy), ye = std::max(ye, wy);
  }
  auto grow = [&](float a, float b, float c, float e) { x0 = std::min(x0, a), y0 = std::min(y0, b), xe = std::max(xe, c), ye = std::max(ye, e); };
  for (int i = 0; i < n_ctg; i++) {
    const float u = t - ap.pull[i], rg = t - ap.regrow[i];
    float mx, my;
    if (u > -0.05f && u < 0.4f) {  // the target on its way to the mouth
      axo_mouth(t, mx, my);
      grow(std::min(ctg[i].x, mx) - 24, std::min(ctg[i].y, my) - 24, std::max(ctg[i].x, mx) + 24, std::max(ctg[i].y, my) + 24);
    }
    if (u > 0.4f && u < 2.3f) {  // the bubbles it blows (where the mouth was)
      axo_mouth(ap.pull[i] + 0.5f, mx, my);
      grow(mx - 30, my - 125, mx + 30, my + 15);
    }
    if (rg > -0.05f && rg < 0.75f)
      grow(ctg[i].x - 32, ctg[i].y - 32, ctg[i].x + 32, ctg[i].y + 32);  // the sparkle where it is back
  }
  x1 = (int32_t) x0 - 8, y1 = (int32_t) y0 - 8, x2 = (int32_t) xe + 8, y2 = (int32_t) ye + 8;
}
inline void paint_axo(const Rz &r, float t) {
  AxoSp sp;
  float ph, amp, mx, my;
  axo_pose(t, sp, ph, amp);
  axo_wp(sp, AXO_MOUTH, 1.0f, mx, my);
  float open = 0, puff = 0;
  for (int i = 0; i < n_ctg; i++) {
    const float u = t - ap.pull[i];
    open = std::max(open, u < -0.15f ? 0.0f : u < 0.05f ? (u + 0.15f) / 0.2f : u < 0.3f ? 1.0f : u < 0.45f ? 1 - (u - 0.3f) / 0.15f : 0.0f);
    if (u >= 0.3f && u < 1.2f)
      puff = std::max(puff, std::sin((u - 0.3f) / 0.9f * 3.14159f));
  }
  for (int i = 0; i < n_ctg; i++) {  // a target in flight, pulled into the mouth; a sparkle where it is back
    const float u = (t - ap.pull[i]) / 0.3f, q = (t - ap.regrow[i]) / 0.6f;
    if (u >= 0 && u < 1 && !ap.back[i]) {
      const float k = ease(u), rad = std::max(2.0f, ctg[i].r * 0.45f) * (1 - 0.8f * k), px = ctg[i].x + (mx - ctg[i].x) * k, py = ctg[i].y + (my - ctg[i].y) * k;
      disc(r, px, py, rad + 2.5f, c565(0xDDE8FF), 8);
      disc(r, px, py, rad, c565(ctg[i].kind == 5 ? 0xFFFFFF : 0xE8F0FF));
    }
    if (q >= 0 && q < 1)
      ring_fill(r, ctg[i].x, ctg[i].y, 5 + 20 * q, 3 + 19 * q, c565(0xFFF0B0), (uint32_t) (22 * (1 - q)));
  }
  paint_axolotl_sp(r, sp, ph, 0, open, puff, amp);
  for (int i = 0; i < n_ctg; i++) {  // the bubbles after each gulp
    const float u = t - ap.pull[i] - 0.5f;
    if (u < 0 || u > 1.7f)
      continue;
    float bx, by;
    axo_mouth(ap.pull[i] + 0.5f, bx, by);
    for (int b = 0; b < 3; b++) {
      const float ub = u - b * 0.18f;
      if (ub < 0)
        continue;
      const float px = bx + 6 + 6 * std::sin(ub * 5 + b), py = by - ub * 55 - b * 6, rr = 3.0f + b;
      ring_fill(r, px, py, rr, rr - 1.2f, c565(0xC8E8FF), 26);
      disc(r, px - rr * 0.3f, py - rr * 0.3f, 1.0f, c565(0xFFFFFF), 20);
    }
  }
}
inline const char *const SAY_AXO_BEFORE[] = {"Snack time.", "Is that a worm?", "Blub.", "Hold still,\nlittle snack."};
inline const char *const SAY_AXO_LEAVE[] = {"Regrows nicely.", "Still smiling.", "Don't worry,\nI can regrow that.", "Blub blub."};
inline int last_axo_before = -1, last_axo_leave = -1;
inline void axo_lines() {
  st.say[0] = nullptr;
  st.say[1] = pick_line(SAY_AXO_BEFORE, sizeof(SAY_AXO_BEFORE) / sizeof(*SAY_AXO_BEFORE), last_axo_before);
  st.say[2] = pick_line(SAY_AXO_LEAVE, sizeof(SAY_AXO_LEAVE) / sizeof(*SAY_AXO_LEAVE), last_axo_leave);
}

// the stem's 11 points (from the pot, 0, to the head's hinge, 10) and the radius at each: an S curve rising and bending over, swaying (the painter's and the redraw box's)
inline void audrey_stem(float bx, float by, float K, float fx, float ex, float ey, float sway, float *sx, float *sy, float *sr) {
  const float c1x = bx - fx * 40 * K + 10 * K * std::sin(sway), c1y = by + (ey - by) * 0.75f;
  for (int k = 0; k <= 10; k++) {
    const float u = k / 10.0f, a = (1 - u) * (1 - u), b = 2 * (1 - u) * u, c = u * u;
    sx[k] = a * bx + b * c1x + c * ex + 3 * K * std::sin(sway * 1.3f + u * 5) * u * (1 - u) * 4;
    sy[k] = a * (by - 3) + b * c1y + c * ey;
    sr[k] = K * (14.0f - 3.5f * u);
  }
}
// ---- Audrey (UI-92): a man-eating plant in a pot at the foot of the screen. The pot is at (bx, by) (the middle of its rim), the head's hinge at
// (ex, ey) on the end of a curving stem; fx: 1 faces right, -1 left; tilt: the head's nose down (radians); open: the jaws (0 shut .. 1 wide); sway: the
// stem and leaves swaying (radians of phase); lump: where a swallowed thing is on its way down the stem (0 at the head .. 1 at the pot, <0 none)
inline void paint_audrey(const Rz &r, float bx, float by, float K, float fx, float ex, float ey, float tilt, float open, float sway, float lump, float tongue = 0.0f, float tph = 0.0f,
                        float *tipx = nullptr, float *tipy = nullptr, float tamp = 1.0f) {  // tongue: how far it is out of the mouth (px), undulating at phase tph (tamp: how much, 0 straight); tip: where its end is
  const uint16_t pot = c565(0xB5532E), potd = c565(0x8E3E22), crack = c565(0x5E2814), green = c565(0x3F9B45), greend = c565(0x2B7433), greenl = c565(0x6CC873), lipc = c565(0xD2354F),
                 tonguec = c565(0xF06A8A), tooth = c565(0xFFF8E8), spot = c565(0x2E7A38), band = c565(0x2F8238);
  float sx[11], sy[11], sr[11];
  audrey_stem(bx, by, K, fx, ex, ey, sway, sx, sy, sr);
  // leaves: low on the stem, fanned out round the pot, swaying
  struct Lf {
    int k;
    float an, len, wid;
  };
  const Lf LF[4] = {{3, fx * 1.0f, 56, 11}, {4, -fx * 1.0f, 56, 11}, {2, fx * 0.45f, 46, 9}, {5, -fx * 0.5f, 48, 9}};
  for (int lf = 0; lf < 4; lf++) {
    const float an = LF[lf].an + 0.12f * std::sin(sway * 1.1f + lf * 2.0f), len = K * LF[lf].len;
    const float x0 = sx[LF[lf].k], y0 = sy[LF[lf].k], x1 = x0 + std::sin(an) * len, y1 = y0 - std::cos(an) * len * 0.9f + len * 0.15f;
    {  // a leaf: a kite from its base to its tip, widest 40% of the way (filled triangles: much cheaper than fat soft-edged strokes)
      const float dx = x1 - x0, dy = y1 - y0, dl = std::max(1.0f, std::hypot(dx, dy)), nx = -dy / dl * K * LF[lf].wid * 1.15f, ny = dx / dl * K * LF[lf].wid * 1.15f;
      const float sx1 = x0 + dx * 0.4f + nx, sy1 = y0 + dy * 0.4f + ny, sx2 = x0 + dx * 0.4f - nx, sy2 = y0 + dy * 0.4f - ny;
      tri_fill(r, x0, y0, sx1, sy1, sx2, sy2, greend);
      tri_fill(r, sx1, sy1, x1, y1, sx2, sy2, greend);
      thin_line(r, x0, y0, x1, y1, greenl);  // the midrib
    }
  }
  // the pot, cracked
  tri_fill(r, bx - 24 * K, by, bx + 24 * K, by, bx - 15 * K, by + 24 * K, pot);
  tri_fill(r, bx + 24 * K, by, bx + 15 * K, by + 24 * K, bx - 15 * K, by + 24 * K, pot);
  tri_fill(r, bx + 6 * K, by, bx + 24 * K, by, bx + 15 * K, by + 24 * K, potd);
  tri_fill(r, bx - 27 * K, by - 2 * K, bx + 27 * K, by - 2 * K, bx - 27 * K, by + 7 * K, pot);
  tri_fill(r, bx + 27 * K, by - 2 * K, bx + 27 * K, by + 7 * K, bx - 27 * K, by + 7 * K, pot);
  tri_fill(r, bx - 27 * K, by + 5 * K, bx + 27 * K, by + 5 * K, bx - 27 * K, by + 7 * K, potd);
  tri_fill(r, bx + 27 * K, by + 5 * K, bx + 27 * K, by + 7 * K, bx - 27 * K, by + 7 * K, potd);
  thin_line(r, bx - 6 * K, by + 8 * K, bx - 2 * K, by + 13 * K, crack);
  thin_line(r, bx - 2 * K, by + 13 * K, bx - 8 * K, by + 17 * K, crack);
  thin_line(r, bx - 8 * K, by + 17 * K, bx - 5 * K, by + 22 * K, crack);
  {  // the stem as a strip (each step two triangles): much cheaper than ten fat soft-edged strokes
    auto nrm = [&](int k, float &nx, float &ny) {
      const int k0 = std::max(0, k - 1), k1 = std::min(10, k + 1);
      const float tx = sx[k1] - sx[k0], ty = sy[k1] - sy[k0], tl = std::max(1.0f, std::hypot(tx, ty));
      nx = -ty / tl, ny = tx / tl;
    };
    disc(r, sx[0], sy[0], sr[0], green);
    for (int k = 0; k < 10; k++) {
      float n0x, n0y, n1x, n1y;
      nrm(k, n0x, n0y);
      nrm(k + 1, n1x, n1y);
      tri_fill(r, sx[k] + n0x * sr[k], sy[k] + n0y * sr[k], sx[k + 1] + n1x * sr[k + 1], sy[k + 1] + n1y * sr[k + 1], sx[k] - n0x * sr[k], sy[k] - n0y * sr[k], green);
      tri_fill(r, sx[k + 1] + n1x * sr[k + 1], sy[k + 1] + n1y * sr[k + 1], sx[k + 1] - n1x * sr[k + 1], sy[k + 1] - n1y * sr[k + 1], sx[k] - n0x * sr[k], sy[k] - n0y * sr[k], green);
      disc(r, sx[k + 1], sy[k + 1], sr[k + 1], green);  // (a joint: the bends of the strip stay closed)
    }
  }
  if (lump >= 0.0f) {  // a swallowed thing going down: the stem bulges
    const float f = std::min(9.99f, (1.0f - lump) * 10.0f);  // (the stem's points run from the pot, 0, up to the head, 10: the lump goes from the head down)
    const int k = (int) f;
    const float lx = sx[k] + (sx[k + 1] - sx[k]) * (f - k), ly = sy[k] + (sy[k + 1] - sy[k]) * (f - k);
    disc(r, lx, ly, K * 16.0f, green);
    disc(r, lx - 3 * K * fx, ly - 2 * K, K * 9.0f, greenl, 18);
  }
  // the tentacles over the pot's rim: curling, swaying (a long one and a short one a side)
  for (int side = -1; side <= 1; side += 2)
    for (int tt = 0; tt < 2; tt++) {
      float x = bx + side * (21 - 9 * tt) * K, y = by - 1 * K, an = side * (1.0f - 0.55f * tt) + 0.1f * std::sin(sway + side + tt);
      const int n = 10 - 2 * tt;
      const float w0 = tt ? 4.6f : 6.0f;
      for (int q = 0; q < n; q++) {
        const float len = K * (9 - 0.3f * q) * (tt ? 0.8f : 1.0f), nx = x + std::sin(an) * len, ny = y - std::cos(an) * len;
        tapered(r, x, y, K * (w0 - 0.38f * q), nx, ny, K * (w0 - 0.38f * (q + 1)), green, 32, 32);
        x = nx, y = ny, an += side * (0.07f + 0.05f * q);  // (it curls over more as it goes)
      }
    }
  // the head: one round head (a disc) with the mouth cut out of it as a wedge from its middle, so the neck runs into the middle of its back and
  // cannot stick out past it; the wedge's two edges are the lips, with teeth along them pointing into the mouth; the head's axis tilted
  const float ct = std::cos(tilt), st_ = std::sin(tilt);
  constexpr float Rh = AUD_RH;  // the head's radius (units)
  auto HW = [&](float u, float v, float &x, float &y) {  // (the middle of the head, where the lips meet, is 28 units on from the stem's end: the neck ends well inside the disc)
    u += 28;
    x = ex + K * fx * (u * ct - v * st_);
    y = ey + K * (u * st_ + v * ct);
  };
  const float aj[2] = {-(0.04f + 0.80f * open), 0.05f + 0.98f * open};  // the upper and lower lips' angles from the head's axis
  constexpr float Ax = -14.0f;  // where the lips meet: 14 units behind the head's middle, so the wedge is deep and the mouth opens wide at the rim
  float cx0, cy0, ax_, ay_;
  HW(0, 0, cx0, cy0);
  HW(Ax, 0, ax_, ay_);
  float lt[2], thc[2];  // each lip's length (from where they meet to the rim) and the angle of its end on the rim, seen from the head's middle
  for (int j = 0; j < 2; j++) {
    const float du = std::cos(aj[j]), dv = std::sin(aj[j]), dd = du * Ax;
    lt[j] = -dd + std::sqrt(dd * dd - Ax * Ax + Rh * Rh);
    thc[j] = std::atan2(dv * lt[j], Ax + du * lt[j]);
  }
  {  // the head: a fan from where the lips meet round everything but the mouth's wedge
    const float sweep = 6.28318f - (thc[1] - thc[0]);
    float qx0, qy0;
    HW(std::cos(thc[0]) * Rh, std::sin(thc[0]) * Rh, qx0, qy0);
    for (int q = 1; q <= 44; q++) {
      const float an = thc[0] - sweep * q / 44;
      float qx1, qy1;
      HW(std::cos(an) * Rh, std::sin(an) * Rh, qx1, qy1);
      tri_fill(r, ax_, ay_, qx0, qy0, qx1, qy1, green);
      qx0 = qx1, qy0 = qy1;
    }
    float h0x, h0y;  // a soft highlight and a few spots on the hood
    HW(std::cos(aj[0] - 0.95f) * Rh * 0.52f, std::sin(aj[0] - 0.95f) * Rh * 0.52f, h0x, h0y);
    disc(r, h0x, h0y, K * Rh * 0.42f, greenl, 9);
    disc(r, h0x, h0y, K * Rh * 0.24f, greenl, 9);
    for (int q = 0; q < 3; q++) {
      float sx0, sy0;
      HW(std::cos(aj[0] - 0.55f - 0.42f * q) * Rh * 0.66f, std::sin(aj[0] - 0.55f - 0.42f * q) * Rh * 0.66f, sx0, sy0);
      disc(r, sx0, sy0, K * (3.6f - 0.6f * q), spot);
    }
  }
  if (tongue > 1.0f) {  // the tongue slithers out of the mouth, along the middle of the opening, in a wave that grows towards its tip
    const float ab = (aj[0] + aj[1]) / 2, bu = std::cos(ab), bv = std::sin(ab), nu = -bv, nv = bu, lu = tongue / K;
    const int N = 16;
    float qx = 0, qy = 0;
    for (int k = 0; k <= N; k++) {
      const float sdist = lu * k / N, wave = 6.5f * tamp * std::sin(tph - sdist * 0.11f) * std::min(1.0f, sdist / 28.0f), u = 4 + sdist;
      float x, y;
      HW(Ax + bu * u + nu * wave, bv * u + nv * wave, x, y);
      if (k > 0) {
        tapered(r, qx, qy, K * (5.8f - 2.4f * (k - 1) / N), x, y, K * (5.8f - 2.4f * k / N), tonguec, 32, 32);
        thin_line(r, qx, qy, x, y, c565(0xC8456A));  // (the groove down its middle)
      }
      qx = x, qy = y;
    }
    disc(r, qx, qy, K * 3.4f, tonguec);  // (a rounded tip)
    if (tipx)
      *tipx = qx, *tipy = qy;
  }
  for (int j = 0; j < 2; j++) {  // the lips: the wedge's two edges, from the middle out to the rim
    float ex1, ey1;
    HW(Ax + std::cos(aj[j]) * lt[j], std::sin(aj[j]) * lt[j], ex1, ey1);
    tapered(r, ax_, ay_, K * 4.4f, ex1, ey1, K * 3.6f, lipc, 32, 32);
  }
  for (int j = 0; j < 2; j++)  // the teeth: evenly spaced triangles along each lip, pointing into the mouth (the lower row between the upper ones)
    for (int i = 0; i < 6; i++) {
      const float s0 = 0.20f + 0.135f * i + (j ? 0.065f : 0.0f), s1 = s0 + 0.07f, sm = (s0 + s1) / 2, h = 13.0f - 0.8f * i;
      const float d_u = std::cos(aj[j]), d_v = std::sin(aj[j]), i_u = j ? d_v : -d_v, i_v = j ? -d_u : d_u;  // (i: into the mouth)
      float ax0, ay0, ax1, ay1, tx, ty;
      HW(Ax + d_u * lt[j] * s0, d_v * lt[j] * s0, ax0, ay0);
      HW(Ax + d_u * lt[j] * s1, d_v * lt[j] * s1, ax1, ay1);
      HW(Ax + d_u * lt[j] * sm + i_u * h, d_v * lt[j] * sm + i_v * h, tx, ty);
      tri_fill(r, ax0, ay0, ax1, ay1, tx, ty, tooth);
    }
}

// ---- Audrey's visit (UI-92): Audrey grows out of its pot at the foot of the screen; for each of 2 to 4 small things on the map (satellites, the ISS, bright
// stars) it reaches its head over to it (turning round if it must), opens wide, slithers its tongue out to it, grabs it, and reels it in and swallows it (a lump
// runs down the stem). When it has eaten them all it settles, they regrow (a sparkle), it says a line, and sinks back into the pot.
struct AudreyPose {
  float hx, hy, tilt, fx, open, tongue, tamp, lump, ph, tph;  // the head's hinge (the stem's end), its tilt, which way it faces, the jaws, the tongue (px), how much it waves, the swallowed lump, the sway
};
inline void audrey_hinge(float t, float &hx, float &hy, float &tilt, float &fx) {
  int j = 0;
  for (int i = 0; i < pl.n; i++)
    if (t >= pl.s[i].t0)
      j = i;
  const AudreySeg &g = pl.s[j];
  const float u = std::min(1.0f, std::max(0.0f, (t - g.t0) / std::max(0.01f, g.t1 - g.t0))), k = u * u * (3 - 2 * u);
  hx = g.hx0 + (g.hx1 - g.hx0) * k, hy = g.hy0 + (g.hy1 - g.hy0) * k, tilt = g.tl0 + (g.tl1 - g.tl0) * k;
  fx = g.f0 != g.f1 ? g.f0 * std::cos(3.14159f * ease(std::min(1.0f, std::max(0.0f, (t - g.t0) / 0.6f)))) : g.f1;  // (a turn: its head's length shortens through it)
}
inline void audrey_pose(float t, AudreyPose &p) {
  audrey_hinge(t, p.hx, p.hy, p.tilt, p.fx);
  p.hx += 4 * std::sin(t * 1.3f), p.hy += 3 * std::sin(t * 1.9f);  // (it never holds still)
  p.open = 0, p.tongue = 0, p.tamp = 1, p.lump = -1;
  for (int i = 0; i < n_ctg; i++) {
    const float u = t - pl.t_open[i], tc = pl.pull[i];
    float o = u < 0 ? 0.0f : u < 0.5f ? ease(u / 0.5f) : t < tc + 0.35f ? 1.0f : t < tc + 0.5f ? 1 - ease((t - tc - 0.35f) / 0.15f) : 0.0f;
    p.open = std::max(p.open, o);
    if (t >= pl.t_out[i] && t < tc + 0.05f) {  // out to the thing
      p.tongue = AUD_DT * ease((t - pl.t_out[i]) / 0.55f);
      p.tamp = std::min(1.0f, std::max(0.0f, (tc - t) / 0.45f));
    } else if (t >= tc + 0.05f && t < tc + 0.35f) {  // reeling it in
      p.tongue = AUD_DT * (1 - ease((t - tc - 0.05f) / 0.3f));
      p.tamp = std::min(1.0f, (t - tc) / 0.15f);
    }
    if (t >= tc + 0.45f && t < tc + 1.7f)
      p.lump = (t - tc - 0.45f) / 1.25f;
  }
  p.ph = t * 1.6f, p.tph = t * 9.0f;
}
inline void audrey_plan() {
  const float K = AUD_K;
  // the pot's place: not far from the middle; targets too near the pot are left (it could not reach them)
  pl.bx = 170 + (float) (rnd() % 140), pl.by = 412;
  for (int i = n_ctg; i-- > 0;)
    if (std::hypot(ctg[i].x - pl.bx, ctg[i].y - (pl.by - 80)) < 110 || ctg[i].y > pl.by - 70) {
      ctg[i] = ctg[--n_ctg];
    }
  int order[4], n = 0, done[4] = {};
  float px = pl.bx, py = pl.by - 150;
  for (int k = 0; k < n_ctg; k++) {  // the nearest to the head first
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < n_ctg; i++)
      if (!done[i] && std::hypot(ctg[i].x - px, ctg[i].y - py) < bd)
        bd = std::hypot(ctg[i].x - px, ctg[i].y - py), best = i;
    done[best] = 1, order[n++] = best, px = ctg[best].x, py = ctg[best].y;
  }
  // where its head must be for each: the hinge is a tongue's length (and the lips' reach) behind the thing, along the tongue's line
  float hx_[4], hy_[4], tl_[4], f_[4];
  for (int k = 0; k < n; k++) {
    const CatTgt &g = ctg[order[k]];
    const float f = g.x >= pl.bx ? 1.0f : -1.0f;
    float tilt = 0.3f, hx = pl.bx, hy = pl.by - 150;
    for (int it = 0; it < 4; it++) {  // (the tongue's angle depends on where the head is: a few rounds settle it)
      const float th = tilt + 0.09f, ct = std::cos(tilt), st = std::sin(tilt);
      hx = g.x - f * (AUD_DT + 4 * K) * std::cos(th) - f * K * 14 * ct, hy = g.y - (AUD_DT + 4 * K) * std::sin(th) - K * 14 * st;
      const float ax = hx + f * K * 14 * ct, ay = hy + K * 14 * st;  // (where the lips meet)
      tilt = std::max(-0.6f, std::min(0.9f, std::atan2(g.y - ay, (g.x - ax) * f) - 0.09f));
    }
    hy = std::min(hy, pl.by - 70);
    hx_[k] = hx, hy_[k] = hy, tl_[k] = tilt, f_[k] = f;
  }
  pl.n = 0;
  float t = 0, cx = pl.bx, cy = pl.by - 14, ct_ = 0.3f, cf = n > 0 ? f_[0] : 1.0f;
  auto add = [&](float dur, float nx, float ny, float ntl, float nf) {
    AudreySeg &g = pl.s[pl.n++];
    g = {t, t + dur, cx, cy, nx, ny, ct_, ntl, cf, nf};
    t += dur, cx = nx, cy = ny, ct_ = ntl, cf = nf;
  };
  add(1.4f, pl.bx + cf * 10, pl.by - 150, 0.25f, cf);  // it grows out of the pot
  pl.t_stalk = t;
  for (int k = 0; k < n; k++) {
    const int i = order[k];
    const float dist = std::hypot(hx_[k] - cx, hy_[k] - cy);
    pl.t_open[i] = t + 0.3f;
    const float d = std::min(1.8f, std::max(0.9f, dist / 200));
    add(d, hx_[k], hy_[k], tl_[k], f_[k]);  // it reaches over
    pl.t_out[i] = t;
    pl.pull[i] = t + 0.55f;
    add(1.05f, hx_[k], hy_[k], tl_[k], f_[k]);  // tongue out, grab, reel in, jaws shut
  }
  add(1.0f, pl.bx + cf * 10, pl.by - 150, 0.25f, cf);  // it settles
  pl.t_regrow = t + 0.2f;
  for (int k = 0; k < n; k++)
    pl.regrow[order[k]] = pl.t_regrow + 0.35f * k;
  const float t_after = pl.t_regrow + 0.35f * n + 1.6f;
  add(std::max(0.5f, t_after - t), cx, cy, ct_, cf);  // (it waits while they regrow and it speaks)
  pl.t_sink = t;
  add(1.3f, pl.bx, pl.by - 14, 0.3f, cf);  // it sinks back into the pot
  pl.t_end = t + 0.1f;
  for (int i = 0; i < 4; i++)
    pl.back[i] = false;
}
inline void patch_apply(int i, bool gone);
inline void audrey_events(float t) {  // a thing it grabs comes off the map; it is back (with a sparkle) when it regrows
  for (int i = 0; i < n_ctg; i++) {
    if (!ctg[i].gone && t >= pl.pull[i]) {
      ctg[i].gone = true;
      vlogf("Audrey t %.1f: grabs target %d (kind %d)", t, i, (int) ctg[i].kind);
      if (pt_ok)
        patch_apply(i, true);
    } else if (!ctg[i].gone && ctg[i].o[0]) {
      lv_area_t a;
      lv_obj_get_coords(ctg[i].o[0], &a);
      ctg[i].x = (a.x1 + a.x2) / 2.0f, ctg[i].y = (a.y1 + a.y2) / 2.0f;
    }
    if (ctg[i].gone && !pl.back[i] && t >= pl.regrow[i]) {
      pl.back[i] = true;
      if (pt_ok)
        patch_apply(i, false);
    }
  }
}
inline void audrey_box(float t, int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2) {  // everything it paints: the pot with its tentacles, the stem with its leaves, the head, the tongue, the sparkles
  AudreyPose p;
  audrey_pose(t, p);
  const float K = AUD_K, fs = p.fx >= 0 ? 1.0f : -1.0f;
  float sx[11], sy[11], sr[11];
  audrey_stem(pl.bx, pl.by, K, fs, p.hx, p.hy, p.ph, sx, sy, sr);
  float x0 = pl.bx - 142, xe = pl.bx + 142, y0 = pl.by - 126, ye = pl.by + 6 + 24 * K;  // the pot and the tentacles curling over its rim
  auto grow = [&](float a, float b, float c, float e) { x0 = std::min(x0, a), y0 = std::min(y0, b), xe = std::max(xe, c), ye = std::max(ye, e); };
  for (int k = 0; k <= 10; k++)  // the stem
    grow(sx[k] - sr[k] - 4, sy[k] - sr[k] - 4, sx[k] + sr[k] + 4, sy[k] + sr[k] + 4);
  for (int k = 2; k <= 5; k++)  // the leaves, up to 74 units long, swaying, sweeping out to either side and a little down
    grow(sx[k] - 110, sy[k] - 100, sx[k] + 110, sy[k] + 30);
  const float ct = std::cos(p.tilt), st = std::sin(p.tilt), Rh = K * (AUD_RH + 6);  // the head: its disc round its middle, 28 units on from the hinge
  const float hcx = p.hx + fs * K * 28 * ct, hcy = p.hy + K * 28 * st;
  grow(hcx - Rh, hcy - Rh, hcx + Rh, hcy + Rh);
  if (p.tongue > 1.0f) {  // the tongue: out from the lips' meeting point along the tilted axis (a box round its length)
    const float th = p.tilt + 0.09f, ex2 = p.hx + fs * (K * 14 * ct + (p.tongue + 20) * std::cos(th)), ey2 = p.hy + K * 14 * st + (p.tongue + 20) * std::sin(th);
    grow(ex2 - 26, ey2 - 26, ex2 + 26, ey2 + 26);
  }
  for (int i = 0; i < n_ctg; i++)
    if (t > pl.regrow[i] - 0.05f && t < pl.regrow[i] + 0.75f)
      grow(ctg[i].x - 32, ctg[i].y - 32, ctg[i].x + 32, ctg[i].y + 32);  // the sparkle
  x1 = (int32_t) x0 - 6, y1 = (int32_t) y0 - 6, x2 = (int32_t) xe + 6, y2 = (int32_t) ye + 6;
}
inline void paint_audrey_visit(const Rz &r, float t) {
  AudreyPose p;
  audrey_pose(t, p);
  float tipx = p.hx, tipy = p.hy;
  paint_audrey(r, pl.bx, pl.by, AUD_K, p.fx >= 0 ? 1.0f : -1.0f, p.hx, p.hy, p.tilt, p.open, p.ph, p.lump, p.tongue, p.tph, &tipx, &tipy, p.tamp);
  for (int i = 0; i < n_ctg; i++) {  // a thing on the tongue's end, shrinking as it is reeled in; a sparkle where it is back
    const float tc = pl.pull[i], q = (t - pl.regrow[i]) / 0.6f;
    if (t >= tc && t < tc + 0.35f) {
      const float rad = std::max(2.0f, ctg[i].r * 0.45f) * (1 - 0.8f * ease((t - tc - 0.25f) / 0.1f));
      disc(r, tipx, tipy, rad + 2.5f, c565(0xDDE8FF), 8);
      disc(r, tipx, tipy, rad, c565(ctg[i].kind == 5 ? 0xFFFFFF : 0xE8F0FF));
    }
    if (q >= 0 && q < 1)
      ring_fill(r, ctg[i].x, ctg[i].y, 5 + 20 * q, 3 + 19 * q, c565(0xFFF0B0), (uint32_t) (22 * (1 - q)));
  }
}
inline const char *const SAY_AUDREY_BEFORE[] = {"Feed me!", "Feed me, Seymour!", "I'm hungry.\nFeed me.", "Is that dinner?"};
inline const char *const SAY_AUDREY_LEAVE[] = {"Mmm. More.", "Burp. Pardon me.", "That was just\na starter.", "Now we're talking."};
inline int last_audrey_before = -1, last_audrey_leave = -1;
inline void audrey_lines() {
  st.say[0] = nullptr;
  st.say[1] = pick_line(SAY_AUDREY_BEFORE, sizeof(SAY_AUDREY_BEFORE) / sizeof(*SAY_AUDREY_BEFORE), last_audrey_before);
  st.say[2] = pick_line(SAY_AUDREY_LEAVE, sizeof(SAY_AUDREY_LEAVE) / sizeof(*SAY_AUDREY_LEAVE), last_audrey_leave);
}

// ---- the speech bubble now: its box (the same maths for the painter and the redraw box)
inline int cat_side = 0;  // 0 the bubble is above the cat, 1 to its right (tail to the left), -1 to its left
inline bool spk_fixed = false;  // (the preview renders: the bubble's tail at a given place)
inline float spk_x = 0, spk_y = 0;
inline void speaker(float t, float &x, float &y) {
  if (spk_fixed) {
    x = spk_x, y = spk_y;
    return;
  }  // where the bubble's tail points: the octopus or the UFO, or the top of the cat's ears
  if (critter == CR_AUDREY) {  // above its head, on its course without the bob; held where it was when it starts to sink into its pot
    float hx, hy, tl, f;
    audrey_hinge(std::min(t, pl.t_sink), hx, hy, tl, f);
    x = hx + (f >= 0 ? 1.0f : -1.0f) * AUD_K * 22, y = hy - AUD_K * 70;
    return;
  }
  if (critter == CR_AXO) {  // above its head, on its course without the bob; held where it was when it starts to swim off
    float fx, d;
    axo_pos(std::min(t, ap.t_exit), x, y, fx, d, true);
    x += (fx >= 0 ? 1.0f : -1.0f) * AXO_K * 26, y -= AXO_K * 38;
    return;
  }
  if (critter != CR_CAT) {
    pos(t, x, y);
    return;
  }
  t = std::min(t, cp.t_leave);  // (the bubble stays where it was when the cat flies off, whichever way)
  cat_xy(t, x, y, true);
  y -= (74 + 26 * CAT_K) * cat_scale(t);
}
inline bool cow_speaks(int beat) { return critter == CR_UFO && ufo_item == IT_COW && beat == 1; }  // the cow's own line, as it is lifted
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
  speaker(t, cx, cy);
  bx = std::max(10.0f, std::min(480 - bw - 10, cx - bw / 2 + 30));
  const float above = critter == CR_UFO ? 30 * UFO_K : (46 + 66) * SC;  // (the UFO's dome tops out at 30 units)
  cat_side = 0;
  if (critter == CR_AUDREY) {  // above its head if there is room, else beside it
    by = std::max(10.0f, cy - bh - 22);
    cat_side = cy - bh - 22 < 10 ? (cx < 240 ? 1 : -1) : 0;
    if (cat_side != 0) {
      bx = cat_side > 0 ? cx + 90 : cx - 90 - bw;
      bx = std::max(30.0f, std::min(480 - bw - 10, bx));
      by = std::max(46.0f, std::min(430.0f - bh, cy + AUD_K * 70 - bh / 2));
    }
  } else if (critter == CR_AXO) {  // above its head if there is room, else beside it
    by = std::max(10.0f, cy - bh - 22);
    cat_side = cy - bh - 22 < 10 ? (cx < 240 ? 1 : -1) : 0;
    if (cat_side != 0) {
      bx = cat_side > 0 ? cx + 90 : cx - 90 - bw;
      bx = std::max(30.0f, std::min(480 - bw - 10, bx));
      by = std::max(46.0f, std::min(430.0f - bh, cy + AXO_K * 38 - bh / 2));
    }
  } else if (critter == CR_CAT) {  // above its ears if there is room, else beside it, level with its body (the tail points at it)
    by = std::max(10.0f, cy - bh - 22);
    float hx, hy;
    cat_xy(std::min(t, cp.t_leave), hx, hy, true);  // (its course without the weave: the bubble does not bob with it)
    cat_side = cy - bh - 22 < 10 ? (hx < 240 ? 1 : -1) : 0;
    if (cat_side != 0) {
      const float S = cat_scale(std::min(t, cp.t_leave));
      bx = cat_side > 0 ? hx + 100 * S + 34 : hx - 100 * S - 34 - bw;
      bx = std::max(30.0f, std::min(480 - bw - 10, bx));
      by = std::max(46.0f, std::min(430.0f - bh, hy - 74 * S - bh / 2));
    }
  } else
    by = beat == N_BEATS - 1 ? std::max(46.0f, cy - 120) : std::max(46.0f, cy - above - bh - 22);
  if (cow_speaks(beat) && n_ctg > 0) {  // beside the beam, level with the cow, the tail pointing at it
    const float k = seg(t, UFO_LIFT + 0.1f, 6.6f), y0 = ctg[0].y, y1 = cy + BEAM_TOP + 6;
    bx = std::min(480 - bw - 10, cx + 100);
    by = std::max(46.0f, y0 + (y1 - y0) * k - bh / 2);
  }
  const float pop = ease((t - beat_a(beat)) / 0.18f);
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
  const float tt = critter == CR_UFO ? st.t - ufo_delay : st.t;  // (the UFO's own clock)
  float cx, cy;
  speaker(tt, cx, cy);
  lv_draw_triangle_dsc_t tri;
  lv_draw_triangle_dsc_init(&tri);
  if (cow_speaks(beat_at(tt)) || ((critter == CR_CAT || critter == CR_AXO || critter == CR_AUDREY) && cat_side != 0)) {  // the tail comes out of the bubble's side towards the cow or cat
    const float my = by + bh / 2, d = (critter == CR_CAT || critter == CR_AXO || critter == CR_AUDREY) && cat_side < 0 ? -1.0f : 1.0f, x0 = d > 0 ? bx : bx + bw;  // (d: 1 the tail is on the left)
    tri.color = lv_color_hex(0x3A4E86);
    tri.p[0].x = x0 + 3 * d, tri.p[0].y = my - 11;
    tri.p[1].x = x0 + 3 * d, tri.p[1].y = my + 11;
    tri.p[2].x = x0 - 24 * d, tri.p[2].y = my + 6;
    lv_draw_triangle(layer, &tri);
    tri.color = lv_color_hex(0x0E1836);
    tri.p[0].x = x0 + 3 * d, tri.p[0].y = my - 7;
    tri.p[1].x = x0 + 3 * d, tri.p[1].y = my + 7;
    tri.p[2].x = x0 - 17 * d, tri.p[2].y = my + 5;
    lv_draw_triangle(layer, &tri);
    rect(layer, d > 0 ? bx : bx + bw - 4, my - 7, d > 0 ? bx + 4 : bx + bw, my + 7, 0x0E1836, 0);  // the seam
  } else {
    const float tx = std::min(cx, 440.0f);
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
  }
  lv_draw_label_dsc_t l;
  lv_draw_label_dsc_init(&l);
  l.font = rocket::st.font;
  l.color = lv_color_hex(0xEEF2FF);
  l.text = st.say[beat_at(tt)];
  const lv_area_t a = {(int32_t) (bx + 14), (int32_t) (by + 9), (int32_t) (bx + bw + 40), (int32_t) (by + bh)};
  lv_draw_label(layer, &l, &a);
}

// the meal's figure: its lines (on the screen), the points the arms hold, where it hangs the octopus
inline void gather_fig() {
  lv_area_t sb;
  lv_obj_get_coords(ui.w.sky, &sb);
  fig.n = 0;
  for (const auto &g : ui.star_segs)
    if (g.poly >= ui.eat_lo && g.poly < ui.eat_hi && fig.n < 48)
      fig.s[fig.n++] = {(float) (sb.x1 + g.x0), (float) (sb.y1 + g.y0), (float) (sb.x1 + g.x1), (float) (sb.y1 + g.y1), g.major};
  float qx[96], qy[96];
  int nq = 0;
  fig.bx0 = fig.by0 = 1e9f, fig.bx1 = fig.by1 = -1e9f;
  for (int i = 0; i < fig.n; i++)
    for (int e = 0; e < 2; e++) {
      const float x = e ? fig.s[i].x1 : fig.s[i].x0, y = e ? fig.s[i].y1 : fig.s[i].y0;
      fig.bx0 = std::min(fig.bx0, x), fig.bx1 = std::max(fig.bx1, x), fig.by0 = std::min(fig.by0, y), fig.by1 = std::max(fig.by1, y);
      bool dup = false;
      for (int k = 0; k < nq && !dup; k++)
        dup = std::fabs(qx[k] - x) < 4 && std::fabs(qy[k] - y) < 4;
      if (!dup && nq < 96)
        qx[nq] = x, qy[nq++] = y;
    }
  // the points, left to right (a bubble sort: nq is small), five of them spread out, each held by an arm from 1 to 6
  for (int a = 0; a < nq; a++)
    for (int b = a + 1; b < nq; b++)
      if (qx[b] < qx[a])
        std::swap(qx[a], qx[b]), std::swap(qy[a], qy[b]);
  fig.n_anch = std::min(5, nq);
  for (int j = 0; j < fig.n_anch; j++) {
    const int k = fig.n_anch == 1 ? nq / 2 : (int) std::lround(j * (nq - 1.0f) / (fig.n_anch - 1));
    fig.px[j] = qx[k], fig.py[j] = qy[k];
    fig.arm[j] = fig.n_anch == 1 ? 3 : 1 + (int) std::lround(j * 5.0f / (fig.n_anch - 1));
  }
  // it hangs above the figure, arms down to it; a figure too high on the screen for that (the mantle would be off the top)
  // has it beside the figure, towards the middle of the map, reaching across to pluck it
  const float gx = (fig.bx0 + fig.bx1) / 2, gy = (fig.by0 + fig.by1) / 2;
  if (fig.by0 - 65 >= 110) {
    hover_x = std::max(150.0f, std::min(330.0f, gx));
    hover_y = std::min(300.0f, fig.by0 - 65);
  } else {
    hover_x = std::max(110.0f, std::min(370.0f, gx + (gx < 240 ? 150.0f : -150.0f)));
    hover_y = std::max(125.0f, std::min(300.0f, gy + 25));
  }
}
inline void undo_meal() {
  if (st.ate)
    const_eat(-1);  // the figure is back
  st.ate = false;
  st.meal = M_NONE;
  is_meal = false;
  hover_x = 260, hover_y = 262;
  fig.n = fig.n_anch = 0;
}
inline void fig_line(const Rz &r, float x0, float y0, float x1, float y1, uint16_t c, bool thick) {
  const float dx = x1 - x0, dy = y1 - y0;
  const int n = (int) (std::max(std::fabs(dx), std::fabs(dy)) * 1.5f) + 1;
  for (int i = 0; i <= n; i++) {
    const float x = x0 + dx * i / n, y = y0 + dy * i / n;
    if (thick)
      disc(r, x, y, 1.0f, c);
    else
      px_a(r, (int) std::lround(x), (int) std::lround(y), c, 32);
  }
}
// the figure as the arms have it: lifted and grown, then every point on its way to the beak (gone, shrunk to nothing, by EAT_T)
inline void paint_fig(const Rz &r, float t, const Model &m) {
  if (!is_meal || fig.n == 0 || t >= EAT_T)
    return;
  const float mx = m.cx, my = m.cy + 6 * SC, held = seg(t, REACH0, PULL0);
  for (int i = 0; i < fig.n; i++) {
    const FigSeg &g = fig.s[i];
    const uint32_t col = mix3(g.major ? sat::C_FIG_MAJOR : sat::C_FIG, 0xFFC857, held);  // it lights up as it is held
    float x0, y0, x1, y1;
    fig_pt(t, g.x0, g.y0, mx, my, x0, y0);
    fig_pt(t, g.x1, g.y1, mx, my, x1, y1);
    fig_line(r, x0, y0, x1, y1, c565(col), g.major || held > 0.3f);
  }
}
inline void fig_box(float t, const Model &m, int32_t &x1, int32_t &y1, int32_t &x2, int32_t &y2) {  // grows the redraw box over the figure
  if (!is_meal || fig.n == 0 || t >= EAT_T)
    return;
  const float mx = m.cx, my = m.cy + 6 * SC;
  float ax, ay, bx, by;  // (the transform is a scaling and a pull: the corners are enough)
  fig_pt(t, fig.bx0, fig.by0, mx, my, ax, ay);
  fig_pt(t, fig.bx1, fig.by1, mx, my, bx, by);
  x1 = std::min<int32_t>(x1, (int32_t) std::min(ax, bx) - 5), y1 = std::min<int32_t>(y1, (int32_t) std::min(ay, by) - 5);
  x2 = std::max<int32_t>(x2, (int32_t) std::max(ax, bx) + 5), y2 = std::max<int32_t>(y2, (int32_t) std::max(ay, by) + 5);
}

inline void on_refr(lv_event_t *);
inline void frame();

inline void cat_undo();
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
  vlogf("end at %.1f s, %u frames, %u ms", st.t, (unsigned) st.frames, (unsigned) ms);
  if (st.frames > 1 && ms) {
    rocket::set_last_fps(st.frames * 1000.0f / ms);
    for (int i = 0; i < 20; i++)
      if (pb[i].n)
        ESP_LOGI("octo", "second %d: %u frames, painting %.1f ms a frame (longest %.1f), longest gap %u ms", i, (unsigned) pb[i].n, pb[i].sum / 1000.0f / pb[i].n, pb[i].mx / 1000.0f, (unsigned) pb[i].gap);
    ESP_LOGI("octo", "visit: %u frames in %u ms (%.1f fps), longest gap %u ms; painting %.1f ms a frame (longest %.1f ms), %u px a frame",
             (unsigned) st.frames, (unsigned) ms, st.frames * 1000.0f / ms, (unsigned) st.max_gap, st.paint_us / 1000.0f / st.frames,
             st.paint_max / 1000.0f, (unsigned) (st.px / st.frames));
  }
  for (lv_obj_t **o : {&st.cv, &st.catcher})
    if (*o) {
      lv_obj_delete(*o);
      *o = nullptr;
    }
  undo_meal();
  cat_undo();
  rocket::st.snap = st.clean;  // the backdrop shows the composite meanwhile; rocket::stop() frees the clean one
  if (st.comp)
    lv_draw_buf_destroy(st.comp);
  st.comp = st.clean = nullptr;
  patches_free();
  rocket::stop();  // the backdrop (and its snapshot)
  st.running = false;
}

inline void inval(const lv_area_t &a) {
  if (rocket::st.back && a.x2 >= a.x1 && a.y2 >= a.y1)
    lv_obj_invalidate_area(rocket::st.back, &a);
}

// UI-90: what the cat can shoot and the UFO can take: the Sun, the Moon, a planet, the ISS or another satellite that is on the map now; `want` of
// them (fewer if fewer are up), at random
// the cat always has `want` targets: bright stars on the map (not under it) make up what the map has too few of
inline void cat_fill_stars(int want) {
  lv_area_t sb = {0, 0, 0, 0};
  if (ui.w.sky)
    lv_obj_get_coords(ui.w.sky, &sb);
  struct S {
    float x, y, r;
  } c[40];
  int n = 0;
  if (ui.stars_shown)
    for (const auto &p : ui.star_pts) {
      const float x = (float) (sb.x1 + p.x), y = (float) (sb.y1 + p.y);
      if (n < 40 && p.size >= 3 && std::hypot((float) (p.x - ui.cx), (float) (p.y - ui.cy)) < ui.radius * 0.85f && !cat_under(x, y, (float) p.size)) {
        bool dup = false;
        for (int i = 0; i < n_ctg; i++)
          dup = dup || (std::fabs(ctg[i].x - x) < 40 && std::fabs(ctg[i].y - y) < 40);
        if (!dup)
          c[n++] = {x, y, (float) p.size};
      }
    }
  while (n_ctg < want && n > 0 && n_ctg < 4) {
    const int k = (int) (rnd() % (uint32_t) n);
    ctg[n_ctg++] = {c[k].x, c[k].y, c[k].r, {nullptr, nullptr}, false, 5, 0};
    c[k] = c[--n];
  }
}
inline void cat_targets(int want, bool avoid_cat = false, bool small = false) {  // small: only the ISS and satellites (the axolotl's)
  struct C {
    lv_obj_t *o[2];
    uint8_t kind;
    int8_t pl;
  } c[2 + planets::N_PLANETS + 4];
  int n = 0;
  auto up = [](lv_obj_t *o) { return o != nullptr && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); };
  if (!small && up(ui.sun))
    c[n++] = {{ui.sun, nullptr}, 0, 0};
  if (!small && up(ui.moon))
    c[n++] = {{ui.moon, nullptr}, 1, 0};
  for (int p = 0; p < planets::N_PLANETS && !small; p++)
    if (up(ui.planet_img[p]))
      c[n++] = {{ui.planet_img[p], ui.planet_lbl[p]}, 2, (int8_t) p};
  if (up(ui.iss.dot))
    c[n++] = {{ui.iss.dot, ui.iss.label}, 3, 0};
  int ns = 0;
  for (const Marker &m : ui.sats)
    if (ns < 3 && m.up && up(m.dot))
      c[n++] = {{m.dot, m.label}, 4, 0}, ns++;
  if (avoid_cat)  // (the cat shoots only what is not under it)
    for (int k = n; k-- > 0;) {
      lv_area_t a;
      lv_obj_get_coords(c[k].o[0], &a);
      if (cat_under((a.x1 + a.x2) / 2.0f, (a.y1 + a.y2) / 2.0f, std::max(8.0f, (a.x2 - a.x1) / 2.0f)))
        c[k] = c[--n];
    }
  n_ctg = 0;
  while (want-- > 0 && n > 0 && n_ctg < 4) {
    const int k = (int) (rnd() % (uint32_t) n);
    CatTgt &g = ctg[n_ctg++];
    g.o[0] = c[k].o[0], g.o[1] = c[k].o[1], g.gone = false, g.kind = c[k].kind, g.pl = c[k].pl;
    c[k] = c[--n];
    lv_area_t a;
    lv_obj_get_coords(g.o[0], &a);
    g.x = (a.x1 + a.x2) / 2.0f, g.y = (a.y1 + a.y2) / 2.0f, g.r = std::max(8.0f, (a.x2 - a.x1) / 2.0f);
  }
}
// a target off the map (on true) or back on it: an object is hidden, a star is not drawn, a constellation's figure and name are not drawn
inline void tgt_hide(const CatTgt &g, bool on) {
  if (g.kind <= 4) {
    for (lv_obj_t *o : g.o)
      if (o) {
        if (on)
          lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else
          lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
      }
  } else if (g.kind == 5) {
    const int k = std::max(0, std::min(3, (int) (&g - ctg)));  // (a slot for each target)
    ui.star_skip_x[k] = on ? (int) lroundf(g.x - disc_ox) : INT_MIN, ui.star_skip_y[k] = on ? (int) lroundf(g.y - disc_oy) : INT_MIN;
  } else if (g.kind == 6) {
    const_eat(on ? g.pl : -1);
  }
}
// UI-90: what the UFO takes this visit: one of (at random, among what there is) a thing on the map, a bright star, a constellation's figure, a cow
inline void ufo_pick() {
  cat_targets(1);
  const bool obj = n_ctg > 0;
  lv_area_t sb = {0, 0, 0, 0};
  if (ui.w.sky)
    lv_obj_get_coords(ui.w.sky, &sb);
  // a bright star well inside the disc (the Nth that qualifies, chosen below)
  auto star_ok = [&](const Ui::StarPt &p) {
    return ui.stars_shown && p.size >= 3 && std::hypot((float) (p.x - ui.cx), (float) (p.y - ui.cy)) < ui.radius * 0.85f;
  };
  int n_star = 0;
  for (const auto &p : ui.star_pts)
    n_star += star_ok(p);
  // a constellation whose figure (at least three lines) and name are on the map and inside the disc
  int n_const = 0, const_idx[sky::N_NAMES];
  if (ui.stars_shown)
    for (int idx = 0; idx < sky::N_NAMES && n_const < sky::N_NAMES; idx++) {
      int lo = 0;
      for (int j = 0; j < idx; j++)
        lo += sky::CFIG[sky::CFIG_OFF[j]];
      const int hi = lo + sky::CFIG[sky::CFIG_OFF[idx]];
      int cnt = 0;
      bool inside = true;
      for (const auto &g : ui.star_segs)
        if (g.poly >= lo && g.poly < hi) {
          cnt++;
          inside = inside && std::hypot((float) (g.x0 - ui.cx), (float) (g.y0 - ui.cy)) < ui.radius * 0.92f && std::hypot((float) (g.x1 - ui.cx), (float) (g.y1 - ui.cy)) < ui.radius * 0.92f;
        }
      bool named = false;
      for (const auto &nt : ui.names)
        named = named || (nt.idx == idx && nt.shown);
      if (cnt >= 3 && cnt <= 48 && inside && named)
        const_idx[n_const++] = idx;
    }
  int opt[4], n = 0;
  if (obj)
    opt[n++] = 0;
  if (n_star)
    opt[n++] = 1;
  if (n_const)
    opt[n++] = 2;
  opt[n++] = 3;
  const int pick = opt[rnd() % (uint32_t) n];
  if (pick == 1) {
    int k = (int) (rnd() % (uint32_t) n_star);
    for (const auto &p : ui.star_pts)
      if (star_ok(p) && k-- == 0) {
        ctg[0] = {sb.x1 + (float) p.x, sb.y1 + (float) p.y, (float) p.size, {nullptr, nullptr}, false, 5, 0};
        break;
      }
    n_ctg = 1;
  } else if (pick == 2) {
    const int idx = const_idx[rnd() % (uint32_t) n_const];
    int lo = 0;
    for (int j = 0; j < idx; j++)
      lo += sky::CFIG[sky::CFIG_OFF[j]];
    const int hi = lo + sky::CFIG[sky::CFIG_OFF[idx]];
    fig.n = 0;
    for (const auto &g : ui.star_segs)
      if (g.poly >= lo && g.poly < hi && fig.n < 48)
        fig.s[fig.n++] = {(float) (sb.x1 + g.x0), (float) (sb.y1 + g.y0), (float) (sb.x1 + g.x1), (float) (sb.y1 + g.y1), g.major};
    fig.bx0 = fig.by0 = 1e9f, fig.bx1 = fig.by1 = -1e9f;
    for (int i = 0; i < fig.n; i++)
      for (int e = 0; e < 2; e++) {
        const float x = e ? fig.s[i].x1 : fig.s[i].x0, y = e ? fig.s[i].y1 : fig.s[i].y0;
        fig.bx0 = std::min(fig.bx0, x), fig.bx1 = std::max(fig.bx1, x), fig.by0 = std::min(fig.by0, y), fig.by1 = std::max(fig.by1, y);
      }
    snprintf(ufo_c, sizeof(ufo_c), "%s", sky::NAMES[idx].text);
    ctg[0] = {(fig.bx0 + fig.bx1) / 2, (fig.by0 + fig.by1) / 2, 20, {nullptr, nullptr}, false, 6, (int8_t) idx};
    n_ctg = 1;
  } else if (pick == 3) {
    ctg[0] = {(float) (90 + rnd() % 300), 415, 20, {nullptr, nullptr}, false, 7, 0};
    n_ctg = 1;
  }
}
// the patch of a screen-sized buffer under a: out to a small array (grab) or back in (put)
inline void patch_grab(uint16_t *dst, const lv_area_t &a, const lv_draw_buf_t *src) {
  const int w = a.x2 - a.x1 + 1;
  for (int y = a.y1; y <= a.y2; y++)
    memcpy(dst + (size_t) (y - a.y1) * w, (const uint8_t *) src->data + (size_t) y * src->header.stride + (size_t) a.x1 * 2, (size_t) w * 2);
}
inline void patch_put(const uint16_t *src, const lv_area_t &a, lv_draw_buf_t *dst) {
  const int w = a.x2 - a.x1 + 1;
  for (int y = a.y1; y <= a.y2; y++)
    memcpy((uint8_t *) dst->data + (size_t) y * dst->header.stride + (size_t) a.x1 * 2, src + (size_t) (y - a.y1) * w, (size_t) w * 2);
}
// target i gone (or back): the object hidden (shown), its patch into the backdrop, and the box to redo
inline void patch_apply(int i, bool gone) {
  tgt_hide(ctg[i], gone);
  const Patch &q = pt[i];
  patch_put(gone ? q.bare : q.with, q.a, st.clean);
  patch_put(gone ? q.bare : q.with, q.a, st.comp);
  if (st.last_box.x2 < st.last_box.x1)
    st.last_box = q.a;
  else
    st.last_box = {std::min(st.last_box.x1, q.a.x1), std::min(st.last_box.y1, q.a.y1), std::max(st.last_box.x2, q.a.x2), std::max(st.last_box.y2, q.a.y2)};
}
// at the start of a visit (the backdrop taken, the objects still up): the map round each target without it, from one retake with them all hidden
inline void patches_make() {
  patches_free();
  const uint32_t t0 = rocket::now_ms();
  if ((critter != CR_CAT && critter != CR_AXO && critter != CR_AUDREY) || n_ctg == 0 || st.clean == nullptr || st.comp == nullptr || rocket::st.back == nullptr)
    return;
  for (int i = 0; i < n_ctg; i++) {
    if (ctg[i].kind > 5)
      return;
    lv_area_t a = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    if (ctg[i].kind == 5)  // a star: the square round it
      a = {(int32_t) (ctg[i].x - ctg[i].r - 4), (int32_t) (ctg[i].y - ctg[i].r - 4), (int32_t) (ctg[i].x + ctg[i].r + 4), (int32_t) (ctg[i].y + ctg[i].r + 4)};
    else
      for (lv_obj_t *o : ctg[i].o)
      if (o) {
        lv_area_t b;
        lv_obj_get_coords(o, &b);
        a = {std::min(a.x1, b.x1), std::min(a.y1, b.y1), std::max(a.x2, b.x2), std::max(a.y2, b.y2)};
      }
    pt[i].a = {std::max<int32_t>(0, a.x1 - 6), std::max<int32_t>(0, a.y1 - 6), std::min<int32_t>(479, a.x2 + 6), std::min<int32_t>(479, a.y2 + 6)};
    const size_t n = (size_t) (pt[i].a.x2 - pt[i].a.x1 + 1) * (pt[i].a.y2 - pt[i].a.y1 + 1) * 2;
    pt[i].with = (uint16_t *) heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pt[i].bare = (uint16_t *) heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pt[i].with == nullptr || pt[i].bare == nullptr) {
      patches_free();
      return;
    }
    patch_grab(pt[i].with, pt[i].a, st.clean);
  }
  bool vis[4][2] = {};  // (an object the page keeps hidden stays hidden)
  for (int i = 0; i < n_ctg; i++)
    if (ctg[i].kind == 5)
      tgt_hide(ctg[i], true);
    else
      for (int k = 0; k < 2; k++)
      if (ctg[i].o[k] && !lv_obj_has_flag(ctg[i].o[k], LV_OBJ_FLAG_HIDDEN)) {
        vis[i][k] = true;
        lv_obj_add_flag(ctg[i].o[k], LV_OBJ_FLAG_HIDDEN);
      }
  lv_obj_add_flag(rocket::st.back, LV_OBJ_FLAG_HIDDEN);
  lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, st.comp);  // (the composite is free until the visit plays)
  lv_obj_remove_flag(rocket::st.back, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < n_ctg; i++) {
    patch_grab(pt[i].bare, pt[i].a, st.comp);
    if (ctg[i].kind == 5)
      tgt_hide(ctg[i], false);
    else
      for (int k = 0; k < 2; k++)
        if (vis[i][k])
          lv_obj_remove_flag(ctg[i].o[k], LV_OBJ_FLAG_HIDDEN);
  }
  for (int32_t y = 0; y < 480; y++)
    memcpy((uint8_t *) st.comp->data + (size_t) y * st.comp->header.stride, (const uint8_t *) st.clean->data + (size_t) y * st.clean->header.stride, 480 * 2);
  pt_ok = true;
  vlogf("cat: %d patches taken in %u ms", n_ctg, (unsigned) (rocket::now_ms() - t0));
}
// the map as it is now (the zapped objects hidden), into the clean copy and the composite: the dinosaur's way of taking a thing off the map
inline void cat_retake() {
  if (st.clean == nullptr || st.comp == nullptr || rocket::st.back == nullptr)
    return;
  for (int i = 0; i < n_ctg; i++)  // (the page may have shown a satellite's marker again meanwhile)
    tgt_hide(ctg[i], ctg[i].gone && !cat_restored);
  lv_obj_add_flag(rocket::st.back, LV_OBJ_FLAG_HIDDEN);
  lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, st.clean);
  lv_obj_remove_flag(rocket::st.back, LV_OBJ_FLAG_HIDDEN);
  for (int32_t y = 0; y < 480; y++)
    memcpy((uint8_t *) st.comp->data + (size_t) y * st.comp->header.stride, (const uint8_t *) st.clean->data + (size_t) y * st.clean->header.stride, 480 * 2);
  st.last_box = {0, 0, 479, 479};  // the whole screen is redone this frame
}
// the zapped objects come back at the end of an early finish
inline void cat_undo() {
  if (critter == CR_OCTO)
    return;
  for (int i = 0; i < n_ctg; i++)
    if (ctg[i].gone)
      tgt_hide(ctg[i], false);
  n_ctg = 0;
}
// each frame: a shot that has landed takes its target off the map; when the disc comes back, everything is put back
// (returns the time the retakes cost, so the timeline does not jump)
inline uint32_t cat_events(float t) {
  if (critter == CR_AXO) {
    axo_events(t);
    return 0;
  }
  if (critter == CR_AUDREY) {
    audrey_events(t);
    return 0;
  }
  if (critter == CR_UFO) {  // the beam takes hold: the object comes off the map
    if (n_ctg == 0 || ctg[0].gone || t - ufo_delay < UFO_LIFT)
      return 0;
    ctg[0].gone = true;
    const uint32_t t0 = rocket::now_ms();
    cat_retake();
    ESP_LOGI("octo", "UFO: t %.1f s: the beam takes hold; the map taken again in %u ms", t, (unsigned) (rocket::now_ms() - t0));
    vlogf("UFO t %.1f: beam takes hold, map taken again in %u ms (kind %d)", t, (unsigned) (rocket::now_ms() - t0), ctg[0].kind);
    return rocket::now_ms() - t0;
  }
  bool retake = false;
  for (int i = 0; i < n_ctg; i++)
    if (!ctg[i].gone && t >= shot_hit(i)) {
      ctg[i].gone = true;
      vlogf("cat t %.1f: shot %d lands on kind %d at %d,%d", t, i, (int) ctg[i].kind, (int) ctg[i].x, (int) ctg[i].y);
      if (pt_ok)
        patch_apply(i, true);
      else
        retake = true;
    } else if (!ctg[i].gone && ctg[i].o[0]) {  // (a satellite moves: aim at where it is)
      lv_area_t a;
      lv_obj_get_coords(ctg[i].o[0], &a);
      ctg[i].x = (a.x1 + a.x2) / 2.0f, ctg[i].y = (a.y1 + a.y2) / 2.0f;
    }
  if (!cat_restored && t >= cp.t_rest) {
    cat_restored = true;
    if (pt_ok)
      for (int i = 0; i < n_ctg; i++)
        patch_apply(i, false);
    else
      retake = n_ctg > 0;  // (nothing was shot: nothing to put back)
  }
  if (!retake)
    return 0;
  const uint32_t t0 = rocket::now_ms();
  cat_retake();
  vlogf("cat t %.1f: %s, map taken again in %u ms (%d shots, disc %d/%d r %d origin %d,%d)", t, cat_restored ? "disc comes back" : "a shot lands", (unsigned) (rocket::now_ms() - t0), n_ctg,
        ui.cx, ui.cy, ui.radius, (int) disc_ox, (int) disc_oy);
  ESP_LOGI("octo", "cat: t %.1f s: %s; the map taken again in %u ms (%d shot, disc %d/%d r %d)", t, cat_restored ? "the disc comes back" : "a shot lands",
           (unsigned) (rocket::now_ms() - t0), n_ctg, ui.cx, ui.cy, ui.radius);
  return rocket::now_ms() - t0;
}

inline void frame() {
  if (!st.running)
    return;
  const uint32_t now = rocket::now_ms();
  // The first step is followed by a heavy refresh (the whole screen is drawn once, from the snapshot); the time it
  // took does not count, so the entrance starts when the screen is ready (the dinosaur's ENTRY_SKIP_MS idea).
  if (st.frames == 1 && now - st.last_model > model_ms() * 2)
    st.t0 += (now - st.last_model) - model_ms();
  float t = (now - st.t0) / 1000.0f;
  if (t >= dur_s() || (rocket::st.back && lv_obj_get_parent(rocket::st.back) != lv_screen_active())) {
    end_visit();
    return;
  }
  if (st.frames > 0 && now - st.last_model < model_ms())
    return;
  if (st.last_frame) {
    const uint32_t gap = now - st.last_frame;
    if (gap > st.max_gap)
      st.max_gap = gap;
    Pb &b = pb_at(t);
    b.gap = std::max(b.gap, gap);
  }
  st.last_frame = now;
  st.last_model = now;
  st.frames++;
  if (critter != CR_OCTO)
    if (const uint32_t cost = cat_events(t)) {  // the snapshot took a moment: the cat did not move meanwhile
      st.t0 += cost;
      t = (rocket::now_ms() - st.t0) / 1000.0f;
    }
  st.t = t;
  int32_t bx1, by1, bx2, by2;
  if (critter != CR_OCTO) {
    critter_box(t, bx1, by1, bx2, by2);
    st.bw = 0;
    if (critter == CR_UFO) {  // it talks too
      if (t >= ufo_delay)
        bubble_geom(t - ufo_delay, beat_at(t - ufo_delay), st.bx, st.by, st.bw, st.bh);
      if (st.bw > 0)
        bx1 = std::min(bx1, (int32_t) st.bx - 30), by1 = std::min(by1, (int32_t) st.by - 4), bx2 = std::max(bx2, (int32_t) (st.bx + st.bw + 6)),
        by2 = std::max(by2, (int32_t) (st.by + st.bh + 24));
    } else {  // the cat talks too
      bubble_geom(t, beat_at(t), st.bx, st.by, st.bw, st.bh);
      if (st.bw > 0)
        bx1 = std::min(bx1, (int32_t) st.bx - 30), by1 = std::min(by1, (int32_t) st.by - 4), bx2 = std::max(bx2, (int32_t) (st.bx + st.bw + 30)),
        by2 = std::max(by2, (int32_t) (st.by + st.bh + 24));
    }
  } else {
    build(t, st.m);
    const int beat = beat_at(t);
    bubble_geom(t, beat, st.bx, st.by, st.bw, st.bh);
    bx1 = (int32_t) st.m.x0, by1 = (int32_t) st.m.y0, bx2 = (int32_t) st.m.x1, by2 = (int32_t) st.m.y1;
    if (st.bw > 0) {  // the bubble and its tail
      bx1 = std::min(bx1, (int32_t) st.bx - 4), by1 = std::min(by1, (int32_t) st.by - 4);
      bx2 = std::max(bx2, (int32_t) (st.bx + st.bw + 6)), by2 = std::max(by2, (int32_t) (st.by + st.bh + 24));
    }
    fig_box(t, st.m, bx1, by1, bx2, by2);
    int cx1, cy1, cx2, cy2;  // the cloud
    if (ink_box(t, cx1, cy1, cx2, cy2))
      bx1 = std::min<int32_t>(bx1, cx1), by1 = std::min<int32_t>(by1, cy1), bx2 = std::max<int32_t>(bx2, cx2), by2 = std::max<int32_t>(by2, cy2);
  }
  lv_area_t box = {std::max<int32_t>(0, bx1), std::max<int32_t>(0, by1), std::min<int32_t>(479, bx2), std::min<int32_t>(479, by2)};
  const bool box_ok = box.x2 >= box.x1 && box.y2 >= box.y1;  // (empty: all of it is off the screen)
  // What is drawn on the screen is one RGB565 image (the composite), blitted over the dirty box in a few
  // milliseconds. The paint happens here, off the scan, into the composite.
  lv_area_t reg = box;
  bool reg_ok = box_ok;
  if (st.last_box.x2 >= st.last_box.x1) {
    if (box_ok) {
      reg.x1 = std::min(reg.x1, st.last_box.x1), reg.y1 = std::min(reg.y1, st.last_box.y1);
      reg.x2 = std::max(reg.x2, st.last_box.x2), reg.y2 = std::max(reg.y2, st.last_box.y2);
    } else {
      reg = st.last_box;  // it has just left: put back what it covered
    }
    reg_ok = true;
  }
  if (!reg_ok) {
    st.last_box = {0, 0, -1, -1};
    return;
  }
  const uint32_t t_paint = rocket::now_us();
  for (int32_t y = reg.y1; y <= reg.y2; y++)  // the screen as it was, under the box
    memcpy((uint8_t *) st.comp->data + (size_t) y * st.comp->header.stride + (size_t) reg.x1 * 2,
           (const uint8_t *) st.clean->data + (size_t) y * st.clean->header.stride + (size_t) reg.x1 * 2, (size_t) (reg.x2 - reg.x1 + 1) * 2);
  const Rz rz = {(uint8_t *) st.comp->data, st.comp->header.stride, reg.x1, reg.y1, reg.x2, reg.y2};
  if (critter == CR_UFO)
    paint_ufo(rz, t);
  else if (critter == CR_CAT)
    paint_cat(rz, t);
  else if (critter == CR_AXO)
    paint_axo(rz, t);
  else if (critter == CR_AUDREY)
    paint_audrey_visit(rz, t);
  else {
    paint_fig(rz, t, st.m);
    paint_octopus(rz, st.m);
    paint_ink(rz, t);
  }
  if (st.bw > 0 || (critter == CR_CAT && counts_active(t))) {
    lv_layer_t layer;
    lv_canvas_init_layer(st.cv, &layer);
    layer._clip_area = reg;
    paint_bubble(&layer);
    if (critter == CR_CAT)
      paint_counts(&layer, t);
    lv_canvas_finish_layer(st.cv, &layer);
  }
  const uint32_t us = rocket::now_us() - t_paint;
  st.paint_us += us;
  st.paint_max = std::max(st.paint_max, us);
  {
    Pb &b = pb_at(t);
    b.n++, b.sum += us, b.mx = std::max(b.mx, us);
  }
  st.px += (uint32_t) ((reg.x2 - reg.x1 + 1) * (reg.y2 - reg.y1 + 1));
  if (t >= EXIT_T && critter == CR_OCTO)
    st.exit_us += us, st.exit_n++, st.exit_px += (uint32_t) ((reg.x2 - reg.x1 + 1) * (reg.y2 - reg.y1 + 1));
  inval(reg);
  st.last_box = box_ok ? box : lv_area_t{0, 0, -1, -1};
}
inline void on_refr(lv_event_t *) {
  frame();
  rocket::st.prof.start = rocket::now_us();  // the refresh that follows: its render time, from here to REFR_READY
}

// UI-85d: what to eat. force -2: the odds (Taurus 1 visit in 100, else seafood 1 in 3); -1 none; 0..2 that meal.
// Only a constellation whose figure is on the map now (at least two of its lines above the horizon, Stars layer on).
struct MealName {
  const char *name;
  int meal;
};
inline int pick_meal(int force) {
  static const MealName MENU[] = {{"Cancer", M_CRAB}, {"Pisces", M_FISH}, {"Piscis Austrinus", M_FISH}, {"Volans", M_FISH},
                                  {"Dorado", M_FISH}, {"Taurus", M_BULL}};
  int want = force;
  if (force == -2) {
    const uint32_t r = rnd() % 300;  // 1 in 100 Taurus, 1 in 3 seafood
    if (r < 3)
      want = M_BULL;
    else if (r < 103)
      want = -3;  // seafood: any
    else
      return M_NONE;
  }
  if (!ui.stars_shown && force != -2)
    ESP_LOGI("octo", "meal: the Stars layer is off");
  if (want == -1 || !ui.stars_shown)
    return M_NONE;
  int cand[6], meal_of[6], n = 0;
  for (const MealName &m : MENU) {
    int idx = -1;  // the constellation (its name tag may be hidden or not placed: the figure is what matters)
    for (int k = 0; k < sky::N_NAMES; k++)
      if (!strcmp(sky::NAMES[k].text, m.name))
        idx = k;
    if (idx < 0)
      continue;
    int lo = 0;
    for (int j = 0; j < idx; j++)
      lo += sky::CFIG[sky::CFIG_OFF[j]];
    const int hi = lo + sky::CFIG[sky::CFIG_OFF[idx]];
    int lines = 0;  // its lines on the map (cut at the horizon)
    for (const auto &g : ui.star_segs)
      lines += g.poly >= lo && g.poly < hi;
    if (force != -2)
      ESP_LOGI("octo", "meal %s: %d figure lines on the map", m.name, lines);
    if (want == -3 ? m.meal == M_BULL : m.meal != want)
      continue;
    if (lines >= 2)
      cand[n] = idx, meal_of[n++] = m.meal;
  }
  if (n == 0)
    return M_NONE;
  const int i = (int) (rnd() % (uint32_t) n);
  st.eat_name = cand[i];
  return meal_of[i];
}
inline int count_lines(const char *const *a, int n) {
  int c = 0;
  while (c < n && a[c][0])
    c++;
  return c;
}

// start a visit now; false: no memory for the backdrop, or something else is playing
inline bool begin(uint32_t seed, int force = -2, int who = CR_OCTO) {
  if (st.running || rocket::playing() || rocket::st.font == nullptr)
    return false;
  // the backdrop and the composite are 450 KB each: with less PSRAM free there is no visit, quietly
  const size_t need = (rocket::st.snap ? 1 : 2) * 460800u + 32768u;
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < need) {
    ESP_LOGD("octo", "not enough PSRAM for a visit (%u KB free)", (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    mem_report("no octopus", false);
    return false;
  }
  st.rng = seed ? seed : 0x9E3779B9u;
  rocket::stop();
  // a meal (UI-85d): chosen first, and its figure and name taken off the map before the backdrop is, so the backdrop
  // is the map without them (the octopus paints the figure itself, to pull it in)
  st.ate = false;
  hover_x = 260, hover_y = 262;
  {  // in and out by a side, the top, the bottom or a corner (never both the same way, so it crosses the screen)
    static const int8_t D[8][2] = {{1, 0}, {-1, 0}, {0, -1}, {0, 1}, {1, -1}, {-1, -1}, {1, 1}, {-1, 1}};
    const int a = rnd() % 8;
    int b = rnd() % 8;
    if (b == a)
      b = (b + 1) % 8;
    in_dx = D[a][0], in_dy = D[a][1], out_dx = D[b][0], out_dy = D[b][1];
  }
  st.eat_name = -1;
  critter = who;
  disc_origin();
  vlog[0] = 0;
  if (who == CR_CAT) {  // out by the opposite side or corner; 2 to 4 things on the map to shoot
    out_dx = -in_dx, out_dy = -in_dy;
    cat_spots();
    const int want = (int) (2 + rnd() % 3);
    cat_targets(want, true);
    cat_fill_stars(want);
    cat_plan();
  }
  if (who == CR_AUDREY) {  // 2 to 4 small things on the map within its reach
    cat_targets(4, false, true);
    cat_fill_stars(4);
    const int want = (int) (2 + rnd() % 3);
    n_ctg = std::min(n_ctg, want + 2);  // (some are left out in the plan: too near the pot)
    audrey_plan();
  }
  if (who == CR_AXO) {  // 2 to 4 small things on the map to eat (the ISS and satellites, bright stars)
    const int want = (int) (2 + rnd() % 3);
    cat_targets(want, false, true);
    cat_fill_stars(want);
    axo_plan();
  }
  vlogf("%s: %d targets", who == CR_UFO ? "UFO" : who == CR_CAT ? "cat" : who == CR_AXO ? "axolotl" : who == CR_AUDREY ? "audrey" : "octopus", n_ctg);
  st.meal = who == CR_OCTO ? pick_meal(force) : M_NONE;
  is_meal = st.meal != M_NONE;
  if (!is_meal && force != -2 && who == CR_OCTO)
    return false;  // a meal was asked for (the debug command) and none is up
  if (is_meal) {
    const_eat(st.eat_name);
    st.ate = true;
    gather_fig();
  }
  rocket::back_take();
  if (rocket::st.snap == nullptr) {
    undo_meal();
    rocket::stop();
    return false;
  }
  // the composite: a copy of the screen the visit paints on; the backdrop (rocket::back_draw) shows it
  st.clean = rocket::st.snap;
  st.comp = lv_draw_buf_create(480, 480, LV_COLOR_FORMAT_RGB565, 0);
  if (st.comp == nullptr) {
    ESP_LOGW("octo", "no memory for the composite: no visit");
    undo_meal();
    st.clean = nullptr;
    rocket::stop();
    return false;
  }
  for (int32_t y = 0; y < 480; y++)
    memcpy((uint8_t *) st.comp->data + (size_t) y * st.comp->header.stride, (const uint8_t *) st.clean->data + (size_t) y * st.clean->header.stride, 480 * 2);
  rocket::st.snap = st.comp;
  patches_make();
  // a meal (UI-85d), or one thing to morph into, and a line for each beat
  st.kind = (int) (rnd() % 3);
  st.say[0] = pick_line(SAY_ARRIVE, sizeof(SAY_ARRIVE) / sizeof(*SAY_ARRIVE), last_arrive);
  st.say[1] = st.kind == K_LION    ? pick_line(SAY_LION, sizeof(SAY_LION) / sizeof(*SAY_LION), last_lion)
              : st.kind == K_SNAKE ? pick_line(SAY_SNAKE, sizeof(SAY_SNAKE) / sizeof(*SAY_SNAKE), last_snake)
                                   : pick_line(SAY_ISS, sizeof(SAY_ISS) / sizeof(*SAY_ISS), last_iss);
  st.say[2] = pick_line(SAY_INK, sizeof(SAY_INK) / sizeof(*SAY_INK), last_ink);
  if (is_meal) {
    st.kind = K_MEAL;
    st.say[0] = pick_line(MEAL_BEFORE[st.meal], (uint32_t) count_lines(MEAL_BEFORE[st.meal], 6), last_before[st.meal]);
    st.say[1] = pick_line(MEAL_AFTER[st.meal], (uint32_t) count_lines(MEAL_AFTER[st.meal], 6), last_after[st.meal]);
  }
  if (who == CR_UFO) {
    ufo_pick();
    ufo_setup();
  }
  if (who == CR_AUDREY)
    audrey_lines();
  if (who == CR_AXO)
    axo_lines();
  if (who == CR_CAT)
    cat_lines();  // (after the octopus's lines above, which would otherwise be the cat's)
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
  for (Pb &b : pb)
    b = Pb();
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
        if (rocket::now_ms() - st.last_model >= model_ms())
          frame();
      },
      20, nullptr);
  frame();
  if (critter != CR_OCTO)
    ESP_LOGI("octo", "visit: %s", critter == CR_UFO ? "UFO" : critter == CR_AXO ? "axolotl" : critter == CR_AUDREY ? "audrey" : "cat on a comet");
  else
  ESP_LOGI("octo", "visit: %s, \"%s\" / \"%s\"", st.kind == K_LION ? "lionfish" : st.kind == K_SNAKE ? "sea snake" : st.kind == K_MEAL ? "a meal" : "ISS", st.say[1], st.say[2]);
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

namespace dino {
#ifdef SAT_HOST_TEST  // no dinosaur on the desktop
inline bool walk(uint32_t) { return false; }
inline bool walk_eat(uint32_t, int, int, int, const char *, lv_obj_t *const *, int) { return false; }
inline bool walking() { return false; }
#else
bool walk(uint32_t seed);  // sky_dino.h (same translation unit, included after this file)
bool walk_eat(uint32_t seed, int x, int y, int r, const char *name, lv_obj_t *const *hide, int n);
inline bool walking();
#endif
}  // namespace dino
// UI-85c: three in four of the dinosaur's walks go to a Sun, Moon or planet that is up on the map and eat it. False: none is up
// (or no memory): the caller walks as usual.
bool dino_hunt(uint32_t seed) {
  lv_obj_t *obj[2 + planets::N_PLANETS], *lbl[2 + planets::N_PLANETS];
  const char *name[2 + planets::N_PLANETS];
  int n = 0;
  auto up = [&](lv_obj_t *o) { return o != nullptr && !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); };
  if (up(ui.sun))
    obj[n] = ui.sun, lbl[n] = nullptr, name[n++] = "The Sun";
  if (up(ui.moon))
    obj[n] = ui.moon, lbl[n] = nullptr, name[n++] = "The Moon";
  for (int p = 0; p < planets::N_PLANETS; p++)
    if (up(ui.planet_img[p]))
      obj[n] = ui.planet_img[p], lbl[n] = ui.planet_lbl[p], name[n++] = planets::name(p);
  if (n == 0)
    return false;
  const int i = (int) ((seed >> 7) % (uint32_t) n);
  lv_area_t a;
  lv_obj_get_coords(obj[i], &a);
  lv_obj_t *hide[2] = {obj[i], lbl[i]};
  return dino::walk_eat(seed, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, (a.x2 - a.x1 + 1) / 2, name[i], hide, lbl[i] ? 2 : 1);
}
namespace octo {
void set_enabled(bool on) {
  st.enabled = on;
  if (!on)
    end_visit();
  else if (st.next_ms == 0)
    schedule(true);
}
bool playing() { return st.running; }
const char *last_log() { return vlog; }
void stop() { end_visit(); }
bool play_now() { return begin(rocket::now_us() * 2654435761u + 12345u); }
bool play_dino() {
  const uint32_t seed = rocket::now_us() * 2654435761u + 12345u;
  if (rnd() % 4 == 0 || !dino_hunt(seed))  // (as a random visit: three walks in four hunt when something is up)
    return dino::walk(seed);
  return true;
}
bool play_critter(int who) { return begin(rocket::now_us() * 2654435761u + 12345u, -2, who); }
bool play_eat(int meal) { return begin(rocket::now_us() * 2654435761u + 12345u, meal) && st.meal != M_NONE; }
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
  if (dino::walking())
    return;
  const uint32_t who = rnd() % 6;  // UI-85b, UI-90..92: a visit in six each: the dinosaur's walk (sky_dino.h), the octopus, a UFO, a cat on a comet, an axolotl, Audrey
  if (who >= 2)
    begin(st.rng ^ rocket::now_us(), -2, who == 2 ? CR_UFO : who == 3 ? CR_CAT : who == 4 ? CR_AXO : CR_AUDREY);
  else if (who == 0) {
    const uint32_t seed = st.rng ^ rocket::now_us();
    if (rnd() % 4 == 0 || !dino_hunt(seed))  // UI-85c: and three in four of those are hunting walks (a plain one when nothing is up)
      dino::walk(seed);
  }
  else
    begin(st.rng ^ rocket::now_us());
  schedule(false);  // also after a refusal (no PSRAM): the next try is minutes away, not a second
}
}  // namespace octo
}  // namespace sat
#endif  // SKY_IMPL
