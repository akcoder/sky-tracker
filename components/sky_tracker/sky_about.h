#pragma once
// UI-65 the logo on the boot page and the About page; UI-67 the About page's text.
// Compiled in its own translation unit (sky_extra.cpp, SKY_IMPL): main.cpp, with every header
// inlined into it, outgrew the reach of Xtensa l32r to its literal pool (4.5.38).
#ifndef SKY_IMPL
namespace sat {
lv_obj_t *logo_show(lv_obj_t *parent, int px);
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
