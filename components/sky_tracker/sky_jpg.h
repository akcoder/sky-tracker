#pragma once
// UI-60: baseline JPEG from memory, decoded at full size and averaged 2x2 (a box filter)
// into an RGB565 picture, centred and cropped (the NASA Dial-A-Moon frame: 730 px -> 365,
// shown at 360). TJpgDec hands over MCU blocks (8 or 16 px, even-sized even at the image
// edge for even image sizes), so every 2x2 square lies inside one block. The device uses
// the ESP32-S3 ROM's TJpgDec (R0.01, RGB888 out); the host tests build ChaN's later release
// from LVGL. Its own descaling is not used (LVGL's build of it is off and misbehaves).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifdef SAT_HOST_TEST
extern "C" {
#include "tjpgd.h"  // host: /home/claude/host/tjpg (JD_FORMAT 0, JD_USE_SCALE 1)
}
namespace skyjpg_rom {
using ::JDEC;
using ::JRECT;
using ::JRESULT;
using ::jd_decomp;
using ::jd_prepare;
typedef size_t in_len_t;
typedef int out_ret_t;
}  // namespace skyjpg_rom
#else
namespace skyjpg_rom {  // the ROM header's short type names (BYTE, WORD, ...) stay in here
#include "rom/tjpgd.h"
typedef UINT in_len_t;
typedef UINT out_ret_t;
}  // namespace skyjpg_rom
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace skyjpg {

// UI-59f: decoding while the file downloads. `have` counts the bytes in so far (the reader
// waits for more until `eof`); `rows` is told how many output rows are finished, top down.
struct Stream {
  volatile size_t *have = nullptr;
  volatile bool *eof = nullptr;
  void (*rows)(int done, void *ctx) = nullptr;
  void *ctx = nullptr;
};

struct Src {
  const uint8_t *p;
  size_t len, pos;
  uint16_t *dst;
  int tw, th, offx, offy;
  const Stream *st = nullptr;
};

inline skyjpg_rom::in_len_t jpg_in(skyjpg_rom::JDEC *jd, uint8_t *buf, skyjpg_rom::in_len_t n) {
  Src *s = (Src *) jd->device;
  if (s->st && s->st->have) {  // wait for the download to bring these bytes (or end)
    while (s->pos + n > *s->st->have && !*s->st->eof) {
#ifndef SAT_HOST_TEST
      vTaskDelay(pdMS_TO_TICKS(5));
#endif
    }
    s->len = *s->st->have;
  }
  const size_t k = n < s->len - s->pos ? (size_t) n : s->len - s->pos;
  if (buf)
    memcpy(buf, s->p + s->pos, k);
  s->pos += k;
  return (skyjpg_rom::in_len_t) k;
}

inline skyjpg_rom::out_ret_t jpg_out(skyjpg_rom::JDEC *jd, void *bitmap, skyjpg_rom::JRECT *r) {
  Src *s = (Src *) jd->device;
  const uint8_t *px = (const uint8_t *) bitmap;  // RGB888, the rectangle row by row
  const int w = r->right - r->left + 1, stride = 3 * w;
  for (int y = r->top; y + 1 <= r->bottom; y += 2) {
    const int ty = y / 2 - s->offy;
    if (ty < 0 || ty >= s->th)
      continue;
    uint16_t *o = s->dst + (size_t) ty * s->tw;
    const uint8_t *a = px + (size_t) (y - r->top) * stride;
    for (int x = r->left; x + 1 <= r->right; x += 2, a += 6) {
      const int tx = x / 2 - s->offx;
      if (tx < 0 || tx >= s->tw)
        continue;
      const uint8_t *b = a + stride;
      const unsigned R = (a[0] + a[3] + b[0] + b[3] + 2) >> 2, G = (a[1] + a[4] + b[1] + b[4] + 2) >> 2,
                     B = (a[2] + a[5] + b[2] + b[5] + 2) >> 2;
      o[tx] = (uint16_t) ((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3));
    }
  }
  if (s->st && s->st->rows && r->right + 1 >= (int) jd->width)  // the end of a row of blocks
    s->st->rows(std::max(0, std::min(s->th, (r->bottom + 1) / 2 - s->offy)), s->st->ctx);
  return 1;
}

// Returns nullptr on success, else a short reason. `sw`/`sh`: the source size.
inline const char *decode_half(const uint8_t *jpg, size_t len, uint16_t *dst, int tw, int th, int *sw = nullptr,
                               int *sh = nullptr, const Stream *st = nullptr) {
  static uint8_t pool[4096] __attribute__((aligned(4)));  // TJpgDec work area (~3.1 KB needed)
  Src s{jpg, len, 0, dst, tw, th, 0, 0, st};
  skyjpg_rom::JDEC jd;
  memset(&jd, 0, sizeof(jd));
  skyjpg_rom::JRESULT r = skyjpg_rom::jd_prepare(&jd, jpg_in, pool, sizeof(pool), &s);
  if (r != 0)
    return r == 8 || r == 7 ? "unsupported JPEG (progressive?)" : "not a readable JPEG";
  if (sw)
    *sw = (int) jd.width;
  if (sh)
    *sh = (int) jd.height;
  const int hw = (int) (jd.width + 1) / 2, hh = (int) (jd.height + 1) / 2;  // at 1/2
  if (hw < tw || hh < th)
    return "picture smaller than expected";
  s.offx = (hw - tw) / 2;
  s.offy = (hh - th) / 2;  // (every target pixel is written: the picture is at least the target)
  r = skyjpg_rom::jd_decomp(&jd, jpg_out, 0);
  if (r == 0 && st && st->rows)
    st->rows(th, st->ctx);
  return r == 0 ? nullptr : "corrupt JPEG data";
}

// Any size down to tw x th (UI-62, the 678 px GOES Earth): a box filter. The sums live in
// `acc` (3 x uint16 per target pixel: at most 4 source pixels of 255 per cell below 2:1).
struct Fit {
  uint16_t *dst, *acc;
  int tw, th, W, H;
  uint16_t *xm, *ym;  // source column / row -> target
  int stride = 0, next = 0;  // UI-59f: rows below `next` are finished (written to dst)
};
inline uint8_t fit_cx[1024], fit_cy[1024];  // source pixels per target column / row
inline uint16_t fit_last[1024];             // the last source row of each target row
inline void fit_row(Fit *f, int y) {  // one target row: the sums averaged into dst
  for (int x = 0; x < f->tw; x++) {
    const uint16_t *a = f->acc + ((size_t) y * f->tw + x) * 3;
    const unsigned n = fit_cx[x] * fit_cy[y], h2 = n / 2;
    const unsigned R = (a[0] + h2) / n, G = (a[1] + h2) / n, B = (a[2] + h2) / n;
    f->dst[(size_t) y * (f->stride > 0 ? f->stride : f->tw) + x] = (uint16_t) ((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3));
  }
}
inline skyjpg_rom::out_ret_t fit_out(skyjpg_rom::JDEC *jd, void *bitmap, skyjpg_rom::JRECT *r) {
  Src *s = (Src *) jd->device;
  Fit *f = (Fit *) s->dst;  // (reused pointer: the Fit rides in Src.dst)
  const uint8_t *px = (const uint8_t *) bitmap;
  for (int y = r->top; y <= r->bottom; y++) {
    if (f->ym[y] == 0xFFFF) {  // outside the source window
      px += 3 * (r->right - r->left + 1);
      continue;
    }
    uint16_t *row = f->acc + (size_t) f->ym[y] * f->tw * 3;
    for (int x = r->left; x <= r->right; x++, px += 3) {
      if (f->xm[x] == 0xFFFF)
        continue;
      uint16_t *a = row + f->xm[x] * 3;
      a[0] += px[0], a[1] += px[1], a[2] += px[2];
    }
  }
  if (r->right + 1 >= f->W) {  // the end of a row of blocks: the target rows now complete
    const int before = f->next;
    while (f->next < f->th && fit_last[f->next] <= r->bottom)
      fit_row(f, f->next++);
    if (s->st && s->st->rows && f->next != before)
      s->st->rows(f->next, s->st->ctx);
  }
  return 1;
}
// `keep`: the fraction of the height kept from the top, centred square window (UI-62a: NOAA's
// regional frames carry a label strip along the bottom). 1 = the whole picture.
// `stride`: pixels per destination row (0 = tw), to decode into part of a larger picture.
inline const char *decode_fit(const uint8_t *jpg, size_t len, uint16_t *dst, int tw, int th, uint16_t *acc,
                              int *sw = nullptr, int *sh = nullptr, float keep = 1.0f, int stride = 0,
                              bool centre = false,  // centre: the kept square in the middle (SOHO's Sun)
                              const Stream *st = nullptr) {
  static uint8_t pool[4096] __attribute__((aligned(4)));
  static uint16_t xm[2048], ym[2048];
  static Fit f;
  Src s{jpg, len, 0, (uint16_t *) &f, tw, th, 0, 0, st};
  skyjpg_rom::JDEC jd;
  memset(&jd, 0, sizeof(jd));
  if (skyjpg_rom::jd_prepare(&jd, jpg_in, pool, sizeof(pool), &s) != 0)
    return "not a readable JPEG";
  const int W = (int) jd.width, H = (int) jd.height;
  if (sw)
    *sw = W;
  if (sh)
    *sh = H;
  const int wh = keep >= 1.0f ? H : (int) (H * keep), ww = keep >= 1.0f ? W : std::min(W, wh);
  const int x0 = (W - ww) / 2, y0 = centre ? (H - wh) / 2 : 0;
  if (ww < tw || wh < th || W > 2048 || H > 2048 || ww >= 2 * tw || wh >= 2 * th)
    return "unexpected picture size";
  uint8_t *cx = fit_cx, *cy = fit_cy;
  memset(fit_cx, 0, sizeof(fit_cx));
  memset(fit_cy, 0, sizeof(fit_cy));
  memset(fit_last, 0, sizeof(fit_last));
  for (int x = 0; x < W; x++)
    xm[x] = x < x0 || x >= x0 + ww ? 0xFFFF : (uint16_t) ((int64_t) (x - x0) * tw / ww);
  for (int y = 0; y < H; y++)
    ym[y] = y < y0 || y >= y0 + wh ? 0xFFFF : (uint16_t) ((int64_t) (y - y0) * th / wh);
  for (int x = 0; x < W; x++)
    if (xm[x] != 0xFFFF)
      cx[xm[x]]++;
  for (int y = 0; y < H; y++)
    if (ym[y] != 0xFFFF) {
      cy[ym[y]]++;
      fit_last[ym[y]] = (uint16_t) y;
    }
  f = Fit{dst, acc, tw, th, W, H, xm, ym, stride, 0};
  memset(acc, 0, sizeof(uint16_t) * 3 * tw * th);
  if (skyjpg_rom::jd_decomp(&jd, fit_out, 0) != 0)
    return "corrupt JPEG data";
  while (f.next < th)  // (normally all done by the last row of blocks)
    fit_row(&f, f.next++);
  if (st && st->rows)
    st->rows(th, st->ctx);
  return nullptr;
}

// UI-61e: 1:1 into part of a larger picture (the planet photos, stored at their display
// size): no scaling, no sums. Its own work area, so the loop can use it while the net task
// decodes a download.
struct Copy {
  uint16_t *dst;
  int w, h, stride;
};
inline skyjpg_rom::out_ret_t copy_out(skyjpg_rom::JDEC *jd, void *bitmap, skyjpg_rom::JRECT *r) {
  Src *s = (Src *) jd->device;
  const Copy *c = (const Copy *) s->dst;
  const uint8_t *px = (const uint8_t *) bitmap;
  for (int y = r->top; y <= r->bottom; y++)
    for (int x = r->left; x <= r->right; x++, px += 3)
      if (x < c->w && y < c->h)
        c->dst[(size_t) y * c->stride + x] = (uint16_t) ((px[0] >> 3) << 11 | (px[1] >> 2) << 5 | (px[2] >> 3));
  return 1;
}
inline const char *decode_copy(const uint8_t *jpg, size_t len, uint16_t *dst, int stride, int maxw, int maxh) {
  static uint8_t pool[4096] __attribute__((aligned(4)));
  static Copy c;
  Src s{jpg, len, 0, (uint16_t *) &c, 0, 0, 0, 0};
  skyjpg_rom::JDEC jd;
  memset(&jd, 0, sizeof(jd));
  if (skyjpg_rom::jd_prepare(&jd, jpg_in, pool, sizeof(pool), &s) != 0)
    return "not a readable JPEG";
  c = Copy{dst, std::min((int) jd.width, maxw), std::min((int) jd.height, maxh), stride};
  return skyjpg_rom::jd_decomp(&jd, copy_out, 0) == 0 ? nullptr : "corrupt JPEG data";
}

}  // namespace skyjpg
