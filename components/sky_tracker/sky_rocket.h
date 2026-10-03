#pragma once
// UI-69 launch animation. When a rocket launch (UI-54, Launch Library) reaches T-0, a cartoon
// rocket lifts off from the lower left of the screen with a puff of smoke at the pad, arcs up
// across the screen leaving a fading smoke trail and leaves past the upper right, about 4.5 s.
// A banner at the bottom names the mission. Once per launch, a tap anywhere skips it. Off with
// the Launch Animation switch; the Play Launch Animation button shows it on demand.
// Everything sits on lv_layer_top and is deleted when it ends. The rocket is drawn once at
// init into FLAMES small ARGB8888 buffers (flames of different length, width and lean, swapped
// in an irregular order for the flicker, UI-69c) and shown
// with an lv_image rotated along the path; the smoke is round lv_objs drawn OPAQUE (UI-69e).
// UI-69f undocking animation: when a Launch Library event "Spacecraft Undocking" at the ISS
// reaches its time, the station is drawn in the middle of the map (opaque shapes: truss, solar
// arrays, radiators, modules) with a capsule at its forward port; the capsule lets go with a
// burst of thruster puffs and backs away off the top of the screen, about 6 s. Same banner,
// tap to skip, switch and frame log as the launch; the Play Undocking Animation button shows it.
// Compiled in sky_extra.cpp (SKY_IMPL), as sky_about.h.
#include <cstdint>

