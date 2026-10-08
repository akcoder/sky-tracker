#pragma once
// UI-65 the logo on the boot page and the About page; UI-67 the About page's text.
// Compiled in its own translation unit (sky_extra.cpp, SKY_IMPL): main.cpp, with every header
// inlined into it, outgrew the reach of Xtensa l32r to its literal pool (4.5.38).
#ifndef SKY_IMPL
namespace sat {
lv_obj_t *logo_show(lv_obj_t *parent, int px);
void logo_meteor(lv_obj_t *parent);
void logo_meteor_step();
void about_device_text(char *b, size_t n, const char *name, const char *ip, const char *ssid, int rssi,
                       const char *mac, double uptime_s);
void tidy_date(const char *d, char *out, size_t n);
}  // namespace sat
#else
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include "sky_logo.h"
#ifdef SAT_HOST_TEST
#include <zlib.h>
#else
#include "miniz.h"  // the ROM's tinfl, as sky_web.h
#include "esp_heap_caps.h"
#endif
#include <cmath>

namespace sat {

// base64 -> bytes; returns the length written (0 on a bad character)
inline size_t b64_decode(const char *s, uint8_t *out, size_t cap) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  size_t n = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (; *s && *s != '='; s++) {
    const int v = val(*s);
    if (v < 0)
      return 0;
    acc = (acc << 6) | (uint32_t) v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap)
        return 0;
      out[n++] = (uint8_t) (acc >> bits);
    }
  }
  return n;
}

// The logo, inflated into PSRAM on first use and kept (120 KB). nullptr if that failed.
inline const lv_image_dsc_t *logo_dsc() {
  static lv_image_dsc_t dsc;
  static bool tried = false, ok = false;
  if (tried)
    return ok ? &dsc : nullptr;
  tried = true;
  const size_t zcap = sizeof(LOGO_B64) * 3 / 4 + 4;
#ifdef SAT_HOST_TEST
  auto *z = (uint8_t *) malloc(zcap);
  auto *raw = (uint8_t *) malloc(LOGO_RAW);
#else
  auto *z = (uint8_t *) heap_caps_malloc(zcap, MALLOC_CAP_SPIRAM);
  auto *raw = (uint8_t *) heap_caps_malloc(LOGO_RAW, MALLOC_CAP_SPIRAM);
#endif
  const size_t zn = z ? b64_decode(LOGO_B64, z, zcap) : 0;
  if (raw && zn) {
#ifdef SAT_HOST_TEST
    z_stream st = {};
    inflateInit2(&st, -15);
    st.next_in = z;
    st.avail_in = (uInt) zn;
    st.next_out = raw;
    st.avail_out = LOGO_RAW;
    ok = inflate(&st, Z_FINISH) == Z_STREAM_END && st.total_out == LOGO_RAW;
    inflateEnd(&st);
#else
    auto *inf = (tinfl_decompressor *) heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    if (inf) {
      tinfl_init(inf);
      size_t in_len = zn, out_len = LOGO_RAW;
      ok = tinfl_decompress(inf, z, &in_len, raw, raw, &out_len, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) ==
               TINFL_STATUS_DONE &&
           out_len == LOGO_RAW;
      heap_caps_free(inf);
    }
#endif
  }
  free(z);
  if (!ok) {
    free(raw);
    ESP_LOGW("sky_ui", "logo: could not unpack");
    return nullptr;
  }
  memset(&dsc, 0, sizeof(dsc));
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
  dsc.header.w = LOGO_PX;
  dsc.header.h = LOGO_PX;
  dsc.header.stride = LOGO_PX * 2;  // of the RGB565 plane; the alpha plane follows it
  dsc.data_size = LOGO_RAW;
  dsc.data = raw;
  return &dsc;
}

