#pragma once
// UI-59: streaming PNG decoder for the NOAA SUVI Sun image (1280x1280 RGBA, ~1.1 MB).
// Bytes are fed as they arrive; nothing but two source rows is held. The picture is
// box-filtered (every source pixel averaged in) straight into a small RGB565 buffer,
// so a 1280 px image comes out smooth at 360 px rather than point-sampled and grainy.
// Inflate: the ESP32-S3 ROM's tinfl (miniz) on the device, zlib in the host tests.
// Supported: 8-bit grey, grey+alpha, RGB, RGBA, not interlaced (what SUVI sends).
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(SAT_HOST_TEST) && !defined(SKYPNG_TINFL)
#define SKYPNG_ZLIB 1
#include <zlib.h>
#else
#include "miniz.h"  // ROM tinfl (host: -DSKYPNG_TINFL tests the same path against miniz.c)
#endif
#ifndef SAT_HOST_TEST
#include "esp_heap_caps.h"
#endif

namespace skypng {

struct Decoder {
  // ---- set by begin()
  uint16_t *out = nullptr;  // tw x th RGB565, written a row at a time
  int tw = 0, th = 0;
  // ---- results
  uint32_t W = 0, H = 0;
  bool done = false;  // IEND seen and every row decoded
  const char *error = nullptr;

  ~Decoder() { release(); }

  // crop: the central fraction of the source that is kept (SUVI: 0.75 fills the view with the disc)
  bool begin(uint16_t *dst, int w, int h, float crop = 1.0f) {
    release();
    out = dst;
    tw = w;
    th = h;
    crop_ = crop > 0.05f && crop <= 1.0f ? crop : 1.0f;
    W = H = 0;
    done = false;
    error = nullptr;
    st_ = SIG;
    hn_ = 0;
    y_ = 0;
    rpos_ = 0;
    acc_ty_ = -1;
    inflate_done_ = false;
#ifdef SKYPNG_ZLIB
    memset(&zs_, 0, sizeof(zs_));
    if (inflateInit(&zs_) != Z_OK)
      return fail("inflate init");
    z_open_ = true;
#else
    inf_ = (tinfl_decompressor *) alloc(sizeof(tinfl_decompressor));
    dict_ = (uint8_t *) alloc(TINFL_LZ_DICT_SIZE);
    if (!inf_ || !dict_)
      return fail("out of memory");
    tinfl_init(inf_);
    dict_ofs_ = 0;
#endif
    return true;
  }

