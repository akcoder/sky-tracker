#ifndef HOST_DIR
#define HOST_DIR "."   // this folder (test data)
#endif
#ifndef OUT_DIR
#define OUT_DIR "out"  // renders and logs
#endif
#include "lvgl.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
extern "C" const lv_font_t mono24, mono16;
static uint16_t fb[480*480];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  int w = a->x2 - a->x1 + 1;
  for (int y = a->y1; y <= a->y2; y++) for (int x = a->x1; x <= a->x2; x++) fb[y*480+x] = ((uint16_t*)px)[(y-a->y1)*w + (x-a->x1)];
  lv_display_flush_ready(d);
}
static uint32_t tick() { return 0; }
int main(int argc, char **argv) {
  int ty = atoi(argv[1]), py = atoi(argv[2]);
  lv_init(); lv_tick_set_cb(tick);
  static uint16_t buf[480*60];
  lv_display_t *d = lv_display_create(480, 480);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(d, flush);
  lv_display_set_buffers(d, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_obj_t *p = lv_screen_active();
  lv_obj_set_style_bg_color(p, lv_color_hex(0x070B1A), 0);
  lv_obj_t *o = lv_obj_create(p);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 300, 130); lv_obj_center(o);
  lv_obj_set_style_bg_color(o, lv_color_hex(0x101B3D), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(0xFF8A1F), 0); lv_obj_set_style_border_width(o, 2, 0);
  lv_obj_set_style_radius(o, 12, 0);
  lv_obj_t *t = lv_label_create(o); lv_label_set_text(t, "UPGRADING"); lv_obj_set_style_text_font(t, &mono24, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(0xFF8A1F), 0); lv_obj_align(t, LV_ALIGN_CENTER, 0, ty);
  lv_obj_t *c = lv_label_create(o); lv_label_set_text(c, "42%"); lv_obj_set_style_text_font(c, &mono16, 0);
  lv_obj_set_style_text_color(c, lv_color_hex(0xC9D3F2), 0); lv_obj_align(c, LV_ALIGN_CENTER, 0, py);
  lv_refr_now(d);
  FILE *f = fopen(argv[3], "wb"); fprintf(f, "P6 480 480 255\n");
  for (int i = 0; i < 480*480; i++) { uint16_t v = fb[i]; v = (uint16_t)((v >> 8) | (v << 8)); unsigned char rgb[3] = {(unsigned char)((v>>11)<<3), (unsigned char)(((v>>5)&63)<<2), (unsigned char)((v&31)<<3)}; fwrite(rgb,1,3,f); }
  fclose(f);
}