// The logo at `px` square (<= LOGO_PX): box-filtered down from the full size once and
// kept (LVGL's own image scaling garbles RGB565A8 on this build). nullptr on failure.
inline const lv_image_dsc_t *logo_at(int px) {
  const lv_image_dsc_t *full = logo_dsc();
  if (full == nullptr || px >= LOGO_PX)
    return full;
  static lv_image_dsc_t dsc;
  static int have = 0;
  if (have == px)
    return &dsc;
  const size_t n = (size_t) px * px * 3;
#ifdef SAT_HOST_TEST
  auto *out = (uint8_t *) (have ? realloc((void *) dsc.data, n) : malloc(n));
#else
  auto *out = (uint8_t *) (have ? heap_caps_realloc((void *) dsc.data, n, MALLOC_CAP_SPIRAM)
                                : heap_caps_malloc(n, MALLOC_CAP_SPIRAM));
#endif
  if (out == nullptr)
    return nullptr;
  const uint8_t *src = full->data, *sa = src + LOGO_PX * LOGO_PX * 2;
  uint8_t *oa = out + px * px * 2;
  for (int y = 0; y < px; y++) {
    const int y0 = y * LOGO_PX / px, y1 = std::max(y0 + 1, (y + 1) * LOGO_PX / px);
    for (int x = 0; x < px; x++) {
      const int x0 = x * LOGO_PX / px, x1 = std::max(x0 + 1, (x + 1) * LOGO_PX / px);
      uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
      for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
          const int i = yy * LOGO_PX + xx;
          const uint16_t v = src[2 * i] | (src[2 * i + 1] << 8);
          const uint32_t al = sa[i];
          r += ((v >> 11) & 31) * al;  // colours weighted by alpha: no dark fringe at the edge
          g += ((v >> 5) & 63) * al;
          b += (v & 31) * al;
          a += al;
          cnt++;
        }
      const uint16_t v = a ? (uint16_t) (((r / a) << 11) | ((g / a) << 5) | (b / a)) : 0;
      out[2 * (y * px + x)] = v & 255;
      out[2 * (y * px + x) + 1] = v >> 8;
      oa[y * px + x] = (uint8_t) (a / cnt);
    }
  }
  dsc = *full;
  dsc.header.w = px;
  dsc.header.h = px;
  dsc.header.stride = px * 2;
  dsc.data_size = n;
  dsc.data = out;
  have = px;
  return &dsc;
}

// The logo, `px` square, centred in `parent`.
lv_obj_t *logo_show(lv_obj_t *parent, int px) {
  const lv_image_dsc_t *d = logo_at(px);
  if (d == nullptr || parent == nullptr)
    return nullptr;
  lv_obj_t *im = lv_image_create(parent);
  lv_image_set_src(im, d);
  lv_obj_center(im);
  lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
  return im;
}

