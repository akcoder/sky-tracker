#pragma once
// EASTER EGG (the first rule of fight club applies): never exposed in the on-screen UI (no on-screen
// button, setting or text; the only on-screen trigger is the hidden touch on the About logo, and it also walks
// by itself, started from sky_octopus.h). Its button is web UI / Home Assistant only.
// Never discuss it outside this file, its web UI entities and its requirement in docs/requirements.md: no README,
// release notes, changelog or marketing.
// A dinosaur walks across the screen when the logo on the About page is touched, stops in the middle, roars
// a word in a speech bubble, and walks on (the timing is sky_dinologic.h, the pictures sky_dino_data.h).
//
// What keeps it fast (what sky-tracker's launch and plane-tracker's cheer learned on this panel):
//  * A SNAPSHOT of the screen is the walk's opaque backdrop, an image that sits on the active screen
//    as its top child. LVGL then culls everything beneath it, so a dirty box costs one image blit and
//    the dinosaur, not a redraw of the page's rows, flags and bars (plane-tracker's UI-66b). The page
//    stands still for the walk. With no memory for the snapshot, no walk.
//  * frame() runs on the display's LV_EVENT_REFR_START, from the clock at that moment, so every frame
//    drawn shows a fresh position; the objects it moves re-arm the next refresh. A 40 ms timer only
//    covers a stretch with no refresh (sky-tracker's UI-69a).
//  * Opaque, small, nothing translucent but the sprite's own antialiased edge; the sprite is an
//    RGB565A8 image drawn 1:1 (LVGL's fast path); a walk to the left uses mirrored copies made once,
//    in PSRAM, for that walk. The bubble is drawn once, when it appears.
//  * A walk logs its frames, rate and longest gap, as sky-tracker's animations do.
//
// Needs the build flag -DLV_USE_SNAPSHOT=1 and the font for the bubble. A touch ends a walk.
// Copied from f1-tracker (which has it walk by itself now and then); here the About logo, the web button and the random visits (sky_octopus.h check()) start one.
// Compiled in its own translation unit (sky_extra.cpp, SKY_IMPL), as sky_about.h (BUILD-7); the rest of
// the firmware sees only walk() and set_font().
#include <cstdint>
#ifndef SKY_IMPL
namespace sat {
namespace dino {
void set_font(const lv_font_t *f);
bool walk(uint32_t seed);
}  // namespace dino
}  // namespace sat
#elif defined(USE_ESP32) && !defined(SAT_HOST_TEST)
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sat {
namespace rocket {
void set_last_fps(float fps);  // sky_rocket.h
}
}  // namespace sat