#ifndef SKY_IMPL
namespace sat {
namespace rocket {
void init(const lv_font_t *banner);
void set_enabled(bool on);
void play_now();  // the button: the next launch's name (or a generic one)
void play_undock_now();  // UI-69f button: the next ISS undocking's name (or a generic one)
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
// UI-69e: smoke is opaque. Translucent pixels make LVGL blend against what is already drawn,
// which on this panel means reading the PSRAM side back pixel by pixel (PERF-9): the unit
// managed 3-5 fps with translucent smoke. Each puff is a solid disc whose colour is the smoke
// colour pre-mixed with the background under its centre (sky or page), so fading is a colour
// change, not transparency.
constexpr int MAP_CX = 240, MAP_CY = 242, MAP_R = 184;  // sky_box in the YAML
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
// UI-69f the ISS, seen from above (opaque rectangles, x y w h colour, border colour or 0)
struct Part {
  int16_t x, y, w, h;
  uint32_t col, edge;
};
constexpr uint32_t C_ARRAY = 0xA87632, C_ARRAY_EDGE = 0x6E4C1E, C_MOD = 0xE6E9EF;
constexpr Part ISS_SHAPE[] = {
    {110, 244, 260, 6, 0xA9ADB5, 0},                                          // truss
    {121, 186, 22, 56, C_ARRAY, C_ARRAY_EDGE}, {149, 186, 22, 56, C_ARRAY, C_ARRAY_EDGE},  // arrays above
    {309, 186, 22, 56, C_ARRAY, C_ARRAY_EDGE}, {337, 186, 22, 56, C_ARRAY, C_ARRAY_EDGE},
    {121, 252, 22, 56, C_ARRAY, C_ARRAY_EDGE}, {149, 252, 22, 56, C_ARRAY, C_ARRAY_EDGE},  // and below
    {309, 252, 22, 56, C_ARRAY, C_ARRAY_EDGE}, {337, 252, 22, 56, C_ARRAY, C_ARRAY_EDGE},
    {198, 254, 14, 36, 0xF0F2F5, 0xC0C4CC},   {268, 254, 14, 36, 0xF0F2F5, 0xC0C4CC},     // radiators
    {233, 176, 14, 150, C_MOD, 0xB8BCC6},                                     // modules, fore to aft
    {226, 192, 28, 16, 0xDCE0E8, 0xB8BCC6},   {226, 272, 28, 16, 0xDCE0E8, 0xB8BCC6},     // nodes
    {231, 326, 18, 36, 0xD8DCE4, 0xB0B4BE},                                   // Russian segment
    {186, 340, 44, 8, 0x3C4F86, 0x26335A},    {250, 340, 44, 8, 0x3C4F86, 0x26335A},      // its arrays
};
constexpr int ISS_PARTS = sizeof(ISS_SHAPE) / sizeof(ISS_SHAPE[0]);
constexpr int CAP_W = 26, CAP_H = 40;       // capsule image; its nose (bottom) docks at PORT
constexpr int PORT_X = 240, PORT_Y = 176;   // the forward port, top of the module stack
constexpr float UNDOCK_S = 6.0f;            // the scene's length
constexpr float UNDOCK_GO = 1.0f;           // docked until then, then the hooks let go
constexpr double LATE_S = 120;       // a launch is "now" for 2 min after T-0 (refresh lag)

struct St {
  bool enabled = true;
  const lv_font_t *font = nullptr;
  lv_draw_buf_t *img_buf[FLAMES] = {};
  int flame_i = 0;
  int32_t last_rot = -100000;
  bool benched = false;
  uint32_t flame_at = 0;
  lv_obj_t *catcher = nullptr, *img = nullptr, *banner = nullptr, *label = nullptr;
  // UI-69i all the smoke is one full-screen object that draws its discs itself: moving ~40
  // objects a frame cost ~5 ms per lv_obj_set_size/set_pos (LVGL's per-object work, code run
  // from PSRAM). A changed disc now only invalidates its old and new areas.
  struct Disc {
    int16_t x = 0, y = 0, r = 0;
    uint32_t col = 0;
    bool on = false;
  };
  lv_obj_t *smoke = nullptr;
  Disc disc[PADS + PUFFS] = {};  // the ground cloud first (drawn under the trail), then the puffs
  float puff_x[PUFFS] = {}, puff_y[PUFFS] = {}, puff_t[PUFFS] = {};
  int8_t puff_step[PUFFS] = {};  // the age step last drawn (-1 hidden): a puff only changes on a new step
  int8_t pad_step[PADS] = {};
  int puff_next = 0;
  float last_emit = -1;
  uint32_t t0 = 0;
  lv_timer_t *anim = nullptr;  // fallback driver; frames normally come from the display refresh
  uint32_t last_frame = 0;
  uint32_t frames = 0, max_gap = 0, last_refr = 0;  // UI-69a: logged when it ends
  // UI-69g: where each frame's time goes (microseconds), logged when it ends
  struct Prof {
    uint32_t start = 0, ready = 0, flush_t0 = 0;  // this frame's refresh start, the last one's end
    uint32_t anim = 0, render = 0, flush = 0, idle = 0;          // sums
    uint32_t anim_max = 0, render_max = 0, flush_max = 0, idle_max = 0;
    uint32_t flush_now = 0, chunks = 0, px = 0, px_max = 0, px_now = 0, n = 0;
    uint32_t t_img = 0, t_puff = 0, t_pad = 0, calls = 0, fallback = 0;  // inside frame()
    uint32_t t_size = 0, t_pos = 0, t_col = 0, t_flag = 0;          // inside smoke_at()
    uint32_t bench_cpu[2] = {}, bench_ram[2] = {};                    // UI-69h: before / during
  } prof;
  uint32_t played_key = 0;  // the last launch shown (name + T-0)
  // UI-69f
  bool undock = false;      // the scene playing: false launch, true undocking
  lv_draw_buf_t *capsule_buf = nullptr;
  lv_obj_t *iss[ISS_PARTS] = {};
  uint32_t undock_key = 0;  // the last undocking shown
};
inline St st;

#ifdef SAT_HOST_TEST
inline uint32_t now_ms() { return (uint32_t) (sat_host_now * 1000.0); }
inline uint32_t now_us() { return (uint32_t) (sat_host_now * 1e6); }
#else
inline uint32_t now_ms() { return esphome::millis(); }
inline uint32_t now_us() { return esphome::micros(); }
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
    ui_feed_wdt();  // FAIL-11: drawn at boot, seconds from PSRAM code; keep the task watchdog fed
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

inline void disc_area(const St::Disc &d, lv_area_t &a) {
  a.x1 = d.x - d.r, a.y1 = d.y - d.r, a.x2 = d.x + d.r - 1, a.y2 = d.y + d.r - 1;
}
inline void inval_disc(const St::Disc &d) {
  lv_area_t a;
  disc_area(d, a);
  lv_obj_invalidate_area(st.smoke, &a);
}
inline void draw_smoke(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.radius = LV_RADIUS_CIRCLE;
  dsc.bg_opa = LV_OPA_COVER;
  dsc.border_width = 0;
  for (const auto &d : st.disc) {
    if (!d.on)
      continue;
    lv_area_t a;
    disc_area(d, a);
    const lv_area_t &c = layer->_clip_area;  // skip discs outside the area being redrawn
    if (a.x2 < c.x1 || a.x1 > c.x2 || a.y2 < c.y1 || a.y1 > c.y2)
      continue;
    dsc.bg_color = lv_color_hex(d.col);
    lv_draw_rect(layer, &dsc, &a);
  }
}
inline void smoke_create() {
  st.smoke = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(st.smoke);
  lv_obj_set_size(st.smoke, 480, 480);
  lv_obj_remove_flag(st.smoke, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(st.smoke, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(st.smoke, draw_smoke, LV_EVENT_DRAW_MAIN, nullptr);
  for (auto &d : st.disc)
    d = St::Disc();
}
inline void hide(int k) {
  St::Disc &d = st.disc[k];
  if (!d.on)
    return;
  inval_disc(d);
  d.on = false;
}
// a disc of colour col at strength a (0..255) over whatever background is under (x, y)
inline void smoke_at(int k, int x, int y, int r, int a, uint32_t col) {
  auto &p = st.prof;
  p.calls++;
  const uint32_t t0 = now_us();
  const int dx = x - MAP_CX, dy = y - MAP_CY;
  const uint32_t bg = dx * dx + dy * dy <= MAP_R * MAP_R ? SKY_BG : PAGE_BG;
  St::Disc n;
  n.x = (int16_t) x, n.y = (int16_t) y, n.r = (int16_t) r, n.on = true;
  n.col = mix(col, bg, (uint32_t) std::min(255, std::max(0, a)));
  St::Disc &d = st.disc[k];
  if (!(d.on && d.x == n.x && d.y == n.y && d.r == n.r && d.col == n.col)) {
    if (d.on)
      inval_disc(d);
    d = n;
    inval_disc(d);
  }
  p.t_size += now_us() - t0;
}
// UI-69h: how much CPU and PSRAM the main loop gets: a fixed integer loop and a 64 KB read of
// PSRAM, timed (microseconds); run before the animation and once in the middle of it
inline uint8_t *bench_buf = nullptr;
inline void bench(int k) {
  volatile uint32_t acc = 0;
  uint32_t t0 = now_us();
  for (uint32_t i = 0; i < 100000; i++)
    acc = acc * 1664525u + 1013904223u + i;
  st.prof.bench_cpu[k] = now_us() - t0;
#ifndef SAT_HOST_TEST
  if (bench_buf == nullptr)
    bench_buf = (uint8_t *) heap_caps_malloc(65536, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  if (bench_buf) {
    t0 = now_us();
    uint32_t sum = 0;
    for (int i = 0; i < 65536; i += 32)  // one read per cache line
      sum += bench_buf[i];
    acc = acc + sum;
    st.prof.bench_ram[k] = now_us() - t0;
  }
}

inline void on_refr(lv_event_t *);
inline void on_prof(lv_event_t *);
inline void stop() {
  if (st.anim && st.frames > 1)
    ESP_LOGI("rocket", "animation: %u frames in %u ms (%.1f fps), longest gap %u ms", (unsigned) st.frames,
             (unsigned) (st.last_refr - st.t0), st.frames * 1000.0f / std::max<uint32_t>(1, st.last_refr - st.t0),
             (unsigned) st.max_gap);
  if (st.anim && st.prof.n > 0) {
    const auto &p = st.prof;
    const uint32_t n = p.n;
    ESP_LOGI("rocket", "per frame (avg/max ms): refresh %.1f/%.1f (flush %.1f/%.1f in %.1f chunks), "
             "loop between refreshes %.1f/%.1f, animation code %.2f/%.2f; dirty %u/%u px",
             p.render / 1000.0f / n, p.render_max / 1000.0f, p.flush / 1000.0f / n, p.flush_max / 1000.0f,
             (float) p.chunks / n, p.idle / 1000.0f / n, p.idle_max / 1000.0f, p.anim / 1000.0f / n,
             p.anim_max / 1000.0f, (unsigned) (p.px / n), (unsigned) p.px_max);
    ESP_LOGI("rocket", "frame() parts (total ms): rocket image %.1f, puffs %.1f, ground cloud %.1f; %u smoke updates, "
             "%u fallback frames", p.t_img / 1000.0f, p.t_puff / 1000.0f, p.t_pad / 1000.0f, (unsigned) p.calls,
             (unsigned) p.fallback);
    const float c = p.calls ? 1000.0f * p.calls : 1.0f;
    ESP_LOGI("rocket", "per smoke update (avg ms): %.3f", p.t_size / c);
    ESP_LOGI("rocket", "bench before/during (us): cpu loop %u/%u, psram 64 KB read %u/%u", (unsigned) p.bench_cpu[0],
             (unsigned) p.bench_cpu[1], (unsigned) p.bench_ram[0], (unsigned) p.bench_ram[1]);
  }
  if (st.anim) {
    if (lv_display_t *d = lv_display_get_default())
      lv_display_remove_event_cb_with_user_data(d, on_prof, nullptr);
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
  if (st.smoke) {
    lv_obj_delete(st.smoke);
    st.smoke = nullptr;
  }
  for (auto &o : st.iss)
    if (o) {
      lv_obj_delete(o);
      o = nullptr;
    }
  st.label = nullptr;
}
inline bool playing() { return st.anim != nullptr; }
bool active() { return playing(); }

inline void frame_undock();
inline void frame_launch() {
  const float ts = (now_ms() - st.t0) / 1000.0f, u = ts / DUR_S;
  if (u > 1.25f) {  // the smoke has faded out
    stop();
    return;
  }
  float x, y, deg;
  path(std::min(u, 1.1f), x, y, deg);
  auto &pf = st.prof;
  if (!st.benched && ts > 2.0f) {
    st.benched = true;
    bench(1);
  }
  uint32_t t_a = now_us();
  // rocket: pivot on the body centre; the flame steps through FLAME_SEQ (UI-69c)
  if (now_ms() - st.flame_at >= FLAME_MS) {
    st.flame_at = now_ms();
    st.flame_i = (st.flame_i + 1) % (int) sizeof(FLAME_SEQ);
    if (lv_draw_buf_t *b = st.img_buf[FLAME_SEQ[st.flame_i]])
      lv_image_set_src(st.img, b);
  }
  const int32_t rot = (int32_t) (deg * 10.0f);
  if (std::abs(rot - st.last_rot) >= 15) {  // a new angle only every 1.5 degrees
    st.last_rot = rot;
    lv_image_set_rotation(st.img, rot);
  }
  lv_obj_set_pos(st.img, (int32_t) std::lround(x) - PX, (int32_t) std::lround(y) - PY);
  uint32_t t_b = now_us();
  pf.t_img += t_b - t_a;
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
      hide(PADS + i);
      continue;
    }
    smoke_at(PADS + i, (int) st.puff_x[i], (int) st.puff_y[i], (int) (7 + 22 * age), a, 0xC8CDD7);
  }
  t_a = now_us();
  pf.t_puff += t_a - t_b;
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
      hide(k);
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
    smoke_at(k, x, y, r, a, b.col);
  }
  pf.t_pad += now_us() - t_a;
}

// UI-69f the capsule, nose down (toward the station): trunk with solar cells, heat shield,
// white capsule tapering to the docking adapter. Drawn once, 4x4 supersampled.
inline uint32_t capsule_sample(float x, float y) {
  if (y < 12)
    return (x >= 4 && x < 22) ? ((x < 13) ? 0xFF2A3148 : 0xFF3B4B7A) : 0;   // trunk, two cell tones
  if (y < 15)
    return (x >= 2 && x < 24) ? 0xFF3A3A3E : 0;                             // heat shield
  if (y < 34) {
    const float f = (y - 15) / 19.0f, l = 2 + 5 * f, r = 24 - 5 * f;          // tapering body
    if (x < l || x >= r)
      return 0;
    const float wx = x - 13, wy = y - 22;
    if (wy > -2 && wy < 2 && (std::fabs(wx - 4) < 1.6f || std::fabs(wx + 4) < 1.6f))
      return 0xFF283250;                                                      // windows
    return 0xFFEEF0F5;
  }
  return (y < CAP_H - 1 && x >= 10 && x < 16) ? 0xFFB0B4BC : 0;               // docking adapter
}
inline lv_draw_buf_t *draw_capsule() {
  lv_draw_buf_t *db = lv_draw_buf_create(CAP_W, CAP_H, LV_COLOR_FORMAT_ARGB8888, 0);
  if (db == nullptr)
    return nullptr;
  for (int py = 0; py < CAP_H; py++) {
    ui_feed_wdt();  // FAIL-11: drawn at boot, seconds from PSRAM code; keep the task watchdog fed
    uint8_t *row = (uint8_t *) db->data + (size_t) py * db->header.stride;
    for (int px = 0; px < CAP_W; px++) {
      uint32_t r = 0, g = 0, b = 0, a = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
          const uint32_t c = capsule_sample(px + (sx + 0.5f) / 4, py + (sy + 0.5f) / 4);
          if (c) {
            r += (c >> 16) & 255, g += (c >> 8) & 255, b += c & 255, a += 255;
          }
        }
      const uint32_t n = a / 255;
      row[px * 4 + 0] = n ? b / n : 0;
      row[px * 4 + 1] = n ? g / n : 0;
      row[px * 4 + 2] = n ? r / n : 0;
      row[px * 4 + 3] = a / 16;
    }
  }
  return db;
}
// capsule position (top-left of its image) at ts seconds into the scene
inline void capsule_at(float ts, float &x, float &y) {
  const float u = std::max(0.0f, ts - UNDOCK_GO);
  x = PORT_X - CAP_W / 2 + 3.0f * u * u;            // drifts a little to the side
  y = PORT_Y - CAP_H - (4.0f * u + 9.0f * u * u);   // backs away, gathering speed
}
inline void frame_undock() {
  const float ts = (now_ms() - st.t0) / 1000.0f;
  if (ts > UNDOCK_S) {
    stop();
    return;
  }
  float x, y;
  capsule_at(ts, x, y);
  lv_obj_set_pos(st.img, (int32_t) std::lround(x), (int32_t) std::lround(y));
  // thruster puffs: a burst of four at separation, then a pair every half second for 2 s
  auto emit = [&](float px, float py) {
    const int i = st.puff_next++ % PUFFS;
    st.puff_x[i] = px, st.puff_y[i] = py, st.puff_t[i] = ts, st.puff_step[i] = -1;
  };
  const int due = ts < UNDOCK_GO ? 0 : 1 + std::min(4, (int) ((ts - UNDOCK_GO) / 0.5f));
  while (st.last_emit < due) {  // last_emit counts bursts here
    st.last_emit += 1;
    const float cy = y + CAP_H - 8;
    emit(x - 2, cy), emit(x + CAP_W + 2, cy);
    if (st.last_emit == 1)
      emit(x + 4, y + CAP_H), emit(x + CAP_W - 4, y + CAP_H);
  }
  for (int i = 0; i < PUFFS && i < st.puff_next; i++) {
    const int step = (int) ((ts - st.puff_t[i]) / 0.06f);
    if (step == st.puff_step[i])
      continue;
    st.puff_step[i] = (int8_t) std::min(step, 127);
    const float age = step * 0.06f;
    const int a = (int) (220 - 340 * age);
    if (a <= 60) {  // gone before it reads as a dark blot (opaque: it fades toward the sky colour)
      hide(PADS + i);
      continue;
    }
    smoke_at(PADS + i, (int) st.puff_x[i], (int) st.puff_y[i], (int) (3 + 9 * age), a, 0xF4F6FA);
  }
}

inline void frame() {
  st.last_frame = now_ms();
  if (st.undock)
    frame_undock();
  else
    frame_launch();
}

inline void on_refr(lv_event_t *) {
  if (!playing())
    return;
  const uint32_t t = now_ms();
  if (st.frames++ > 0)
    st.max_gap = std::max(st.max_gap, t - st.last_refr);
  st.last_refr = t;
  auto &p = st.prof;
  const uint32_t a = now_us();
  if (p.ready) {  // the gap since the last refresh ended: the rest of the main loop
    const uint32_t idle = a - p.ready;
    p.idle += idle, p.idle_max = std::max(p.idle_max, idle);
  }
  frame();
  const uint32_t b = now_us();
  p.anim += b - a, p.anim_max = std::max(p.anim_max, b - a);
  p.start = a;
}
// UI-69g: render, flush and dirty-area accounting while the animation plays
inline void on_prof(lv_event_t *e) {
  auto &p = st.prof;
  const uint32_t t = now_us();
  switch (lv_event_get_code(e)) {
    case LV_EVENT_FLUSH_START:
      p.flush_t0 = t;
      p.chunks++;
      break;
    case LV_EVENT_FLUSH_FINISH:
      p.flush_now += t - p.flush_t0;
      break;
    case LV_EVENT_INVALIDATE_AREA:
      if (const lv_area_t *ar = (const lv_area_t *) lv_event_get_param(e))
        p.px_now += (uint32_t) lv_area_get_size(ar);
      break;
    case LV_EVENT_REFR_READY:
      if (p.start) {
        const uint32_t r = t - p.start;
        p.render += r, p.render_max = std::max(p.render_max, r);
        p.flush += p.flush_now, p.flush_max = std::max(p.flush_max, p.flush_now);
        p.px += p.px_now, p.px_max = std::max(p.px_max, p.px_now);
        p.n++;
        p.start = 0;
      }
      p.flush_now = 0, p.px_now = 0;
      p.ready = t;
      break;
    default:
      break;
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

inline void play_scene(bool undock, const char *name) {
  stop();
  if (!st.img_buf[0] || (undock && !st.capsule_buf))
    return;
  st.undock = undock;
  // a transparent full-screen layer: any tap skips
  st.catcher = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(st.catcher);
  lv_obj_set_size(st.catcher, 480, 480);
  lv_obj_add_flag(st.catcher, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(st.catcher, [](lv_event_t *) { stop(); }, LV_EVENT_CLICKED, nullptr);
  if (undock) {
    for (int k = 0; k < ISS_PARTS; k++) {
      const Part &q = ISS_SHAPE[k];
      lv_obj_t *o = lv_obj_create(lv_layer_top());
      lv_obj_remove_style_all(o);
      lv_obj_set_pos(o, q.x, q.y);
      lv_obj_set_size(o, q.w, q.h);
      lv_obj_set_style_bg_color(o, lv_color_hex(q.col), 0);
      lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
      if (q.edge) {
        lv_obj_set_style_border_color(o, lv_color_hex(q.edge), 0);
        lv_obj_set_style_border_width(o, 1, 0);
      }
      lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
      st.iss[k] = o;
    }
  }
  smoke_create();  // over the ISS, under the rocket and the banner
  st.img = lv_image_create(lv_layer_top());
  lv_image_set_src(st.img, undock ? st.capsule_buf : st.img_buf[0]);
  if (!undock)
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
  banner_text(name && *name ? name : undock ? "Undocking from the ISS" : "Rocket launch", b, sizeof(b));
  lv_label_set_text(st.label, b);
  // A long name wraps to two lines (no more: dots after that); the banner grows upward to hold
  // them, its bottom staying put (a fixed 46 px banner let the second line spill over the
  // border).
  const int32_t lh = st.font ? lv_font_get_line_height(st.font) : 20;
  lv_obj_set_style_max_height(st.label, 2 * lh, 0);
  lv_obj_update_layout(st.label);
  const int32_t text_h = std::min<int32_t>(lv_obj_get_height(st.label), 2 * lh);
  lv_obj_set_height(st.label, text_h);  // DOTS cuts a third line here
  const int32_t bh = std::max<int32_t>(46, text_h + 18);
  lv_obj_set_height(st.banner, bh);
  lv_obj_set_y(st.banner, 470 - bh);
  // the label box is ascent + descent tall; caps and digits sit in the top part of it, so
  // centring the box leaves the text high. Drop it by a third of the descent.
  const int32_t drop = st.font ? std::max<int32_t>(1, st.font->base_line / 3) : 1;
  lv_obj_align(st.label, LV_ALIGN_CENTER, 0, drop);
  st.t0 = now_ms();
  st.puff_next = 0;
  st.frames = st.max_gap = 0;
  st.last_emit = undock ? 0 : -1;  // undocking: counts thruster bursts
  for (auto &p : st.pad_step)
    p = -1;
  // Move everything at the start of each display refresh, from the clock at that moment, so
  // every frame drawn shows a fresh position (a separate 33 ms timer beat against the 16 ms
  // refresh and dropped or doubled steps). The objects it moves re-arm the next refresh. The
  // timer only covers a stretch where nothing moved and no refresh came.
  st.prof = St::Prof();
  st.last_rot = -100000;
  st.benched = false;
  bench(0);
  if (lv_display_t *d = lv_display_get_default()) {
    lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_START, nullptr);
    for (lv_event_code_t c : {LV_EVENT_REFR_READY, LV_EVENT_FLUSH_START, LV_EVENT_FLUSH_FINISH, LV_EVENT_INVALIDATE_AREA})
      lv_display_add_event_cb(d, on_prof, c, nullptr);
  }
  st.anim = lv_timer_create(
      [](lv_timer_t *) {
        if (now_ms() - st.last_frame >= 150) {
          st.prof.fallback++;
          frame();
        }
      },
      40, nullptr);
  frame();
  ESP_LOGI("rocket", "%s animation: %s", undock ? "undocking" : "launch", b);
}
inline void play(const char *name) { play_scene(false, name); }

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
  for (const auto &e : live.events) {  // UI-69f
    if (!e.iss || !e.exact || !strstr(e.type, "Undocking") || e.t > t || t - e.t > LATE_S)
      continue;
    const uint32_t k = key_of(e.name, e.t);
    if (k == st.undock_key)
      continue;
    st.undock_key = k;
    play_scene(true, e.name);
    return;
  }
}

void init(const lv_font_t *banner) {
  st.font = banner;
  for (int i = 0; i < FLAMES; i++)
    st.img_buf[i] = draw_rocket(FLAME[i]);
  st.capsule_buf = draw_capsule();
  lv_timer_create([](lv_timer_t *) { check(); }, 1000, nullptr);
}
void set_enabled(bool on) {
  st.enabled = on;
  if (!on)
    stop();
}
void play_undock_now() {
  const double t = clock_valid() ? clock_now() : 0;
  for (const auto &e : live.events)
    if (e.iss && strstr(e.type, "Undocking") && e.t > t - LATE_S) {
      play_scene(true, e.name);
      return;
    }
  play_scene(true, "Undocking from the ISS");
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
