// Comps of the Settings page with four tabs (Display, Location, Celestial, Satellites).
//   ./setcomp A|B celestial|satellites  -> out/setcomp_<opt>_<tab>.ppm
// C (4.6.23): five icon-only tabs, Alerts tab.
#include "lvgl.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "sky_picons.h"  // the firmware's planet and alignment pictures
extern "C" const lv_font_t mdi20, mono12, mono15, mono16;
static uint16_t fb[480 * 480];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  int w = a->x2 - a->x1 + 1;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) fb[y * 480 + x] = ((uint16_t *) px)[(y - a->y1) * w + (x - a->x1)];
  lv_display_flush_ready(d);
}
static uint32_t tick_ms = 0;
static uint32_t tick() { return tick_ms; }
static lv_obj_t *L(lv_obj_t *p, int x, int y, const char *t, const lv_font_t *f, uint32_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
  lv_obj_set_pos(l, x, y);
  return l;
}
static lv_obj_t *S(lv_obj_t *p, int x, int y, bool on) {
  lv_obj_t *s = lv_switch_create(p);
  lv_obj_set_pos(s, x, y);
  lv_obj_set_size(s, 56, 28);
  if (on) lv_obj_add_state(s, LV_STATE_CHECKED);
  return s;
}
static lv_obj_t *button(lv_obj_t *p, int x, int y, int w, int h, uint32_t bg, const char *t) {
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  if (t) {
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, &mono16, 0);
    lv_obj_center(l);
  }
  return b;
}
// a row: label, icon, switch on the left half; the same on the right
// the device's planet pictures (sat::make_dsc) at the spots settings_planet_icon uses
static lv_image_dsc_t pic(const uint32_t *px, int w) {
  lv_image_dsc_t d;
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_ARGB8888;
  d.header.w = d.header.h = w;
  d.header.stride = w * 4;
  d.data_size = w * w * 4;
  d.data = (const uint8_t *) px;
  return d;
}
static void image(lv_obj_t *parent, const lv_image_dsc_t *d, int x, int y) {
  lv_obj_t *i = lv_image_create(parent);
  lv_obj_remove_style_all(i);
  lv_image_set_src(i, d);
  lv_obj_set_pos(i, x, y);
}
static int RX = 250;  // right-hand labels (the Celestial panel uses 246)
static void row(lv_obj_t *p, int y, const char *l1, const char *i1, uint32_t c1, bool s1, const char *l2, const char *i2,
                uint32_t c2, bool s2) {
  const uint32_t T = 0xE6EAF5;
  L(p, 16, y + 6, l1, &mono16, T);
  if (i1) L(p, 122, y + 4, i1, &mdi20, c1);
  S(p, 150, y, s1);
  if (!l2) return;
  L(p, RX, y + 6, l2, &mono16, T);
  if (i2) L(p, 372, y + 4, i2, &mdi20, c2);
  S(p, 400, y, s2);
}
int main(int argc, char **argv) {
  // ./setcomp C celestial|satellites|alerts: the 4.6.23 layout, five icon-only tabs
  const char *tab = argc > 2 ? argv[2] : "satellites";
  const int sel = !strcmp(tab, "celestial") ? 2 : !strcmp(tab, "alerts") ? 4 : 3;
  lv_init();
  lv_tick_set_cb(tick);
  alignas(LV_DRAW_BUF_ALIGN) static uint16_t buf[480 * 60];
  lv_display_t *d = lv_display_create(480, 480);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(d, flush);
  lv_display_set_buffers(d, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_theme_t *th = lv_theme_default_init(d, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), true, &mono16);
  lv_display_set_theme(d, th);
  lv_obj_t *p = lv_screen_active();
  lv_obj_set_style_bg_color(p, lv_color_hex(0x070B18), 0);
  L(p, 16, 16, "SETTINGS", &mono16, 0xFF8A1F);
  button(p, 240, 6, 110, 38, 0x1A2547, "Cancel");
  button(p, 360, 6, 110, 38, 0x2D5BD0, "Save");
  // tabs: 5 x 91 px, 4 px apart, icon only
  const char *ICON[5] = {"\xF3\xB0\x8D\xB9", "\xF3\xB0\x8D\x8E", "\xF3\xB0\x96\x94", "\xF3\xB0\x91\xB1",
                         "\xF3\xB0\x82\x9E"};  // monitor, map-marker, weather-night, satellite-variant, bell-ring
  for (int k = 0; k < 5; k++) {
    lv_obj_t *b = button(p, 4 + k * 95, 52, 91, 44, k == sel ? 0x2D5BD0 : 0x1A2547, nullptr);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *i = lv_label_create(b);
    lv_label_set_text(i, ICON[k]);
    lv_obj_set_style_text_font(i, &mdi20, 0);
    lv_obj_set_style_text_color(i, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(i);
  }
  lv_obj_t *pn = lv_obj_create(p);  // the tab's panel
  lv_obj_remove_style_all(pn);
  lv_obj_set_pos(pn, 0, 104);
  lv_obj_set_size(pn, 480, 336);
  if (sel == 3) {
    L(pn, 16, 14, "LEO cone", &mono16, 0xE6EAF5);
    lv_obj_t *sl = lv_slider_create(pn);
    lv_obj_set_pos(sl, 150, 18);
    lv_obj_set_size(sl, 230, 14);
    lv_slider_set_range(sl, 15, 90);
    lv_slider_set_value(sl, 80, LV_ANIM_OFF);
    L(pn, 404, 14, "80\xC2\xB0", &mono16, 0xE6EAF5);
    L(pn, 150, 36, "degrees from overhead", &mono12, 0x7E8BB3);
    row(pn, 64, "LEO", "\xF3\xB0\x91\xB1", 0xF1F4FF, true, "Starlink", "\xF3\xB0\xA4\x89", 0x8FA8F0, true);
    row(pn, 108, "MEO", "\xF3\xB0\x86\xA4", 0x4FD1C5, true, "GEO", "\xF3\xB0\x87\xA7", 0xF6B93B, false);
    row(pn, 152, "Debris", "\xF3\xB0\xA9\xB9", 0xB39B7D, false, "Sat Trails", "\xF3\xB1\x9D\x81", 0x9FB2EA, true);
    row(pn, 196, "Stations", "\xF3\xB1\x8E\x83", 0xFF8A1F, true, nullptr, nullptr, 0, false);
  } else if (sel == 2) {
    row(pn, 14, "Stars", "\xF3\xB0\xAB\xA2", 0xFFF1B8, true, "After dusk", "\xF3\xB0\x96\x9B", 0xF2A65A, true);
    RX = 246;
    row(pn, 58, "Planets", nullptr, 0, true, "Comets", "\xF3\xB0\x98\xA9", 0xA8F0E0, true);
    row(pn, 102, "Milky Way", "\xF3\xB0\x82\xB8", 0xB4C4F0, true, nullptr, nullptr, 0, false);
    static lv_image_dsc_t saturn = pic(sat::picons::BIG_PX[4], sat::picons::BIG);
    image(pn, &saturn, 122, 62);  // settings_planet_icon(pnl_celestial, 122, 62, ...)
  } else {
    RX = 246;
    row(pn, 14, "Aurora", "\xF3\xB1\xAE\xB9", 0x7EE0B0, true, "Planets", nullptr, 0, true);
    row(pn, 58, "Sky events", "\xF3\xB0\x96\x94", 0xA8D8FF, true, "Stations", "\xF3\xB1\x8E\x83", 0xFF8A1F, true);
    static lv_image_dsc_t align = pic(sat::picons::ALIGN_PX, sat::picons::BIG);
    image(pn, &align, 372, 18);  // settings_planet_icon(..., pnl_alerts, 372, 18)
    row(pn, 102, "Launches", "\xF3\xB1\x93\x9E", 0xFFC46B, true, "Dockings", "\xF3\xB1\x98\x96", 0x8FD3FF, true);
  }
  button(p, 362, 440, 110, 34, 0x1A2547, "About");
  tick_ms += 1000;
  lv_timer_handler();
  lv_refr_now(d);
  char fn[64];
  snprintf(fn, sizeof(fn), "out/setcomp_C_%s.ppm", tab);
  FILE *f = fopen(fn, "wb");
  fprintf(f, "P6 480 480 255\n");
  for (int i = 0; i < 480 * 480; i++) {
    uint16_t c = fb[i];
    c = (uint16_t) ((c >> 8) | (c << 8));
    unsigned char rgb[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2), (unsigned char) ((c & 31) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  printf("%s\n", fn);
}