#include "sky_dinologic.h"
#include "sky_dino_data.h"
#include "esp_heap_caps.h"
#include "esphome/core/log.h"
namespace sat {
namespace dino {

inline const lv_font_t *bubble_font = nullptr;

struct St {
  bool running = false, done = false;
  Walk w;
  uint32_t t0 = 0, start = 0, last_frame = 0;
  lv_draw_buf_t *snap = nullptr;
  lv_obj_t *root = nullptr, *img = nullptr, *bubble = nullptr, *label = nullptr, *tail = nullptr;
  lv_timer_t *timer = nullptr;
  uint8_t *mirror = nullptr;                  // the poses facing left: one block in PSRAM, per walk
  lv_image_dsc_t pose_dsc[N_POSES];
  lv_image_dsc_t tail_dsc;
  uint8_t *tail_mirror = nullptr;
  int pose = -1, x = -99999, y = -99999;
  bool bubble_on = false;
  uint32_t frames = 0, max_gap = 0;
};
inline St st;
inline constexpr size_t POSE_BYTES = (size_t) DATA_W * DATA_H * 3, TAIL_BYTES = (size_t) TAIL_W * TAIL_H * 3;

inline void *big_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
inline void big_free(void *p) { heap_caps_free(p); }

// An RGB565A8 image flipped left to right: its RGB plane (2 bytes a pixel), then its alpha plane.
inline void mirror_into(uint8_t *dst, const uint8_t *src, int w, int h) {
  const uint16_t *s565 = (const uint16_t *) src;
  uint16_t *d565 = (uint16_t *) dst;
  const uint8_t *sa = src + (size_t) w * h * 2;
  uint8_t *da = dst + (size_t) w * h * 2;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      d565[(size_t) y * w + x] = s565[(size_t) y * w + (w - 1 - x)];
      da[(size_t) y * w + x] = sa[(size_t) y * w + (w - 1 - x)];
    }
}

inline void describe(lv_image_dsc_t &d, const uint8_t *data, int w, int h) {
  std::memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_RGB565A8;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * 2;                 // the RGB plane's
  d.data_size = (uint32_t) w * h * 3;
  d.data = data;
}

inline constexpr uint32_t ENTRY_SKIP_MS = 1300;
inline void end_walk();

// Move everything for this refresh, from the clock at this moment.
inline void frame() {
  if (!st.running || st.done) return;
  const uint32_t now = lv_tick_get();
  const uint32_t t = now - st.t0;
  const uint32_t since = now - st.start;     // real time since the tap (t starts ahead: see begin_walk)
  if (st.last_frame) {
    const uint32_t gap = now - st.last_frame;
    if (gap > st.max_gap) st.max_gap = gap;
  }
  st.last_frame = now;
  st.frames++;
  // a touch since the start ends the walk; so does the page changing under it
  if ((since > 2500 && lv_display_get_inactive_time(nullptr) + 100 < since) || lv_obj_get_parent(st.root) != lv_screen_active() ||
      finished(st.w, t)) {
    st.done = true;                        // torn down by the timer, outside the refresh
    return;
  }
  const Frame f = at(st.w, t);
  if (f.pose != st.pose) {
    st.pose = f.pose;
    lv_image_set_src(st.img, &st.pose_dsc[f.pose]);
  }
  if (f.x != st.x || f.y != st.y) {
    st.x = f.x;
    st.y = f.y;
    lv_obj_set_pos(st.img, f.x, f.y);
  }
  if (f.bubble != st.bubble_on) {
    st.bubble_on = f.bubble;
    if (f.bubble) {
      lv_obj_update_layout(st.bubble);
      const int bw = lv_obj_get_width(st.bubble), bh = lv_obj_get_height(st.bubble);
      // over the head: right edge to the sprite's right edge going right, left edge to its left edge going left
      int bx = st.w.flip ? f.x - 6 : f.x + DATA_W + 6 - bw;
      if (bx < 4) bx = 4;
      if (bx + bw > SCREEN_W - 4) bx = SCREEN_W - 4 - bw;
      const int by = f.y - bh - TAIL_H + 4;
      lv_obj_set_pos(st.bubble, bx, by);
      lv_obj_set_pos(st.tail, st.w.flip ? bx + 14 : bx + bw - TAIL_W - 14, by + bh - 2);
      lv_obj_remove_flag(st.bubble, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(st.tail, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(st.bubble, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(st.tail, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

inline void on_refr(lv_event_t *) { frame(); }

inline void end_walk() {
  if (!st.running) return;
  if (lv_display_t *d = lv_display_get_default()) lv_display_remove_event_cb_with_user_data(d, on_refr, nullptr);
  if (st.timer) lv_timer_delete(st.timer);
  st.timer = nullptr;
  const uint32_t ms = lv_tick_get() - st.start;
  if (st.root) lv_obj_delete(st.root);                         // and its children
  st.root = st.img = st.bubble = st.label = st.tail = nullptr;
  if (st.snap) lv_draw_buf_destroy(st.snap);
  st.snap = nullptr;
  big_free(st.mirror);
  big_free(st.tail_mirror);
  st.mirror = st.tail_mirror = nullptr;
  if (ms) rocket::set_last_fps(st.frames * 1000.0f / ms);   // the Last Animation FPS entity
  ESP_LOGI("dino", "walk: %u frames in %u ms (%.1f fps), longest gap %u ms", (unsigned) st.frames, (unsigned) ms,
           ms ? st.frames * 1000.0f / ms : 0.0f, (unsigned) st.max_gap);
  st.running = false;
  st.done = false;
}

// Start a walk now (the seed picks it). False: no walk (no memory).
inline bool begin_walk(uint32_t seed) {
  if (st.running || bubble_font == nullptr) return false;
  lv_obj_t *scr = lv_screen_active();
  st.w = make_walk(seed);
  // the screen as it is, as the opaque backdrop
  st.snap = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
  if (st.snap == nullptr) {
    ESP_LOGW("dino", "no memory for the snapshot: no walk");
    return false;
  }
  const uint8_t *pose_src[N_POSES], *tail_src = st.w.flip ? TAIL_L : TAIL_R;
  for (int p = 0; p < N_POSES; p++) pose_src[p] = POSES[p];
  if (st.w.flip) {                                             // facing left: mirrored copies for this walk
    st.mirror = (uint8_t *) big_alloc(POSE_BYTES * N_POSES);
    if (st.mirror == nullptr) {
      lv_draw_buf_destroy(st.snap);
      st.snap = nullptr;
      return false;
    }
    for (int p = 0; p < N_POSES; p++) {
      mirror_into(st.mirror + POSE_BYTES * p, POSES[p], DATA_W, DATA_H);
      pose_src[p] = st.mirror + POSE_BYTES * p;
    }
  }
  for (int p = 0; p < N_POSES; p++) describe(st.pose_dsc[p], pose_src[p], DATA_W, DATA_H);
  describe(st.tail_dsc, tail_src, TAIL_W, TAIL_H);

  st.root = lv_image_create(scr);
  lv_image_set_src(st.root, st.snap);
  lv_obj_set_size(st.root, SCREEN_W, SCREEN_H);   // fixed: an image sizes itself to its content, and a sprite past the
                                                   // right edge made it wider and shifted the picture right
  lv_obj_set_pos(st.root, 0, 0);
  lv_obj_remove_flag(st.root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(st.root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_foreground(st.root);

  st.img = lv_image_create(st.root);
  lv_image_set_src(st.img, &st.pose_dsc[WALK0]);
  lv_obj_remove_flag(st.img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(st.img, -DATA_W - 10, -DATA_H - 10);   // off screen until the first frame

  st.bubble = lv_obj_create(st.root);
  lv_obj_remove_style_all(st.bubble);
  lv_obj_set_size(st.bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(st.bubble, lv_color_hex(0xF2F5FC), 0);
  lv_obj_set_style_bg_opa(st.bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(st.bubble, lv_color_hex(0xFF8A1F), 0);
  lv_obj_set_style_border_width(st.bubble, 2, 0);
  lv_obj_set_style_radius(st.bubble, 10, 0);
  lv_obj_set_style_pad_hor(st.bubble, 10, 0);
  lv_obj_set_style_pad_ver(st.bubble, 5, 0);
  lv_obj_remove_flag(st.bubble, LV_OBJ_FLAG_CLICKABLE);
  st.label = lv_label_create(st.bubble);
  lv_obj_set_style_text_font(st.label, bubble_font, 0);
  lv_obj_set_style_text_color(st.label, lv_color_hex(0x101420), 0);
  lv_label_set_text(st.label, WORDS[st.w.word]);
  lv_obj_add_flag(st.bubble, LV_OBJ_FLAG_HIDDEN);

  st.tail = lv_image_create(st.root);
  lv_image_set_src(st.tail, &st.tail_dsc);
  lv_obj_remove_flag(st.tail, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(st.tail, LV_OBJ_FLAG_HIDDEN);

  st.pose = -1;
  st.x = st.y = -99999;
  st.bubble_on = false;
  st.frames = st.max_gap = st.last_frame = 0;
  st.done = false;
  st.running = true;
  st.start = lv_tick_get();
  st.t0 = st.start - ENTRY_SKIP_MS;          // already walking in when it is first drawn, not 1.8 s off screen
  if (lv_display_t *d = lv_display_get_default()) lv_display_add_event_cb(d, on_refr, LV_EVENT_REFR_START, nullptr);
  st.timer = lv_timer_create(
      [](lv_timer_t *) {
        if (st.done) { end_walk(); return; }
        if (lv_tick_get() - st.last_frame >= 150) frame();     // no refresh came: cover the gap
      },
      40, nullptr);
  frame();
  return true;
}

inline bool walking() { return st.running; }

void set_font(const lv_font_t *f) { bubble_font = f; }
bool walk(uint32_t seed) { return begin_walk(seed); }

}  // namespace dino
}  // namespace sat
#endif  // device only
