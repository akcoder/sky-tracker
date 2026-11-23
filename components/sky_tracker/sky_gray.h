#pragma once
// A greyscale baseline JPEG (one component) into RGB565. The ROM's TJpgDec (R0.01) takes colour only, so this carries ChaN's
// later release (R0.03, sky_tjpgd*) under other names (sky_gjd_*), for greyscale files only: tracker_common is untouched.
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// jpg, n: the whole file (its sampling byte is rewritten in place); dst: tw x th RGB565, filled by nearest pixel from the closest 1/2^k scale; rows(done, ctx): rows finished so far
// (may be null). Returns null, or the reason it failed.
const char *sky_gray_decode(uint8_t *jpg, size_t n, uint16_t *dst, int tw, int th, void (*rows)(int done, void *ctx), void *ctx);
#ifdef __cplusplus
}
#endif