// UI-67a: a shooting star now and then across the About page's logo: a white head and a
// tail fading back along its path, in the open sky above the satellite, burning out at the end,
// ~0.7 s, every 3-7 s while the page shows
#ifdef SAT_HOST_TEST
extern double sat_host_now;
#endif
namespace meteor {
struct State {
  lv_obj_t *obj = nullptr;
  uint32_t t0 = 0, next = 0, dur = 700;
  float x0 = 0, y0 = 0, dx = 0, dy = 0;  // start and travel (px, in the logo box)
  float bend = 0;                         // sideways bow at mid-path (px), each one its own
  bool on = false;
};
inline State m;
constexpr float TAIL = 0.85f;  // tail length, as a part of the travel
inline uint32_t now_ms() {
#ifdef SAT_HOST_TEST
  return (uint32_t) (uint64_t) (sat_host_now * 1000.0);
#else
  return lv_tick_get();
#endif
}
inline void start(uint32_t t) {
  // in the open sky above the satellite (the logo's satellite and its dotted track run from
  // lower left to upper right through the middle; the Sun sits lower right): starts near the
  // top, falls down and to the left, and burns out before reaching the track or the rim
  const float w = (float) lv_obj_get_width(m.obj);
  const float sx = w * (0.32f + lv_rand(0, 8) / 100.0f), sy = w * (0.15f + lv_rand(0, 5) / 100.0f);
  const float a = (float) lv_rand(222, 240) * 0.0174533f;  // heading down and to the left
  const float len = w * (0.16f + lv_rand(0, 5) / 100.0f);
  m.x0 = sx;
  m.y0 = sy;
  m.dx = len * cosf(a);
  m.dy = -len * sinf(a);
  m.dur = 600 + lv_rand(0, 250);
  m.bend = ((float) lv_rand(0, 100) / 100.0f - 0.5f) * w * 0.06f;  // up to ~3 px either way at 112 px
  m.t0 = t;
  m.on = true;
}
inline void draw_cb(lv_event_t *e) {
  if (!m.on)
    return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(m.obj, &a);
  const float p = std::min(1.0f, (now_ms() - m.t0) / (float) m.dur);
  const float fade_in = std::min(1.0f, p / 0.12f);
  // burn-out over the last 30 %: the head flares (bigger, warmer) then shrinks to nothing, the
  // tail shortens and fades behind it
  const float burn = p < 0.7f ? 0.0f : (p - 0.7f) / 0.3f;           // 0 .. 1
  const float flare = burn < 0.35f ? burn / 0.35f : (1.0f - burn) / 0.65f;  // up, then down
  const float head_r = burn < 0.35f ? 1.5f + 1.3f * flare : 2.8f * (1.0f - burn) / 0.65f;
  const float glow = fade_in * (1.0f - burn * burn);                 // overall brightness
  const float hp = p * (2 - p);  // easing: quick start, slowing as it burns out
  const float len = sqrtf(m.dx * m.dx + m.dy * m.dy), nx = -m.dy / len, ny = m.dx / len;
  auto at = [&](float q, float &x, float &y) {  // the path: a straight run bowed sideways by bend
    const float b = m.bend * 4.0f * q * (1.0f - q);
    x = a.x1 + m.x0 + m.dx * q + nx * b;
    y = a.y1 + m.y0 + m.dy * q + ny * b;
  };
  float hx, hy;
  at(hp, hx, hy);
  const float tl = std::min(hp, TAIL) * (1.0f - 0.8f * burn);
  // white, warming to orange in the flare
  const lv_color_t col = lv_color_mix(lv_color_hex(0xFFB060), lv_color_hex(0xE8F0FF), (uint8_t) (255 * flare));
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = col;
  d.round_start = d.round_end = 1;
  constexpr int SEG = 6;
  for (int i = 0; i < SEG; i++) {  // brightest and widest at the head
    const float f0 = (float) i / SEG, f1 = (float) (i + 1) / SEG;
    d.width = i < 2 ? 2 : 1;
    d.opa = (lv_opa_t) (glow * 255 * (1.0f - f0) * (1.0f - f0 * 0.5f));
    float x0, y0, x1, y1;
    at(hp - tl * f0, x0, y0);
    at(hp - tl * f1, x1, y1);
    d.p1.x = (lv_value_precise_t) x0;
    d.p1.y = (lv_value_precise_t) y0;
    d.p2.x = (lv_value_precise_t) x1;
    d.p2.y = (lv_value_precise_t) y1;
    lv_draw_line(layer, &d);
  }
  if (head_r < 0.4f)
    return;
  lv_draw_rect_dsc_t h;
  lv_draw_rect_dsc_init(&h);
  h.radius = LV_RADIUS_CIRCLE;
  if (flare > 0.05f) {  // a soft glow round the flaring head
    h.bg_color = col;
    h.bg_opa = (lv_opa_t) (90 * flare * fade_in);
    const float g = head_r + 2.0f;
    const lv_area_t ga = {(int32_t) (hx - g), (int32_t) (hy - g), (int32_t) (hx + g), (int32_t) (hy + g)};
    lv_draw_rect(layer, &h, &ga);
  }
  h.bg_color = lv_color_mix(col, lv_color_hex(0xFFFFFF), (uint8_t) (160 * flare));
  h.bg_opa = (lv_opa_t) (255 * fade_in * std::min(1.0f, 0.3f + (1.0f - burn)));
  const lv_area_t ha = {(int32_t) (hx - head_r), (int32_t) (hy - head_r), (int32_t) (hx + head_r), (int32_t) (hy + head_r)};
  lv_draw_rect(layer, &h, &ha);
}

}  // namespace meteor
// one animation step (the timer's; host tests call it per frame)
void logo_meteor_step() {
  using namespace meteor;
  if (m.obj == nullptr)
    return;
  if (lv_obj_get_screen(m.obj) != lv_screen_active()) {  // not showing: start over when it does
    m.on = false;
    m.next = 0;
    return;
  }
  const uint32_t t = now_ms();
  if (m.next == 0)
    m.next = t + 1200;  // the first one soon after the page opens
  if (!m.on && (int32_t) (t - m.next) >= 0)
    start(t);
  if (m.on) {
    lv_obj_invalidate(m.obj);
    if (t - m.t0 >= m.dur) {
      m.on = false;
      m.next = t + 3000 + lv_rand(0, 4000);
    }
  }
}
void logo_meteor(lv_obj_t *parent) {
  using namespace meteor;
  if (parent == nullptr || m.obj)
    return;
  m.obj = lv_obj_create(parent);
  lv_obj_remove_style_all(m.obj);
  lv_obj_set_size(m.obj, lv_pct(100), lv_pct(100));
  lv_obj_remove_flag(m.obj, (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_event_cb(m.obj, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
#ifndef SAT_HOST_TEST
  lv_timer_create([](lv_timer_t *) { logo_meteor_step(); }, 30, nullptr);
#endif
}

// UI-67: the About page's device lines, refreshed each second while it shows
void about_device_text(char *b, size_t n, const char *name, const char *ip, const char *ssid, int rssi,
                              const char *mac, double uptime_s) {
  char up[24];
  fmt_dur(uptime_s, up, sizeof(up));  // UI-17a
  snprintf(b, n, "%s  %s\nWi-Fi %s  %d dBm\nMAC %s\nUp %s", name, ip && *ip ? ip : "-", ssid && *ssid ? ssid : "-",
           rssi, mac, up);
}
// "Oct  2 2026" (the compiler's __DATE__) -> "Oct 2 2026"
void tidy_date(const char *d, char *out, size_t n) {
  char m[4] = "";
  int day = 0, y = 0;
  if (sscanf(d, "%3s %d %d", m, &day, &y) == 3)
    snprintf(out, n, "%s %d %d", m, day, y);
  else
    snprintf(out, n, "%s", d);
}

}  // namespace sat
#endif  // SKY_IMPL
