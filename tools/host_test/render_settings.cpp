// VER-3: the Settings page drawn by the firmware's own widget code (settings_gen.inc, lifted from
// ESPHome's main.cpp by render_settings.py) with LVGL built from ESPHome's lv_conf.h. Each tab is
// shown as sat::settings_tab shows it, the controls set as Settings opens them (default states),
// the planet pictures added as sat::settings_planet_icon adds them. Writes <dir>/settings_<tab>.ppm.
#include "lvgl.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "sky_picons.h"  // the firmware's planet and alignment pictures
#define SAT_HOST_TEST
#define SKY_IMPL
namespace sat { double sat_host_now = 0; }  // the meteor animation reads the host clock
#define ESP_LOGW(tag, fmt, ...) printf("W [%s] " fmt "\n", tag, ##__VA_ARGS__)
namespace sat {  // sat_tracker.h's fmt_dur (UI-17a), which sky_about.h's device text calls; that header is too big to pull in
inline void fmt_dur(double s, char *buf, size_t n) {
  const int m = (int) (s / 60.0);
  if (m >= 24 * 60) snprintf(buf, n, "%dd %dh", m / 1440, (m % 1440) / 60);
  else if (m >= 60) snprintf(buf, n, "%dh %dm", m / 60, m % 60);
  else if (s >= 60) snprintf(buf, n, "%dm", m);
  else snprintf(buf, n, "%ds", (int) s);
}
}  // namespace sat
#include "sky_about.h"  // the About page's logo and device text
#include "fonts_gen.inc"
#include "settings_gen.inc"
#include "about_gen.inc"

static uint16_t fb[480 * 480];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const int w = a->x2 - a->x1 + 1;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) fb[y * 480 + x] = ((uint16_t *) px)[(y - a->y1) * w + (x - a->x1)];
  lv_display_flush_ready(d);
}
static uint32_t ms = 0;
static uint32_t tick() { return ms; }