  // Feed the next bytes of the file. False once an error is set (see `error`).
  bool feed(const uint8_t *p, size_t n) {
    while (n > 0 && !error && st_ != END) {
      switch (st_) {
        case SIG: {
          static const uint8_t SIGB[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
          if (*p != SIGB[hn_])
            return fail("not a PNG");
          p++, n--;
          if (++hn_ == 8)
            st_ = HDR, hn_ = 0;
          break;
        }
        case HDR:
          hb_[hn_++] = *p++, n--;
          if (hn_ == 8) {
            left_ = be32(hb_);
            memcpy(type_, hb_ + 4, 4);
            hn_ = 0;
            if (left_ > 0x7FFFFFFF)
              return fail("bad chunk");
            if (memcmp(type_, "IHDR", 4) == 0 && left_ != 13)
              return fail("bad header");
            st_ = left_ ? DATA : CRC;
            if (!left_ && !chunk_end())
              return false;
          }
          break;
        case DATA: {
          const size_t k = n < left_ ? n : left_;
          if (memcmp(type_, "IHDR", 4) == 0) {
            memcpy(ihdr_ + (13 - left_), p, k);
          } else if (memcmp(type_, "IDAT", 4) == 0) {
            if (!cur_)
              return fail("image data before header");
            if (!inflate(p, k))
              return false;
          }
          p += k, n -= k, left_ -= (uint32_t) k;
          if (left_ == 0) {
            st_ = CRC;
            if (!chunk_end())
              return false;
          }
          break;
        }
        case CRC:  // not checked: TLS already guards the transfer
          p++, n--;
          if (++hn_ == 4) {
            hn_ = 0;
            st_ = memcmp(type_, "IEND", 4) == 0 ? END : HDR;
            if (st_ == END) {
              if (y_ < H)
                return fail("image cut short");
              done = true;
            }
          }
          break;
        case END:
          break;
      }
    }
    return error == nullptr;
  }

  void release() {
#ifdef SKYPNG_ZLIB
    if (z_open_)
      inflateEnd(&zs_);
    z_open_ = false;
#else
    free_(inf_);
    free_(dict_);
    inf_ = nullptr;
    dict_ = nullptr;
#endif
    free_(cur_);
    free_(prev_);
    free_(acc_);
    free_(xmap_);
    cur_ = prev_ = nullptr;
    acc_ = nullptr;
    xmap_ = nullptr;
  }

 private:
  enum St : uint8_t { SIG, HDR, DATA, CRC, END } st_ = SIG;
  uint8_t hb_[8];
  int hn_ = 0;
  uint32_t left_ = 0;
  char type_[4] = {0, 0, 0, 0};
  uint8_t ihdr_[13];
  int bpp_ = 0;  // bytes per pixel
  size_t stride_ = 0, rpos_ = 0;
  uint8_t *cur_ = nullptr, *prev_ = nullptr;  // filter byte + row
  uint32_t *acc_ = nullptr;                    // tw x (r, g, b, n)
  uint16_t *xmap_ = nullptr;                   // source column -> target column
  uint32_t y_ = 0;
  int acc_ty_ = -1;
  float crop_ = 1.0f;
  uint32_t cx0_ = 0, cy0_ = 0, cw_ = 0, ch_ = 0;  // kept source window
  bool inflate_done_ = false;
#ifdef SKYPNG_ZLIB
  z_stream zs_;
  bool z_open_ = false;
#else
  tinfl_decompressor *inf_ = nullptr;
  uint8_t *dict_ = nullptr;
  size_t dict_ofs_ = 0;
#endif

  static uint32_t be32(const uint8_t *b) {
    return (uint32_t) b[0] << 24 | (uint32_t) b[1] << 16 | (uint32_t) b[2] << 8 | b[3];
  }
  static void *alloc(size_t n) {
#ifdef SAT_HOST_TEST
    return malloc(n);
#else
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
#endif
  }
  static void free_(void *p) {
#ifdef SAT_HOST_TEST
    free(p);
#else
    heap_caps_free(p);
#endif
  }
  bool fail(const char *e) {
    if (!error)
      error = e;
    return false;
  }

  bool chunk_end() {
    if (memcmp(type_, "IHDR", 4) != 0)
      return true;
    W = be32(ihdr_);
    H = be32(ihdr_ + 4);
    const uint8_t depth = ihdr_[8], color = ihdr_[9], interlace = ihdr_[12];
    bpp_ = color == 0 ? 1 : color == 4 ? 2 : color == 2 ? 3 : color == 6 ? 4 : 0;
    if (depth != 8 || bpp_ == 0 || interlace != 0)
      return fail("unsupported PNG type");
    if (W == 0 || H == 0 || W > 4096 || H > 4096)
      return fail("unexpected image size");
    cw_ = (uint32_t) (W * crop_ + 0.5f);
    ch_ = (uint32_t) (H * crop_ + 0.5f);
    cx0_ = (W - cw_) / 2;
    cy0_ = (H - ch_) / 2;
    if ((int) cw_ < tw || (int) ch_ < th)
      return fail("unexpected image size");
    stride_ = (size_t) W * bpp_ + 1;
    cur_ = (uint8_t *) alloc(stride_);
    prev_ = (uint8_t *) alloc(stride_);
    acc_ = (uint32_t *) alloc(sizeof(uint32_t) * 4 * tw);
    xmap_ = (uint16_t *) alloc(sizeof(uint16_t) * W);
    if (!cur_ || !prev_ || !acc_ || !xmap_)
      return fail("out of memory");
    memset(prev_, 0, stride_);
    memset(acc_, 0, sizeof(uint32_t) * 4 * tw);
    for (uint32_t x = 0; x < W; x++)  // 0xFFFF: outside the crop
      xmap_[x] = x < cx0_ || x >= cx0_ + cw_ ? 0xFFFF : (uint16_t) ((uint64_t) (x - cx0_) * tw / cw_);
    return true;
  }

  bool inflate(const uint8_t *p, size_t n) {
    if (inflate_done_)
      return true;  // trailing bytes after the zlib stream: ignored
#ifdef SKYPNG_ZLIB
    static uint8_t obuf[16384];
    zs_.next_in = (Bytef *) p;
    zs_.avail_in = (uInt) n;
    do {
      zs_.next_out = obuf;
      zs_.avail_out = sizeof(obuf);
      const int r = ::inflate(&zs_, Z_NO_FLUSH);
      if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR)
        return fail("corrupt image data");
      if (!rows(obuf, sizeof(obuf) - zs_.avail_out))
        return false;
      if (r == Z_STREAM_END) {
        inflate_done_ = true;
        break;
      }
      if (r == Z_BUF_ERROR)
        break;
    } while (zs_.avail_in > 0 || zs_.avail_out == 0);
    return true;
#else
    for (;;) {
      size_t in_sz = n, out_sz = TINFL_LZ_DICT_SIZE - dict_ofs_;
      const tinfl_status s = tinfl_decompress(inf_, p, &in_sz, dict_, dict_ + dict_ofs_, &out_sz,
                                              TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
      p += in_sz;
      n -= in_sz;
      if (out_sz && !rows(dict_ + dict_ofs_, out_sz))
        return false;
      dict_ofs_ = (dict_ofs_ + out_sz) & (TINFL_LZ_DICT_SIZE - 1);
      if (s < TINFL_STATUS_DONE)
        return fail("corrupt image data");
      if (s == TINFL_STATUS_DONE) {
        inflate_done_ = true;
        return true;
      }
      if (s == TINFL_STATUS_NEEDS_MORE_INPUT && n == 0)
        return true;
      if (in_sz == 0 && out_sz == 0 && s != TINFL_STATUS_HAS_MORE_OUTPUT)
        return true;  // no progress: wait for more input
    }
#endif
  }

  bool rows(const uint8_t *d, size_t n) {
    while (n > 0) {
      if (y_ >= H)
        return true;  // padding after the last row
      const size_t k = n < stride_ - rpos_ ? n : stride_ - rpos_;
      memcpy(cur_ + rpos_, d, k);
      rpos_ += k, d += k, n -= k;
      if (rpos_ == stride_) {
        if (!unfilter())
          return false;
        take_row();
        uint8_t *t = prev_;
        prev_ = cur_;
        cur_ = t;
        rpos_ = 0;
        y_++;
        if (y_ == H)
          flush_row();
      }
    }
    return true;
  }

  bool unfilter() {
    uint8_t *r = cur_ + 1;
    const uint8_t *u = prev_ + 1;
    const size_t len = stride_ - 1;
    const int b = bpp_;
    switch (cur_[0]) {
      case 0:
        break;
      case 1:
        for (size_t i = b; i < len; i++)
          r[i] += r[i - b];
        break;
      case 2:
        for (size_t i = 0; i < len; i++)
          r[i] += u[i];
        break;
      case 3:
        for (size_t i = 0; i < len; i++)
          r[i] += (uint8_t) (((i >= (size_t) b ? r[i - b] : 0) + u[i]) >> 1);
        break;
      case 4:
        for (size_t i = 0; i < len; i++) {
          const int a = i >= (size_t) b ? r[i - b] : 0, c = i >= (size_t) b ? u[i - b] : 0, up = u[i];
          const int pp = a + up - c, pa = abs(pp - a), pb = abs(pp - up), pc = abs(pp - c);
          r[i] += (uint8_t) (pa <= pb && pa <= pc ? a : pb <= pc ? up : c);
        }
        break;
      default:
        return fail("bad row filter");
    }
    return true;
  }

  void take_row() {
    if (y_ < cy0_ || y_ >= cy0_ + ch_)
      return;
    const int ty = (int) ((uint64_t) (y_ - cy0_) * th / ch_);
    if (ty != acc_ty_) {
      flush_row();
      acc_ty_ = ty;
    }
    const uint8_t *s = cur_ + 1;
    for (uint32_t x = 0; x < W; x++, s += bpp_) {
      uint32_t r, g, bl, a;
      switch (bpp_) {
        case 1: r = g = bl = s[0], a = 255; break;
        case 2: r = g = bl = s[0], a = s[1]; break;
        case 3: r = s[0], g = s[1], bl = s[2], a = 255; break;
        default: r = s[0], g = s[1], bl = s[2], a = s[3]; break;
      }
      if (xmap_[x] == 0xFFFF)
        continue;
      uint32_t *q = acc_ + 4 * xmap_[x];
      q[0] += r * a;  // over black
      q[1] += g * a;
      q[2] += bl * a;
      q[3] += 1;
    }
  }

  void flush_row() {
    if (acc_ty_ < 0 || acc_ty_ >= th)
      return;
    uint16_t *o = out + (size_t) acc_ty_ * tw;
    for (int x = 0; x < tw; x++) {
      uint32_t *q = acc_ + 4 * x;
      const uint32_t d = q[3] ? q[3] * 255 : 1;
      const uint32_t r = (q[0] + d / 2) / d, g = (q[1] + d / 2) / d, b = (q[2] + d / 2) / d;
      o[x] = (uint16_t) ((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
      q[0] = q[1] = q[2] = q[3] = 0;
    }
    acc_ty_ = -1;
  }
};

}  // namespace skypng
