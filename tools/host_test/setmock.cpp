#ifndef HOST_DIR
#define HOST_DIR "."   // this folder (test data)
#endif
#ifndef OUT_DIR
#define OUT_DIR "out"  // renders and logs
#endif
#include "lvgl.h"
#include <cstdio>
#include <cstdint>
extern "C" const lv_font_t mdi20, mono16;
static uint16_t fb[480*480];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  int w = a->x2 - a->x1 + 1;
  for (int y = a->y1; y <= a->y2; y++) for (int x = a->x1; x <= a->x2; x++) fb[y*480+x] = ((uint16_t*)px)[(y-a->y1)*w + (x-a->x1)];
  lv_display_flush_ready(d);
}
static uint32_t tick_ms = 0; static uint32_t tick() { return tick_ms; }
static lv_obj_t *L(lv_obj_t *p, int x, int y, const char *t, const lv_font_t *f, uint32_t c) {
  lv_obj_t *l = lv_label_create(p); lv_label_set_text(l, t); lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(c), 0); lv_obj_set_pos(l, x, y); return l; }
static lv_obj_t *S(lv_obj_t *p, int x, int y, bool on) {
  lv_obj_t *s = lv_switch_create(p); lv_obj_set_pos(s, x, y); lv_obj_set_size(s, 56, 28);
  if (on) lv_obj_add_state(s, LV_STATE_CHECKED); return s; }
int main(int argc, char **argv) {
  bool leo = argc < 2;
  lv_init(); lv_tick_set_cb(tick);
  static uint16_t buf[480*60];
  lv_display_t *d = lv_display_create(480, 480);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(d, flush);
  lv_display_set_buffers(d, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_theme_t *th = lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, &mono16);
  lv_display_set_theme(d, th);
  lv_obj_t *p = lv_screen_active();
  lv_obj_set_style_bg_color(p, lv_color_hex(0x070B1A), 0);
  const uint32_t T = 0xE6EAF5;
  L(p, 16, 16, "SETTINGS", &mono16, 0xFF8A1F);
  L(p, 16, 222, "Time format", &mono16, T); L(p, 190, 222, "12h", &mono16, T); S(p, 232, 216, true); L(p, 298, 222, "24h", &mono16, T);
  L(p, 16, 258, "Brightness", &mono16, T);
  lv_obj_t *sl = lv_slider_create(p); lv_obj_set_pos(sl, 200, 262); lv_obj_set_size(sl, 250, 14); lv_slider_set_value(sl, 60, LV_ANIM_OFF);
  const int R[5] = {286, 322, 358, 394, 430};
  L(p, 16, R[0]+6, "LEO", &mono16, T); S(p, 150, R[0], leo);
  lv_obj_t *sll = L(p, 250, R[0]+6, "Starlink", &mono16, T); lv_obj_t *slsw = S(p, 400, R[0], true);
  L(p, 16, R[1]+6, "MEO", &mono16, T); S(p, 150, R[1], true);
  L(p, 250, R[1]+6, "GEO", &mono16, T); S(p, 400, R[1], false);
  L(p, 16, R[2]+6, "Debris", &mono16, T); S(p, 150, R[2], true);
  L(p, 250, R[2]+6, "Sat Trails", &mono16, T); S(p, 400, R[2], true);
  L(p, 16, R[3]+6, "Stars", &mono16, T); S(p, 150, R[3], true);
  L(p, 250, R[3]+6, "After dusk", &mono16, T); S(p, 400, R[3], true);
  L(p, 16, R[4]+6, "Miles", &mono16, T); S(p, 150, R[4], false);
  L(p, 420, 460, "v4.2.0", &mono16, 0x5A6687);
  L(p, 122, R[0]+4, "\xF3\xB0\x91\xB1", &mdi20, 0xF1F4FF);
  lv_obj_t *isl = L(p, 372, R[0]+4, "\xF3\xB0\xA4\x89", &mdi20, 0x8FA8F0);
  L(p, 122, R[1]+4, "\xF3\xB0\x86\xA4", &mdi20, 0x4FD1C5);
  L(p, 372, R[1]+4, "\xF3\xB0\x87\xA7", &mdi20, 0xF6B93B);
  L(p, 122, R[2]+4, "\xF3\xB0\xA9\xB9", &mdi20, 0xB39B7D);
  if (!leo) {
    lv_obj_add_state(slsw, LV_STATE_DISABLED); lv_obj_set_style_text_color(sll, lv_color_hex(0x5A6687), 0);
    lv_obj_set_style_text_color(isl, lv_color_hex(0x3A4566), 0);
  }
  tick_ms += 1000; lv_timer_handler(); lv_refr_now(d);
  FILE *f = fopen(leo ? "setmock.ppm" : "setmock_off.ppm", "wb"); fprintf(f, "P6 480 480 255\n");
  for (int i = 0; i < 480*480; i++) { uint16_t c = fb[i]; c = (uint16_t)((c >> 8) | (c << 8)); unsigned char rgb[3] = {(unsigned char)((c>>11)<<3), (unsigned char)(((c>>5)&63)<<2), (unsigned char)((c&31)<<3)}; fwrite(rgb,1,3,f); }
  fclose(f);
}