static lv_image_dsc_t pic(const uint32_t *px, int w) {  // sat::make_dsc
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
static void save(const char *fn) {
  FILE *f = fopen(fn, "wb");
  fprintf(f, "P6 480 480 255\n");
  for (int i = 0; i < 480 * 480; i++) {
    const uint16_t c = (uint16_t) ((fb[i] >> 8) | (fb[i] << 8));  // LV_COLOR_16_SWAP (lv_conf.h)
    const unsigned char rgb[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2),
                                  (unsigned char) ((c & 31) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  printf("%s\n", fn);
}
static void on(lv_obj_t *o, bool v) {
  if (v)
    lv_obj_add_state(o, LV_STATE_CHECKED);
  else
    lv_obj_remove_state(o, LV_STATE_CHECKED);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  lv_init();
  lv_tick_set_cb(tick);
  alignas(LV_DRAW_BUF_ALIGN) static uint16_t buf[480 * 60];
  lv_display_t *disp = lv_display_create(480, 480);  // the default theme, as on the device
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, flush);
  lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_obj_t *page = lv_obj_create(nullptr);  // as LvPageType
  build_settings(page);
  lv_screen_load(page);
  // as the settings page opens (sky-tracker.yaml on_load): the defaults, a typical location
  lv_textarea_set_text(ta_heading, "0");
  lv_textarea_set_text(ta_lat, "61.5814");
  lv_textarea_set_text(ta_lon, "-149.4394");
  lv_slider_set_value(sl_bright, 60, LV_ANIM_OFF);
  lv_slider_set_value(sl_cone, 80, LV_ANIM_OFF);
  lv_label_set_text(lbl_cone, "80\xC2\xB0");
  for (lv_obj_t *o : {sw_sats, sw_starlink, sw_meo, sw_trails, sw_stations, sw_stars, sw_dusk, sw_planets, sw_comets,
                      sw_milky_way, sw_aur_alerts, sw_pl_alerts, sw_sky_alerts, sw_station_alerts, sw_launch_alerts,
                      sw_event_alerts, sw_lunar_alerts, sw_solar_alerts, sw_swx_alerts, sw_reentry_alerts, sw_history_alerts, sw_countdown, sw_comet_alerts, sw_splash_alerts, sw_autob})
    on(o, true);
  for (lv_obj_t *o : {sw_geo, sw_debris, sw_miles, sw_night, sw_dms, sw_24h})
    on(o, false);
  static lv_image_dsc_t saturn = pic(sat::picons::BIG_PX[4], sat::picons::BIG), align = pic(sat::picons::ALIGN_PX, sat::picons::BIG);
  image(pnl_celestial, &saturn, 130, 62);  // settings_planet_icon(pnl_celestial, 130, 62, pnl_alerts, 130, 62)
  image(pnl_alerts, &align, 130, 62);
  lv_obj_t *const P[5] = {pnl_display, pnl_location, pnl_celestial, pnl_satellites, pnl_alerts};
  lv_obj_t *const T[5] = {tab_display, tab_location, tab_celestial, tab_satellites, tab_alerts};
  static const char *const NAME[5] = {"DISPLAY", "LOCATION", "CELESTIAL", "SATELLITES", "ALERTS"};
  static const char *const FILE_[5] = {"display", "location", "celestial", "satellites", "alerts"};
  for (int k = 0; k < 5; k++) {  // sat::settings_tab
    for (int j = 0; j < 5; j++) {
      if (j == k)
        lv_obj_remove_flag(P[j], LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(P[j], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(T[j], lv_color_hex(j == k ? 0x2D5BD0 : 0x1A2547), 0);
    }
    lv_label_set_text(settings_title, NAME[k]);
    ms += 1000;
    lv_timer_handler();
    lv_obj_invalidate(page);
    lv_refr_now(disp);
    char fn[512];
    snprintf(fn, sizeof(fn), "%s/settings_%s.ppm", dir, FILE_[k]);
    FILE *f = fopen(fn, "wb");
    fprintf(f, "P6 480 480 255\n");
    for (int i = 0; i < 480 * 480; i++) {
      const uint16_t c = (uint16_t) ((fb[i] >> 8) | (fb[i] << 8));  // LV_COLOR_16_SWAP (lv_conf.h)
      const unsigned char rgb[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2),
                                    (unsigned char) ((c & 31) << 3)};
      fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%s\n", fn);
    if (k == 0) {  // the Display tab again with its time zone list open (Alaska picked)
      static int open_pass = 0;
      if (open_pass++ == 0) {
        lv_dropdown_set_selected(dd_tz, 2);
        lv_dropdown_open(dd_tz);
        ms += 1000;
        lv_timer_handler();
        lv_refr_now(disp);
        snprintf(fn, sizeof(fn), "%s/settings_display_tz_open.ppm", dir);
        FILE *g = fopen(fn, "wb");
        fprintf(g, "P6 480 480 255\n");
        for (int i = 0; i < 480 * 480; i++) {
          const uint16_t c = (uint16_t) ((fb[i] >> 8) | (fb[i] << 8));
          const unsigned char rgb[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2),
                                        (unsigned char) ((c & 31) << 3)};
          fwrite(rgb, 1, 3, g);
        }
        fclose(g);
        lv_dropdown_close(dd_tz);
        lv_dropdown_set_selected(dd_tz, 0);
        printf("%s\n", fn);
      }
    }
  }
  // the About page (UI-67): the YAML's widgets, the logo and device text as the firmware fills them
  lv_obj_t *about = lv_obj_create(nullptr);
  build_about(about);
  lv_screen_load(about);
  sat::logo_show(about_logo, 112);
  lv_label_set_text(about_built, "ESPHome 2026.9.1, built Oct 10 2026");
  char b[200];
  sat::about_device_text(b, sizeof(b), "sky-tracker-9cad68", "192.168.1.42", "home-wifi", -51, "34:85:18:9C:AD:68", 3 * 3600 + 12 * 60);
  lv_label_set_text(about_dev, b);
  ms += 1000;
  lv_timer_handler();
  lv_obj_invalidate(about);
  lv_refr_now(disp);
  char fn[512];
  snprintf(fn, sizeof(fn), "%s/about.ppm", dir);
  save(fn);
  return 0;
}
