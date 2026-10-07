// sat_tracker.h — Sky Tracker scheduling and LVGL drawing (requirements rev 4)
//
// Runs on the ESPHome main loop only (ARCH-6). Entry points called from YAML:
//   sat::setup(cfg, widgets)  on_boot
//   sat::tick()               every 2 s (interval)
//   sat::ota_begin()/ota_error()
// and read-only accessors for the Home Assistant sensors (ARCH-9).
#pragma once

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "sat_net.h"
#include "sky_stars.h"
#include "sky_flags.h"
#include "sky_wmm.h"
#include "sky_planets.h"
#include "sky_pview.h"
#include "sky_picons.h"
#include "sky_lore.h"
#include "sky_cfig.h"
#include "sky_events.h"
#include "sky_mw.h"

#ifndef SAT_HOST_TEST
#include "esphome/core/time.h"
#include "esphome/components/network/util.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_lcd_panel_rgb.h"
#include "esphome/components/mipi_rgb/mipi_rgb.h"
#endif

namespace sat {

// ================================================================== PERF-14 scan-synced updates
// The panel has one framebuffer in PSRAM, read out top to bottom every frame (~22 ms at 12 MHz)
// through 10-row bounce buffers. A change written to rows the scan is passing shows for one
// frame half old, half new: a tear. The status line (rows ~26-69) changes are rendered right
// after the scan has passed it, so they land behind it. wait_scan_past() waits at most a frame.
namespace vs {
#ifdef SAT_HOST_TEST
inline void install(void *) {}
inline void wait_scan_past(int) {}
#else
inline volatile int64_t last_us = 0, period_us = 0;
inline bool on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  const int64_t n = esp_timer_get_time();
  if (last_us)
    period_us = n - last_us;
  last_us = n;
  return false;
}
struct Peek : esphome::mipi_rgb::MipiRgb {  // the driver keeps its panel handle protected
  static esp_lcd_panel_handle_t handle(esphome::mipi_rgb::MipiRgb *m) { return static_cast<Peek *>(m)->handle_; }
};
inline void install(esphome::mipi_rgb::MipiRgb *d) {
  if (d == nullptr || Peek::handle(d) == nullptr)
    return;
  esp_lcd_rgb_panel_event_callbacks_t cb = {};
  cb.on_vsync = on_vsync;
  if (esp_lcd_rgb_panel_register_event_callbacks(Peek::handle(d), &cb, nullptr) != ESP_OK)
    ESP_LOGW("sat_ui", "vsync callback not registered");
}
// wait until the scan (and the bounce-buffer refill ~20 rows ahead of it) is below row
// `row` and well short of the bottom, so rows above `row` can be written untorn
inline void wait_scan_past(int row) {
  constexpr int V_RES = 480, V_BLANK = 30, LEAD = 24, TOTAL = V_RES + V_BLANK;
  const int64_t per = period_us;
  if (per < 10000 || per > 50000)
    return;  // no vsync seen yet
  const int64_t lo = per * (V_BLANK + row + LEAD) / TOTAL, hi = per * 85 / 100;
  const int64_t give_up = esp_timer_get_time() + per + 2000;
  while (esp_timer_get_time() < give_up) {
    const int64_t ph = (esp_timer_get_time() - last_us) % per;
    if (ph >= lo && ph <= hi)
      return;
    esp_rom_delay_us(200);
  }
}
#endif
}  // namespace vs

static const char *const UI_TAG = "sat_ui";

// ================================================================== geometry (see sky_math.h)
namespace geo {

constexpr float EXTRAP_CAP_S = 150.0f;  // MOTION-2

// Satellite ECEF at time t, extrapolated along its arc (MOTION-1..3).
inline V3 sat_ecef(const SatRec &r, double t) {
  V3 p = v3(r.p);
  float alt = r.alt_km;
  if (r.has_vel) {
    float dt = (float) (t - r.t_fix);
    if (dt > EXTRAP_CAP_S)
      dt = EXTRAP_CAP_S;
    if (dt < -EXTRAP_CAP_S)
      dt = -EXTRAP_CAP_S;
    p = rotate(p, v3(r.axis), r.rate * dt);
    alt += r.alt_rate * dt;
  }
  const float lat = asinf(std::max(-1.0f, std::min(1.0f, p.z))) / DEG;
  const float lon = atan2f(p.y, p.x) / DEG;
  return ecef(lat, lon, alt);
}

}  // namespace geo

// ================================================================== drawing constants
#ifndef SAT_SKY_BG
#define SAT_SKY_BG 0x0B1220  // UI-11: deep, slightly desaturated blue (4.3.2)
#endif
constexpr uint32_t SKY_BG = SAT_SKY_BG;  // UI-11: keep in step with sky_box bg_color in the YAML
constexpr uint32_t C_RING = 0x2B3C6B;
constexpr uint32_t C_TEXT = 0xC9D3F2;
constexpr uint32_t C_DIM = 0x7E8BB3;
constexpr uint32_t C_LEO = 0xF1F4FF;
constexpr uint32_t C_MEO = 0x4FD1C5;
constexpr uint32_t C_GEO = 0xF6B93B;
constexpr uint32_t C_DEBRIS = 0xB39B7D;  // UI-29: trash-can icon
constexpr uint32_t C_GEO_DOT = 0x9C7A3C; // UI-28: dim GEO dots
constexpr uint32_t C_STARLINK = 0x4F74DB;
constexpr uint32_t C_ISS = 0xFF8A1F;
constexpr uint32_t C_BAR = 0x2D5BD0, C_BAR_BG = 0x1A2547;  // UI-71: every progress bar (fill, track)
constexpr uint32_t C_SUN = 0xFFD54A;
constexpr uint32_t C_MOON = 0xDCE2EE;
// UI-40 planets: name tags and alert text, one tint per planet
constexpr uint32_t C_PLANET[5] = {0xC8C4C0, 0xF4E6B8, 0xF08A5A, 0xE8CFA0, 0xE8D29A};
constexpr uint32_t C_ALIGN = 0xB8C4F0;   // UI-41 alignment alerts
constexpr uint32_t C_CSS = 0xFF5C6C;     // UI-52 Tiangong
constexpr uint32_t C_METEOR = 0xA8D8FF;  // UI-47 meteor showers
constexpr uint32_t C_ECLIPSE = 0xFFB070; // UI-51 eclipses
constexpr uint32_t C_LAUNCH = 0xFFC46B;  // UI-54 rocket launches
constexpr uint32_t C_EVENT = 0x8FD3FF;   // UI-54c dockings, undockings, EVAs
constexpr uint32_t C_DSO = 0xC3B2F5;     // UI-56 deep-sky objects
constexpr uint32_t C_COMET = 0xA8F0E0;   // UI-63 comets: pale cyan-green (a gas coma)
constexpr uint32_t C_MW = 0xB4C4F0;      // UI-64 Milky Way, mixed faintly over SKY_BG
constexpr float LOW_EL = 10.0f;          // UI-43: below this, markers are dimmed and untagged

// PERF-9: nothing on the map is drawn with transparency or shadows. Blending makes
// LVGL read the PSRAM buffer back for every pixel; these colours are pre-mixed
// over SKY_BG instead and drawn opaque.
constexpr uint32_t mix(uint32_t fg, uint32_t bg, uint32_t a) {  // a: 0..255 of fg
  return ((((fg >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * (255 - a)) / 255) << 16 |
         ((((fg >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * (255 - a)) / 255) << 8 |
         (((fg & 0xFF) * a + (bg & 0xFF) * (255 - a)) / 255);
}
constexpr uint32_t C_RING_INNER = mix(C_RING, SKY_BG, 179);
constexpr uint32_t C_CROSS = mix(C_RING, SKY_BG, 128);
constexpr uint32_t C_LABEL = mix(C_TEXT, SKY_BG, 204);
constexpr uint32_t C_STARLINK_DIM = mix(C_STARLINK, SKY_BG, 179);
constexpr uint32_t C_STARLINK_ICON = mix(C_STARLINK, SKY_BG, 210);  // UI-31: Starlink icons, dimmer than satellites
constexpr uint32_t C_SUN_HALO = mix(C_SUN, SKY_BG, 80);
constexpr uint32_t C_MOON_EDGE = mix(C_DIM, SKY_BG, 128);

// UI-21 star map
#ifndef SAT_PAGE_BG
#define SAT_PAGE_BG 0x000000  // black around the disc (4.3.2)
#endif
constexpr uint32_t PAGE_BG = SAT_PAGE_BG;          // page background around the disc
constexpr uint32_t C_STAR = 0xEEF1FA;
constexpr uint32_t C_FIG = mix(0x7F93D0, SKY_BG, 64);    // constellation figures: faint
constexpr uint32_t C_FIG_MAJOR = mix(0x9FB2EA, SKY_BG, 150);  // sky::FIG_MAJOR figures: brighter, 2 px
constexpr uint32_t C_CNAME = mix(0x8E9BC6, SKY_BG, 150);  // constellation names
constexpr float STAR_MAG_DOT = 3.5f;       // brighter: sized dot; fainter figure stars: 1 px
constexpr int NAME_MAX_RANK = 2;           // 1 major .. 3 minor (d3-celestial ranks)
constexpr double STAR_PERIOD = 120;        // the sky turns ~0.5 px a minute on this map; one full-disc redraw per period
constexpr float DUSK_SUN_EL = -6.0f;       // civil twilight ends

// Boot-time pool creation can take a while with a large pool: keep the task watchdog fed
// and report how long it took.
#ifdef SAT_HOST_TEST
inline void ui_feed_wdt() {}
inline int64_t ui_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#else
#include "esp_timer.h"
inline void ui_feed_wdt() { esphome::App.feed_wdt(); }
inline int64_t ui_us() { return esp_timer_get_time(); }
#endif
constexpr int SAT_POOL = 72;       // UI-14 (LEO + MEO markers, UI-28)
constexpr int STARLINK_POOL = 200;  // UI-14: markers made at boot (80° cone from 61°N: typically 100-190 up; 400 crashed at boot)
constexpr int SAT_PX = 7, STARLINK_PX = 4, ISS_PX = 16, SUN_PX = 20, SUN_HALO_PX = 30, MOON_PX = 20;
constexpr float DECAY = 0.55f;      // MOTION-5, per 2 s tick
constexpr float SNAP_DEG = 30.0f;   // MOTION-5
constexpr int LIST_ROWS = 18;       // UI-10: table rows including the header

// DATA-4: propagation cadence (on-board maths, no API limits) and the element check
constexpr double SAT_PERIOD = 10, STARLINK_PERIOD = 30, STARLINK_OFFSET = 5, ISS_PERIOD = 5;
inline double ELEM_PERIOD = 300;  // TEST-1: shortened by a debug build
constexpr double STATUS_OK_SHOW_S = 5 * 60.0;  // UI-25: how long "updated X ago" stays on screen
constexpr double STALE_S = 3 * 86400.0;  // UI-25: warn when the orbit data was downloaded longer ago than this

struct Widgets {
  lv_obj_t *sky = nullptr;        // sky disc (bg SKY_BG, no padding/border, not scrollable)
  lv_obj_t *status = nullptr;     // UI-25: data status / error line under the title
  lv_obj_t *counts = nullptr;
  lv_obj_t *clock = nullptr;
  lv_obj_t *foot_iss = nullptr;
  lv_obj_t *foot_pass = nullptr;
  lv_obj_t *coords = nullptr;                              // UI-23: observer, top centre
  lv_obj_t *alm_sun = nullptr;                             // UI-20, bottom-left corner
  lv_obj_t *alm_moon = nullptr, *alm_phase = nullptr;       // UI-20, bottom-right corner
  lv_obj_t *moon_img = nullptr;                             // UI-20 phase picture: an ARGB8888 canvas
  lv_obj_t *list = nullptr;       // page 2: lv_table (UI-10)
  const lv_font_t *label_font = nullptr;  // map name tags, list, card body (mono12)
  const lv_font_t *title_font = nullptr;  // card title (mono16)
  const lv_font_t *card_font = nullptr;        // UI-24: card body, 2 pt larger (mono14)
  const lv_font_t *card_title_font = nullptr;  // UI-24: card title (mono18)
  const lv_font_t *alert_font = nullptr;       // UI-41: status-line alerts (mono15)
  const lv_font_t *icon_font = nullptr;   // UI-29: trash-can glyph for debris
  const lv_font_t *card_icon_font = nullptr;  // UI-30: object-type icon on the details card
  const lv_font_t *card_icon_big = nullptr;   // UI-30: the same icons at 40 px, the Moon picture's size (4.5.5)
};

// ================================================================== loop-owned state
struct Marker {
  lv_obj_t *dot = nullptr;
  lv_obj_t *label = nullptr;
  lv_obj_t *icon = nullptr;  // UI-31: type icon inside the (transparent) dot
  lv_obj_t *flag = nullptr;  // UI-31: GNSS constellation flag inside the label
  int32_t id = -1;     // bound satellite id (MOTION-4)
  SatRec rec;          // the record this marker draws; replaced only on fresh data
  uint32_t color = 0;
  int label_w = 0;
  int px = INT_MIN, py = INT_MIN;  // last pushed top-left (PERF-5)
  int lx = INT_MIN, ly = INT_MIN;
  bool shown = false;
  bool label_left = false;
  float offx = 0, offy = 0;  // MOTION-5 offset, pixels
  int dx = 0, dy = 0;        // last drawn centre (UI-19)
  float el = 0;              // elevation at that draw
  bool up = false;           // drawn this tick (above its horizon)
  // UI-26 motion trail: centre now and 10/20/30 s ago, disc coordinates
  int16_t tx[4] = {}, ty[4] = {};
  lv_area_t tr_area{0, 0, -1, -1};  // what the trail covers on screen (for invalidation); empty when x2 < x1
  uint8_t style = 255;              // UI-28/29: MS_* marker style last applied
  bool centred = false;             // label drawn on the point itself (debris icon)
  uint32_t icol = 0, drawn_col = 0; // UI-43: full colour, and the colour last applied (dimmed when low)
  bool low = false;                 // UI-43: below LOW_EL at the last draw
};
enum MarkerStyle : uint8_t { MS_ROUND = 0, MS_SQUARE = 1, MS_DEBRIS = 2 };
struct GeoPt {  // UI-28: a geosynchronous satellite drawn as a dot by the sky callback
  int16_t x, y;
  int32_t idx;  // index into live.geo
};

struct Ui {
  Widgets w;
  geo::Observer obs;
  int size = 0, cx = 0, cy = 0, radius = 0;
  int n_deco = 0;
  pvector<Marker> sats, starlink;
  std::unordered_map<int32_t, int> sat_slot, starlink_slot;
  Marker iss;
  lv_obj_t *sun = nullptr, *moon = nullptr;
  float map_moon_k = -1, map_moon_ang = 0;  // UI-6 phase picture last drawn
  int sun_x = INT_MIN, sun_y = INT_MIN, moon_x = INT_MIN, moon_y = INT_MIN, shadow_x = INT_MIN, shadow_y = INT_MIN;
  bool sun_shown = false, moon_shown = false;
  astro::SunMoon sm{};
  astro::Event sun_ev, moon_ev;   // UI-20
  double alm_next = 0;            // next rise/set search, UTC seconds
  // UI-21/22 layer switches
  bool sats_on = true, starlink_on = true;  // sats_on: the LEO switch (UI-28)
  bool meo_on = true, geo_on = false;
  std::vector<GeoPt> geo_pts;
  lv_area_t geo_area{0, 0, -1, -1};
  Marker geo_sel;  // UI-28: the tapped GEO satellite, for the details card
  bool stars_on = true, stars_dusk = true;  // stars_dusk: only after civil twilight
  bool stars_shown = false;
  bool net_down = false;          // NET-8: Wi-Fi was down on the last schedule() call
  bool star_cb = false;           // draw callback installed on the sky disc
  struct StarPt {
    int16_t x, y;
    uint8_t size;
    uint8_t shade;                // 0..255 of C_STAR over SKY_BG
  };
  struct Seg {
    int16_t x0, y0, x1, y1;
    bool major;                   // part of a sky::FIG_MAJOR figure
  };
  std::vector<StarPt> star_pts;   // disc coordinates, recomputed every STAR_PERIOD
  struct DsoPt {
    int16_t x, y;
    uint8_t idx;                  // ev::DSO
  };
  std::vector<DsoPt> dso_pts;     // UI-56, with the stars
  std::vector<Seg> star_segs;
  double star_next = 0;
  struct NameTag {
    lv_obj_t *l = nullptr;
    int idx = 0;                  // into sky::NAMES
    int x = 0, y = 0, w = 0, h = 0;
    bool fits = false;            // above the horizon, inside the disc, clear of higher-rank names
    bool shown = false;
  };
  std::vector<NameTag> names;
  float icon_illum = -1;          // phase picture last drawn for this illumination...
  bool icon_waxing = false, icon_south = false;  // ...this side, this hemisphere
  // derived ISS state for the footer and the sensors
  geo::AzEl iss_azel{0, -90};
  bool iss_sunlit = false;
  bool iss_valid = false;
  int sat_undrawn = 0, starlink_undrawn = 0;
  int starlink_in_cone = 0;  // UI-4: Starlink inside starlink_radius (drawn layer, counts, list)
  // schedule (FAIL-7: job times live here only)
  bool started = false;
  double next_sat = 0, next_starlink = 0, next_iss = 0, next_elem = 0;
  int64_t pass_refetched_for = 0;
  double pass_retry_at = 0;  // DATA-6a
  bool want_pass = false;
  double loaded_seen = 0;          // live.status.loaded last acted on
  // UI-24 tap-for-details
  lv_obj_t *card = nullptr, *card_title = nullptr, *card_body = nullptr, *sel_ring = nullptr;
  lv_obj_t *card_icon = nullptr;  // UI-30
  lv_obj_t *card_flag = nullptr;  // UI-34: owner flag after the country code
  lv_obj_t *card_img_btn = nullptr, *card_img_lbl = nullptr;  // UI-59/60: "Sun image" / "Moon image", bottom left
  lv_obj_t *card_moon = nullptr;  // UI-36: phase picture on the Moon card (UI-46a: or a constellation figure)
  float moon_drawn_k = -1;         // phase last drawn into card_moon (-1: something else is there)
  int fig_drawn = -1;              // UI-46a: constellation figure last drawn into card_moon
  float decl = 0, maglat = 0;     // HW-9a declination, UI-37 geomagnetic latitude (degrees)
  lv_obj_t *aurora_icon = nullptr;  // UI-38: beside the status-line notice
  double decl_at = 0;             // when they were computed (UTC)
  bool debris_on = true;          // UI-35: rocket bodies and debris shown
  int sel_kind = -1;               // Kind of the selected object, -1 = nothing selected
  int32_t sel_id = 0;
  double sel_since = 0;
  bool arc_shown = false;          // pass / path arc drawn (UI-24 ISS, UI-36 Sun and Moon)
  uint32_t arc_color = 0;
  bool arc_rise_dot = false;       // ISS: dot at the rise end
  std::vector<lv_point_t> arc_ticks;  // UI-36: whole local hours along a Sun/Moon path
  std::vector<lv_point_t> arc;     // next pass, disc coordinates
  // UI-26
  bool trails_on = true;
  // UI-10 list row kinds for the table colours
  std::vector<uint8_t> row_kind;
  bool ota = false;
  bool ready = false;
  float heading = 0;               // UI-15: compass direction at the top of the map
  bool h24 = true;                 // UI-17: clock and pass times in 24-hour format
  bool miles = false;              // UI-29: distances in miles instead of km
  lv_obj_t *compass_tags[4] = {};  // N, E, S, W
  // UI-40 planets
  bool planets_on = true;
  lv_obj_t *planet_img[planets::N_PLANETS] = {}, *planet_lbl[planets::N_PLANETS] = {};
  bool planet_shown[planets::N_PLANETS] = {}, planet_lbl_shown[planets::N_PLANETS] = {};
  int planet_x[planets::N_PLANETS] = {}, planet_y[planets::N_PLANETS] = {};
  geo::AzEl planet_azel[planets::N_PLANETS] = {};
  float planet_mag[planets::N_PLANETS] = {};
  bool planet_visible[planets::N_PLANETS] = {};  // UI-41: bright, high and the sky dark enough
  double planet_next = 0;
  // UI-63 comets: the brightest few (live.comet_list index per slot, -1 = empty)
  static constexpr int MAX_COMETS = 4;
  bool comets_on = true;
  lv_obj_t *comet_dot[MAX_COMETS] = {}, *comet_lbl[MAX_COMETS] = {};
  bool comet_shown[MAX_COMETS] = {}, comet_lbl_shown[MAX_COMETS] = {};
  int comet_idx[MAX_COMETS] = {-1, -1, -1, -1};
  int comet_x[MAX_COMETS] = {}, comet_y[MAX_COMETS] = {};    // head centre, disc coordinates
  int comet_tx[MAX_COMETS] = {}, comet_ty[MAX_COMETS] = {};  // tail end
  lv_area_t comet_area[MAX_COMETS] = {{0, 0, -1, -1}, {0, 0, -1, -1}, {0, 0, -1, -1}, {0, 0, -1, -1}};
  geo::AzEl comet_azel[MAX_COMETS] = {};
  float comet_mag[MAX_COMETS] = {};
  bool comet_visible[MAX_COMETS] = {};
  double comet_next = 0, comets_at = -1;
  // UI-64 Milky Way: per 2x2 block of the disc, where it looks (hour angle, Dec) and the
  // horizon fade; the drawn spans are rebuilt from those each star period
  struct MwCell {
    uint16_t h;    // hour angle, 360/65536 degree units
    int16_t dec;   // 0.01 degree
    uint8_t fade;  // 0 = outside the disc
  };
  struct MwSpan {
    int16_t x0, x1;  // disc x, inclusive
    uint8_t lvl;     // 1..MW_LV
  };
  bool mw_on = true;
  pvector<MwCell> mw_cells;
  pvector<MwSpan> mw_spans;
  pvector<uint32_t> mw_band;  // first span of each 2-px band (+1 end)
  int mw_n = 0;
  float mw_heading = NAN, mw_lat = NAN;
  lv_obj_t *alert_img = nullptr;   // UI-41: planet / alignment picture beside the status line
  // UI-42 Starlink trains (marker indices into ui.starlink, in order along the train)
  struct Train {
    std::vector<int> idx;
    lv_area_t area{0, 0, -1, -1};  // screen area last invalidated
    lv_obj_t *lbl = nullptr;
    bool shown = false;
    int lx = INT_MIN, ly = INT_MIN;
  };
  Train trains[2];
  int n_trains = 0;
  // UI-45 night mode
  bool night_on = false, night_active = false;
  // UI-41a alert switches (UI-47..51: sky events)
  bool aurora_alerts = true, planet_alerts = true, sky_alerts = true;
  bool station_alerts = true, launch_alerts = true, event_alerts = true;  // UI-41e
  bool lunar_alerts = true;  // UI-41f: full Moon, Moon near a planet or star, lunar eclipses
  bool comet_alerts = true;  // UI-41g: a comet bright enough to see (the layer must be on too)
  bool splash_alerts = true;  // UI-41h: capsules splashing down (Launch Library "Spacecraft Landing")
  bool stations_on = true;  // UI-52b: ISS and Tiangong drawn on the map
  // UI-52 Tiangong
  Marker css;
  double css_pass_retry_at = 0;
  int64_t css_refetched_for = 0;
  // UI-47 meteor shower radiant on the map (index into ev::SHOWERS, -1 none)
  int radiant = -1, radiant_x = 0, radiant_y = 0;
  bool radiant_up = false;
  // UI-53 time scrub: the sky (stars, planets, Sun, Moon, radiants) drawn at now + scrub_s
  bool scrub_on = false;
  double scrub_s = 0, scrub_touched = 0;
  float view_sun_el = 0;  // Sun elevation at the time drawn
  lv_obj_t *scrub_panel = nullptr, *scrub_slider = nullptr, *scrub_label = nullptr;
};
inline Ui ui;
constexpr int K_SUN = 4, K_MOON = 5, K_PLANET = 6, K_CONST = 7, K_STAR = 8, K_SHOWER = 9, K_CSS = 10, K_DSO = 11, K_COMET = 12, K_INFO = 13;  // UI selections beyond the net's Kind values
inline void scrub_build();
inline bool scrub_swallow_click = false;  // UI-53

// UI-38: tonight's aurora outlook, for the status-line notice and the northern glow
struct AuroraNow {
  double until = 0;       // re-evaluate after this (UTC); 0 = now
  bool show = false;      // notice on: possible or likely during tonight's dark hours
  bool likely = false;    // overhead likely (else low in the north)
  bool dark_now = false;  // Sun below -12° now: the glow is drawn
  float kp = -1;          // most in tonight's dark window
  double k0 = 0, k1 = 0;  // dark window (Sun below -12°)
  bool nowcast = false;   // UI-58: raised by NOAA's OVATION nowcast, not the Kp outlook
};
inline AuroraNow aur;
inline void aurora_update(double t);
inline const char *aurora_word(float kp) {  // UI-38: plain words for Kp
  return kp < 3.5f ? "faint" : kp < 4.5f ? "moderate" : kp < 6.0f ? "strong" : "very strong";
}

// ================================================================== small LVGL helpers
inline void set_text_if(lv_obj_t *l, const char *s) {
  if (l == nullptr)
    return;
  const char *cur = lv_label_get_text(l);
  if (cur == nullptr || strcmp(cur, s) != 0)
    lv_label_set_text(l, s);  // PERF-6a
}
inline void set_shown(lv_obj_t *o, bool &state, bool want) {
  if (state == want)
    return;
  state = want;
  if (want)
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
inline lv_obj_t *plain(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                                         LV_OBJ_FLAG_SCROLL_ON_FOCUS));
  return o;
}
inline lv_obj_t *dot(lv_obj_t *parent, int px, uint32_t color, lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, px, px);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  return o;
}
// UI-31: satellites are drawn as 14 px icons when the icon font is there
constexpr int ICON_PX = 14;
constexpr int COMET_PX = 6;  // UI-63
inline int mark_r() { return ui.w.icon_font ? ICON_PX / 2 : SAT_PX / 2; }
inline int sat_px() { return ui.w.icon_font ? ICON_PX : SAT_PX; }
inline int sl_px() { return ui.w.icon_font ? ICON_PX : STARLINK_PX; }
inline lv_obj_t *make_icon(lv_obj_t *dot, uint32_t color) {
  lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(dot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_t *ic = lv_label_create(dot);
  lv_obj_remove_style_all(ic);
  lv_obj_set_style_text_font(ic, ui.w.icon_font, 0);
  lv_obj_set_style_text_color(ic, lv_color_hex(color), 0);
  lv_label_set_text(ic, "");
  lv_obj_center(ic);
  return ic;
}

// UI-31: flags for the GNSS constellations (G USA, E EU, R Russia, C China), 12x9 from
// sky_flags.h; UI-34: 16x12 owner flags for the details card.
inline lv_image_dsc_t make_dsc(const uint32_t *px, int w, int h) {
  lv_image_dsc_t d;
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_ARGB8888;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * 4;
  d.data_size = w * h * 4;
  d.data = (const uint8_t *) px;
  return d;
}
// UI-40/41 planet pictures (14 px on the map, 20 px in alerts) and the alignment icon
inline const lv_image_dsc_t *planet_dsc(int p, bool big) {
  static lv_image_dsc_t small[planets::N_PLANETS], large[planets::N_PLANETS];
  static bool made = false;
  if (!made) {
    made = true;
    for (int i = 0; i < planets::N_PLANETS; i++) {
      small[i] = make_dsc(picons::SMALL_PX[i], picons::SMALL, picons::SMALL);
      large[i] = make_dsc(picons::BIG_PX[i], picons::BIG, picons::BIG);
    }
  }
  return big ? &large[p] : &small[p];
}
inline const lv_image_dsc_t *planet_card_dsc(int p) {  // 40 px, for the planet card
  static lv_image_dsc_t d[planets::N_PLANETS];
  static bool made = false;
  if (!made) {
    made = true;
    for (int i = 0; i < planets::N_PLANETS; i++)
      d[i] = make_dsc(picons::CARD_PX[i], picons::CARD, picons::CARD);
  }
  return &d[p];
}
inline const lv_image_dsc_t *align_dsc() {
  static lv_image_dsc_t d = make_dsc(picons::ALIGN_PX, picons::BIG, picons::BIG);
  return &d;
}
inline const lv_image_dsc_t *flag_for(char tag) {
  static lv_image_dsc_t dsc[4];
  static bool made = false;
  if (!made) {
    made = true;
    for (int i = 0; i < 4; i++)
      dsc[i] = make_dsc(flags::MAP[i], flags::MAP_W, flags::MAP_H);
  }
  switch (tag) {
    case 'G': return &dsc[0];
    case 'E': return &dsc[1];
    case 'R': return &dsc[2];
    case 'C': return &dsc[3];
    default: return nullptr;
  }
}
inline const lv_image_dsc_t *card_flag_for(const char *iso3) {
  constexpr size_t N = sizeof(flags::CARD) / sizeof(flags::CARD[0]);
  static lv_image_dsc_t dsc[N];
  static bool made = false;
  if (!made) {
    made = true;
    for (size_t i = 0; i < N; i++)
      dsc[i] = make_dsc(flags::CARD[i].px, flags::CARD_W, flags::CARD_H);
  }
  for (size_t i = 0; i < N; i++)
    if (strcmp(flags::CARD[i].iso3, iso3) == 0)
      return &dsc[i];
  return nullptr;
}

inline void ring(lv_obj_t *parent, int cx, int cy, int r, uint32_t color) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, 2 * r + 1, 2 * r + 1);
  lv_obj_set_pos(o, cx - r, cy - r);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
  ui.n_deco++;
}
inline void bar(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = plain(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_bg_color(o, lv_color_hex(C_CROSS), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  ui.n_deco++;
}
inline lv_obj_t *tag(lv_obj_t *parent, const char *s) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_label_set_text(l, s);
  lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
  if (ui.w.label_font)
    lv_obj_set_style_text_font(l, ui.w.label_font, 0);
  lv_obj_update_layout(l);
  ui.n_deco++;
  return l;
}

// UI-21: the star layer is drawn by the sky disc itself (LV_EVENT_DRAW_MAIN, after its
// background, before its children), straight into LVGL's draw buffer. There is no
// star image: a PSRAM canvas meant every redraw of a moving marker read the image
// back from PSRAM, and that extra PSRAM traffic starved the RGB panel (garbage lines).
inline void star_draw_cb(lv_event_t *e);
inline void star_layer_build(lv_obj_t *sky) {
  if (!ui.star_cb) {
    lv_obj_add_event_cb(sky, star_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    ui.star_cb = true;
  }
}

inline void name_tags_build(lv_obj_t *sky) {
  ui.names.clear();
  for (int i = 0; i < sky::N_NAMES; i++) {
    if (sky::NAMES[i].rank > NAME_MAX_RANK)
      continue;
    Ui::NameTag nt;
    nt.idx = i;
    nt.l = lv_label_create(sky);
    lv_obj_remove_style_all(nt.l);
    lv_label_set_text_static(nt.l, sky::NAMES[i].text);
    lv_obj_set_style_text_color(nt.l, lv_color_hex(C_CNAME), 0);
    if (ui.w.label_font)
      lv_obj_set_style_text_font(nt.l, ui.w.label_font, 0);
    lv_point_t sz;
    lv_text_get_size(&sz, sky::NAMES[i].text, ui.w.label_font ? ui.w.label_font : LV_FONT_DEFAULT, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    nt.w = (int) sz.x;
    nt.h = (int) sz.y;
    lv_obj_add_flag(nt.l, LV_OBJ_FLAG_HIDDEN);
    ui.names.push_back(nt);
    ui.n_deco++;
  }
  // higher rank first: the minute placement gives them first claim on space
  std::stable_sort(ui.names.begin(), ui.names.end(), [](const Ui::NameTag &a, const Ui::NameTag &b) {
    return sky::NAMES[a.idx].rank < sky::NAMES[b.idx].rank;
  });
}

// UI-1/UI-15 projection: zenith at centre, horizon at `radius`, the heading at the top
// and 90° clockwise from it on the right (north up, east right at heading 0).
// Works below the horizon too (r > radius) so the Moon's shadow can aim at a set Sun.
inline void project(geo::AzEl a, float &x, float &y, float inset = 0) {
  // inset > 0 (UI-6): keep a body of that radius wholly inside the horizon ring, so the
  // Sun and Moon near the horizon are neither sliced by the sky box nor drawn outside it
  const float r = std::min(ui.radius * (90.0f - a.el) / 90.0f, ui.radius - inset);
  const float s = (a.az - ui.heading) * geo::DEG;
  x = ui.cx + r * sinf(s);
  y = ui.cy - r * cosf(s);
}

// Compass letters just inside the horizon, nudged off the crosshair lines.
inline void place_compass_tags() {
  for (int i = 0; i < 4; i++) {
    lv_obj_t *l = ui.compass_tags[i];
    if (l == nullptr)
      continue;
    const float s = (i * 90.0f + 7.0f - ui.heading) * geo::DEG;
    const float r = ui.radius - 11.0f;
    const int x = (int) lroundf(ui.cx + r * sinf(s)), y = (int) lroundf(ui.cy - r * cosf(s));
    lv_obj_set_pos(l, x - lv_obj_get_width(l) / 2, y - lv_obj_get_height(l) / 2);
  }
}

inline const char *orbit_class(uint8_t cls) {  // UI-28
  static const char *const N[4] = {"LEO", "MEO", "GEO", "HEO"};
  return N[cls & 3];
}

// UI-29: distance in the chosen unit, and the unit's name
constexpr float KM_PER_MI = 1.609344f;
inline float dist(float km) { return ui.miles ? km / KM_PER_MI : km; }
inline const char *dist_unit() { return ui.miles ? "mi" : "km"; }

inline uint32_t orbit_color(float alt_km) {  // UI-3
  if (alt_km >= 35000.0f)
    return C_GEO;
  if (alt_km >= 2000.0f)
    return C_MEO;
  return C_LEO;
}

// Local HH:MM, or h:MM AM/PM (UI-17). buf needs at least 9 bytes.
#ifdef SAT_HOST_TEST
inline int host_tz_s = 0;  // host renders: local time offset (tests default to UTC)
#endif
inline void local_hm(int64_t t, char *buf, size_t n) {
  int hour, minute;
#ifdef SAT_HOST_TEST
  time_t tt = (time_t) (t + host_tz_s);
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  hour = tmv.tm_hour;
  minute = tmv.tm_min;
#else
  esphome::ESPTime lt = esphome::ESPTime::from_epoch_local((time_t) t);
  hour = lt.hour;
  minute = lt.minute;
#endif
  if (ui.h24)
    snprintf(buf, n, "%02d:%02d", hour, minute);
  else
    snprintf(buf, n, "%d:%02d %s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
}

// UI-17a: every duration on the device is written this one way: "45s", "12m", "1h 31m",
// "2d 4h" (a space between units, minutes padded after hours).
inline void fmt_dur(double s, char *buf, size_t n) {
  if (s < 0)
    s = 0;
  const int m = (int) (s / 60.0);
  if (m >= 24 * 60)
    snprintf(buf, n, "%dd %dh", m / 1440, (m % 1440) / 60);
  else if (m >= 60)
    snprintf(buf, n, "%dh %dm", m / 60, m % 60);
  else if (s >= 60)
    snprintf(buf, n, "%dm", m);
  else
    snprintf(buf, n, "%ds", (int) s);
}

// ================================================================== setup
inline void ui_build() {
  lv_obj_t *sky = ui.w.sky;
  lv_obj_update_layout(sky);
  ui.size = std::min(lv_obj_get_content_width(sky), lv_obj_get_content_height(sky));
  ui.cx = ui.cy = ui.size / 2;
  ui.radius = ui.size / 2 - 4;
  lv_obj_remove_flag(sky, LV_OBJ_FLAG_SCROLLABLE);

  // decorations first: they stay at the bottom of the child list (UI-6)
  ui.n_deco = 0;
  star_layer_build(sky);  // UI-21: the star canvas is the bottom-most child
  ring(sky, ui.cx, ui.cy, ui.radius / 3, C_RING_INNER);      // 60°
  ring(sky, ui.cx, ui.cy, 2 * ui.radius / 3, C_RING_INNER);  // 30°
  ring(sky, ui.cx, ui.cy, ui.radius, C_RING);                // horizon
  bar(sky, ui.cx, ui.cy - ui.radius, 1, 2 * ui.radius);
  bar(sky, ui.cx - ui.radius, ui.cy, 2 * ui.radius, 1);
  name_tags_build(sky);
  static const char *const NESW[4] = {"N", "E", "S", "W"};
  for (int i = 0; i < 4; i++)
    ui.compass_tags[i] = tag(sky, NESW[i]);
  place_compass_tags();

  // Sun and Moon (UI-6)
  ui.sun = dot(sky, SUN_HALO_PX, C_SUN_HALO);  // opaque halo instead of a blurred shadow
  lv_obj_t *core = plain(ui.sun);
  lv_obj_set_size(core, SUN_PX, SUN_PX);
  lv_obj_set_pos(core, (SUN_HALO_PX - SUN_PX) / 2, (SUN_HALO_PX - SUN_PX) / 2);
  lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(core, lv_color_hex(C_SUN), 0);
  lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
  // UI-6: the Moon is its phase picture, lit side towards the Sun on the map
  ui.moon = lv_canvas_create(sky);
  lv_obj_remove_style_all(ui.moon);
  lv_obj_remove_flag(ui.moon, (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  if (lv_draw_buf_t *mb = lv_draw_buf_create(MOON_PX, MOON_PX, LV_COLOR_FORMAT_ARGB8888, 0)) {
    lv_draw_buf_clear(mb, nullptr);
    lv_canvas_set_draw_buf(ui.moon, mb);
  }
  lv_obj_add_flag(ui.moon, LV_OBJ_FLAG_HIDDEN);
  // UI-40 planets: picture and name tag, above the Sun and Moon, below every satellite
  for (int p = 0; p < planets::N_PLANETS; p++) {
    ui.planet_img[p] = lv_image_create(sky);
    lv_obj_remove_style_all(ui.planet_img[p]);
    lv_obj_remove_flag(ui.planet_img[p], (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_image_set_src(ui.planet_img[p], planet_dsc(p, false));
    lv_obj_add_flag(ui.planet_img[p], LV_OBJ_FLAG_HIDDEN);
    ui.planet_lbl[p] = lv_label_create(sky);
    lv_obj_remove_style_all(ui.planet_lbl[p]);
    if (ui.w.label_font)
      lv_obj_set_style_text_font(ui.planet_lbl[p], ui.w.label_font, 0);
    lv_obj_set_style_text_color(ui.planet_lbl[p], lv_color_hex(mix(C_PLANET[p], SKY_BG, 200)), 0);
    lv_label_set_text(ui.planet_lbl[p], planets::name(p));
    lv_obj_add_flag(ui.planet_lbl[p], LV_OBJ_FLAG_HIDDEN);
  }
  // UI-63 comets: a small bright head (the tail is drawn under everything by the disc)
  for (int k = 0; k < Ui::MAX_COMETS; k++) {
    ui.comet_dot[k] = dot(sky, COMET_PX, C_COMET);
    lv_obj_remove_flag(ui.comet_dot[k], (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_shadow_width(ui.comet_dot[k], 6, 0);
    lv_obj_set_style_shadow_color(ui.comet_dot[k], lv_color_hex(C_COMET), 0);
    lv_obj_set_style_shadow_opa(ui.comet_dot[k], LV_OPA_50, 0);
    ui.comet_lbl[k] = lv_label_create(sky);
    lv_obj_remove_style_all(ui.comet_lbl[k]);
    if (ui.w.label_font)
      lv_obj_set_style_text_font(ui.comet_lbl[k], ui.w.label_font, 0);
    lv_obj_set_style_text_color(ui.comet_lbl[k], lv_color_hex(mix(C_COMET, SKY_BG, 200)), 0);
    lv_label_set_text(ui.comet_lbl[k], "");
    lv_obj_add_flag(ui.comet_lbl[k], LV_OBJ_FLAG_HIDDEN);
  }

  // Starlink below satellites (UI-4); pools fixed at start-up (UI-14)
  const int64_t pool_t0 = ui_us();
  ui.starlink.resize(STARLINK_POOL);
  for (auto &m : ui.starlink) {
    if ((&m - ui.starlink.data()) % 16 == 0)
      ui_feed_wdt();
    m.dot = dot(sky, sl_px(), C_STARLINK_DIM);
    m.icol = m.drawn_col = ui.w.icon_font ? C_STARLINK_ICON : C_STARLINK_DIM;  // UI-43
    if (ui.w.icon_font) {  // UI-31
      m.icon = make_icon(m.dot, C_STARLINK_ICON);
      lv_label_set_text(m.icon, "\xF3\xB0\xA4\x89");  // satellite-uplink
    }
  }
  ui.sats.resize(SAT_POOL);
  for (auto &m : ui.sats) {
    m.dot = dot(sky, sat_px(), C_LEO);
    if (ui.w.icon_font)
      m.icon = make_icon(m.dot, C_LEO);  // UI-31
  }
  for (auto &m : ui.sats) {
    m.label = lv_label_create(sky);
    lv_obj_remove_style_all(m.label);
    lv_obj_set_style_text_color(m.label, lv_color_hex(C_LABEL), 0);
    if (ui.w.label_font)
      lv_obj_set_style_text_font(m.label, ui.w.label_font, 0);
    lv_label_set_text(m.label, "");
    lv_obj_add_flag(m.label, LV_OBJ_FLAG_HIDDEN);
    if (ui.w.icon_font) {  // UI-31: GNSS flag in place of the constellation letter
      m.flag = lv_image_create(m.label);
      lv_obj_add_flag(m.label, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
      lv_obj_align(m.flag, LV_ALIGN_LEFT_MID, 0, 0);
      lv_obj_add_flag(m.flag, LV_OBJ_FLAG_HIDDEN);
    }
  }
  ESP_LOGI(UI_TAG, "marker pools: %d Starlink, %d satellites in %lld ms", STARLINK_POOL, SAT_POOL,
           (long long) ((ui_us() - pool_t0) / 1000));
  // ISS on top (UI-5)
  ui.iss.dot = dot(sky, ISS_PX, C_ISS);
  if (ui.w.card_icon_font) {  // UI-5: space-station icon instead of the orange dot
    lv_obj_set_style_bg_opa(ui.iss.dot, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(ui.iss.dot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *ic = lv_label_create(ui.iss.dot);
    lv_obj_remove_style_all(ic);
    lv_obj_set_style_text_font(ic, ui.w.card_icon_font, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(C_ISS), 0);
    lv_label_set_text(ic, "\xF3\xB1\x8E\x83");  // space-station
    lv_obj_center(ic);
  } else {
    lv_obj_set_style_border_width(ui.iss.dot, 2, 0);
    lv_obj_set_style_border_color(ui.iss.dot, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(ui.iss.dot, LV_OPA_COVER, 0);
  }
  // UI-52 Tiangong: the ISS's marker in red, just under it
  ui.css.dot = dot(sky, ISS_PX, C_CSS);
  lv_obj_add_flag(ui.css.dot, LV_OBJ_FLAG_HIDDEN);
  if (ui.w.card_icon_font) {
    lv_obj_set_style_bg_opa(ui.css.dot, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(ui.css.dot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *ic = lv_label_create(ui.css.dot);
    lv_obj_remove_style_all(ic);
    lv_obj_set_style_text_font(ic, ui.w.card_icon_font, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(C_CSS), 0);
    lv_label_set_text(ic, "\xF3\xB1\x8E\x83");  // space-station
    lv_obj_center(ic);
  }
}

inline void reassert_order() {  // UI-6
  lv_obj_move_to_index(ui.sun, ui.n_deco);
  lv_obj_move_to_index(ui.moon, ui.n_deco + 1);
  if (ui.css.dot)
    lv_obj_move_to_index(ui.css.dot, -1);  // UI-52
  lv_obj_move_to_index(ui.iss.dot, -1);
}

// ================================================================== markers
inline void place_label(Marker &m, int dx, int dy, bool visible) {
  if (m.label == nullptr)
    return;
  bool shown = !lv_obj_has_flag(m.label, LV_OBJ_FLAG_HIDDEN);
  if (!visible) {
    set_shown(m.label, shown, false);
    return;
  }
  const bool left = dx > ui.cx;  // keep tags inside the disc on the east side
  int lx, ly;
  if (m.centred) {  // UI-29: the icon is the marker
    lx = dx - m.label_w / 2;
  } else if (left) {
    lx = dx - mark_r() - 3 - m.label_w;
  } else {
    lx = dx + mark_r() + 3;
  }
  ly = m.centred ? dy - 8 : dy - 7;
  if (lx != m.lx || ly != m.ly || left != m.label_left) {
    m.lx = lx;
    m.ly = ly;
    m.label_left = left;
    lv_obj_set_pos(m.label, lx, ly);
  }
  set_shown(m.label, shown, true);
}

// Draw one marker at time t. With `fresh` set, `r` replaces the marker's record and
// the jump between the old and new prediction for this instant becomes an offset
// that decays on later ticks (MOTION-5). A newly bound marker starts without one.
// UI-43: near the horizon (below LOW_EL) a marker is drawn at 55 % over the sky and
// loses its tag, so the rim of a wide view stays readable.
inline void apply_marker_color(Marker &m, bool low) {
  m.low = low;
  if (m.icol == 0)
    return;  // no managed colour (ISS)
  const uint32_t col = low ? mix(m.icol, SKY_BG, 140) : m.icol;
  if (col == m.drawn_col)
    return;
  m.drawn_col = col;
  if (m.centred && m.label)
    lv_obj_set_style_text_color(m.label, lv_color_hex(col), 0);
  else if (m.icon)
    lv_obj_set_style_text_color(m.icon, lv_color_hex(col), 0);
  else
    lv_obj_set_style_bg_color(m.dot, lv_color_hex(col), 0);
}
inline void draw_marker(Marker &m, const SatRec *fresh, int px_size, double t, bool is_new, float min_el = 0.0f) {
  if (fresh != nullptr) {
    if (is_new) {
      m.offx = m.offy = 0;
    } else {
      const geo::AzEl was = geo::azel(ui.obs, geo::sat_ecef(m.rec, t));
      const geo::AzEl now = geo::azel(ui.obs, geo::sat_ecef(*fresh, t));
      if (geo::separation(was, now) > SNAP_DEG) {
        m.offx = m.offy = 0;  // too far: snap
      } else {
        float ox, oy, nx, ny;
        project(was, ox, oy);
        project(now, nx, ny);
        m.offx += ox - nx;  // drawn position stays put this tick
        m.offy += oy - ny;
      }
    }
    m.rec = *fresh;
  } else {
    m.offx *= DECAY;
    m.offy *= DECAY;
  }
  if (fabsf(m.offx) < 0.5f && fabsf(m.offy) < 0.5f)
    m.offx = m.offy = 0;

  const geo::AzEl a = geo::azel(ui.obs, geo::sat_ecef(m.rec, t));
  const bool station = &m == &ui.iss || &m == &ui.css;  // UI-52b: the stations can be switched off
  const bool up = a.el >= min_el && (!station || ui.stations_on);  // UI-7: hide, never clamp (min_el > 0: Starlink cone, UI-4)
  set_shown(m.dot, m.shown, up);
  m.up = up;
  if (!up) {
    place_label(m, 0, 0, false);
    return;
  }
  float x, y;
  project(a, x, y);
  const int dx = (int) lroundf(x + m.offx), dy = (int) lroundf(y + m.offy);
  const int px = dx - px_size / 2, py = dy - px_size / 2;
  if (px != m.px || py != m.py) {  // PERF-5
    m.px = px;
    m.py = py;
    lv_obj_set_pos(m.dot, px, py);
  }
  m.dx = dx;
  m.dy = dy;
  m.el = a.el;
  apply_marker_color(m, a.el < LOW_EL);  // UI-43
  // labelled markers are placed by declutter_labels() once the whole layer is drawn
}

// UI-19: where satellites overlap, only the lowest keeps its name tag. Tags are
// placed lowest first; one is dropped when its dot overlaps a lower satellite's dot
// or its tag would cover a tag already placed.
inline void declutter_labels() {
  constexpr int R2 = SAT_PX * SAT_PX;  // centres closer than one dot width
  constexpr int TAG_H = 14;
  struct Box {
    int x0, y0, x1, y1;
  };
  std::vector<Marker *> order;
  order.reserve(ui.sats.size());
  for (auto &m : ui.sats)
    if (m.id >= 0 && m.up)
      order.push_back(&m);
  std::sort(order.begin(), order.end(), [](const Marker *a, const Marker *b) {
    return a->el != b->el ? a->el < b->el : a->id < b->id;
  });
  std::vector<Box> placed;
  placed.reserve(order.size());
  for (size_t i = 0; i < order.size(); i++) {
    Marker &m = *order[i];
    bool keep = true;
    if (m.centred) {  // UI-29: a debris icon is its marker: always drawn
      const Box bx{m.dx - m.label_w / 2, m.dy - 8, m.dx + m.label_w / 2, m.dy + 7};
      placed.push_back(bx);
      place_label(m, m.dx, m.dy, true);
      continue;
    }
    if (m.el < LOW_EL) {  // UI-43: no tags near the horizon
      place_label(m, m.dx, m.dy, false);
      continue;
    }
    for (size_t j = 0; j < i && keep; j++) {
      const int ex = order[j]->dx - m.dx, ey = order[j]->dy - m.dy;
      keep = ex * ex + ey * ey >= R2;
    }
    const int x0 = m.dx > ui.cx ? m.dx - mark_r() - 3 - m.label_w : m.dx + mark_r() + 3;
    const Box bx{x0, m.dy - 7, x0 + m.label_w, m.dy - 7 + TAG_H};
    for (size_t j = 0; j < placed.size() && keep; j++) {
      const Box &o = placed[j];
      keep = bx.x1 <= o.x0 || o.x1 <= bx.x0 || bx.y1 <= o.y0 || o.y1 <= bx.y0;
    }
    if (keep)
      placed.push_back(bx);
    place_label(m, m.dx, m.dy, keep);
  }
  // UI-21: constellation names give way to satellites (their dots and shown tags)
  for (auto &nt : ui.names) {
    bool keep = ui.stars_shown && nt.fits;
    const Box nb{nt.x, nt.y, nt.x + nt.w, nt.y + nt.h};
    for (size_t j = 0; j < placed.size() && keep; j++) {
      const Box &o = placed[j];
      keep = nb.x1 <= o.x0 || o.x1 <= nb.x0 || nb.y1 <= o.y0 || o.y1 <= nb.y0;
    }
    for (size_t j = 0; j < order.size() && keep; j++) {
      const int r = SAT_PX / 2 + 1, x = order[j]->dx, y = order[j]->dy;
      keep = x + r <= nb.x0 || nb.x1 <= x - r || y + r <= nb.y0 || nb.y1 <= y - r;
    }
    set_shown(nt.l, nt.shown, keep);
  }
}

inline void release(Marker &m) {
  m.id = -1;
  m.up = false;
  m.offx = m.offy = 0;
  set_shown(m.dot, m.shown, false);
  if (m.label) {
    bool shown = true;
    set_shown(m.label, shown, false);
  }
}

// Rebind a layer's pool to fresh records by satellite id (MOTION-4). rec_of[slot] is
// the index of that slot's record in `recs` (-1 when free). Returns the number of
// records left without a marker (UI-14).
inline int rebind(pvector<Marker> &pool, std::unordered_map<int32_t, int> &slot, const pvector<SatRec> &recs,
                  std::vector<int> &rec_of, std::vector<char> &is_new) {
  std::unordered_map<int32_t, int> idx;
  idx.reserve(recs.size());
  for (size_t i = 0; i < recs.size(); i++)
    idx[recs[i].id] = (int) i;
  rec_of.assign(pool.size(), -1);
  is_new.assign(pool.size(), 0);
  for (auto it = slot.begin(); it != slot.end();) {
    auto f = idx.find(it->first);
    if (f == idx.end()) {  // satellite left the layer: free its marker
      release(pool[it->second]);
      it = slot.erase(it);
    } else {
      rec_of[it->second] = f->second;
      ++it;
    }
  }
  std::vector<int> free_slots;
  for (int i = (int) pool.size() - 1; i >= 0; i--)
    if (pool[i].id < 0)
      free_slots.push_back(i);
  int undrawn = 0;
  for (size_t i = 0; i < recs.size(); i++) {
    if (slot.count(recs[i].id))
      continue;
    if (free_slots.empty()) {
      undrawn++;
      continue;
    }
    const int s = free_slots.back();
    free_slots.pop_back();
    slot[recs[i].id] = s;
    pool[s].id = recs[i].id;
    rec_of[s] = (int) i;
    is_new[s] = 1;
  }
  return undrawn;
}

inline void short_name(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < n && j < 12; i++)
    out[j++] = in[i];
  while (j > 0 && out[j - 1] == ' ')
    j--;
  out[j] = 0;
}

// UI-13: one layer, one pass, only on fresh data.
// UI-30: MDI glyph for an object on the details card and the list (UTF-8)
constexpr const char *ICON_SUN = "\xF3\xB0\x96\x99";   // weather-sunny
constexpr const char *ICON_MOON = "\xF3\xB0\xBD\xA5";  // moon-waning-crescent
constexpr const char *ICON_ISS = "\xF3\xB1\x8E\x83";   // space-station
constexpr const char *ICON_COMET = "\xF3\xB0\x98\xA9";  // meteor (UI-63 comets: MDI has no comet)
inline const char *card_icon_for(int kind, const SatRec &r) {
  if (kind == K_ISS || kind == K_CSS)  // UI-52
    return ICON_ISS;
  if (r.debris)
    return "\xF3\xB0\xA9\xB9";  // trash-can
  if (kind == K_STARLINK)
    return "\xF3\xB0\xA4\x89";  // satellite-uplink
  if (r.tag)
    return "\xF3\xB0\x86\xA4";  // crosshairs-gps: GNSS
  if (kind == K_GEO || r.cls == C_CLS_GEO)
    return "\xF3\xB0\x87\xA7";  // earth: GEO
  return "\xF3\xB0\x91\xB1";    // satellite-variant
}

// PERF-11: many markers appearing at once (a layer switched on) would each invalidate a
// small area, and LVGL redraws the stars and lines under every one of them. Invalidating
// the whole sky first makes LVGL fold them into one redraw (lv_inv_area skips areas
// already inside an invalidated one).
constexpr int BATCH_INVALIDATE = 8;
inline void batch_invalidate(const std::vector<char> &is_new) {
  int n = 0;
  for (char c : is_new)
    n += c ? 1 : 0;
  if (n > BATCH_INVALIDATE && ui.w.sky)
    lv_obj_invalidate(ui.w.sky);
}

inline void restyle_sats(double t) {
  std::vector<int> rec_of;
  std::vector<char> is_new;
  ui.sat_undrawn = rebind(ui.sats, ui.sat_slot, live.sats, rec_of, is_new);
  batch_invalidate(is_new);
  if (ui.sat_undrawn > 0)
    ESP_LOGW(UI_TAG, "%d satellites have no marker (pool %d)", ui.sat_undrawn, SAT_POOL);
  for (size_t i = 0; i < ui.sats.size(); i++) {
    Marker &m = ui.sats[i];
    if (m.id < 0)
      continue;
    const SatRec &r = live.sats[rec_of[i]];
    // UI-28/29 style: debris = trash icon only; MEO/HEO = square dot with the GNSS
    // letter; LEO = round dot with name and country
    const uint8_t st = (r.debris && ui.w.icon_font) ? MS_DEBRIS : (r.cls == C_CLS_LEO ? MS_ROUND : MS_SQUARE);
    if (st != m.style) {
      m.style = st;
      m.centred = st == MS_DEBRIS;
      lv_obj_set_style_radius(m.dot, st == MS_SQUARE ? 1 : LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(m.dot, (st == MS_DEBRIS || m.icon) ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
      lv_obj_set_style_text_font(m.label, st == MS_DEBRIS ? ui.w.icon_font : ui.w.label_font, 0);
      lv_obj_set_style_text_color(m.label, lv_color_hex(st == MS_DEBRIS ? C_DEBRIS : C_LABEL), 0);
      lv_label_set_text(m.label, "");  // forces the text (and width) below
      if (st == MS_DEBRIS) {  // UI-43: the debris label is the marker
        m.icol = C_DEBRIS;
        m.drawn_col = 0;
      }
    }
    char nm[24];
    if (st == MS_DEBRIS) {
      snprintf(nm, sizeof(nm), "%s", "\xF3\xB0\xA9\xB9");  // mdi:trash-can U+F0A79
    } else if (st == MS_SQUARE && r.tag && m.flag && flag_for(r.tag)) {
      snprintf(nm, sizeof(nm), "%s", "  ");  // UI-31: room for the flag image
    } else if (st == MS_SQUARE && r.tag) {
      snprintf(nm, sizeof(nm), "%c", r.tag);
    } else {
      short_name(r.name, nm, 16);
      if (r.cc[0]) {  // UI-18: owner country
        const size_t n = strlen(nm);
        snprintf(nm + n, sizeof(nm) - n, " %s", r.cc);
      }
    }
    if (m.icon) {  // UI-31: debris keeps its centred trash label instead
      const char *g = st == MS_DEBRIS ? "" : card_icon_for(K_SAT, r);
      if (strcmp(lv_label_get_text(m.icon), g) != 0)
        lv_label_set_text(m.icon, g);
    }
    if (m.flag) {  // UI-31
      const lv_image_dsc_t *fl = (st == MS_SQUARE && r.tag) ? flag_for(r.tag) : nullptr;
      if (fl) {
        lv_image_set_src(m.flag, fl);
        lv_obj_remove_flag(m.flag, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(m.flag, LV_OBJ_FLAG_HIDDEN);
      }
    }
    const char *cur = lv_label_get_text(m.label);
    if (cur == nullptr || strcmp(cur, nm) != 0) {  // PERF-6: text on fresh data only
      lv_label_set_text(m.label, nm);
      const lv_font_t *f = st == MS_DEBRIS ? ui.w.icon_font : ui.w.label_font;
      lv_point_t sz;
      lv_text_get_size(&sz, nm, f ? f : LV_FONT_DEFAULT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      m.label_w = (int) sz.x;
      m.lx = INT_MIN;
    }
    const uint32_t col = orbit_color(r.alt_km);
    if (col != m.color) {
      m.color = col;
      lv_obj_set_style_bg_color(m.dot, lv_color_hex(col), 0);
      if (m.icon)
        lv_obj_set_style_text_color(m.icon, lv_color_hex(col), 0);
      if (st != MS_DEBRIS) {  // UI-43: draw_marker re-applies the dimming
        m.icol = col;
        m.drawn_col = 0;
      }
    }
    draw_marker(m, &r, sat_px(), t, is_new[i]);
  }
  declutter_labels();
  reassert_order();
}

// UI-4 / DATA-3: the Starlink list covers the whole sky; only those inside the
// starlink_radius cone are drawn and counted.
inline float starlink_min_el() { return 90.0f - (float) config().starlink_radius; }
constexpr double STARLINK_LOOKAHEAD_S = 90;  // bind markers for satellites entering the cone before the next fetch

inline void count_starlink(double t) {
  const float min_el = starlink_min_el();
  int n = 0;
  for (const auto &r : live.starlink)
    if (geo::azel(ui.obs, geo::sat_ecef(r, t)).el >= min_el)
      n++;
  ui.starlink_in_cone = n;
}

inline void restyle_starlink(double t) {
  const float min_el = starlink_min_el();
  // candidates: in the cone now, or due to enter it before the next fetch
  std::vector<std::pair<float, int>> cand;
  for (size_t i = 0; i < live.starlink.size(); i++) {
    const SatRec &r = live.starlink[i];
    const float el_now = geo::azel(ui.obs, geo::sat_ecef(r, t)).el;
    bool take = el_now >= min_el;
    // A Starlink climbs at most ~50° in the look-ahead; skip the second prediction
    // for anything further below the cone (PERF-7: ~320 records per fetch).
    if (!take && el_now >= min_el - 50.0f)
      take = geo::azel(ui.obs, geo::sat_ecef(r, t + STARLINK_LOOKAHEAD_S)).el >= min_el;
    if (take)
      cand.push_back({el_now, (int) i});
  }
  std::sort(cand.begin(), cand.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
  pvector<SatRec> subset;
  subset.reserve(cand.size());
  for (const auto &c : cand)
    subset.push_back(live.starlink[c.second]);
  std::vector<int> rec_of;
  std::vector<char> is_new;
  ui.starlink_undrawn = rebind(ui.starlink, ui.starlink_slot, subset, rec_of, is_new);
  if (ui.starlink_undrawn > 0)
    ESP_LOGW(UI_TAG, "%d Starlink have no marker (pool %d)", ui.starlink_undrawn, STARLINK_POOL);
  batch_invalidate(is_new);
  for (size_t i = 0; i < ui.starlink.size(); i++) {
    Marker &m = ui.starlink[i];
    if (m.id >= 0)
      draw_marker(m, &subset[rec_of[i]], sl_px(), t, is_new[i], min_el);
  }
  count_starlink(t);
  reassert_order();
}

inline void update_iss_state(double t) {
  if (ui.iss.id < 0) {
    ui.iss_valid = false;
    return;
  }
  const geo::V3 p = geo::sat_ecef(ui.iss.rec, t);
  ui.iss_azel = geo::azel(ui.obs, p);
  ui.iss_sunlit = astro::sunlit(p, ui.sm.sun_ecef_dir);
  ui.iss_valid = true;
}

inline void restyle_css(double t) {  // UI-52
  if (!live.css_ok || ui.css.dot == nullptr)
    return;
  const SatRec &r = live.css;
  const bool is_new = ui.css.id < 0;
  ui.css.id = r.id;
  draw_marker(ui.css, &r, ISS_PX, t, is_new);
}
inline void restyle_iss(double t) {
  if (!live.iss_ok)
    return;
  const SatRec &r = live.iss;
  const bool is_new = ui.iss.id < 0;
  ui.iss.id = r.id;
  draw_marker(ui.iss, &r, ISS_PX, t, is_new);
  update_iss_state(t);
  reassert_order();
}

// UI-20 / UI-6 phase picture on an ARGB8888 canvas. Each pixel is 4x4 supersampled:
// alpha is the disc's coverage (smooth limb), colour mixes the dark side (earthshine)
// and the lit side, lit with a little limb darkening. (ux, uy) is the unit direction
// towards the Sun in canvas coordinates; along it, on a chord of half-width w, the
// terminator sits at w(1-2k) for illuminated fraction k.
constexpr uint32_t C_MOON_DARK = 0x2A3350;
// UI-46a: constellation stick figure (sky_cfig.h, 40 px, north up, east left) drawn
// into the card's 40 px canvas: lines in the map's figure blue, stars as white dots
// sized by brightness.
inline void render_cfig(lv_obj_t *cv, int idx) {
  lv_draw_buf_t *db = cv ? lv_canvas_get_draw_buf(cv) : nullptr;
  if (db == nullptr || idx < 0 || idx >= sky::N_NAMES)
    return;
  lv_draw_buf_clear(db, nullptr);
  lv_layer_t layer;
  lv_canvas_init_layer(cv, &layer);
  const uint8_t *b = sky::CFIG + sky::CFIG_OFF[idx];
  const uint8_t *const end = sky::CFIG + sky::CFIG_OFF[idx + 1];
  const float sc = (float) std::min(db->header.w, db->header.h) / sky::CFIG_PX;
  lv_draw_line_dsc_t ld;
  lv_draw_line_dsc_init(&ld);
  ld.color = lv_color_hex(0x7F96D7);
  ld.width = 1;
  ld.round_start = ld.round_end = 1;
  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.bg_color = lv_color_hex(0xF0F4FF);
  rd.bg_opa = LV_OPA_COVER;
  rd.radius = LV_RADIUS_CIRCLE;
  for (int pass = 0; pass < 2; pass++) {  // all lines first, then every star on top
    const uint8_t *q = b;
    int np = q < end ? *q++ : 0;
    while (np-- > 0 && q < end) {
      const int n = *q++;
      if (q + 3 * n > end)
        break;
      for (int k = 0; k < n; k++, q += 3) {
        const float x = q[0] * sc + 0.5f * sc, y = q[1] * sc + 0.5f * sc;
        if (pass == 0 && k > 0) {
          ld.p1.x = q[-3] * sc + 0.5f * sc;
          ld.p1.y = q[-2] * sc + 0.5f * sc;
          ld.p2.x = x;
          ld.p2.y = y;
          lv_draw_line(&layer, &ld);
        } else if (pass == 1) {
          const int r = q[2];  // 0: 1 px dot, 1: 3 px, 2: 4 px
          const lv_area_t a = {(int32_t) x - (r > 0 ? 1 : 0), (int32_t) y - (r > 0 ? 1 : 0),
                               (int32_t) x + (r > 1 ? 2 : r), (int32_t) y + (r > 1 ? 2 : r)};
          lv_draw_rect(&layer, &rd, &a);
        }
      }
    }
  }
  lv_canvas_finish_layer(cv, &layer);
}

inline void render_moon(lv_obj_t *cv, float k, float ux, float uy) {
  lv_draw_buf_t *db = cv ? lv_canvas_get_draw_buf(cv) : nullptr;
  if (db == nullptr)
    return;
  const int n = (int) std::min(db->header.w, db->header.h);  // not the object size: may not be laid out yet
  const float R = n / 2.0f - 0.5f, c = n / 2.0f;
  const lv_color_t dark = lv_color_hex(C_MOON_DARK), lit_col = lv_color_hex(C_MOON);
  for (int py = 0; py < n; py++) {
    for (int px = 0; px < n; px++) {
      int in = 0;
      float lit = 0;
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          const float x = px + (sx + 0.5f) / 4 - c, y = py + (sy + 0.5f) / 4 - c;
          const float r2 = x * x + y * y;
          if (r2 > R * R)
            continue;
          in++;
          const float along = x * ux + y * uy, across = -x * uy + y * ux;
          const float w = sqrtf(std::max(0.0f, R * R - across * across));
          if (along > w * (1 - 2 * k))
            lit += 0.82f + 0.18f * sqrtf(1 - r2 / (R * R));  // limb darkening
        }
      }
      lv_color_t col = dark;
      if (in > 0 && lit > 0)
        col = lv_color_mix(lit_col, dark, (uint8_t) std::min(255.0f, lit / in * 255.0f));
      lv_canvas_set_px(cv, px, py, col, (lv_opa_t) (in * 255 / 16));
    }
  }
  lv_obj_invalidate(cv);
}

inline void draw_sun_moon(double t, double tv) {
  const Config &c = config();
  ui.sm = astro::compute(t, c.lat, c.lon, c.alt_m);
  // UI-53: the map shows them at the scrubbed time; everything else uses now (ui.sm)
  const astro::SunMoon v = tv == t ? ui.sm : astro::compute(tv, c.lat, c.lon, c.alt_m);
  ui.view_sun_el = v.sun.el;
  float sx, sy, mx, my;
  project(v.sun, sx, sy, SUN_HALO_PX / 2 + 1);
  project(v.moon, mx, my, MOON_PX / 2 + 1);

  set_shown(ui.sun, ui.sun_shown, v.sun.el >= 0);
  const int spx = (int) lroundf(sx) - SUN_HALO_PX / 2, spy = (int) lroundf(sy) - SUN_HALO_PX / 2;
  if (ui.sun_shown && (spx != ui.sun_x || spy != ui.sun_y)) {
    ui.sun_x = spx;
    ui.sun_y = spy;
    lv_obj_set_pos(ui.sun, spx, spy);
  }

  set_shown(ui.moon, ui.moon_shown, v.moon.el >= 0);
  if (!ui.moon_shown)
    return;
  const int mpx = (int) lroundf(mx) - MOON_PX / 2, mpy = (int) lroundf(my) - MOON_PX / 2;
  if (mpx != ui.moon_x || mpy != ui.moon_y) {
    ui.moon_x = mpx;
    ui.moon_y = mpy;
    lv_obj_set_pos(ui.moon, mpx, mpy);
  }
  // UI-6 phase: redrawn when the lit fraction or the Sun's direction on the map moves
  float ux = sx - mx, uy = sy - my;
  const float len = sqrtf(ux * ux + uy * uy);
  if (len > 1e-3f) {
    ux /= len;
    uy /= len;
  } else {
    ux = 1;
    uy = 0;
  }
  const float k = std::max(0.0f, std::min(1.0f, v.moon_illum));
  const float ang = atan2f(uy, ux);
  if (fabsf(k - ui.map_moon_k) > 0.004f || fabsf(remainderf(ang - ui.map_moon_ang, 2 * (float) M_PI)) > 0.05f) {
    ui.map_moon_k = k;
    ui.map_moon_ang = ang;
    render_moon(ui.moon, k, ux, uy);
  }
}

// ================================================================== HUD and list
// ================================================================== UI-21 star map
// Stars are fixed in RA/Dec, so only the sidereal time moves them: 15°/h, about
// half a pixel a minute here. Positions are recomputed every STAR_PERIOD and when
// the view changes; star_draw_cb() draws them under every other object on the disc.
struct Enu {
  float e, n, u;  // unit vector: east, north, up
};
inline Enu star_enu(float ra_deg, float dec_deg, float lst_deg, float sin_lat, float cos_lat) {
  const float h = (lst_deg - ra_deg) * geo::DEG, d = dec_deg * geo::DEG;
  const float cd = cosf(d), sd = sinf(d), ch = cosf(h);
  return {-cd * sinf(h), sd * cos_lat - cd * sin_lat * ch, sin_lat * sd + cos_lat * cd * ch};
}
inline void enu_xy(const Enu &v, float &x, float &y) {
  geo::AzEl a;
  a.el = asinf(std::max(-1.0f, std::min(1.0f, v.u))) / geo::DEG;
  a.az = atan2f(v.e, v.n) / geo::DEG;
  if (a.az < 0)
    a.az += 360.0f;
  project(a, x, y);
}
inline float star_ra(const sky::Star &s) { return s.ra * (360.0f / 65536.0f); }
inline float star_dec(const sky::Star &s) { return s.dec * (90.0f / 32767.0f); }

// ================================================================== UI-64 Milky Way
// sky_mw.h holds its brightness on a 1-degree RA/Dec grid. Each 2x2 block of the disc keeps
// the hour angle and Dec it looks at (worked out again only when the heading or the
// location changes); each star period the blocks are shaded from the grid at RA = LST - h
// and stored as runs of equal shade per 2-px band, which the disc draws under the stars.
// Faded towards the horizon (to a quarter at 0°, full above 15°), as the eye sees it.
constexpr int MW_LV = 5;  // shades above none
constexpr uint8_t MW_W[MW_LV + 1] = {0, 18, 30, 42, 56, 72};  // of C_MW over SKY_BG, per shade
inline void mw_prepare() {
  const Config &c = config();
  const int n = ui.size / 2;
  if (n == ui.mw_n && ui.mw_heading == ui.heading && ui.mw_lat == (float) c.lat)
    return;
  ui.mw_n = n;
  ui.mw_heading = ui.heading;
  ui.mw_lat = (float) c.lat;
  ui.mw_cells.resize((size_t) n * n);
  const float sl = sinf((float) c.lat * geo::DEG), cl = cosf((float) c.lat * geo::DEG);
  for (int by = 0; by < n; by++) {
    if ((by & 15) == 0)
      ui_feed_wdt();
    for (int bx = 0; bx < n; bx++) {
      Ui::MwCell &m = ui.mw_cells[(size_t) by * n + bx];
      const float dx = bx * 2 + 1.0f - ui.cx, dy = by * 2 + 1.0f - ui.cy, r = sqrtf(dx * dx + dy * dy);
      if (r > ui.radius) {
        m = {0, 0, 0};
        continue;
      }
      const float el = 90.0f * (1.0f - r / ui.radius), az = ui.heading * geo::DEG + atan2f(dx, -dy);
      const float ce = cosf(el * geo::DEG), e = ce * sinf(az), nn = ce * cosf(az), u = sinf(el * geo::DEG);
      float h = atan2f(-e, u * cl - nn * sl) / geo::DEG;
      if (h < 0)
        h += 360.0f;
      const float dec = asinf(std::max(-1.0f, std::min(1.0f, nn * cl + u * sl))) / geo::DEG;
      m.h = (uint16_t) lroundf(h * (65536.0f / 360.0f));
      m.dec = (int16_t) lroundf(dec * 100.0f);
      m.fade = (uint8_t) lroundf(255.0f * std::max(0.25f, std::min(1.0f, 0.25f + el / 20.0f)));
    }
  }
}
// the grid at (ra, dec) degrees, bilinear, 0..255
inline int mw_sample(float ra, float dec) {
  float fx = ra - 0.5f, fy = dec + 89.5f;
  fx -= 360.0f * floorf(fx / 360.0f);
  fy = std::max(0.0f, std::min((float) (sky::MW_H - 1), fy));
  const int x0 = (int) fx, y0 = std::min((int) fy, sky::MW_H - 2), x1 = (x0 + 1) % sky::MW_W;
  const float ax = fx - x0, ay = fy - y0;
  const uint8_t *r0 = sky::MW + y0 * sky::MW_W, *r1 = r0 + sky::MW_W;
  const float top = r0[x0] + (r0[x1] - r0[x0]) * ax, bot = r1[x0] + (r1[x1] - r1[x0]) * ax;
  return (int) (top + (bot - top) * ay);
}
inline void mw_build(float lst_deg) {
  mw_prepare();
  const int n = ui.mw_n;
  ui.mw_spans.clear();
  ui.mw_band.resize((size_t) n + 1);
  for (int by = 0; by < n; by++) {
    ui.mw_band[by] = (uint32_t) ui.mw_spans.size();
    int run_x = -1, run_l = 0;
    for (int bx = 0; bx <= n; bx++) {
      int l = 0;
      if (bx < n) {
        const Ui::MwCell &m = ui.mw_cells[(size_t) by * n + bx];
        if (m.fade) {
          const int v = mw_sample(lst_deg - m.h * (360.0f / 65536.0f), m.dec * 0.01f) * m.fade / 255;
          l = (v * MW_LV + 127) / 255;
        }
      }
      if (l != run_l) {
        if (run_l > 0)
          ui.mw_spans.push_back({(int16_t) (run_x * 2), (int16_t) (bx * 2 - 1), (uint8_t) run_l});
        run_x = bx;
        run_l = l;
      }
    }
  }
  ui.mw_band[n] = (uint32_t) ui.mw_spans.size();
}
inline void draw_milky_way(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy) {
  const int nb = (int) ui.mw_band.size() - 1;
  if (nb <= 0)
    return;
  const int b0 = std::max(0, (int) (clip.y1 - oy) / 2), b1 = std::min(nb - 1, (int) (clip.y2 - oy) / 2);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_opa = LV_OPA_COVER;
  lv_color_t col[MW_LV + 1];
  for (int l = 1; l <= MW_LV; l++)
    col[l] = lv_color_hex(mix(C_MW, SKY_BG, MW_W[l]));
  for (int b = b0; b <= b1; b++) {
    const int y = oy + 2 * b;
    for (uint32_t i = ui.mw_band[b]; i < ui.mw_band[b + 1]; i++) {
      const Ui::MwSpan &sp = ui.mw_spans[i];
      const int x0 = ox + sp.x0, x1 = ox + sp.x1;
      if (x1 < clip.x1 || x0 > clip.x2)
        continue;
      d.bg_color = col[sp.lvl];
      const lv_area_t a = {x0, y, x1, y + 1};
      lv_draw_rect(layer, &d, &a);
    }
  }
}

inline bool stars_wanted() { return ui.stars_on && (!ui.stars_dusk || ui.view_sun_el < DUSK_SUN_EL); }  // UI-53: at the time drawn

inline void draw_starmap(double t) {
  const Config &c = config();
  const float lst = (float) astro::wrap360(astro::gmst_deg(astro::jd(t)) + c.lon);
  const float sl = sinf((float) c.lat * geo::DEG), cl = cosf((float) c.lat * geo::DEG);
  static pvector<Enu> enu;  // scratch, PSRAM; the drawn lists below stay in internal RAM
  enu.resize(sky::N_STARS);
  for (int i = 0; i < sky::N_STARS; i++)
    enu[i] = star_enu(star_ra(sky::STARS[i]), star_dec(sky::STARS[i]), lst, sl, cl);

  ui.star_segs.clear();
  int poly = 0, mi = 0;
  for (int k = 0; k < sky::N_FIG; poly++) {  // a segment crossing the horizon is cut where it crosses
    const int cnt = sky::FIG[k++];
    while (mi < sky::N_FIG_MAJOR && sky::FIG_MAJOR[mi] < poly)
      mi++;
    const bool major = mi < sky::N_FIG_MAJOR && sky::FIG_MAJOR[mi] == poly;
    for (int j = 0; j + 1 < cnt; j++) {
      Enu a = enu[sky::FIG[k + j]], b = enu[sky::FIG[k + j + 1]];
      if (a.u < 0 && b.u < 0)
        continue;
      if (a.u < 0 || b.u < 0) {
        const float f = a.u / (a.u - b.u);
        Enu h{a.e + f * (b.e - a.e), a.n + f * (b.n - a.n), 0};
        (a.u < 0 ? a : b) = h;
      }
      float x0, y0, x1, y1;
      enu_xy(a, x0, y0);
      enu_xy(b, x1, y1);
      ui.star_segs.push_back(
          {(int16_t) lroundf(x0), (int16_t) lroundf(y0), (int16_t) lroundf(x1), (int16_t) lroundf(y1), major});
    }
    k += cnt;
  }
  ui.star_pts.clear();
  for (int i = 0; i < sky::N_STARS; i++) {  // size and brightness by magnitude
    if (enu[i].u < 0)
      continue;
    const float mag = sky::STARS[i].mag10 / 10.0f;
    float x, y;
    enu_xy(enu[i], x, y);
    const uint8_t size = mag < 0.5f ? 5 : mag < 1.5f ? 4 : mag < 2.5f ? 3 : mag <= STAR_MAG_DOT ? 2 : 1;
    const uint8_t shade = (uint8_t) std::max(90.0f, std::min(255.0f, 255.0f - (mag - 1.0f) * 30.0f));
    ui.star_pts.push_back({(int16_t) lroundf(x), (int16_t) lroundf(y), size, shade});
  }
  if (ui.mw_on)  // UI-64
    mw_build(lst);
  else {
    ui.mw_spans.clear();
    ui.mw_band.clear();
  }
  ui.dso_pts.clear();  // UI-56
  for (int i = 0; i < ev::N_DSO; i++) {
    const Enu v = star_enu(ev::DSO[i].ra, ev::DSO[i].dec, lst, sl, cl);
    if (v.u < 0)
      continue;
    float x, y;
    enu_xy(v, x, y);
    ui.dso_pts.push_back({(int16_t) lroundf(x), (int16_t) lroundf(y), (uint8_t) i});
  }
  if (ui.stars_shown)
    lv_obj_invalidate(ui.w.sky);

  // names: centred on their label point; drop any outside the disc or on a higher-rank name
  struct Box {
    int x0, y0, x1, y1;
  };
  std::vector<Box> used;
  for (lv_obj_t *l : ui.compass_tags)  // keep clear of N/E/S/W
    if (l != nullptr) {
      const int x = (int) lv_obj_get_x(l), y = (int) lv_obj_get_y(l);
      used.push_back({x - 2, y, x + (int) lv_obj_get_width(l) + 2, y + (int) lv_obj_get_height(l)});
    }
  for (auto &nt : ui.names) {
    const sky::Name &nm = sky::NAMES[nt.idx];
    const Enu v = star_enu(nm.ra * (360.0f / 65536.0f), nm.dec * (90.0f / 32767.0f), lst, sl, cl);
    nt.fits = false;
    if (v.u < sinf(5.0f * geo::DEG))
      continue;
    float x, y;
    enu_xy(v, x, y);
    nt.x = (int) lroundf(x) - nt.w / 2;
    nt.y = (int) lroundf(y) - nt.h / 2;
    const Box b{nt.x, nt.y, nt.x + nt.w, nt.y + nt.h};
    bool ok = true;
    const int corner[4][2] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x0, b.y1}, {b.x1, b.y1}};
    for (const auto &p : corner) {
      const float dx = p[0] - ui.cx, dy = p[1] - ui.cy;
      ok &= dx * dx + dy * dy < (float) (ui.radius - 2) * (ui.radius - 2);
    }
    for (size_t j = 0; j < used.size() && ok; j++)
      ok = b.x1 + 4 <= used[j].x0 || used[j].x1 + 4 <= b.x0 || b.y1 <= used[j].y0 || used[j].y1 <= b.y0;
    if (!ok)
      continue;
    used.push_back(b);
    nt.fits = true;
    lv_obj_set_pos(nt.l, nt.x, nt.y);
  }
}

// Draw what draw_starmap() computed, culled to the area being redrawn: a moving
// marker redraws a few pixels, so only the lines and stars touching them are drawn.
inline void draw_overlays(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy);
inline void star_draw_cb(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  const lv_area_t &clip = layer->_clip_area;
  const int ox = box.x1, oy = box.y1;
  if (ui.stars_shown && ui.mw_on)  // UI-64: the bottom-most thing on the disc
    draw_milky_way(layer, clip, ox, oy);
  if (aur.show && aur.dark_now && ui.aurora_alerts) {  // UI-38: faint glow hugging the northern horizon (UI-41a switch)
    const int bands = 18, reach = aur.likely ? 48 : 32, max_opa = aur.likely ? 64 : 48;
    const float step = (float) reach / bands;
    const float north = -ui.heading - 90.0f;  // LVGL angle of true north on screen
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = lv_color_hex(0x50DC96);
    d.center.x = ox + ui.cx;
    d.center.y = oy + ui.cy;
    for (int i = 0; i < bands; i++) {
      const float f = 1.0f - (float) i / bands;
      d.radius = (uint16_t) (ui.radius - i * step);
      d.width = (uint16_t) ceilf(step) + 1;
      const float half = 75.0f * (1.0f - 0.5f * (1.0f - f));  // shorter towards the centre
      constexpr int STEPS = 7;  // angular steps fading out on each side
      for (int k = 0; k < STEPS; k++) {
        const float a0 = half * k / STEPS, a1 = half * (k + 1) / STEPS;
        const float fk = 1.0f - (float) k / STEPS;
        d.opa = (lv_opa_t) (max_opa * powf(f, 1.6f) * fk * fk);
        if (d.opa < 2)
          continue;
        for (int side = -1; side <= 1; side += 2) {
          float s0 = north + side * a0, s1 = north + side * a1;
          if (s0 > s1)
            std::swap(s0, s1);
          d.start_angle = (lv_value_precise_t) fmodf(s0 + 720.0f, 360.0f);
          d.end_angle = (lv_value_precise_t) fmodf(s1 + 720.0f, 360.0f);
          lv_draw_arc(layer, &d);
        }
      }
    }
  }
  if (!ui.stars_shown) {
    draw_overlays(layer, clip, ox, oy);  // UI-24 arc, UI-26 trails
    return;
  }
  for (int pass = 0; pass < 2; pass++) {  // faint figures, then the major ones on top
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(pass ? C_FIG_MAJOR : C_FIG);
    d.width = pass ? 2 : 1;
    d.round_start = d.round_end = pass;
    d.opa = LV_OPA_COVER;
    for (const auto &g : ui.star_segs) {
      if (g.major != (pass == 1))
        continue;
      const int x0 = ox + std::min(g.x0, g.x1) - 2, x1 = ox + std::max(g.x0, g.x1) + 2;
      const int y0 = oy + std::min(g.y0, g.y1) - 2, y1 = oy + std::max(g.y0, g.y1) + 2;
      if (x1 < clip.x1 || x0 > clip.x2 || y1 < clip.y1 || y0 > clip.y2)
        continue;
      d.p1.x = (lv_value_precise_t) (ox + g.x0);
      d.p1.y = (lv_value_precise_t) (oy + g.y0);
      d.p2.x = (lv_value_precise_t) (ox + g.x1);
      d.p2.y = (lv_value_precise_t) (oy + g.y1);
      lv_draw_line(layer, &d);
    }
  }
  {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    for (const auto &p : ui.star_pts) {
      const int x0 = ox + (int) lroundf(p.x - p.size / 2.0f), y0 = oy + (int) lroundf(p.y - p.size / 2.0f);
      const lv_area_t a = {x0, y0, x0 + p.size - 1, y0 + p.size - 1};
      if (a.x2 < clip.x1 || a.x1 > clip.x2 || a.y2 < clip.y1 || a.y1 > clip.y2)
        continue;
      d.bg_color = lv_color_hex(mix(C_STAR, SKY_BG, p.shade));
      lv_draw_rect(layer, &d, &a);
    }
  }
  {  // UI-56 deep-sky objects: a small ring and the catalogue name
    lv_draw_arc_dsc_t r;
    lv_draw_arc_dsc_init(&r);
    r.color = lv_color_hex(mix(C_DSO, SKY_BG, 190));
    r.width = 1;
    r.radius = 4;
    r.start_angle = 0;
    r.end_angle = 360;
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = ui.w.label_font;
    l.color = lv_color_hex(mix(C_DSO, SKY_BG, 150));
    for (const auto &p : ui.dso_pts) {
      const int cx = ox + p.x, cy = oy + p.y;
      if (cx + 50 < clip.x1 || cx - 6 > clip.x2 || cy + 10 < clip.y1 || cy - 8 > clip.y2)
        continue;
      r.center.x = cx;
      r.center.y = cy;
      lv_draw_arc(layer, &r);
      if (l.font) {
        l.text = ev::DSO[p.idx].cat;
        const lv_area_t a = {cx + 6, cy - 7, cx + 90, cy + 9};
        lv_draw_label(layer, &l, &a);
      }
    }
  }
  draw_overlays(layer, clip, ox, oy);
}

// Show, hide or refresh the star layer (every tick; the positions once per STAR_PERIOD).
inline void update_starmap(double t) {
  if (!ui.star_cb)
    return;
  const bool want = stars_wanted() && clock_valid();
  bool changed = false;
  if (want && (!ui.stars_shown || t >= ui.star_next)) {
    draw_starmap(t);
    ui.star_next = t + STAR_PERIOD;
    changed = true;
  }
  if (want != ui.stars_shown) {
    ui.stars_shown = want;
    lv_obj_invalidate(ui.w.sky);
    changed = true;
  }
  if (changed)
    declutter_labels();  // names follow the layer
}

// UI-20 phase picture. Each pixel is 4x4 supersampled: alpha is the disc's coverage
// (smooth limb), colour mixes the dark side (earthshine) and the lit side, lit with a
// little limb darkening. On a row of half-width w the terminator sits at x = w(1-2k)
// for illuminated fraction k; the lit side faces the Sun: right when waxing seen from
// the northern hemisphere, left from the southern.
inline void draw_moon_icon(float illum, bool waxing, bool south) {
  lv_obj_t *cv = ui.w.moon_img;
  if (cv == nullptr)
    return;
  const float k = std::max(0.0f, std::min(1.0f, illum));
  if (fabsf(k - ui.icon_illum) < 0.004f && waxing == ui.icon_waxing && south == ui.icon_south)
    return;
  ui.icon_illum = k;
  ui.icon_waxing = waxing;
  ui.icon_south = south;
  render_moon(cv, k, waxing != south ? 1.0f : -1.0f, 0.0f);
}

// UI-20: next sunrise or sunset, next moonrise or moonset, and the Moon's phase.
// A body that stays below the horizon for the next 24 h gets an empty line.
inline void draw_almanac(double t) {
  if (ui.w.alm_sun == nullptr || !clock_valid())
    return;
  const Config &c = config();
  if (t >= ui.alm_next || (ui.sun_ev.t > 0 && t >= ui.sun_ev.t) || (ui.moon_ev.t > 0 && t >= ui.moon_ev.t)) {
    astro::next_events(t, c.lat, c.lon, c.alt_m, ui.sun_ev, ui.moon_ev);
    ui.alm_next = t + 900;  // the 24 h window slides; refresh every 15 min
  }
  char b[40], hm[12];
  auto line = [&](lv_obj_t *l, const astro::Event &e, const char *body) {
    if (e.t > 0) {
      local_hm((int64_t) (e.t + 30), hm, sizeof(hm));  // nearest minute
      snprintf(b, sizeof(b), "%s%s %s", body, e.rise ? "rise" : "set", hm);
    } else if (e.up) {
      snprintf(b, sizeof(b), "%s up all day", body);
    } else {
      b[0] = 0;
    }
    set_text_if(l, b);
  };
  line(ui.w.alm_sun, ui.sun_ev, "Sun");
  line(ui.w.alm_moon, ui.moon_ev, "Moon");
  snprintf(b, sizeof(b), "%s %.0f%%", astro::phase_name(ui.sm.moon_illum, ui.sm.moon_elong),
           std::max(0.0f, std::min(1.0f, ui.sm.moon_illum)) * 100.0f);
  set_text_if(ui.w.alm_phase, b);
  draw_moon_icon(ui.sm.moon_illum, ui.sm.moon_elong < 180.0f, c.lat < 0);
}

inline bool network_up();

// Age for the status line: the one duration format (UI-17a), at least a minute
inline void fmt_age(double s, char *buf, size_t n) { fmt_dur(std::max(60.0, s), buf, n); }

constexpr uint32_t C_OK = 0x7E8BB3, C_WARN = 0xFFB347, C_BAD = 0xFF6B6B;
constexpr uint32_t C_AURORA = 0x7EE0B0;  // UI-38: soft aurora green

// UI-25: one or two lines under the title saying how fresh the orbits are, or what
// went wrong and what the device is doing about it. Returns the colour to use.
inline uint32_t status_text(double t, char *b, size_t n) {
  const DataStatus &s = live.status;
  const bool have = s.n_sats > 0 || s.have_iss || s.n_starlink > 0;
  char age[24], retry[24];
  if (!clock_valid()) {
    snprintf(b, n, "Waiting for the time...");
    return C_OK;
  }
  if (!have && s.error[0] == 0) {
    snprintf(b, n, network_up() ? "Downloading orbital data..." : "Waiting for Wi-Fi to download orbital data");
    return C_OK;
  }
  fmt_age(std::max(0.0, s.next_try - t), retry, sizeof(retry));
  if (!have) {
    snprintf(b, n, "%s.\nNo orbital data yet; retrying in %s", s.error, retry);
    return C_BAD;
  }
  // Only the ISS loaded (both layers off, or none downloaded yet): say so.
  const bool iss_only = s.have_iss && s.n_sats == 0 && s.n_starlink == 0;
  const char *what = iss_only ? "ISS orbital data" : "Orbital data";
  fmt_age(t - s.data_time, age, sizeof(age));
  if (s.error[0]) {
    snprintf(b, n, "%s.\nUsing %s%s %s old; retrying in %s", s.error, iss_only ? "ISS " : "", "orbital data", age, retry);
    return C_WARN;
  }
  if (t - s.data_time > STALE_S) {
    snprintf(b, n, "%s is %s old;\npositions may drift", what, age);
    return C_WARN;
  }
  fmt_age(t - s.loaded, age, sizeof(age));
  snprintf(b, n, "%s updated %s ago", what, age);
  return C_OK;
}

constexpr double PASS_RETRY_S = 600;  // DATA-6a

inline const Pass *next_pass(double t) {
  for (const auto &x : live.passes)
    if (x.end > t)
      return &x;
  return nullptr;
}

// HW-9: a temporary message that replaces the status line (compass calibration).
inline char notice_text[120] = {0};
inline double notice_until = 0;

// UI-32: layer counts in a column down the right edge, one row per layer that is on:
// icon + number (or "SAT 20" text without the icon font), right-aligned under the clock.
constexpr int COUNT_ROWS = 3, COUNT_Y0 = 34, COUNT_DY = 26, COUNT_RIGHT = 472;  // UI-32: 20 px icons, mono16
inline lv_obj_t *count_icon[COUNT_ROWS] = {}, *count_num[COUNT_ROWS] = {};
inline void draw_counts(const Config &c) {
  struct Row {
    const char *icon, *tag;
    int n;
    uint32_t col;
  } rows[COUNT_ROWS];
  int k = 0;
  if (ui.sats_on || ui.meo_on || ui.debris_on)  // UI-35: debris is its own layer
    rows[k++] = {"\xF3\xB0\x91\xB1", "SAT", live.sat_total, C_LEO};  // satellite-variant
  if (ui.starlink_on && c.starlink_radius > 0)
    rows[k++] = {"\xF3\xB0\xA4\x89", "SL", ui.starlink_in_cone, 0x8FA8F0};  // satellite-uplink
  if (ui.geo_on)
    rows[k++] = {"\xF3\xB0\x87\xA7", "GEO", live.geo_total, C_GEO_DOT};  // earth
  if (ui.w.counts == nullptr)
    return;
  if (ui.w.icon_font == nullptr) {  // text fallback, one layer per line
    char b[64] = {0};
    for (int i = 0; i < k; i++) {
      const size_t n = strlen(b);
      snprintf(b + n, sizeof(b) - n, "%s%s %d", i ? "\n" : "", rows[i].tag, rows[i].n);
    }
    set_text_if(ui.w.counts, b);
    return;
  }
  lv_obj_t *parent = lv_obj_get_parent(ui.w.counts);
  if (count_icon[0] == nullptr) {
    lv_obj_add_flag(ui.w.counts, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < COUNT_ROWS; i++) {
      count_num[i] = lv_label_create(parent);
      lv_obj_remove_style_all(count_num[i]);
      const lv_font_t *nf = ui.w.title_font ? ui.w.title_font : ui.w.label_font;  // UI-32: mono16
      if (nf)
        lv_obj_set_style_text_font(count_num[i], nf, 0);
      lv_obj_set_style_text_color(count_num[i], lv_color_hex(C_DIM), 0);
      lv_obj_align(count_num[i], LV_ALIGN_TOP_RIGHT, COUNT_RIGHT - 480, COUNT_Y0 + i * COUNT_DY + 2);
      count_icon[i] = lv_label_create(parent);
      lv_obj_remove_style_all(count_icon[i]);
      lv_obj_set_style_text_font(count_icon[i], ui.w.card_icon_font ? ui.w.card_icon_font : ui.w.icon_font, 0);
      // fixed 20 px icon column; numbers (up to 3 digits) right-aligned beside it
      lv_obj_align(count_icon[i], LV_ALIGN_TOP_RIGHT, COUNT_RIGHT - 480 - 36, COUNT_Y0 + i * COUNT_DY);
    }
  }
  char num[12];
  for (int i = 0; i < COUNT_ROWS; i++) {
    const bool on = i < k;
    if (!on) {
      lv_obj_add_flag(count_num[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(count_icon[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    snprintf(num, sizeof(num), "%d", rows[i].n);
    const bool new_icon = strcmp(lv_label_get_text(count_icon[i]), rows[i].icon) != 0;
    set_text_if(count_num[i], num);
    if (new_icon) {
      lv_label_set_text(count_icon[i], rows[i].icon);
      lv_obj_set_style_text_color(count_icon[i], lv_color_hex(rows[i].col), 0);
    }
    lv_obj_remove_flag(count_num[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(count_icon[i], LV_OBJ_FLAG_HIDDEN);
  }
}

// UI-41: status-line alerts, shown in turn when the status line has nothing more
// important (warnings and errors keep it).
struct Alert {
  char text[96];
  uint32_t col;
  const char *glyph;           // MDI glyph (card icon font), or
  const lv_image_dsc_t *img;   // a picture (planets, alignments)
  // UI-41d what a tap on it opens: an object's card (kind >= 0, id), or else a details card
  // (K_INFO) of type info about item idx
  int8_t kind;
  int32_t id;
  uint8_t info;
  int16_t idx;
  const lv_image_dsc_t *flag;  // UI-54d: the country's flag, shown on the alert's details card
};
// UI-41h / UI-69m: a capsule splashing down (not a landing on land or the Moon)
inline bool is_splashdown(const net::EventRec &e) {
  return strstr(e.type, "Landing") != nullptr && (strstr(e.name, "plashdown") || strstr(e.name, "plash Down"));
}
enum AlertInfo : uint8_t { AI_NONE, AI_ISS_PASS, AI_CSS_PASS, AI_AURORA, AI_WIND, AI_LAUNCH, AI_EVENT, AI_CONJ, AI_PARADE,
                           AI_ECLIPSE, AI_SEASON };
inline Alert shown_alert;          // UI-41d the alert on the status line now
inline bool shown_alert_ok = false;
inline Alert info_alert;           // the one whose details card is open (K_INFO)
inline lv_obj_t *alert_zone = nullptr;  // the alert's tap zone (UI-41d)
inline lv_obj_t *card_find_btn = nullptr;  // the card's Find button (hidden on K_INFO)
inline void alert_click_cb(lv_event_t *e);
constexpr int MAX_ALERTS = 12;
constexpr int ALERT_DY = 3;  // UI-41: alerts sit this much lower than the status line (4.5.4)
constexpr double ALERT_ROTATE_S = 60;  // UI-41: each alert for a minute (was 6 s)
inline int collect_alerts(double t, Alert *out, int max);
// UI-41c where an alert's icon sits against its text: 1 its top on the text's cap height,
// 2 centred on the text (one or two lines), 3 centred on the first line; 0 the old fixed spot
inline int alert_icon_align = 2;
inline const Alert *alert_override = nullptr;  // host tests: show this alert
// the first code point of a UTF-8 string
inline uint32_t utf8_first(const char *s) {
  const uint8_t *u = (const uint8_t *) s;
  if (u[0] < 0x80)
    return u[0];
  if ((u[0] & 0xE0) == 0xC0)
    return (uint32_t) (u[0] & 0x1F) << 6 | (u[1] & 0x3F);
  if ((u[0] & 0xF0) == 0xE0)
    return (uint32_t) (u[0] & 0x0F) << 12 | (uint32_t) (u[1] & 0x3F) << 6 | (u[2] & 0x3F);
  return (uint32_t) (u[0] & 0x07) << 18 | (uint32_t) (u[1] & 0x3F) << 12 | (uint32_t) (u[2] & 0x3F) << 6 | (u[3] & 0x3F);
}
// ink of a glyph inside its label box: top offset and height (font metrics, not the box)
inline void glyph_ink(const lv_font_t *f, uint32_t cp, int32_t &top, int32_t &h) {
  lv_font_glyph_dsc_t g;
  const int32_t asc = lv_font_get_line_height(f) - f->base_line;
  if (lv_font_get_glyph_dsc(f, &g, cp, 0)) {
    top = asc - (g.box_h + g.ofs_y);
    h = g.box_h;
  } else {
    top = 0;
    h = lv_font_get_line_height(f);
  }
}

inline void draw_hud(double t) {
  char b[160];
  const Config &c = config();
  static uint32_t status_col = 0;
  uint32_t col;
  if (notice_text[0] && t < notice_until) {
    snprintf(b, sizeof(b), "%s", notice_text);
    col = C_WARN;
  } else {
    col = status_text(t, b, sizeof(b));
    // UI-25: the "... updated X ago" all-is-well line only shows for 5 minutes after a
    // download; warnings and errors stay. (HA's Data Status keeps the full text.)
    if (col == C_OK && live.status.loaded > 0 && t - live.status.loaded > STATUS_OK_SHOW_S &&
        strstr(b, " updated ") != nullptr)
      b[0] = 0;
  }
  // UI-38/41: alerts (aurora, ISS pass, planets, alignments) in place of an all-is-well
  // (or empty) status line, one at a time, each for ALERT_ROTATE_S
  aurora_update(t);
  Alert alerts[MAX_ALERTS];
  const int n_alerts = (col == C_OK && !(notice_text[0] && t < notice_until)) ? collect_alerts(t, alerts, MAX_ALERTS) : 0;
  const Alert *al = n_alerts ? &alerts[(int64_t) (t / ALERT_ROTATE_S) % n_alerts] : nullptr;
  if (alert_override)
    al = alert_override, col = C_OK;
  if (al) {
    snprintf(b, sizeof(b), "%s", al->text);
    col = al->col;
    shown_alert = *al;  // UI-41d: what a tap on the status line opens
  }
  shown_alert_ok = al != nullptr;
  if (ui.w.status) {
    static int base_x = INT_MIN, base_y = INT_MIN;
    static const lv_font_t *status_font = nullptr;
    if (base_x == INT_MIN) {
      base_x = lv_obj_get_x(ui.w.status);
      base_y = lv_obj_get_y(ui.w.status);
      status_font = lv_obj_get_style_text_font(ui.w.status, LV_PART_MAIN);
      // UI-41d a tap on an alert opens its details; anything else still reaches the page.
      // The tap zone is the header's left part above the map (icon, both text lines and the
      // space round them): 400 x 56 from the top left, clear of the counts and the sky disc
      lv_obj_t *zone = alert_zone = lv_obj_create(lv_obj_get_parent(ui.w.status));
      lv_obj_remove_style_all(zone);
      lv_obj_set_pos(zone, 0, 0);
      lv_obj_set_size(zone, 400, 56);
      lv_obj_add_flag(zone, (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
      lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(zone, alert_click_cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *parent = lv_obj_get_parent(ui.w.status);
    if (ui.aurora_icon == nullptr && ui.w.card_icon_font) {  // glyph slot (aurora, ISS)
      ui.aurora_icon = lv_label_create(parent);
      lv_obj_remove_style_all(ui.aurora_icon);
      lv_obj_set_style_text_font(ui.aurora_icon, ui.w.card_icon_font, 0);
      lv_label_set_text(ui.aurora_icon, "");
      lv_obj_set_pos(ui.aurora_icon, base_x, base_y + ALERT_DY - 2);
      lv_obj_add_flag(ui.aurora_icon, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(ui.aurora_icon, LV_OBJ_FLAG_CLICKABLE);  // UI-41d: a tap goes to the alert's zone
    }
    if (ui.alert_img == nullptr) {  // picture slot (planets, alignments)
      ui.alert_img = lv_image_create(parent);
      lv_obj_remove_style_all(ui.alert_img);
      lv_obj_set_pos(ui.alert_img, base_x, base_y + ALERT_DY - 2);
      lv_obj_add_flag(ui.alert_img, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(ui.alert_img, LV_OBJ_FLAG_CLICKABLE);  // UI-41d: images take taps by default
    }
    static const void *shown_icon = nullptr;
    const void *want_icon = al ? (al->img ? (const void *) al->img : (const void *) al->glyph) : nullptr;
    if (want_icon != shown_icon) {
      shown_icon = want_icon;
      const bool glyph = al && !al->img && al->glyph && ui.aurora_icon;
      const bool img = al && al->img;
      if (glyph) {
        lv_label_set_text(ui.aurora_icon, al->glyph);
        lv_obj_set_style_text_color(ui.aurora_icon, lv_color_hex(al->col), 0);
        lv_obj_remove_flag(ui.aurora_icon, LV_OBJ_FLAG_HIDDEN);
      } else if (ui.aurora_icon) {
        lv_obj_add_flag(ui.aurora_icon, LV_OBJ_FLAG_HIDDEN);
      }
      if (img) {
        lv_image_set_src(ui.alert_img, al->img);
        lv_obj_remove_flag(ui.alert_img, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(ui.alert_img, LV_OBJ_FLAG_HIDDEN);
      }
      const bool icon = glyph || img;
      lv_obj_set_x(ui.w.status, base_x + (icon ? 24 : 0));  // text clears the icon
      // UI-41: every alert in one font (mono15; mono14 without it), ALERT_DY lower than
      // the status line, which keeps its own font
      const lv_font_t *af = ui.w.alert_font ? ui.w.alert_font : ui.w.card_font;
      const lv_font_t *want = al && af ? af : status_font;
      lv_obj_set_style_text_font(ui.w.status, want, 0);
      lv_obj_set_y(ui.w.status, base_y + (al && af ? ALERT_DY : 0));
    }
  }
  // PERF-14: a new status line is drawn as the scan leaves it (see the end of draw_hud)
  const bool status_changed = ui.w.status && strcmp(lv_label_get_text(ui.w.status), b) != 0;
  set_text_if(ui.w.status, b);
  // UI-41c: the icon against the text as it now stands (one or two lines)
  if (al && ui.w.status && alert_icon_align) {
    lv_obj_t *icon = al->img ? ui.alert_img : ui.aurora_icon;
    const lv_font_t *tf = lv_obj_get_style_text_font(ui.w.status, LV_PART_MAIN);
    if (icon && tf) {
      lv_obj_update_layout(ui.w.status);
      const int32_t lh = lv_font_get_line_height(tf), ty = lv_obj_get_y(ui.w.status);
      const int32_t lines = std::max<int32_t>(1, (lv_obj_get_height(ui.w.status) + lh / 2) / lh);
      int32_t cap_off, cap_h;
      glyph_ink(tf, 'H', cap_off, cap_h);
      const int32_t cap_top = ty + cap_off, last_base = ty + (lines - 1) * lh + cap_off + cap_h;
      int32_t ink_off = 0, ink_h = 20;
      if (al->img)
        ink_h = al->img->header.h;
      else if (ui.w.card_icon_font)
        glyph_ink(ui.w.card_icon_font, utf8_first(al->glyph), ink_off, ink_h);
      const int32_t first_base = cap_top + cap_h;
      const int32_t y = alert_icon_align == 2   ? (cap_top + last_base) / 2 - ink_h / 2 - ink_off
                        : alert_icon_align == 3 ? (cap_top + first_base) / 2 - ink_h / 2 - ink_off
                                                : cap_top - ink_off;
      if (lv_obj_get_y(icon) != y)
        lv_obj_set_y(icon, y);

    }
  }
  if (col != status_col && ui.w.status) {
    status_col = col;
    lv_obj_set_style_text_color(ui.w.status, lv_color_hex(col), 0);
  }

  draw_counts(c);

  if (clock_valid()) {
    local_hm((int64_t) t, b, sizeof(b));
    set_text_if(ui.w.clock, b);
  }
  draw_almanac(t);

  // UI-9 footer, line 1: ISS
  if (!ui.iss_valid)
    snprintf(b, sizeof(b), "ISS  no orbital data yet");
  else if (ui.iss_azel.el >= 0)
    snprintf(b, sizeof(b), "ISS  %s %.0f°  up %.0f°  %s", compass(ui.iss_azel.az), ui.iss_azel.az, ui.iss_azel.el,
             ui.iss_sunlit ? "sunlit" : "in shadow");
  else
    snprintf(b, sizeof(b), "ISS  below horizon (%s)  %s", compass(ui.iss_azel.az), ui.iss_sunlit ? "sunlit" : "in shadow");
  set_text_if(ui.w.foot_iss, b);

  // line 2: next pass; yellow when it can be seen (sunlit ISS, dark sky), dim otherwise
  const Pass *p = next_pass(t);
  bool visible = false;
  if (p == nullptr) {
    snprintf(b, sizeof(b), "%s", live.status.have_iss ? "No ISS pass above 10° in 4 days" : "Next pass: ...");
  } else {
    char hm[12], dur[24];
    visible = p->visible;
    const char *what = p->visible ? "Visible pass" : "Pass";
    if (p->start <= t) {
      local_hm(p->end, hm, sizeof(hm));
      snprintf(b, sizeof(b), "%s NOW until %s  peak %.0f° %s", what, hm, p->max_el, p->max_dir);
    } else {
      local_hm(p->start, hm, sizeof(hm));
      fmt_dur((double) p->start - t, dur, sizeof(dur));
      snprintf(b, sizeof(b), "%s %s in %s  peak %.0f° %s", what, hm, dur, p->max_el, p->max_dir);
    }
  }
  set_text_if(ui.w.foot_pass, b);
  static int pass_vis = -1;
  if ((int) visible != pass_vis && ui.w.foot_pass) {
    pass_vis = visible;
    lv_obj_set_style_text_color(ui.w.foot_pass, lv_color_hex(visible ? C_SUN : C_DIM), 0);
  }
  if (status_changed && ui.w.status && lv_obj_is_visible(ui.w.status)) {
    vs::wait_scan_past(lv_obj_get_y(ui.w.status) + lv_obj_get_height(ui.w.status) + 8);
    lv_refr_now(nullptr);
  }
}

struct Row {
  float el;
  float az;
  const SatRec *r;
};

inline void rows_for(const pvector<SatRec> &v, double t, std::vector<Row> &out, float min_el = -90.0f) {
  out.clear();
  out.reserve(v.size());
  for (const auto &r : v) {
    const geo::AzEl a = geo::azel(ui.obs, geo::sat_ecef(r, t));
    if (a.el >= min_el)
      out.push_back({a.el, a.az, &r});
  }
  // UI-10 / MOTION-4: sort a copy, never the live list
  std::sort(out.begin(), out.end(), [](const Row &a, const Row &b) { return a.el > b.el; });
}

enum RowKind : uint8_t { RK_HEAD, RK_ISS, RK_SUN, RK_MOON, RK_SAT, RK_STARLINK, RK_NOTE, RK_PLANET, RK_CSS = 20, RK_LAUNCH = 21, RK_COMET = 22 };  // RK_PLANET + p (UI-40b)

// UI-10: header and row colours, applied as each cell's text is drawn.
inline void list_draw_cb(lv_event_t *e) {
  lv_draw_task_t *task = lv_event_get_draw_task(e);
  lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *) lv_draw_task_get_draw_dsc(task);
  if (base == nullptr || base->part != LV_PART_ITEMS)
    return;
  const uint32_t row = base->id1;
  if (row >= ui.row_kind.size())
    return;
  const uint8_t k = ui.row_kind[row];
  if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_LABEL) {
    lv_draw_label_dsc_t *d = lv_draw_task_get_label_dsc(task);
    uint32_t c = C_TEXT;
    switch (k) {
      case RK_HEAD: c = C_DIM; break;
      case RK_ISS: c = C_ISS; break;
      case RK_CSS: c = C_CSS; break;  // UI-52
      case RK_LAUNCH: c = C_LAUNCH; break;  // UI-54
      case RK_COMET: c = C_COMET; break;    // UI-63
      case RK_SUN: c = C_SUN; break;
      case RK_MOON: c = C_MOON; break;
      case RK_STARLINK: c = 0x8FA8F0; break;
      case RK_NOTE: c = C_DIM; break;
      default:
        if (k >= RK_PLANET && k < (int) RK_PLANET + (int) planets::N_PLANETS)
          c = C_PLANET[k - RK_PLANET];  // UI-40b
        break;
    }
    d->color = lv_color_hex(c);
    if (base->id2 == 0 && ui.w.icon_font)  // UI-30: icon column
      d->font = ui.w.icon_font;
    if (base->id2 == 0 && k >= RK_PLANET && k < (int) RK_PLANET + (int) planets::N_PLANETS && base->layer) {
      // UI-40b: the planet's own 14 px picture in place of the placeholder glyph
      d->opa = LV_OPA_TRANSP;
      lv_area_t a;
      lv_draw_task_get_area(task, &a);
      const int32_t cy = (a.y1 + a.y2) / 2, H = picons::SMALL / 2;
      const lv_area_t ia = {a.x1, cy - H, a.x1 + picons::SMALL - 1, cy - H + picons::SMALL - 1};
      lv_draw_image_dsc_t im;
      lv_draw_image_dsc_init(&im);
      im.src = planet_dsc(k - RK_PLANET, false);
      lv_draw_image(base->layer, &im, &ia);
    }
  } else if (lv_draw_task_get_type(task) == LV_DRAW_TASK_TYPE_FILL && k == RK_HEAD) {
    lv_draw_fill_dsc_t *d = lv_draw_task_get_fill_dsc(task);
    if (d)
      d->color = lv_color_hex(0x101B3D);
  }
}

inline void list_build() {
  lv_obj_t *t = ui.w.list;
  if (t == nullptr)
    return;
  lv_table_set_column_count(t, 7);
  // UI-10 + UI-28 class column + UI-30 icon column (none without the icon font)
  const int w[7] = {ui.w.icon_font ? 24 : 0, 128, 46, 42, 82, 62, 80};
  for (int i = 0; i < 7; i++)
    lv_table_set_column_width(t, i, w[i]);
  lv_obj_add_flag(t, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
  lv_obj_add_event_cb(t, list_draw_cb, LV_EVENT_DRAW_TASK_ADDED, nullptr);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);  // taps fall through to the page (UI-2)
}

// ================================================================== UI-54 rocket launches
constexpr float LAUNCH_NEAR_KM = 1500.0f;     // close enough that the rocket may be seen
constexpr double LAUNCH_SOON_S = 3600.0;      // any launch: alert in its last hour
constexpr double LAUNCH_AHEAD_S = 3 * 86400.0;  // UI-54/54c: alerts from 3 days ahead
inline float launch_km(const net::LaunchRec &l, float *bearing = nullptr) {
  if (std::isnan(l.lat) || std::isnan(l.lon))
    return 1e9f;
  const Config &c = config();
  const double la1 = astro::rad(c.lat), la2 = astro::rad(l.lat), dl = astro::rad(l.lon - c.lon);
  const double a = sin((la2 - la1) / 2) * sin((la2 - la1) / 2) + cos(la1) * cos(la2) * sin(dl / 2) * sin(dl / 2);
  if (bearing) {
    const double b = atan2(sin(dl) * cos(la2), cos(la1) * sin(la2) - sin(la1) * cos(la2) * cos(dl));
    *bearing = (float) astro::wrap360(b * 180.0 / M_PI);
  }
  return (float) (6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a)));
}
inline bool launch_pending(const net::LaunchRec &l) {  // not flown, not scrubbed
  return strcmp(l.status, "Success") != 0 && strcmp(l.status, "Failure") != 0 && strcmp(l.status, "Partial F") != 0;
}
inline void countdown(double s, char *b, size_t n) { fmt_dur(s, b, n); }  // UI-17a
inline const char *short_site(const char *where) {  // "Cape Canaveral SFS, FL, USA" -> "Cape Canaveral SFS"
  static char b[32];
  const char *c = strchr(where, ',');
  const size_t k = c ? std::min((size_t) (c - where), sizeof(b) - 1) : std::min(strlen(where), sizeof(b) - 1);
  memcpy(b, where, k);
  b[k] = 0;
  return b;
}

inline void draw_list(double t) {
  lv_obj_t *tb = ui.w.list;
  if (tb == nullptr)
    return;
  struct Cells {
    char c[7][24];
  };
  std::vector<Cells> rows;
  std::vector<uint8_t> kinds;
  auto add = [&](uint8_t k, const char *ic, const char *a, const char *b, const char *cl, const char *c, const char *d,
                 const char *e) {
    Cells x;
    const char *v[7] = {ic, a, b, cl, c, d, e};
    for (int i = 0; i < 7; i++) {
      strncpy(x.c[i], v[i], sizeof(x.c[i]) - 1);
      x.c[i][sizeof(x.c[i]) - 1] = 0;
    }
    rows.push_back(x);
    kinds.push_back(k);
  };
  char dir[16], el[16], alt[16];
  auto fmt_pos = [&](geo::AzEl a) {
    snprintf(dir, sizeof(dir), "%s %.0f\xC2\xB0", compass(a.az), a.az);  // UI-10: degree sign
    snprintf(el, sizeof(el), "%.1f°", a.el);
  };
  add(RK_HEAD, "", "NAME", "CTRY", "CL", "DIR", "EL", "HEIGHT");
  if (ui.iss_valid) {
    fmt_pos(ui.iss_azel);
    snprintf(alt, sizeof(alt), "%.0f %s", dist(live.iss.alt_km), dist_unit());
    if (ui.stations_on)  // UI-52b
      add(RK_ISS, ICON_ISS, "ISS", "ISS", "LEO", dir, el, alt);
  } else {
    if (ui.stations_on)  // UI-52b
      add(RK_ISS, ICON_ISS, "ISS", "", "", "no data", "", "");
  }
  if (ui.css.id >= 0 && ui.css.up) {  // UI-52: Tiangong while it is above the horizon
    const geo::AzEl a = geo::azel(ui.obs, geo::sat_ecef(ui.css.rec, t));
    fmt_pos(a);
    snprintf(alt, sizeof(alt), "%.0f %s", dist(ui.css.rec.alt_km), dist_unit());
    if (ui.stations_on)  // UI-52b
      add(RK_CSS, ICON_ISS, "Tiangong", "CN", "LEO", dir, el, alt);
  }
  fmt_pos(ui.sm.sun);
  snprintf(alt, sizeof(alt), "%.2f AU", planets::sun_dist_au(astro::jd(t)));  // UI-36b
  add(RK_SUN, ICON_SUN, "Sun", "", "STR", dir, el, alt);  // UI-40b: star
  fmt_pos(ui.sm.moon);
  char lit[16];
  snprintf(lit, sizeof(lit), "%.0f%% lit", ui.sm.moon_illum * 100.0f);
  add(RK_MOON, ICON_MOON, "Moon", "", "LUN", dir, el, lit);  // lunar
  // UI-40b: planets above the horizon, highest first, distance in the last column
  if (ui.planets_on) {
    int order[planets::N_PLANETS], n = 0;
    for (int p = 0; p < planets::N_PLANETS; p++)
      if (ui.planet_shown[p])
        order[n++] = p;
    for (int i = 1; i < n; i++)  // highest first (insertion sort: at most 5)
      for (int j = i; j > 0 && ui.planet_azel[order[j]].el > ui.planet_azel[order[j - 1]].el; j--)
        std::swap(order[j], order[j - 1]);
    const Config &c = config();
    for (int i = 0; i < n; i++) {
      const int p = order[i];
      planets::Pos ps = planets::position(p, astro::jd(t));
      (void) c;
      fmt_pos(ui.planet_azel[p]);
      snprintf(alt, sizeof(alt), "%.2f AU", ps.dist_au);
      add((uint8_t) ((int) RK_PLANET + p), "\xF3\xB0\x80\x98", planets::name(p), "", "PL", dir, el, alt);  // mdi:orbit
    }
  }
  // UI-63: comets on the map (brightest first)
  for (int k = 0; k < Ui::MAX_COMETS; k++) {
    if (ui.comet_idx[k] < 0 || !ui.comet_shown[k])
      continue;
    fmt_pos(ui.comet_azel[k]);
    snprintf(alt, sizeof(alt), "mag %.1f", ui.comet_mag[k]);
    add(RK_COMET, ICON_COMET, live.comet_list[ui.comet_idx[k]].tag, "", "CM", dir, el, alt);
  }

  // UI-54: the next two launches in the next two days: rocket, direction of the site, countdown
  {
    int n = 0;
    for (const auto &l : live.launches) {
      if (n >= 2 || !launch_pending(l) || l.net < t - 900 || l.net - t > 2 * 86400.0)
        continue;
      float brg = 0;
      const float km = launch_km(l, &brg);
      char nm[20], cd[24], d[16];
      snprintf(nm, sizeof(nm), "%.15s", l.rocket[0] ? l.rocket : l.name);
      countdown(l.net - t, cd, sizeof(cd));
      snprintf(alt, sizeof(alt), "T-%.11s", cd);
      if (km < 1e8f)
        snprintf(d, sizeof(d), "%s %.0f\xC2\xB0", compass(brg), brg);
      else
        d[0] = 0;
      add(RK_LAUNCH, "\xF3\xB1\x93\x9E", nm, "", "RKT", d, "", alt);
      n++;
    }
  }
  std::vector<Row> sl_rows, sat_rows, geo_rows;
  if (ui.starlink_on)
    rows_for(live.starlink, t, sl_rows, starlink_min_el());  // UI-4: the cone only
  if (ui.sats_on || ui.meo_on || ui.debris_on)  // UI-35: debris is its own layer
    rows_for(live.sats, t, sat_rows, 0.0f);
  if (ui.geo_on)
    rows_for(live.geo, t, geo_rows, 0.0f);
  const int room = LIST_ROWS - (int) rows.size();
  const int sl_shown = std::min((int) sl_rows.size(), std::max(0, std::min(5, room / 3)));
  const int sl_more = (int) sl_rows.size() - sl_shown;
  // UI-28: GEO gets what LEO/MEO leave, at most 4 rows, below them
  const int geo_cap = geo_rows.empty() ? 0 : 4;
  int sat_room = room - sl_shown - (sl_more > 0 ? 1 : 0) - (geo_cap ? std::min((int) geo_rows.size(), geo_cap) + 1 : 0);
  const int sat_n = (int) sat_rows.size();
  const int sat_shown = sat_n <= sat_room ? sat_n : std::max(0, sat_room - 1);
  char name[20];
  for (int i = 0; i < sat_shown; i++) {
    const SatRec &r = *sat_rows[i].r;
    fmt_pos({sat_rows[i].az, sat_rows[i].el});
    snprintf(alt, sizeof(alt), "%.0f %s", dist(r.alt_km), dist_unit());
    snprintf(name, sizeof(name), "%.15s", r.name);
    add(RK_SAT, card_icon_for(K_SAT, r), name, r.cc, orbit_class(r.cls), dir, el, alt);
  }
  char more[32];
  if (sat_shown < sat_n) {
    snprintf(more, sizeof(more), "+ %d more", sat_n - sat_shown);
    add(RK_NOTE, "", more, "", "", "", "", "");
  }
  if (geo_cap) {
    const int gshow = std::min((int) geo_rows.size(), geo_cap);
    for (int i = 0; i < gshow; i++) {
      const SatRec &r = *geo_rows[i].r;
      fmt_pos({geo_rows[i].az, geo_rows[i].el});
      snprintf(alt, sizeof(alt), "%.0f %s", dist(r.alt_km), dist_unit());
      snprintf(name, sizeof(name), "%.15s", r.name);
      add(RK_SAT, card_icon_for(K_GEO, r), name, r.cc, "GEO", dir, el, alt);
    }
    if ((int) geo_rows.size() > gshow) {
      snprintf(more, sizeof(more), "+ %d GEO", (int) geo_rows.size() - gshow);
      add(RK_NOTE, "", more, "", "", "", "", "");
    }
  }
  for (int i = 0; i < sl_shown; i++) {
    const SatRec &r = *sl_rows[i].r;
    fmt_pos({sl_rows[i].az, sl_rows[i].el});
    snprintf(alt, sizeof(alt), "%.0f %s", dist(r.alt_km), dist_unit());
    snprintf(name, sizeof(name), "%.15s", r.name);
    add(RK_STARLINK, card_icon_for(K_STARLINK, r), name, r.cc, "LEO", dir, el, alt);
  }
  if (sl_more > 0) {
    snprintf(more, sizeof(more), "+ %d Starlink", sl_more);
    add(RK_NOTE, "", more, "", "", "", "", "");
  }
  lv_table_set_row_count(tb, rows.size());
  for (size_t r = 0; r < rows.size(); r++)
    for (int c = 0; c < 7; c++) {
      const char *cur = lv_table_get_cell_value(tb, r, c);
      if (cur == nullptr || strcmp(cur, rows[r].c[c]) != 0)
        lv_table_set_cell_value(tb, r, c, rows[r].c[c]);
    }
  if (kinds != ui.row_kind) {
    ui.row_kind = kinds;
    lv_obj_invalidate(tb);
  }
}

// ================================================================== UI-26 motion trails
// Where each satellite (and the ISS) was 10, 20 and 30 s ago, drawn under the markers
// by the sky disc's draw callback as three segments fading into the sky. Starlink
// has none: hundreds of short streaks would only be noise. Each tick invalidates the
// old and new trail areas, a few small rectangles per satellite.
constexpr double TRAIL_STEP_S = 10;

inline void invalidate_disc_area(const lv_area_t &a) {
  if (a.x2 < a.x1)
    return;
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_area_t abs_a = {box.x1 + a.x1, box.y1 + a.y1, box.x1 + a.x2, box.y1 + a.y2};
  lv_obj_invalidate_area(ui.w.sky, &abs_a);
}

inline void trail_update(Marker &m, double t) {
  const lv_area_t old = m.tr_area;
  lv_area_t nw{0, 0, -1, -1};
  if (ui.trails_on && m.id >= 0 && m.up && m.rec.has_vel) {
    m.tx[0] = (int16_t) m.dx;
    m.ty[0] = (int16_t) m.dy;
    int x0 = m.dx, x1 = m.dx, y0 = m.dy, y1 = m.dy;
    for (int k = 1; k < 4; k++) {
      const geo::AzEl a = geo::azel(ui.obs, geo::sat_ecef(m.rec, t - TRAIL_STEP_S * k));
      if (a.el < 0) {  // rose less than 30 s ago: the trail stops at the horizon
        m.tx[k] = m.tx[k - 1];
        m.ty[k] = m.ty[k - 1];
        continue;
      }
      float x, y;
      project(a, x, y);
      m.tx[k] = (int16_t) lroundf(x + m.offx);
      m.ty[k] = (int16_t) lroundf(y + m.offy);
      x0 = std::min(x0, (int) m.tx[k]);
      x1 = std::max(x1, (int) m.tx[k]);
      y0 = std::min(y0, (int) m.ty[k]);
      y1 = std::max(y1, (int) m.ty[k]);
    }
    nw = {x0 - 3, y0 - 3, x1 + 3, y1 + 3};
  }
  const bool changed = old.x1 != nw.x1 || old.y1 != nw.y1 || old.x2 != nw.x2 || old.y2 != nw.y2 ||
                       (nw.x2 >= nw.x1 && (m.tx[1] != m.tx[0] || m.ty[1] != m.ty[0]));
  if (changed) {
    invalidate_disc_area(old);
    invalidate_disc_area(nw);
  }
  m.tr_area = nw;
}

inline void update_trails(double t) {
  for (auto &m : ui.sats)
    trail_update(m, t);
  trail_update(ui.iss, t);
}

inline void draw_trail(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy, const Marker &m, uint32_t col, int w) {
  if (m.tr_area.x2 < m.tr_area.x1)
    return;
  if (ox + m.tr_area.x2 < clip.x1 || ox + m.tr_area.x1 > clip.x2 || oy + m.tr_area.y2 < clip.y1 ||
      oy + m.tr_area.y1 > clip.y2)
    return;
  static const uint8_t SHADE[3] = {150, 95, 50};
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.width = w;
  d.round_start = d.round_end = 1;
  d.opa = LV_OPA_COVER;
  for (int k = 2; k >= 0; k--) {  // faintest first
    if (m.tx[k] == m.tx[k + 1] && m.ty[k] == m.ty[k + 1])
      continue;
    d.color = lv_color_hex(mix(col, SKY_BG, SHADE[k]));
    d.p1.x = (lv_value_precise_t) (ox + m.tx[k]);
    d.p1.y = (lv_value_precise_t) (oy + m.ty[k]);
    d.p2.x = (lv_value_precise_t) (ox + m.tx[k + 1]);
    d.p2.y = (lv_value_precise_t) (oy + m.ty[k + 1]);
    lv_draw_line(layer, &d);
  }
}

// ================================================================== UI-24 tap for details
// Tapping near a satellite, a Starlink or the ISS opens a card with its details and
// rings the marker; tapping the ISS also draws its next pass across the sky. A tap on
// empty sky closes the card; with no card open it flips the page as before (UI-2).
constexpr int TAP_R = 22;          // px: how near a tap must land
constexpr double CARD_TIMEOUT_S = 90;

// UI-28: project the GEO list to dots (on fresh GEO data and when the view changes).
inline void geo_project(double t) {
  invalidate_disc_area(ui.geo_area);
  ui.geo_pts.clear();
  lv_area_t a{INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN};
  for (size_t i = 0; i < live.geo.size(); i++) {
    const geo::AzEl e = geo::azel(ui.obs, geo::sat_ecef(live.geo[i], t));
    if (e.el < 0)
      continue;
    float x, y;
    project(e, x, y);
    const GeoPt g{(int16_t) lroundf(x), (int16_t) lroundf(y), (int32_t) i};
    ui.geo_pts.push_back(g);
    a.x1 = std::min<int32_t>(a.x1, g.x - 2);
    a.y1 = std::min<int32_t>(a.y1, g.y - 2);
    a.x2 = std::max<int32_t>(a.x2, g.x + 2);
    a.y2 = std::max<int32_t>(a.y2, g.y + 2);
  }
  ui.geo_area = ui.geo_pts.empty() ? lv_area_t{0, 0, -1, -1} : a;
  invalidate_disc_area(ui.geo_area);
}

inline void draw_trains(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy);
inline void draw_radiant(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy) {  // UI-47
  if (ui.radiant < 0 || !ui.radiant_up)
    return;
  const int cx = ox + ui.radiant_x, cy = oy + ui.radiant_y;
  if (cx + 90 < clip.x1 || cx - 90 > clip.x2 || cy + 12 < clip.y1 || cy - 12 > clip.y2)
    return;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(C_METEOR);
  d.width = 1;
  d.opa = LV_OPA_COVER;
  for (int k = 0; k < 8; k++) {  // a small burst: meteors seem to fly out of this point
    const float a = k * (float) M_PI / 4 + 0.39f, r0 = (k & 1) ? 3.0f : 2.0f, r1 = (k & 1) ? 6.0f : 8.0f;
    d.p1.x = (lv_value_precise_t) (cx + r0 * cosf(a));
    d.p1.y = (lv_value_precise_t) (cy + r0 * sinf(a));
    d.p2.x = (lv_value_precise_t) (cx + r1 * cosf(a));
    d.p2.y = (lv_value_precise_t) (cy + r1 * sinf(a));
    lv_draw_line(layer, &d);
  }
  if (ui.w.label_font) {
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.text = ev::SHOWERS[ui.radiant].name;
    l.font = ui.w.label_font;
    l.color = lv_color_hex(mix(C_METEOR, SKY_BG, 210));
    const bool left = ui.radiant_x > ui.cx;  // keep the name inside the disc
    const lv_area_t a = left ? lv_area_t{cx - 100, cy - 7, cx - 11, cy + 9} : lv_area_t{cx + 11, cy - 7, cx + 100, cy + 9};
    l.align = left ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
    lv_draw_label(layer, &l, &a);
  }
}
inline void draw_comet_tails(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy);
inline void draw_overlays(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy) {
  draw_comet_tails(layer, clip, ox, oy);  // UI-63
  draw_radiant(layer, clip, ox, oy);  // UI-47
  if (ui.scrub_on)
    return;  // UI-53: no satellites (trains, GEO, trails, pass arcs) at another time
  draw_trains(layer, clip, ox, oy);  // UI-42
  if (!ui.geo_pts.empty()) {  // UI-28: under everything else
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    r.bg_color = lv_color_hex(C_GEO_DOT);
    for (const auto &g : ui.geo_pts) {
      const lv_area_t a = {ox + g.x - 1, oy + g.y - 1, ox + g.x + 1, oy + g.y + 1};
      if (a.x2 < clip.x1 || a.x1 > clip.x2 || a.y2 < clip.y1 || a.y1 > clip.y2)
        continue;
      lv_draw_rect(layer, &r, &a);
    }
  }
  if (ui.arc_shown && ui.arc.size() >= 2) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = 2;
    d.round_start = d.round_end = 1;
    d.opa = LV_OPA_COVER;
    d.color = lv_color_hex(mix(ui.arc_color, SKY_BG, 170));
    for (size_t i = 0; i + 1 < ui.arc.size(); i++) {
      d.p1.x = (lv_value_precise_t) (ox + ui.arc[i].x);
      d.p1.y = (lv_value_precise_t) (oy + ui.arc[i].y);
      d.p2.x = (lv_value_precise_t) (ox + ui.arc[i + 1].x);
      d.p2.y = (lv_value_precise_t) (oy + ui.arc[i + 1].y);
      lv_draw_line(layer, &d);
    }
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_opa = LV_OPA_COVER;
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = lv_color_hex(ui.arc_color);
    if (ui.arc_rise_dot) {  // rise end
      const lv_area_t a = {ox + ui.arc[0].x - 3, oy + ui.arc[0].y - 3, ox + ui.arc[0].x + 3, oy + ui.arc[0].y + 3};
      lv_draw_rect(layer, &r, &a);
    }
    for (const auto &k : ui.arc_ticks) {  // UI-36: hour marks
      const lv_area_t a = {ox + k.x - 2, oy + k.y - 2, ox + k.x + 2, oy + k.y + 2};
      lv_draw_rect(layer, &r, &a);
    }
  }
  if (ui.trails_on) {
    for (const auto &m : ui.sats)
      if (m.id >= 0)
        draw_trail(layer, clip, ox, oy, m, m.color, 2);
    if (ui.iss.id >= 0)
      draw_trail(layer, clip, ox, oy, ui.iss, C_ISS, 3);
  }
}

inline Marker *find_marker(int kind, int32_t id) {
  if (kind == K_ISS)
    return ui.iss.id == id ? &ui.iss : nullptr;
  if (kind == K_CSS)  // UI-52
    return ui.css.id == id ? &ui.css : nullptr;
  if (kind == K_GEO) {  // UI-28: refresh the scratch marker from the GEO list
    for (const auto &g : ui.geo_pts)
      if (live.geo[g.idx].id == id) {
        ui.geo_sel.rec = live.geo[g.idx];
        ui.geo_sel.id = id;
        ui.geo_sel.dx = g.x;
        ui.geo_sel.dy = g.y;
        ui.geo_sel.up = true;
        return &ui.geo_sel;
      }
    return nullptr;
  }
  auto &slots = kind == K_SAT ? ui.sat_slot : ui.starlink_slot;
  auto &pool = kind == K_SAT ? ui.sats : ui.starlink;
  auto it = slots.find(id);
  return it == slots.end() ? nullptr : &pool[it->second];
}

// ================================================================== UI-36 Sun / Moon
  // UI selections beyond the net's Kind values (K_PLANET: sel_id = planet)

// HW-9a: magnetic declination (and UI-37 geomagnetic latitude) for the observer, WMM2025.
inline double decimal_year(double t) { return 1970.0 + t / (365.2425 * 86400.0); }
inline void update_declination(double t) {
  const Config &c = config();
  const double yr = t > 1.7e9 ? decimal_year(t) : 2026.5;
  ui.decl = (float) wmm::compute(c.lat, c.lon, c.alt_m / 1000.0, yr).decl;
  ui.maglat = (float) wmm::dipole_lat(c.lat, c.lon, yr);
  // UI-58a: the Kp at which the aurora outlook reaches "low in north" here (69.5 - 2 Kp = maglat + 2);
  // the forecast already looks 3 h ahead and a wind warning covers sudden rises
  net::kp_gate = std::max(0.0f, (67.5f - ui.maglat) / 2.0f);
  ui.decl_at = t;
  ESP_LOGI(UI_TAG, "declination %.2f° (WMM2025), geomagnetic latitude %.1f°", ui.decl, ui.maglat);
}
inline float declination() { return ui.decl; }

// Local calendar parts (UTC in host tests).
inline void local_parts(double t, int &mo, int &d, int &h, int &mi, int &se) {
#ifdef SAT_HOST_TEST
  time_t tt = (time_t) (t + host_tz_s);
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  mo = tmv.tm_mon + 1;
  d = tmv.tm_mday;
  h = tmv.tm_hour;
  mi = tmv.tm_min;
  se = tmv.tm_sec;
#else
  esphome::ESPTime lt = esphome::ESPTime::from_epoch_local((time_t) t);
  mo = lt.month;
  d = lt.day_of_month;
  h = lt.hour;
  mi = lt.minute;
  se = lt.second;
#endif
}
inline double local_midnight(double t) {
  int mo, d, h, mi, se;
  local_parts(t, mo, d, h, mi, se);
  return floor(t) - (h * 3600 + mi * 60 + se);
}
inline void local_md(double t, char *buf, size_t n) {
  static const char *const M[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int mo, d, h, mi, se;
  local_parts(t, mo, d, h, mi, se);
  snprintf(buf, n, "%s %d", M[(mo + 11) % 12], d);
}

// ================================================================== UI-40 planets
// Positions every PLANET_PERIOD (they move far less than a pixel a minute) and when the
// view changes. A planet is drawn while above the horizon; its name tag follows the
// UI-43 rule (none below LOW_EL). "Visible" (UI-41 alert): at least 10° up, magnitude
// 1.0 or brighter, and the sky dark enough for it (Sun below -6°, or -3° for the very
// bright ones: Venus, Jupiter at magnitude -3 or brighter).
constexpr double PLANET_PERIOD = 30;
constexpr float PLANET_ALERT_EL = 10.0f, PLANET_ALERT_MAG = 1.0f;
inline bool planet_sky_dark(float mag) { return ui.sm.sun.el <= (mag <= -3.0f ? -3.0f : -6.0f); }
inline void update_planets(double t, bool force = false) {
  if (!clock_valid() || ui.planet_img[0] == nullptr || (!force && t < ui.planet_next))
    return;
  ui.planet_next = t + PLANET_PERIOD;
  const Config &c = config();
  constexpr int H = picons::SMALL / 2;
  for (int p = 0; p < planets::N_PLANETS; p++) {
    planets::Pos ps;
    const geo::AzEl a = planets::horizontal(p, t, c.lat, c.lon, &ps);
    geo::AzEl v = a;
    v.az = a.az;  // project() applies the heading
    ui.planet_azel[p] = a;
    ui.planet_mag[p] = ps.mag;
    if (!ui.scrub_on)  // UI-53: alerts are about now
      ui.planet_visible[p] = a.el >= PLANET_ALERT_EL && ps.mag <= PLANET_ALERT_MAG && planet_sky_dark(ps.mag);
    const bool show = ui.planets_on && a.el >= 0;
    set_shown(ui.planet_img[p], ui.planet_shown[p], show);
    const bool tag = show;  // 4.5.6: planets keep their names even low (UI-43 is for satellites)
    set_shown(ui.planet_lbl[p], ui.planet_lbl_shown[p], tag);
    if (!show)
      continue;
    float x, y;
    project(v, x, y, H + 1);
    const int px = (int) lroundf(x) - H, py = (int) lroundf(y) - H;
    if (force || px != ui.planet_x[p] || py != ui.planet_y[p]) {
      ui.planet_x[p] = px;
      ui.planet_y[p] = py;
      lv_obj_set_pos(ui.planet_img[p], px, py);
      if (tag) {
        lv_point_t sz;
        lv_text_get_size(&sz, planets::name(p), ui.w.label_font ? ui.w.label_font : LV_FONT_DEFAULT, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        const int lx = px + H > ui.cx ? px - 3 - sz.x : px + 2 * H + 3;  // inside the disc on the east side
        lv_obj_set_pos(ui.planet_lbl[p], lx, py);
      }
    }
  }
}
// UI-41a: the aurora notice (and its glow) and the planet alerts can be turned off
inline void set_alerts(bool aurora, bool planet) {
  const bool glow = aurora != ui.aurora_alerts;
  ui.aurora_alerts = aurora;
  ui.planet_alerts = planet;
  if (glow && ui.ready && ui.w.sky)
    lv_obj_invalidate(ui.w.sky);
}
inline void set_sky_alerts(bool on) { ui.sky_alerts = on; }  // UI-47..51 (and UI-41b)
inline void set_lunar_alerts(bool on) { ui.lunar_alerts = on; }  // UI-41f
inline void set_comet_alerts(bool on) { ui.comet_alerts = on; }  // UI-41g
inline void set_splash_alerts(bool on) { ui.splash_alerts = on; }  // UI-41h
inline void set_alert_kinds(bool stations, bool launches, bool events) {  // UI-41e
  ui.station_alerts = stations;
  ui.launch_alerts = launches;
  ui.event_alerts = events;
}
inline void deselect();
inline void set_stations(bool on) {  // UI-52b
  if (ui.stations_on == on)
    return;
  ui.stations_on = on;
  if (!ui.ready)
    return;
  const double t = clock_now();
  if (ui.iss.id >= 0)
    draw_marker(ui.iss, nullptr, ISS_PX, t, false);
  if (ui.css.id >= 0)
    draw_marker(ui.css, nullptr, ISS_PX, t, false);
  if (!on && (ui.sel_kind == K_ISS || ui.sel_kind == K_CSS))
    deselect();
  draw_list(t);
}
inline void set_planets(bool on) {
  if (ui.planets_on == on)
    return;
  ui.planets_on = on;
  if (ui.ready)
    update_planets(clock_now(), true);
}

// ================================================================== UI-63 comets
// Every PLANET_PERIOD: each comet in live.comet_list is placed (two-body, sky_comets.h) and
// the MAX_COMETS brightest at magnitude COMET_MAP_MAG or brighter are drawn while up: a
// head, a name tag and a tail pointing straight away from the Sun as seen from here, longer
// for a brighter comet and foreshortened when it points towards or away from us. Alert
// (Sky Event Alerts): 10° up, magnitude 6 or brighter, and the sky properly dark.
constexpr float COMET_MAP_MAG = 6.5f, COMET_ALERT_MAG = 6.0f, COMET_ALERT_EL = 10.0f, COMET_DARK_SUN = -9.0f;
inline void deselect();
inline void comet_inval(int k) {
  invalidate_disc_area(ui.comet_area[k]);
  ui.comet_area[k] = {0, 0, -1, -1};
}
inline void update_comets(double t, bool force = false) {
  if (!clock_valid() || ui.comet_dot[0] == nullptr || (!force && t < ui.comet_next))
    return;
  ui.comet_next = t + PLANET_PERIOD;
  if (ui.comets_at != live.comets_at) {  // a new list: indices changed
    ui.comets_at = live.comets_at;
    if (ui.sel_kind == K_COMET)
      deselect();
  }
  struct Best {
    int idx;
    comets::Pos p;
  };
  Best best[Ui::MAX_COMETS];
  int nb = 0;
  const double j = astro::jd(t);
  if (ui.comets_on)
    for (int i = 0; i < (int) live.comet_list.size(); i++) {
      const comets::Pos p = comets::position(live.comet_list[i], j);
      if (!p.ok || !(p.mag <= COMET_MAP_MAG))
        continue;
      int at = nb < Ui::MAX_COMETS ? nb++ : Ui::MAX_COMETS;
      if (at == Ui::MAX_COMETS) {
        if (p.mag >= best[Ui::MAX_COMETS - 1].p.mag)
          continue;
        at = Ui::MAX_COMETS - 1;
      }
      for (; at > 0 && best[at - 1].p.mag > p.mag; at--)
        best[at] = best[at - 1];
      best[at] = {i, p};
    }
  const Config &c = config();
  const double lst = astro::rad(astro::wrap360(astro::gmst_deg(j) + c.lon));
  for (int k = 0; k < Ui::MAX_COMETS; k++) {
    if (k >= nb) {
      ui.comet_idx[k] = -1;
      ui.comet_visible[k] = false;
      set_shown(ui.comet_dot[k], ui.comet_shown[k], false);
      set_shown(ui.comet_lbl[k], ui.comet_lbl_shown[k], false);
      comet_inval(k);
      continue;
    }
    const comets::Pos &p = best[k].p;
    const geo::AzEl a = astro::horizontal(p.equ, c.lat, lst), at = astro::horizontal(p.tail, c.lat, lst);
    ui.comet_idx[k] = best[k].idx;
    ui.comet_azel[k] = a;
    ui.comet_mag[k] = p.mag;
    if (!ui.scrub_on)  // UI-53: alerts are about now
      ui.comet_visible[k] = a.el >= COMET_ALERT_EL && p.mag <= COMET_ALERT_MAG && ui.sm.sun.el <= COMET_DARK_SUN;
    const bool show = a.el >= 0;
    set_shown(ui.comet_dot[k], ui.comet_shown[k], show);
    set_shown(ui.comet_lbl[k], ui.comet_lbl_shown[k], show);
    if (!show) {
      comet_inval(k);
      continue;
    }
    float x, y, tx, ty;
    project(a, x, y, COMET_PX / 2 + 1);
    project(at, tx, ty);
    // the tail on the map: up to 12..44 px by brightness, foreshortened by how far the
    // 0.1 AU tail point appears from the head compared with side-on
    const double sep = acos(std::max(-1.0, std::min(1.0, sin(p.equ.dec) * sin(p.tail.dec) +
                                                          cos(p.equ.dec) * cos(p.tail.dec) * cos(p.equ.ra - p.tail.ra))));
    const float side = (float) atan2(0.1, p.d);
    const float frac = std::max(0.2f, std::min(1.0f, (float) sep / side));
    const float len = std::max(12.0f, std::min(44.0f, 12.0f + (COMET_MAP_MAG - p.mag) * 6.0f)) * frac;
    float vx = tx - x, vy = ty - y;
    const float vl = sqrtf(vx * vx + vy * vy);
    if (vl > 0.01f) {
      vx *= len / vl;
      vy *= len / vl;
    } else {
      vx = vy = 0;
    }
    const int hx = (int) lroundf(x), hy = (int) lroundf(y);
    const int ex = (int) lroundf(x + vx), ey = (int) lroundf(y + vy);
    const lv_area_t nw = {std::min(hx, ex) - 4, std::min(hy, ey) - 4, std::max(hx, ex) + 4, std::max(hy, ey) + 4};
    if (force || hx != ui.comet_x[k] || hy != ui.comet_y[k] || ex != ui.comet_tx[k] || ey != ui.comet_ty[k] ||
        ui.comet_area[k].x2 < ui.comet_area[k].x1) {
      comet_inval(k);
      ui.comet_x[k] = hx;
      ui.comet_y[k] = hy;
      ui.comet_tx[k] = ex;
      ui.comet_ty[k] = ey;
      ui.comet_area[k] = nw;
      invalidate_disc_area(nw);
      lv_obj_set_pos(ui.comet_dot[k], hx - COMET_PX / 2, hy - COMET_PX / 2);
      const char *tag = live.comet_list[best[k].idx].tag;
      set_text_if(ui.comet_lbl[k], tag);
      lv_point_t sz;
      lv_text_get_size(&sz, tag, ui.w.label_font ? ui.w.label_font : LV_FONT_DEFAULT, 0, 0, LV_COORD_MAX,
                       LV_TEXT_FLAG_NONE);
      // the tag on the side away from the tail, inside the disc
      const bool left = vx > 0.5f ? true : vx < -0.5f ? false : hx > ui.cx;
      const int lx = left ? hx - COMET_PX / 2 - 4 - sz.x : hx + COMET_PX / 2 + 4;
      lv_obj_set_pos(ui.comet_lbl[k], lx, hy - sz.y / 2);
    }
  }
}
// the tails, under every marker (called from draw_overlays)
inline void draw_comet_tails(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.color = lv_color_hex(C_COMET);
  d.round_start = d.round_end = 1;
  for (int k = 0; k < Ui::MAX_COMETS; k++) {
    if (ui.comet_idx[k] < 0 || !ui.comet_shown[k])
      continue;
    const lv_area_t &a = ui.comet_area[k];
    if (a.x2 < a.x1 || ox + a.x2 < clip.x1 || ox + a.x1 > clip.x2 || oy + a.y2 < clip.y1 || oy + a.y1 > clip.y2)
      continue;
    const float hx = ui.comet_x[k], hy = ui.comet_y[k], ex = ui.comet_tx[k], ey = ui.comet_ty[k];
    constexpr int SEG = 5;  // fading, thinning towards the end
    for (int i = 0; i < SEG; i++) {
      const float f0 = (float) i / SEG, f1 = (float) (i + 1) / SEG;
      d.width = i < 1 ? 4 : i < 3 ? 3 : 2;
      d.opa = (lv_opa_t) (220 * (1.0f - f0) * (1.0f - f0 * 0.4f));
      d.p1.x = (lv_value_precise_t) (ox + hx + (ex - hx) * f0);
      d.p1.y = (lv_value_precise_t) (oy + hy + (ey - hy) * f0);
      d.p2.x = (lv_value_precise_t) (ox + hx + (ex - hx) * f1);
      d.p2.y = (lv_value_precise_t) (oy + hy + (ey - hy) * f1);
      lv_draw_line(layer, &d);
    }
  }
}
inline void set_comets(bool on) {
  if (ui.comets_on == on)
    return;
  ui.comets_on = on;
  if (ui.ready)
    update_comets(clock_now(), true);
}
inline void set_milky_way(bool on) {  // UI-64
  if (ui.mw_on == on)
    return;
  ui.mw_on = on;
  ui.star_next = 0;
  if (ui.ready && ui.w.sky)
    lv_obj_invalidate(ui.w.sky);
}

// ================================================================== UI-41 alignments
// Looks ahead ALIGN_DAYS (60) for (a) conjunctions: two planets within 2° of each other and
// (b) planet parades: 4 or more planets up at once, both while the sky is dark (Sun
// below -6°) and the planets are at least 5° up and magnitude 1.5 or brighter. Every
// half hour of each day is sampled; one day is scanned per tick, a full scan hourly.
constexpr int ALIGN_DAYS = 60;
constexpr float CONJ_DEG = 2.0f, ALIGN_EL = 5.0f, ALIGN_MAG = 1.5f;
constexpr int PARADE_MIN = 4;
struct AlignEv {
  bool valid = false;
  double t = 0;        // best moment (closest approach, or first parade)
  int a = -1, b = -1;  // conjunction pair
  float sep = 99;
  int count = 0;       // parade size
};
struct AlignScan {
  bool running = false;
  double t0 = 0, next = 0;
  int day = 0;
  AlignEv c, p;
};
inline AlignScan align_scan;
inline AlignEv conj_ev, parade_ev;
inline void align_step(double t) {
  if (!clock_valid())
    return;
  AlignScan &S = align_scan;
  if (!S.running) {
    if (t < S.next)
      return;
    S = AlignScan{};
    S.running = true;
    S.t0 = floor(t / 1800) * 1800;
  }
  const Config &c = config();
  const double lst0 = 0;
  (void) lst0;
  for (int k = 0; k < 48; k++) {
    const double ts = S.t0 + S.day * 86400.0 + k * 1800.0;
    const double j = astro::jd(ts);
    const double lst = astro::rad(astro::wrap360(astro::gmst_deg(j) + c.lon));
    if (astro::horizontal(astro::sun(j), c.lat, lst).el > DUSK_SUN_EL)
      continue;
    geo::AzEl az[planets::N_PLANETS];
    bool vis[planets::N_PLANETS];
    int n = 0;
    for (int p = 0; p < planets::N_PLANETS; p++) {
      const planets::Pos ps = planets::position(p, j);
      az[p] = astro::horizontal(ps.equ, c.lat, lst);
      vis[p] = az[p].el >= ALIGN_EL && ps.mag <= ALIGN_MAG;
      n += vis[p];
    }
    if (n >= PARADE_MIN && (!S.p.valid || (n > S.p.count && ts - S.p.t < 3 * 86400.0))) {
      S.p.valid = true;
      S.p.t = ts;
      S.p.count = n;
    }
    for (int a = 0; a < planets::N_PLANETS; a++)
      for (int b2 = a + 1; b2 < planets::N_PLANETS; b2++) {
        if (!vis[a] || !vis[b2])
          continue;
        const float sep = geo::separation(az[a], az[b2]);
        if (sep > CONJ_DEG)
          continue;
        const bool same = S.c.valid && S.c.a == a && S.c.b == b2;
        if (!S.c.valid || (same && sep < S.c.sep && ts - S.c.t < 10 * 86400.0)) {
          S.c.valid = true;
          S.c.t = ts;
          S.c.a = a;
          S.c.b = b2;
          S.c.sep = sep;
        }
      }
  }
  if (++S.day >= ALIGN_DAYS) {
    conj_ev = S.c;
    parade_ev = S.p;
    S.running = false;
    S.next = t + 3600;
    if (conj_ev.valid)
      ESP_LOGI(UI_TAG, "alignment: %s & %s %.1f° apart at %.0f", planets::name(conj_ev.a), planets::name(conj_ev.b),
               conj_ev.sep, conj_ev.t);
    if (parade_ev.valid)
      ESP_LOGI(UI_TAG, "alignment: %d-planet parade at %.0f", parade_ev.count, parade_ev.t);
  }
}
// "tonight", "this morning", "Nov 15 morning"
inline void when_text(double t_now, double t_ev, char *buf, size_t n) {
  int mo, d, h, mi, se, mo2, d2, h2;
  local_parts(t_ev, mo, d, h, mi, se);
  local_parts(t_now, mo2, d2, h2, mi, se);
  const bool morning = h < 12;
  const double hours = (t_ev - t_now) / 3600.0;
  if (hours < 18 && hours > -6) {
    snprintf(buf, n, "%s", morning ? (h2 < 12 ? "this morning" : "tomorrow morning") : "tonight");
    return;
  }
  char md[16];
  local_md(t_ev, md, sizeof(md));
  snprintf(buf, n, "%s %s", md, morning ? "morning" : "evening");
}

// ================================================================== UI-41 alerts
constexpr double ISS_ALERT_S = 30 * 60.0;  // a visible ISS pass is announced this long ahead
constexpr double ALIGN_ALERT_S = 3 * 86400.0;  // alignments are announced at most 3 days ahead (4.5.6)
// UI-41b: the next solstice or equinox (the Sun's apparent ecliptic longitude at a
// multiple of 90°). Low-precision Sun (0.01°, about 15 min), refined by Newton steps;
// the date is what is shown. kind: 0 March equinox, 1 June solstice, 2 September
// equinox, 3 December solstice.
struct SeasonEv {
  double t = 0;
  int kind = 0;
};
inline SeasonEv next_season(double t) {
  auto lon = [](double tt) { return astro::wrap360(astro::sun(astro::jd(tt)).lon_ecl); };
  const double l0 = lon(t);
  int k = (int) floor(l0 / 90.0) + 1;
  double target = k * 90.0, te = t + (target - l0) / 0.98561 * 86400.0;
  for (int i = 0; i < 4; i++) {
    double d = lon(te) - astro::wrap360(target);
    d -= 360.0 * floor((d + 180.0) / 360.0);  // -180..180
    te -= d / 0.98561 * 86400.0;
  }
  return {te, k % 4};
}
// ================================================================== UI-47..51 sky events
struct SkyEvents {
  // UI-51 eclipses seen from here, soonest first; found one syzygy per tick
  std::vector<ev::Eclipse> ecl, ecl_new;
  double scan_t = 0, scan_end = 0, scan_lat = 1e9, scan_lon = 1e9, scan_started = 0;
  bool scan_done = false;
  // UI-48 the next full Moon
  double full_t = 0, full_next_at = 0;
  const char *full_name = "Full Moon";
  bool full_super = false;
  // UI-49 the Moon near a planet or bright star, in the next 18 h of dark sky
  double conj_next_at = 0, conj_t = 0;
  float conj_sep = 0;
  const char *conj_with = nullptr;
  // UI-47 the next (or current) peak of each shower
  double shower_next_at = 0;
  double peak[ev::N_SHOWERS] = {};
};
inline SkyEvents sev;
constexpr double ECL_LOOK_S = 3 * 365.25 * 86400.0;  // eclipses: three years ahead
constexpr double SKY_ALERT_S = 3 * 86400.0;          // sky events: announced at most 3 days ahead
constexpr float CONJ_MAX_DEG = 5.0f;                 // UI-49: "near"
constexpr int CONJ_STARS[5] = {8, 13, 9, 10, 11};    // ev::BRIGHT: Aldebaran, Regulus, Spica, Antares, Pollux

inline void eclipse_step(double t) {  // UI-51
  const Config &c = config();
  if (c.lat != sev.scan_lat || c.lon != sev.scan_lon || t - sev.scan_started > 86400) {
    sev.scan_lat = c.lat;
    sev.scan_lon = c.lon;
    sev.scan_started = t;
    sev.scan_t = t - 12 * 3600.0;
    sev.scan_end = t + ECL_LOOK_S;
    sev.scan_done = false;
    sev.ecl_new.clear();
  }
  if (sev.scan_done)
    return;
  const double tn = astro::moon_phase_time(sev.scan_t, 0, 1), tf = astro::moon_phase_time(sev.scan_t, 180, 1);
  const double ts = (tn > 0 && (tf <= 0 || tn < tf)) ? tn : tf;
  if (ts <= 0 || ts > sev.scan_end || sev.ecl_new.size() >= 3) {
    std::swap(sev.ecl, sev.ecl_new);
    sev.scan_done = true;
    ESP_LOGI(UI_TAG, "eclipses: %u seen from here in the next 3 years", (unsigned) sev.ecl.size());
    return;
  }
  sev.scan_t = ts + 86400.0;
  if (!ev::eclipse_possible(ts))
    return;
  ev::Eclipse e;
  const bool full = ts == tf;
  bool ok = full ? ev::lunar_eclipse(ts, c.lat, c.lon, c.alt_m, e) : ev::solar_eclipse(ts, c.lat, c.lon, c.alt_m, e);
  if (ok && full && (e.t0 == 0 || (e.type == ev::ECL_PENUMBRAL && e.mag < 0.7f)))
    ok = false;  // Moon down, or a penumbral eclipse too slight to notice
  if (ok) {
    sev.ecl_new.push_back(e);
    ESP_LOGI(UI_TAG, "eclipse: %s at %.0f, mag %.2f", ev::eclipse_name(e.type), e.t_max, e.mag);
  }
}
inline const ev::Eclipse *next_eclipse(double t) {
  for (const auto &e : sev.ecl)
    if (e.t1 >= t)
      return &e;
  return nullptr;
}

inline void full_moon_update(double t) {  // UI-48
  if (t < sev.full_next_at)
    return;
  sev.full_next_at = t + 3600;
  const double nf = astro::moon_phase_time(t - 1.5 * 86400.0, 180, 1);
  if (nf <= 0)
    return;
  sev.full_t = nf;
  int mo, d, h, mi, se;
  local_parts(nf, mo, d, h, mi, se);
  const char *name = "Full Moon";
  if (config().lat >= 0) {  // the traditional names are northern
    name = ev::full_moon_name(mo);
    const double syn = 29.530589 * 86400.0, te = ev::sun_lon_time(nf - 60 * 86400.0, 180.0);
    const double prev = astro::moon_phase_time(nf - 2 * 86400.0, 180, -1);
    if (fabs(nf - te) <= syn / 2)
      name = "Harvest Moon";  // the full Moon nearest the September equinox
    else if (prev > 0 && fabs(prev - te) <= syn / 2)
      name = "Hunter's Moon";  // the one after it
    int mo2, d2, h2, mi2, se2;
    if (prev > 0) {
      local_parts(prev, mo2, d2, h2, mi2, se2);
      if (mo2 == mo)
        name = "Blue Moon";  // the second full Moon in a calendar month
    }
  }
  sev.full_name = name;
  sev.full_super = ev::moon_precise(astro::jd(nf)).dist_km < 360000.0;
}

inline void moon_conj_update(double t) {  // UI-49
  if (t < sev.conj_next_at)
    return;
  sev.conj_next_at = t + 600;
  sev.conj_with = nullptr;
  const Config &c = config();
  float best = CONJ_MAX_DEG;
  for (int k = 0; k <= 18; k++) {
    const double th = t + k * 3600.0, j = astro::jd(th);
    const astro::SunMoon sm = astro::compute(th, c.lat, c.lon, c.alt_m);
    if (sm.sun.el > -6.0f || sm.moon.el < 5.0f)
      continue;  // daylight, or the Moon low or down
    const astro::Equ m = ev::moon_topo(th, c.lat, c.lon, c.alt_m);
    const double lst = astro::rad(astro::wrap360(astro::gmst_deg(j) + c.lon));
    auto consider = [&](const astro::Equ &e, const char *name) {
      if (astro::horizontal(e, c.lat, lst).el < 5.0f)
        return;
      const float sp = (float) ev::equ_sep_deg(m, e);
      if (sp < best) {
        best = sp;
        sev.conj_with = name;
        sev.conj_sep = sp;
        sev.conj_t = th;
      }
    };
    for (int p = 0; p < planets::N_PLANETS; p++) {
      const planets::Pos ps = planets::position(p, j);
      if (ps.mag <= 1.5f)
        consider(ps.equ, planets::name(p));
    }
    for (int si : CONJ_STARS) {
      const ev::BrightStar &b = ev::BRIGHT[si];
      astro::Equ e;
      e.ra = astro::rad(b.ra);
      e.dec = astro::rad(b.dec);
      e.lon_ecl = 0;
      consider(e, b.name);
    }
  }
}

inline void shower_update(double t) {  // UI-47
  if (t < sev.shower_next_at)
    return;
  sev.shower_next_at = t + 3600;
  const double T = (astro::jd(t) - 2451545.0) / 36525.0;
  for (int i = 0; i < ev::N_SHOWERS; i++) {
    const ev::Shower &sh = ev::SHOWERS[i];
    const double lam = astro::wrap360(sh.lam + 1.3969713 * T);  // J2000 -> of date
    sev.peak[i] = ev::sun_lon_time(t - (sh.after + 1) * 86400.0, lam);
  }
}
// the shower whose radiant goes on the map: within 3 days before to 1 day after its peak
inline int radiant_shower(double t) {
  int best = -1;
  double bd = 1e18;
  for (int i = 0; i < ev::N_SHOWERS; i++) {
    const double d = sev.peak[i] - t;
    if (d <= SKY_ALERT_S && d >= -86400.0 && fabs(d) < bd) {
      bd = fabs(d);
      best = i;
    }
  }
  return best;
}
inline const char *moon_word(double t) {  // UI-47: how much the Moon will spoil it
  const float k = astro::compute(t, config().lat, config().lon, config().alt_m).moon_illum;
  return k < 0.35f ? "dark sky" : k < 0.7f ? "some moonlight" : "bright Moon";
}
inline void day_word(double t_now, double t_ev, char *buf, size_t n) {  // "today", "tomorrow", "Dec 31"
  const double m_ev = local_midnight(t_ev), m_now = local_midnight(t_now);
  if (m_ev <= m_now)
    snprintf(buf, n, "today");
  else if (m_ev <= m_now + 86400.0 + 3600.0)
    snprintf(buf, n, "tomorrow");
  else
    local_md(t_ev, buf, n);
}
inline void sky_events_step(double t) {
  if (!clock_valid())
    return;
  shower_update(t);
  full_moon_update(t);
  moon_conj_update(t);
  static double ecl_next = 0;  // one syzygy every 2 s at most: the phase searches are the cost
  if (t >= ecl_next || t < ecl_next - 10) {
    ecl_next = t + 2;
    eclipse_step(t);
  }
}

// ================================================================== UI-55 solar wind
// Bz south (negative) with a fast wind couples the solar wind to the magnetosphere:
// aurora often brightens 30-60 min later, ahead of the 3-hourly Kp.
using net::WIND_BZ_SOUTH;
using net::WIND_BZ_STRONG;
using net::WIND_FAST;
// UI-58: NOAA OVATION nowcast at the observer (after dark only; 15 or 75 min, UI-58a)
constexpr float AUR_VIEW_PCT = 10, AUR_HERE_LIKELY_PCT = 30, AUR_VIEW_LIKELY_PCT = 50;
inline bool ovation_fresh(double t) {  // under 2 h old (covers the 75 min quiet spacing), and dark now
  return live.ovation.obs > 0 && t - live.ovation.obs < 2 * 3600 && !std::isnan(live.ovation.here) &&
         ui.sm.sun.el <= net::OVATION_DARK_EL;
}
inline float aurora_chance() { return ovation_fresh(clock_now()) ? live.ovation.here : NAN; }
inline float aurora_chance_view() { return ovation_fresh(clock_now()) ? live.ovation.view : NAN; }
// "3% overhead, 12% low NNE" (just the first part when the view adds nothing)
inline void ovation_text(char *b, size_t n) {
  const net::AuroraChance &o = live.ovation;
  if (!std::isnan(o.view_az) && o.view >= o.here + 3)
    snprintf(b, n, "%.0f%% overhead, %.0f%% low %s", o.here, o.view, compass(o.view_az));
  else
    snprintf(b, n, "%.0f%% overhead", o.here);
}

inline bool wind_fresh(double t) { return live.wind.t > 0 && t - live.wind.t < 1800 && !std::isnan(live.wind.bz); }
inline bool wind_warning(double t) { return net::wind_warns(live.wind, t); }

constexpr double SEASON_ALERT_S = 3 * 86400.0;  // like the alignments: at most 3 days ahead

inline int collect_alerts(double t, Alert *out, int max) {
  int n = 0;
  auto add = [&](uint32_t col, const char *glyph, const lv_image_dsc_t *img) -> Alert * {
    if (n >= max)
      return nullptr;
    Alert *a = &out[n++];
    a->col = col;
    a->glyph = glyph;
    a->img = img;
    a->text[0] = 0;
    a->kind = -1, a->id = 0, a->info = AI_NONE, a->idx = -1, a->flag = nullptr;
    return a;
  };
  // ISS: a visible pass soon or in progress (UI-9's yellow line, promoted)
  if (const Pass *p = next_pass(t); ui.station_alerts && p && p->visible && t >= p->start - ISS_ALERT_S && t <= p->end) {
    if (Alert *a = add(C_ISS, "\xF3\xB1\x8E\x83", nullptr)) {  // space-station
      if (t < p->start) {
        char hm[16];
        local_hm(p->start, hm, sizeof(hm));
        snprintf(a->text, sizeof(a->text), "ISS visible %s - %s to %s, %.0f° up", hm, p->start_dir, p->end_dir, p->max_el);
        a->info = AI_ISS_PASS;
      } else if (ui.iss_valid && ui.iss_azel.el > 0) {
        if (ui.iss.id >= 0 && ui.iss.up)
          a->kind = K_ISS, a->id = ui.iss.id;
        else
          a->info = AI_ISS_PASS;
        snprintf(a->text, sizeof(a->text), "ISS passing now - look %s, %.0f° up", compass(ui.iss_azel.az),
                 ui.iss_azel.el);
      } else {
        n--;  // between rise and first fix
      }
    }
  }
  // UI-38 aurora
  // (UI-38b: not for a faint one, Kp under 3.5, unless the nowcast says likely)
  if (aur.show && ui.aurora_alerts && (aur.kp >= 3.5f || aur.likely))
    if (Alert *a = add(C_AURORA, "\xF3\xB1\xAE\xB9", nullptr)) {  // mdi:aurora
      a->info = AI_AURORA;
      char hm[16];
      local_hm((int64_t) (aur.dark_now ? aur.k1 : aur.k0), hm, sizeof(hm));
      if (aur.dark_now && ovation_fresh(t)) {  // UI-58: say how likely, right now
        char oc[40];
        ovation_text(oc, sizeof(oc));
        snprintf(a->text, sizeof(a->text), "Aurora %s now - %s", aur.likely ? "likely" : "possible", oc);
      } else {
        snprintf(a->text, sizeof(a->text), "Aurora %s %s %s - %s", aur.likely ? "likely" : "possible",
                 aur.dark_now ? "until" : "after", hm, aurora_word(aur.kp));
      }
    }
  // UI-55 solar wind early warning (dark sky, no aurora notice up yet)
  if (ui.aurora_alerts && !aur.show && wind_warning(t) && ui.sm.sun.el < -12.0f)
    if (Alert *a = add(C_AURORA, "\xF3\xB1\xAE\xB9", nullptr))
      a->info = AI_WIND, snprintf(a->text, sizeof(a->text), "Solar wind: Bz %+.0f nT, %.0f km/s - aurora may flare up", live.wind.bz,
               std::isnan(live.wind.speed) ? 0.0f : live.wind.speed);
  // UI-54 launches: the next three within 3 days (a nearby one says which way to look);
  // UI-54c then space events (dockings, undockings, releases, EVAs) within 3 days
  {
    int shown = 0;
    for (int li = 0; li < (int) live.launches.size() && ui.launch_alerts; li++) {
      const auto &l = live.launches[li];
      if (shown >= 3 || !launch_pending(l) || l.net < t - 900 || l.net - t > LAUNCH_AHEAD_S)
        continue;
      float brg;
      const float km = launch_km(l, &brg);
      const bool near = km < LAUNCH_NEAR_KM;
      if (Alert *a = add(C_LAUNCH, "\xF3\xB1\x93\x9E", nullptr)) {  // rocket-launch
        a->info = AI_LAUNCH, a->idx = (int16_t) li;
        a->flag = l.cc[0] ? card_flag_for(l.cc) : nullptr;  // UI-54d: shown on its details card
        char cd[24];
        countdown(l.net - t, cd, sizeof(cd));
        if (near)
          snprintf(a->text, sizeof(a->text), "%s from %s in %s - look %s", l.rocket[0] ? l.rocket : "Launch",
                   short_site(l.where), cd, compass(brg));
        else
          snprintf(a->text, sizeof(a->text), "%s launches in %s - %s", l.rocket[0] ? l.rocket : "Rocket", cd,
                   short_site(l.where));
        shown++;
      }
    }
    shown = 0;
    for (int ei = 0; ei < (int) live.events.size(); ei++) {
      const auto &e = live.events[ei];
      const bool splash = is_splashdown(e);  // UI-41h: splashdowns have their own switch
      if (splash ? !ui.splash_alerts : !ui.event_alerts)
        continue;
      if (shown >= 2 || !e.exact || e.t < t - 600 || e.t - t > LAUNCH_AHEAD_S)
        continue;
      if (Alert *a = add(C_EVENT, splash ? "\xF3\xB0\xB2\xB4" : "\xF3\xB1\x8E\x83", nullptr)) {  // parachute / space-station
        a->info = AI_EVENT, a->idx = (int16_t) ei;
        a->flag = e.cc[0] ? card_flag_for(e.cc) : nullptr;  // UI-54d: shown on its details card
        char cd[24];
        if (e.t > t) {
          countdown(e.t - t, cd, sizeof(cd));
          snprintf(a->text, sizeof(a->text), "%s in %s", e.name, cd);
        } else {
          snprintf(a->text, sizeof(a->text), "%s now", e.name);
        }
        shown++;
      }
    }
  }
  // UI-40 planets that can be seen now (with the layer on)
  if (ui.planets_on && ui.planet_alerts)
    for (int p = 0; p < planets::N_PLANETS; p++)
      if (ui.planet_visible[p])
        if (Alert *a = add(C_PLANET[p], nullptr, planet_dsc(p, true)))
          a->kind = K_PLANET, a->id = p, snprintf(a->text, sizeof(a->text), "%s visible - %s, %.0f° up", planets::name(p),
                   compass(ui.planet_azel[p].az), ui.planet_azel[p].el);
  // UI-63 a comet bright enough to see (Sky Event Alerts)
  if (ui.comets_on && ui.comet_alerts)  // UI-41g (was Sky events)
    for (int k = 0; k < Ui::MAX_COMETS; k++)
      if (ui.comet_idx[k] >= 0 && ui.comet_visible[k])
        if (Alert *a = add(C_COMET, ICON_COMET, nullptr))
          a->kind = K_COMET, a->id = ui.comet_idx[k], snprintf(a->text, sizeof(a->text), "Comet %s visible - mag %.1f, %s %.0f° up",
                   live.comet_list[ui.comet_idx[k]].tag, ui.comet_mag[k], compass(ui.comet_azel[k].az),
                   ui.comet_azel[k].el);
  // upcoming alignments
  if (ui.planets_on && ui.planet_alerts) {
    char w[32];
    if (conj_ev.valid && conj_ev.t > t - 3 * 3600.0 && conj_ev.t - t <= ALIGN_ALERT_S)
      if (Alert *a = add(C_ALIGN, nullptr, align_dsc())) {
        a->info = AI_CONJ;
        when_text(t, conj_ev.t, w, sizeof(w));
        snprintf(a->text, sizeof(a->text), "%s & %s %.1f° apart - %s", planets::name(conj_ev.a),
                 planets::name(conj_ev.b), conj_ev.sep, w);
      }
    if (parade_ev.valid && parade_ev.t > t - 3 * 3600.0 && parade_ev.t - t <= ALIGN_ALERT_S)
      if (Alert *a = add(C_ALIGN, nullptr, align_dsc())) {
        a->info = AI_PARADE;
        when_text(t, parade_ev.t, w, sizeof(w));
        snprintf(a->text, sizeof(a->text), "Planet parade - %d planets %s", parade_ev.count, w);
      }
  }
  // UI-52 Tiangong: a visible pass soon or in progress, like the ISS
  if (ui.css.id >= 0 && ui.station_alerts)
    for (int pi = 0; pi < (int) live.css_passes.size(); pi++) {
      const auto &p = live.css_passes[pi];
      if (p.end <= t)
        continue;
      if (p.visible && t >= p.start - ISS_ALERT_S)
        if (Alert *a = add(C_CSS, "\xF3\xB1\x8E\x83", nullptr)) {
          if (t >= p.start && ui.css.id >= 0 && ui.css.up)
            a->kind = K_CSS, a->id = ui.css.id;
          else
            a->info = AI_CSS_PASS, a->idx = (int16_t) pi;
          char hm[16];
          local_hm(p.start, hm, sizeof(hm));
          if (t < p.start)
            snprintf(a->text, sizeof(a->text), "Tiangong visible %s - %s to %s, %.0f° up", hm, p.start_dir, p.end_dir,
                     p.max_el);
          else
            snprintf(a->text, sizeof(a->text), "Tiangong passing now - peak %.0f° %s", p.max_el, p.max_dir);
        }
      break;
    }
  if (ui.sky_alerts || ui.lunar_alerts) {  // UI-41f: the Moon's own alerts follow Lunar
    char w[32], h0[16], h1[16];
    // UI-51 eclipses seen from here (lunar ones follow Lunar, solar ones Sky events)
    if (const ev::Eclipse *e = next_eclipse(t);
        e && e->t0 - t <= SKY_ALERT_S && (ev::is_lunar(e->type) ? ui.lunar_alerts : ui.sky_alerts))
      if (Alert *a = add(C_ECLIPSE, ev::is_lunar(e->type) ? "\xF3\xB0\xBD\xA2" : "\xF3\xB0\x96\x99", nullptr)) {
        a->info = AI_ECLIPSE;
        const bool tot = e->c1 > e->c0 && e->c0 > 0;
        const double s0 = tot ? std::max(e->c0, e->t0) : e->t0, s1 = tot ? std::min(e->c1, e->t1) : e->t1;
        local_hm((int64_t) s0, h0, sizeof(h0));
        local_hm((int64_t) s1, h1, sizeof(h1));
        if (t >= e->t0)
          snprintf(a->text, sizeof(a->text), "%s now - until %s", ev::eclipse_name(e->type), h1);
        else {
          day_word(t, e->t0, w, sizeof(w));
          if (ev::is_lunar(e->type))
            snprintf(a->text, sizeof(a->text), "%s %s %s-%s", ev::eclipse_name(e->type), w, h0, h1);
          else
            snprintf(a->text, sizeof(a->text), "%s %s %s, %.0f%%", ev::eclipse_name(e->type), w, h0, e->mag * 100.0f);
        }
      }
    // UI-47 meteor showers: from 3 days before the peak to the end of the peak's day
    for (int i = 0; i < ev::N_SHOWERS && ui.sky_alerts; i++) {
      const double pk = sev.peak[i];
      if (pk - t > SKY_ALERT_S || t >= local_midnight(pk) + 86400.0)
        continue;
      if (Alert *a = add(C_METEOR, "\xF3\xB1\x9D\x81", nullptr)) {  // star-shooting
        a->kind = K_SHOWER, a->id = i;
        when_text(t, pk, w, sizeof(w));
        snprintf(a->text, sizeof(a->text), "%s peak %s - up to %d/hr, %s", ev::SHOWERS[i].name, w, ev::SHOWERS[i].zhr,
                 moon_word(pk));
      }
    }
    // UI-48 the full Moon, on its day and the day before
    if (ui.lunar_alerts && sev.full_t > 0 && sev.full_t - t <= 1.5 * 86400.0 && t < local_midnight(sev.full_t) + 86400.0)
      if (Alert *a = add(C_MOON, "\xF3\xB0\xBD\xA2", nullptr)) {  // moon-full
        a->kind = K_MOON;
        day_word(t, sev.full_t, w, sizeof(w));
        local_hm((int64_t) sev.full_t, h0, sizeof(h0));
        if (!strcmp(w, "today"))
          snprintf(a->text, sizeof(a->text), "%s tonight - full at %s%s", sev.full_name, h0,
                   sev.full_super ? ", supermoon" : "");
        else
          snprintf(a->text, sizeof(a->text), "%s %s%s", sev.full_name, w, sev.full_super ? " - supermoon" : "");
      }
    // UI-49 the Moon near a planet or bright star
    if (ui.lunar_alerts && sev.conj_with)
      if (Alert *a = add(C_MOON, "\xF3\xB0\xBD\xA2", nullptr)) {
        a->kind = K_MOON;
        if (sev.conj_t - t < 1800)
          snprintf(a->text, sizeof(a->text), "Moon %.0f° from %s now", sev.conj_sep, sev.conj_with);
        else {
          when_text(t, sev.conj_t, w, sizeof(w));
          snprintf(a->text, sizeof(a->text), "Moon near %s %s - %.0f° apart", sev.conj_with, w, sev.conj_sep);
        }
      }
  }
  // UI-41b solstices and equinoxes (from 3 days before until the end of that day)
  if (ui.sky_alerts) {
    static double next_at = 0;
    static SeasonEv ev;
    if (t >= next_at || fabs(ev.t - t) > 400 * 86400.0) {
      ev = next_season(t - 86400.0);  // keeps today's event through the day
      next_at = t + 3600;
    }
    const double m_ev = local_midnight(ev.t);
    if (ev.t - t <= SEASON_ALERT_S && t < m_ev + 86400.0)
      if (Alert *a = add(C_SUN, "\xF3\xB0\x96\x99", nullptr)) {  // weather-sunny
        a->info = AI_SEASON;
        const bool south = config().lat < 0;
        static const char *const NAME[4] = {"Spring equinox", "Summer solstice", "Autumn equinox", "Winter solstice"};
        const int named = south ? (ev.kind + 2) % 4 : ev.kind;  // seasons swap south of the equator
        const char *what = ev.kind % 2 == 0 ? "day and night nearly equal"
                           : named == 1   ? "longest day of the year"
                                          : "shortest day of the year";
        const double m_now = local_midnight(t);
        char when[16];
        if (m_ev <= m_now)
          snprintf(when, sizeof(when), "today");
        else if (m_ev <= m_now + 86400.0 + 3600.0)
          snprintf(when, sizeof(when), "tomorrow");
        else
          local_md(ev.t, when, sizeof(when));
        snprintf(a->text, sizeof(a->text), "%s %s - %s", NAME[named], when, what);
      }
  }
  return n;
}

// ================================================================== UI-42 Starlink trains
// A fresh launch's satellites fly in a line until they spread out to their slots. A
// train is 4 or more Starlink from one launch (UI-42 launch key) in view, each within
// TRAIN_GAP_PX of the next along the line. Found on each Starlink restyle (30 s); the
// line and its "Starlink train" tag follow the markers every tick.
constexpr int TRAIN_MIN = 4, TRAIN_GAP_PX = 40;
constexpr uint32_t C_TRAIN = mix(0x8FA8F0, SKY_BG, 150);
inline void find_trains() {
  ui.n_trains = 0;
  std::unordered_map<uint16_t, std::vector<int>> groups;
  for (int i = 0; i < (int) ui.starlink.size(); i++) {
    const Marker &m = ui.starlink[i];
    if (m.id >= 0 && m.up && m.rec.launch != 0)
      groups[m.rec.launch].push_back(i);
  }
  struct Run {
    std::vector<int> idx;
  };
  std::vector<Run> runs;
  for (auto &g : groups) {
    auto &v = g.second;
    if ((int) v.size() < TRAIN_MIN)
      continue;
    // order along the line: project onto the direction between the two farthest markers
    int fa = v[0], fb = v[0], best = -1;
    for (int a : v)
      for (int b : v) {
        const int dx = ui.starlink[a].dx - ui.starlink[b].dx, dy = ui.starlink[a].dy - ui.starlink[b].dy;
        if (dx * dx + dy * dy > best) {
          best = dx * dx + dy * dy;
          fa = a;
          fb = b;
        }
      }
    const float ux = (float) (ui.starlink[fb].dx - ui.starlink[fa].dx), uy = (float) (ui.starlink[fb].dy - ui.starlink[fa].dy);
    std::sort(v.begin(), v.end(), [&](int a, int b) {
      return ui.starlink[a].dx * ux + ui.starlink[a].dy * uy < ui.starlink[b].dx * ux + ui.starlink[b].dy * uy;
    });
    Run r;
    for (size_t k = 0; k < v.size(); k++) {
      if (!r.idx.empty()) {
        const Marker &p = ui.starlink[r.idx.back()], &q = ui.starlink[v[k]];
        const int dx = p.dx - q.dx, dy = p.dy - q.dy;
        if (dx * dx + dy * dy > TRAIN_GAP_PX * TRAIN_GAP_PX) {
          if ((int) r.idx.size() >= TRAIN_MIN)
            runs.push_back(r);
          r.idx.clear();
        }
      }
      r.idx.push_back(v[k]);
    }
    if ((int) r.idx.size() >= TRAIN_MIN)
      runs.push_back(r);
  }
  std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.idx.size() > b.idx.size(); });
  for (auto &tr : ui.trains)
    tr.idx.clear();
  for (size_t k = 0; k < runs.size() && k < 2; k++)
    ui.trains[ui.n_trains++].idx = runs[k].idx;
}
// Every tick: invalidate where each train's line was and is, and move its tag.
inline void update_trains() {
  for (int k = 0; k < 2; k++) {
    auto &tr = ui.trains[k];
    lv_area_t a{INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN};
    int up = 0;
    for (int i : tr.idx) {
      const Marker &m = ui.starlink[i];
      if (m.id < 0 || !m.up)
        continue;
      up++;
      a.x1 = std::min<int32_t>(a.x1, m.dx - 2);
      a.y1 = std::min<int32_t>(a.y1, m.dy - 2);
      a.x2 = std::max<int32_t>(a.x2, m.dx + 2);
      a.y2 = std::max<int32_t>(a.y2, m.dy + 2);
    }
    if (up < 2)
      a = {0, 0, -1, -1};
    if (a.x1 != tr.area.x1 || a.y1 != tr.area.y1 || a.x2 != tr.area.x2 || a.y2 != tr.area.y2) {
      invalidate_disc_area(tr.area);
      invalidate_disc_area(a);
      tr.area = a;
    }
    const bool show = up >= TRAIN_MIN;
    if (tr.lbl == nullptr && ui.w.sky) {
      tr.lbl = lv_label_create(ui.w.sky);
      lv_obj_remove_style_all(tr.lbl);
      if (ui.w.label_font)
        lv_obj_set_style_text_font(tr.lbl, ui.w.label_font, 0);
      lv_obj_set_style_text_color(tr.lbl, lv_color_hex(mix(0x8FA8F0, SKY_BG, 230)), 0);
      lv_label_set_text(tr.lbl, "Starlink train");
      lv_obj_add_flag(tr.lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (tr.lbl == nullptr)
      continue;
    set_shown(tr.lbl, tr.shown, show);
    if (show) {  // beside the first car, on the side away from the disc edge
      const Marker &m = ui.starlink[tr.idx.front()];
      const int lx = m.dx > ui.cx ? m.dx - 8 - 96 : m.dx + 10, ly = m.dy - 18;
      if (lx != tr.lx || ly != tr.ly) {
        tr.lx = lx;
        tr.ly = ly;
        lv_obj_set_pos(tr.lbl, lx, ly);
      }
    }
  }
}
inline void draw_trains(lv_layer_t *layer, const lv_area_t &clip, int ox, int oy) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.width = 1;
  d.opa = LV_OPA_COVER;
  d.color = lv_color_hex(C_TRAIN);
  for (int k = 0; k < ui.n_trains; k++) {
    const auto &tr = ui.trains[k];
    if (tr.area.x2 < tr.area.x1 || ox + tr.area.x2 < clip.x1 || ox + tr.area.x1 > clip.x2 || oy + tr.area.y2 < clip.y1 ||
        oy + tr.area.y1 > clip.y2)
      continue;
    const Marker *prev = nullptr;
    for (int i : tr.idx) {
      const Marker &m = ui.starlink[i];
      if (m.id < 0 || !m.up)
        continue;
      if (prev) {
        d.p1.x = (lv_value_precise_t) (ox + prev->dx);
        d.p1.y = (lv_value_precise_t) (oy + prev->dy);
        d.p2.x = (lv_value_precise_t) (ox + m.dx);
        d.p2.y = (lv_value_precise_t) (oy + m.dy);
        lv_draw_line(layer, &d);
      }
      prev = &m;
    }
  }
}

// ================================================================== UI-44 / UI-45 display
// UI-44 auto brightness: the set brightness is the daytime level; it is scaled down as
// the Sun sets, to 25 % of it once the Sun is 8° below the horizon.
inline float auto_bright_factor() {
  if (!clock_valid())
    return 1.0f;
  const float f = 0.25f + 0.75f * (ui.sm.sun.el + 8.0f) / 13.0f;
  return std::max(0.25f, std::min(1.0f, f));
}
// UI-45 night mode: after civil dusk (Sun below -6°) everything on screen is drawn in
// red (its brightness only) to keep night vision. Done on the pixels LVGL hands to the
// display, just before each flush, so no widget needs a second colour scheme.
constexpr float NIGHT_SUN_EL = -6.0f;
inline void night_flush_cb(lv_event_t *e) {
  if (!ui.night_active)
    return;
  auto *disp = (lv_display_t *) lv_event_get_target(e);
  const lv_area_t *a = (const lv_area_t *) lv_event_get_param(e);
  lv_draw_buf_t *db = lv_display_get_buf_active(disp);
  if (db == nullptr || a == nullptr || db->header.cf != LV_COLOR_FORMAT_RGB565)
    return;
  const int w = lv_area_get_width(a), h = lv_area_get_height(a);
  const uint32_t stride = db->header.stride ? db->header.stride : (uint32_t) w * 2;
  for (int y = 0; y < h; y++) {
    uint16_t *row = (uint16_t *) (db->data + (size_t) y * stride);
    for (int x = 0; x < w; x++) {
      const uint16_t c = row[x];
      const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      const uint32_t y8 = (r * 627 + g * 604 + b * 117) >> 8;  // luma: (r<<3)*.3 + (g<<2)*.59 + (b<<3)*.11
      row[x] = (uint16_t) (std::min<uint32_t>(31, y8 >> 3) << 11);
    }
  }
}
inline void night_install() {
  static bool done = false;
  lv_display_t *d = lv_display_get_default();
  if (done || d == nullptr)
    return;
  done = true;
  lv_display_add_event_cb(d, night_flush_cb, LV_EVENT_FLUSH_START, nullptr);
}
inline void night_update() {
  const bool act = ui.night_on && clock_valid() && ui.sm.sun.el < NIGHT_SUN_EL;
  if (act == ui.night_active)
    return;
  ui.night_active = act;
  ESP_LOGI(UI_TAG, "night mode %s", act ? "on (red)" : "off");
  lv_obj_invalidate(lv_screen_active());
}
inline void set_night(bool on) {
  ui.night_on = on;
  if (ui.ready)
    night_update();
}

// UI-37: Kp now (latest interval started), the most in the next 24 h, and an outlook
// from the equatorward edge of the auroral oval, about 66.5 - 2 Kp degrees geomagnetic.
inline bool kp_values(double t, float &now, float &max24) {
  now = -1;
  max24 = -1;
  for (const auto &k : live.kp) {
    if (k.t <= t && k.t > t - 3 * 3600)
      now = k.kp;
    if (k.t > t - 3 * 3600 && k.t < t + 24 * 3600)
      max24 = std::max(max24, k.kp);
  }
  if (now < 0)  // stale list: the newest value that is not in the future
    for (const auto &k : live.kp)
      if (k.t <= t)
        now = k.kp;
  return now >= 0;
}
inline float kp_now() {
  float n, m;
  return kp_values(clock_now(), n, m) ? n : NAN;
}
inline const char *aurora_outlook(float kp, bool dark) {
  if (!dark)
    return "sky too bright";
  // Where bright, visible arcs sit (geomagnetic latitude): a few degrees poleward of the
  // oval's faint equatorward edge (~66.5 - 2 Kp). Tuned so a site near 62° (geographic)
  // get "low in north" from about Kp 2.5 and "overhead" from about Kp 4.5.
  const float arc = 69.5f - 2.0f * kp;
  if (arc <= ui.maglat - 2)
    return "overhead likely";
  if (arc <= ui.maglat + 2)
    return "low in north";
  return "unlikely";
}

// UI-38: tonight's dark window (Sun below -12°) and the most Kp during it
inline void aurora_update(double t) {
  if (t < aur.until)
    return;
  const bool was = aur.show, was_dark = aur.dark_now, was_likely = aur.likely;
  aur = AuroraNow();
  aur.until = t + 600;
  if (!clock_valid() || live.kp.empty())
    return;
  const Config &c = config();
  astro::Crossing cr[8];
  const int n = astro::crossings(t - 14 * 3600, t + 24 * 3600, -12, false, c.lat, c.lon, c.alt_m, cr, 8, 900);
  aur.dark_now = astro::compute(t, c.lat, c.lon, c.alt_m).sun.el < -12;
  double k0 = 0, k1 = 0;
  for (int i = 0; i < n; i++) {
    if (!cr[i].rising && (aur.dark_now ? cr[i].t <= t : cr[i].t > t) && (k0 == 0 || aur.dark_now))
      k0 = cr[i].t;  // dark now: the last dusk before now; else the next dusk
    if (cr[i].rising && k0 > 0 && cr[i].t > std::max(k0, t) && k1 == 0)
      k1 = cr[i].t;
  }
  if (aur.dark_now && k0 == 0)
    k0 = t - 3600;
  if (k0 == 0 || k1 == 0 || k0 - t > 12 * 3600)
    return;  // no dark sky tonight (summer), or not until tomorrow
  aur.k0 = k0;
  aur.k1 = k1;
  for (const auto &k : live.kp)
    if (k.t < k1 && k.t + 3 * 3600 > std::max(k0, t))
      aur.kp = std::max(aur.kp, k.kp);
  if (aur.kp < 0)
    return;
  const char *o = aurora_outlook(aur.kp, true);
  aur.likely = strcmp(o, "overhead likely") == 0;
  aur.show = aur.likely || strcmp(o, "low in north") == 0;
  if (aur.dark_now && ovation_fresh(t)) {  // UI-58: the nowcast can raise the notice, never lower it
    const net::AuroraChance &oc = live.ovation;
    const bool view = oc.view >= AUR_VIEW_PCT, likely = oc.here >= AUR_HERE_LIKELY_PCT || oc.view >= AUR_VIEW_LIKELY_PCT;
    aur.nowcast = (view && !aur.show) || (likely && !aur.likely);
    aur.show = aur.show || view || likely;
    aur.likely = aur.likely || likely;
  }
  if (ui.ready && ui.w.sky && (aur.show != was || aur.dark_now != was_dark || aur.likely != was_likely))
    lv_obj_invalidate(ui.w.sky);  // the glow appears, changes or goes
}

// UI-27: orbital data lines for the debug page
inline void orbital_debug(char *b, size_t n, double t) {
  const DataStatus &s = live.status;
  char a[16] = "never", o[16] = "-", r[16];
  if (s.loaded > 0)
    fmt_dur(t - s.loaded, a, sizeof(a));
  if (s.data_time > 0)
    fmt_dur(t - s.data_time, o, sizeof(o));
  int k = snprintf(b, n, "\nORBITAL DATA\n  newest %s%s, oldest group %s%s\n", a, s.loaded > 0 ? " ago" : "", o,
                   s.data_time > 0 ? " ago" : "");
  if (k < (int) n)
    k += snprintf(b + k, n - k, "  %d satellites, %d Starlink, ISS %s\n", s.n_sats, s.n_starlink,
                  s.have_iss ? "yes" : "no");
  if (s.next_try > t && k < (int) n) {
    fmt_dur(s.next_try - t, r, sizeof(r));
    k += snprintf(b + k, n - k, "  next CelesTrak try in %s\n", r);
  }
  if (s.error[0] && k < (int) n)
    k += snprintf(b + k, n - k, "  last error: %.60s\n", s.error);
  float kn, km;
  if (k < (int) n)
    k += snprintf(b + k, n - k, "  Kp %s  decl %.1f\xC2\xB0  geomag lat %.1f\xC2\xB0\n",
                  kp_values(t, kn, km) ? (snprintf(r, sizeof(r), "%.1f", kn), r) : "-", declination(), ui.maglat);
  if (wind_fresh(t) && k < (int) n)  // UI-55
    k += snprintf(b + k, n - k, "  Solar wind %.0f km/s  Bz %+.1f nT  Bt %.1f\n", live.wind.speed, live.wind.bz,
                  live.wind.bt);
  if (ovation_fresh(t) && k < (int) n) {  // UI-58
    char oc[40], ag[16];
    ovation_text(oc, sizeof(oc));
    fmt_dur(t - live.ovation.obs, ag, sizeof(ag));
    k += snprintf(b + k, n - k, "  Aurora %s (%s old)\n", oc, ag);
  }
  if (net::iss_from_fallback && k < (int) n)  // DATA-13
    k += snprintf(b + k, n - k, "  ISS elements from SatNOGS\n");
}

struct BodyInfo {
  // UI-36c: the searched times and distances, kept for 10 min (the searches cost ~1000
  // Sun/Moon positions); the card's text is written from them on every refresh, so
  // units, clock format and the live values are always current.
  int kind = -1;
  double until = 0, lat = 1e9, lon = 1e9;
  double rise = 0, set = 0;  // Sun: today's rise/set; Moon: the same
  // Sun
  int ncross = 0;            // horizon crossings today (0: up or down all day)
  float peak = 0;            // noon elevation
  double noon = 0, day_len = 0, day_delta = 0;
  bool have_delta = false;
  double dawn = 0, dusk = 0, gold0 = 0, gold1 = 0, dark0 = 0, dark1 = 0;
  bool dark = false;         // Sun: gets below -12° in the next 24 h
  // Moon
  double prev_new = 0, full = 0, next_new = 0;
  std::vector<geo::AzEl> path, ticks;  // UI-36: current (or next) pass above the horizon
};
inline BodyInfo body;

inline void body_path(bool moon, double t, const Config &c) {
  body.path.clear();
  body.ticks.clear();
  constexpr double STEP = 600, SPAN = 24 * 3600;
  auto el_at = [&](double tt) {
    const astro::SunMoon r = astro::compute(tt, c.lat, c.lon, c.alt_m);
    return moon ? r.moon : r.sun;
  };
  // walk back to the rise (or 24 h), then forward to the set; if down now, the next pass
  double start = t;
  if (el_at(t).el >= 0) {
    while (start > t - SPAN && el_at(start - STEP).el >= 0)
      start -= STEP;
  } else {
    while (start < t + SPAN && el_at(start).el < 0)
      start += STEP;
    if (start >= t + SPAN)
      return;  // no rise in the next day
  }
  for (double tt = start; tt < start + 1.5 * SPAN; tt += STEP) {
    const geo::AzEl a = el_at(tt);
    if (a.el < 0)
      break;
    body.path.push_back(a);
  }
  // hour marks: whole local hours inside the pass
  const double end = start + STEP * (double) body.path.size();
  const double m0 = local_midnight(start);
  for (double h = m0 + 3600 * ceil((start - m0) / 3600.0); h < end; h += 3600) {
    const geo::AzEl a = el_at(h);
    if (a.el >= 0)
      body.ticks.push_back(a);
  }
}

inline void body_info_update(int kind, double t) {
  const Config &c = config();
  if (body.kind == kind && t < body.until && body.lat == c.lat && body.lon == c.lon)
    return;
  body.kind = kind;
  body.until = t + 600;
  body.lat = c.lat;
  body.lon = c.lon;
  const bool moon = kind == K_MOON;
  const double m0 = local_midnight(t);
  astro::Crossing cr[8];
  // first rise in [t0, t1] and the set that follows it (which may fall after t1);
  // with no rise, the first set in the window
  auto rise_set = [&](double t0, double t1, double h, double &rise, double &set) {
    rise = set = 0;
    const int n = astro::crossings(t0, t1, h, moon, c.lat, c.lon, c.alt_m, cr, 8);
    for (int i = 0; i < n; i++)
      if (cr[i].rising && rise == 0)
        rise = cr[i].t;
    for (int i = 0; i < n; i++)
      if (!cr[i].rising && set == 0 && (rise == 0 || cr[i].t > rise))
        set = cr[i].t;
    if (rise > 0 && set == 0) {
      astro::Crossing c2[4];
      const int n2 = astro::crossings(t1, rise + 86400, h, moon, c.lat, c.lon, c.alt_m, c2, 4);
      for (int i = 0; i < n2 && set == 0; i++)
        if (!c2[i].rising)
          set = c2[i].t;
    }
    return n;
  };
  if (!moon) {
    body.ncross = rise_set(m0, m0 + 86400, -0.27, body.rise, body.set);
    body.noon = astro::sun_peak(m0, m0 + 86400, c.lat, c.lon, c.alt_m, &body.peak);
    body.day_len = 0;
    body.have_delta = false;
    if (body.rise > 0 && body.set > body.rise) {
      body.day_len = body.set - body.rise;
      double r0, s0;
      rise_set(m0 - 86400, m0, -0.27, r0, s0);
      if (r0 > 0 && s0 > r0) {
        body.day_delta = body.day_len - (s0 - r0);
        body.have_delta = true;
      }
    }
    // civil twilight: first rise and last set through -6°
    const int nc = astro::crossings(m0, m0 + 86400, -6, false, c.lat, c.lon, c.alt_m, cr, 8);
    body.dawn = body.dusk = 0;
    for (int i = 0; i < nc; i++) {
      if (cr[i].rising && body.dawn == 0)
        body.dawn = cr[i].t;
      if (!cr[i].rising)
        body.dusk = cr[i].t;
    }
    // golden hour: this evening (Sun from 6° down to the horizon), or tomorrow morning
    double g0 = 0, g1 = 0;
    if (body.set > 0 && t < body.set) {
      const int ng = astro::crossings(body.noon, body.set + 60, 6, false, c.lat, c.lon, c.alt_m, cr, 8);
      for (int i = 0; i < ng; i++)
        if (!cr[i].rising)
          g0 = cr[i].t;
      g1 = body.set;
    } else {
      double r1, s1;
      rise_set(m0 + 86400, m0 + 2 * 86400, -0.27, r1, s1);
      const int ng = astro::crossings(m0 + 86400, m0 + 2 * 86400, 6, false, c.lat, c.lon, c.alt_m, cr, 8);
      for (int i = 0; i < ng; i++)
        if (cr[i].rising && g1 == 0)
          g1 = cr[i].t;
      g0 = r1;
    }
    body.gold0 = g0;
    body.gold1 = g1;
    // dark: Sun below -12° (nautical) from this evening into tomorrow morning
    double k0 = 0, k1 = 0;
    const int nk = astro::crossings(t - 6 * 3600, t + 24 * 3600, -12, false, c.lat, c.lon, c.alt_m, cr, 8);
    const bool dark_now = astro::compute(t, c.lat, c.lon, c.alt_m).sun.el < -12;
    for (int i = 0; i < nk; i++) {
      if (!cr[i].rising && k0 == 0 && (cr[i].t > t || dark_now))
        k0 = cr[i].t;
      if (cr[i].rising && k0 > 0 && k1 == 0 && cr[i].t > k0)
        k1 = cr[i].t;
    }
    if (dark_now && k0 == 0)
      k0 = t;
    body.dark0 = k0;
    body.dark1 = k1;
    body.dark = k0 > 0;
  } else {
    body.prev_new = astro::moon_phase_time(t, 0, -1);
    rise_set(m0, m0 + 86400, -0.27, body.rise, body.set);
    body.full = astro::moon_phase_time(t, 180, 1);
    body.next_new = astro::moon_phase_time(t, 0, 1);
  }
  body_path(moon, t, c);
  // the path moved: redraw it on the next arc_update
  ui.arc.clear();
  ui.arc_shown = false;
}

inline void arc_update(double t) {
  std::vector<lv_point_t> pts;
  std::vector<lv_point_t> ticks;
  // UI-41d an upcoming pass's details card draws that pass's arc too
  const bool info_iss = ui.sel_kind == K_INFO && info_alert.info == AI_ISS_PASS;
  const bool info_css = ui.sel_kind == K_INFO && info_alert.info == AI_CSS_PASS;
  ui.arc_rise_dot = ui.sel_kind == K_ISS || ui.sel_kind == K_CSS || info_iss || info_css;
  ui.arc_color = ui.sel_kind == K_SUN ? C_SUN : ui.sel_kind == K_MOON ? C_MOON : ui.sel_kind == K_CSS || info_css ? C_CSS : C_ISS;
  if (ui.sel_kind == K_ISS || ui.sel_kind == K_CSS || info_iss || info_css) {
    const Pass *p = nullptr;
    if (ui.sel_kind == K_ISS || info_iss)
      p = next_pass(t);
    else
      for (const auto &q : live.css_passes)  // UI-52
        if (q.end > t) {
          p = &q;
          break;
        }
    if (p) {
      for (int i = 0; i < p->npath; i++) {
        float x, y;
        project({p->path[i].az, p->path[i].el}, x, y);
        pts.push_back({(int32_t) lroundf(x), (int32_t) lroundf(y)});
      }
    }
  } else if ((ui.sel_kind == K_SUN || ui.sel_kind == K_MOON) && body.kind == ui.sel_kind) {
    for (const auto &a : body.path) {
      float x, y;
      project(a, x, y);
      pts.push_back({(int32_t) lroundf(x), (int32_t) lroundf(y)});
    }
    for (const auto &a : body.ticks) {
      float x, y;
      project(a, x, y);
      ticks.push_back({(int32_t) lroundf(x), (int32_t) lroundf(y)});
    }
  }
  ui.arc_ticks = ticks;
  const bool shown = pts.size() >= 2;
  bool same = shown == ui.arc_shown && pts.size() == ui.arc.size();
  for (size_t i = 0; same && i < pts.size(); i++)
    same = pts[i].x == ui.arc[i].x && pts[i].y == ui.arc[i].y;
  if (same)
    return;
  auto inval = [&](const std::vector<lv_point_t> &v) {
    if (v.empty())
      return;
    lv_area_t a{v[0].x, v[0].y, v[0].x, v[0].y};
    for (const auto &q : v) {
      a.x1 = std::min(a.x1, q.x);
      a.y1 = std::min(a.y1, q.y);
      a.x2 = std::max(a.x2, q.x);
      a.y2 = std::max(a.y2, q.y);
    }
    a.x1 -= 5;
    a.y1 -= 5;
    a.x2 += 5;
    a.y2 += 5;
    invalidate_disc_area(a);
  };
  if (ui.arc_shown)
    inval(ui.arc);
  ui.arc = pts;
  ui.arc_shown = shown;
  if (shown)
    inval(ui.arc);
}

inline void deselect() {
  ui.sel_kind = -1;
  if (ui.card)
    lv_obj_add_flag(ui.card, LV_OBJ_FLAG_HIDDEN);
  if (ui.sel_ring)
    lv_obj_add_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  arc_update(clock_now());
}



inline void body_card_update(double t);  // UI-36, below
inline void img_request(int kind, double t);  // UI-59/60, below
inline void planet_request(int p, double t);   // UI-61a, below
inline void planet_card_update(double t);  // UI-40a, below
inline void comet_card_update(double t);   // UI-63, below
inline void const_card_update(double t);   // UI-46, below
inline void sky_point_card_update(double t);  // UI-50 / UI-47, below

// UI-30: the type icon sits on the title line, centred on it vertically (the title's
// right padding keeps the text clear of it), pushed into the card's padding so it
// sits at the far right (4.5.4)
constexpr int CARD_ICON_DX = 6;  // into the card's 10 px padding
inline void card_align_icon() {
  if (ui.card_icon == nullptr || lv_obj_has_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN))
    return;
  // 4.5.5: every card picture is 40 px (the Moon's size) in the top right corner
  lv_obj_align(ui.card_icon, LV_ALIGN_TOP_RIGHT, CARD_ICON_DX, -2);
}

// UI-24b: "20 Nov 1998" from days since 1957-01-01 (+1); false when unknown
inline bool launch_text(uint16_t launched, char *buf, size_t n) {
  if (launched == 0)
    return false;
  int64_t z = (int64_t) launched - 1 + net::SPACE_AGE_DAY + 719468;  // civil_from_days (H. Hinnant)
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned) (z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  const int y = (int) (yoe + era * 400 + (m <= 2));
  static const char *const M[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  snprintf(buf, n, "%u %s %d", d, M[m - 1], y);
  return true;
}
constexpr uint16_t ISS_LAUNCHED = 15299;  // 20 Nov 1998, Zarya (the first module)
constexpr uint16_t CSS_LAUNCHED = 23495;  // 29 Apr 2021, Tianhe (UI-52)

// ================================================================== UI-41d alert details
// A tap on the status line while an alert shows opens that alert's details: the card of its
// object (ISS or Tiangong overhead, a planet, comet, meteor shower, the Moon), or else this
// card, built from the same live data, in the alert's colour and icon.
inline void select_object(int kind, int32_t id);
inline void info_card_update(double t) {
  const Alert &al = info_alert;
  if (t - ui.sel_since > CARD_TIMEOUT_S) {
    deselect();
    return;
  }
  char title[64], body[640], d0[16], h0[16], h1[16], h2[16], cd[24];
  snprintf(title, sizeof(title), "Details");
  snprintf(body, sizeof(body), "%s", al.text);
  switch (al.info) {
    case AI_ISS_PASS:
    case AI_CSS_PASS: {
      const Pass *p = nullptr;
      if (al.info == AI_ISS_PASS)
        p = next_pass(t);
      else if (al.idx >= 0 && al.idx < (int) live.css_passes.size())
        p = &live.css_passes[al.idx];
      snprintf(title, sizeof(title), "%s pass", al.info == AI_ISS_PASS ? "ISS" : "Tiangong");
      if (p == nullptr || p->end <= t)
        break;
      local_hm(p->start, h0, sizeof(h0));
      local_hm(p->max, h1, sizeof(h1));
      local_hm(p->end, h2, sizeof(h2));
      day_word(t, (double) p->start, d0, sizeof(d0));
      if (t < p->start)
        countdown(p->start - t, cd, sizeof(cd));
      snprintf(body, sizeof(body),
               "%s%s%s\nRises %s in the %s\nHighest %s, %.0f\xC2\xB0 up in the %s\nSets %s in the %s\n"
               "Sunlit against a dark sky: a bright, steady star moving across.",
               t < p->start ? "In " : "Passing now", t < p->start ? cd : "", t < p->start ? (std::string(", ") + d0).c_str() : "",
               h0, p->start_dir, h1, p->max_el, p->max_dir, h2, p->end_dir);
      break;
    }
    case AI_LAUNCH: {
      if (al.idx < 0 || al.idx >= (int) live.launches.size())
        break;
      const auto &l = live.launches[al.idx];
      snprintf(title, sizeof(title), "%s", l.rocket[0] ? l.rocket : "Rocket launch");
      const char *bar = strchr(l.name, '|');
      const char *mission = bar ? bar + 1 : l.name;
      while (*mission == ' ')
        mission++;
      local_hm((int64_t) l.net, h0, sizeof(h0));
      local_md(l.net, d0, sizeof(d0));
      if (l.net > t)
        countdown(l.net - t, cd, sizeof(cd));
      else
        snprintf(cd, sizeof(cd), "now");
      char far[48] = "";
      float brg;
      const float km = launch_km(l, &brg);
      if (!std::isnan(km))
        snprintf(far, sizeof(far), "\n%.0f %s away, to the %s", dist(km), dist_unit(), compass(brg));  // UI-29
      snprintf(body, sizeof(body), "%s\n%s %s (%s%s)\nFrom %s\nStatus: %s%s", mission, d0, h0, l.net > t ? "in " : "",
               cd, l.where, l.status[0] ? l.status : "unknown", far);
      break;
    }
    case AI_EVENT: {
      if (al.idx < 0 || al.idx >= (int) live.events.size())
        break;
      const auto &e = live.events[al.idx];
      snprintf(title, sizeof(title), "%s", e.type[0] ? e.type : "Space event");
      local_hm((int64_t) e.t, h0, sizeof(h0));
      local_md(e.t, d0, sizeof(d0));
      if (e.t > t)
        countdown(e.t - t, cd, sizeof(cd));
      snprintf(body, sizeof(body), "%s\n%s %s%s%s%s", e.name, d0, h0, e.t > t ? " (in " : " (now", e.t > t ? cd : "", ")");
      if (e.iss)
        snprintf(body + strlen(body), sizeof(body) - strlen(body), "\nAt the International Space Station");
      break;
    }
    case AI_AURORA:
    case AI_WIND: {
      snprintf(title, sizeof(title), "%s", al.info == AI_AURORA ? "Aurora" : "Solar wind");
      int n = snprintf(body, sizeof(body), "%s\nKp %.1f: %s", al.text, aur.kp, aurora_word(aur.kp));
      if (ovation_fresh(t) && n < (int) sizeof(body)) {
        char oc[40];
        ovation_text(oc, sizeof(oc));
        n += snprintf(body + n, sizeof(body) - n, "\nNOAA nowcast: %s", oc);
      }
      if (!std::isnan(live.wind.bz) && n < (int) sizeof(body))
        n += snprintf(body + n, sizeof(body) - n, "\nSolar wind Bz %+.0f nT, %.0f km/s", live.wind.bz,
                      std::isnan(live.wind.speed) ? 0.0f : live.wind.speed);
      if (n < (int) sizeof(body))
        snprintf(body + n, sizeof(body) - n, "\nLook north, away from lights. A southward (negative) Bz lets it "
                                             "flare up within the hour.");
      break;
    }
    case AI_CONJ:
    case AI_PARADE: {
      const AlignEv &v = al.info == AI_CONJ ? conj_ev : parade_ev;
      if (al.info == AI_CONJ)
        snprintf(title, sizeof(title), "%s & %s", planets::name(v.a), planets::name(v.b));
      else
        snprintf(title, sizeof(title), "Planet parade");
      local_hm((int64_t) v.t, h0, sizeof(h0));
      local_md(v.t, d0, sizeof(d0));
      if (al.info == AI_CONJ)
        snprintf(body, sizeof(body), "Closest %s %s, %.1f\xC2\xB0 apart (a thumb's width at arm's length is "
                 "about 2\xC2\xB0).\nThey only look close: they line up as seen from Earth.", d0, h0, v.sep);
      else
        snprintf(body, sizeof(body), "%d planets above the horizon together, %s %s.\nThey sit along the ecliptic, "
                 "the Sun's path across the sky.", v.count, d0, h0);
      break;
    }
    case AI_ECLIPSE: {
      const ev::Eclipse *e = next_eclipse(t);
      if (e == nullptr)
        break;
      snprintf(title, sizeof(title), "%s", ev::eclipse_name(e->type));
      local_md(e->t0, d0, sizeof(d0));
      local_hm((int64_t) e->t0, h0, sizeof(h0));
      local_hm((int64_t) e->t_max, h1, sizeof(h1));
      local_hm((int64_t) e->t1, h2, sizeof(h2));
      int n = snprintf(body, sizeof(body), "%s: seen from here %s to %s\nGreatest %s, %.0f\xC2\xB0 up\n", d0, h0, h2, h1,
                       e->el);
      if (e->c1 > e->c0 && e->c0 > 0 && n < (int) sizeof(body)) {
        char c0[16], c1[16];
        local_hm((int64_t) e->c0, c0, sizeof(c0));
        local_hm((int64_t) e->c1, c1, sizeof(c1));
        n += snprintf(body + n, sizeof(body) - n, "%s %s to %s\n", ev::is_lunar(e->type) ? "Totality" : "Central phase", c0, c1);
      }
      if (n < (int) sizeof(body))
        snprintf(body + n, sizeof(body) - n, ev::is_lunar(e->type)
                     ? "Magnitude %.2f. Safe to watch with the naked eye."
                     : "%.0f%% of the Sun covered. Never look at the Sun without eclipse glasses.",
                 ev::is_lunar(e->type) ? e->mag : e->mag * 100.0f);
      break;
    }
    case AI_SEASON:
      snprintf(title, sizeof(title), "Season");
      break;
    default:
      break;
  }
  set_text_if(ui.card_title, title);
  set_text_if(ui.card_body, body);
  if (ui.card_moon)
    lv_obj_add_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_flag) {  // UI-54d: the launch's or spacecraft's country, after the title (as UI-34)
    if (al.flag) {
      if (lv_image_get_src(ui.card_flag) != al.flag)
        lv_image_set_src(ui.card_flag, al.flag);
      lv_point_t sz;
      lv_text_get_size(&sz, title, lv_obj_get_style_text_font(ui.card_title, LV_PART_MAIN), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      lv_obj_update_layout(ui.card);
      const int room = lv_obj_get_content_width(ui.card_title) - flags::CARD_W - 6;
      lv_obj_align_to(ui.card_flag, ui.card_title, LV_ALIGN_LEFT_MID, std::min((int) sz.x, room) + 6, 0);
      lv_obj_remove_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (ui.card_icon) {
    if (al.glyph) {
      set_text_if(ui.card_icon, al.glyph);
      lv_obj_remove_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
      card_align_icon();
    } else {
      lv_obj_add_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_obj_add_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_obj_update_layout(ui.card);
  const int ch = lv_obj_get_height(ui.card), cw = lv_obj_get_width(ui.card);
  lv_obj_set_pos(ui.card, box.x1 + ui.cx - cw / 2, box.y1 + std::max(0, std::min(ui.size - ch, 16)));
  arc_update(t);
}
inline void alert_click_cb(lv_event_t *e) {
  if (!shown_alert_ok)
    return;  // no alert: the tap goes on to the page
  lv_event_stop_bubbling(e);
  const Alert &a = shown_alert;
  if (a.kind >= 0) {
    select_object(a.kind, a.id);
  } else {
    info_alert = a;
    select_object(K_INFO, 0);
  }
}
inline void card_update(double t) {
  if (ui.sel_kind < 0 || ui.card == nullptr)
    return;
  if (card_find_btn) {  // UI-41d: an alert's details card has nothing to point at
    if (ui.sel_kind == K_INFO)
      lv_obj_add_flag(card_find_btn, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_remove_flag(card_find_btn, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui.sel_kind == K_INFO) {
    if (ui.card_img_btn)
      lv_obj_add_flag(ui.card_img_btn, LV_OBJ_FLAG_HIDDEN);
    info_card_update(t);
    return;
  }
  if (ui.card_img_btn) {  // UI-59/60: Sun and Moon cards; the picture is fetched as the card opens
    if (ui.sel_kind == K_SUN || ui.sel_kind == K_MOON || ui.sel_kind == K_PLANET) {
      lv_obj_remove_flag(ui.card_img_btn, LV_OBJ_FLAG_HIDDEN);
      if (ui.sel_kind != K_PLANET)  // (UI-61e: the planet photos are ready in the firmware)
        img_request(ui.sel_kind == K_MOON ? net::IMG_MOON : net::IMG_SUN, t);
    } else {
      lv_obj_add_flag(ui.card_img_btn, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (ui.sel_kind == K_SUN || ui.sel_kind == K_MOON) {
    body_card_update(t);
    return;
  }
  if (ui.sel_kind == K_PLANET) {
    planet_card_update(t);
    return;
  }
  if (ui.sel_kind == K_COMET) {
    comet_card_update(t);  // UI-63
    return;
  }
  if (ui.sel_kind == K_CONST) {
    const_card_update(t);
    return;
  }
  if (ui.sel_kind == K_STAR || ui.sel_kind == K_SHOWER || ui.sel_kind == K_DSO) {
    sky_point_card_update(t);  // UI-50 / UI-47
    return;
  }
  if (ui.card_moon)
    lv_obj_add_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_icon)
    lv_obj_remove_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
  Marker *m = find_marker(ui.sel_kind, ui.sel_id);
  if (m == nullptr || !m->up || t - ui.sel_since > CARD_TIMEOUT_S) {
    deselect();
    return;
  }
  const SatRec &r = m->rec;
  const geo::V3 pos = geo::sat_ecef(r, t);
  const geo::AzEl a = geo::azel(ui.obs, pos);
  const float range = geo::norm(geo::sub(pos, ui.obs.pos));
  const bool lit = astro::sunlit(pos, ui.sm.sun_ecef_dir);
  const float alt = r.alt_km + r.alt_rate * (float) (t - r.t_fix);
  char title[48], body[420];
  snprintf(title, sizeof(title), "%s%s%s", r.name, r.cc[0] ? "  " : "", r.cc);
  int n = snprintf(body, sizeof(body),
                   "Az %.0f° %s   El %.0f°\n"
                   "Height %.0f %s   Range %.0f %s\n"
                   "Speed %.2f %s/s   %s\n"
                   "Orbit %s, %.0f min, %.1f° incl.",
                   a.az, compass(a.az), a.el, dist(alt), dist_unit(), dist(range), dist_unit(), dist(r.speed_kms),
                   dist_unit(), lit ? "Sunlit" : "In Earth's shadow",
                   orbit_class(r.cls), r.period_min, r.incl_deg);
  char launched[24];
  const bool has_launch = launch_text(ui.sel_kind == K_ISS && r.launched == 0   ? ISS_LAUNCHED
                                      : ui.sel_kind == K_CSS && r.launched == 0 ? CSS_LAUNCHED
                                                                                : r.launched,
                                      launched, sizeof(launched));
  if (ui.sel_kind == K_CSS) {  // UI-52
    if (has_launch)
      n += snprintf(body + n, sizeof(body) - n, "\nLaunched %s (Tianhe)", launched);
    for (const auto &p : live.css_passes)
      if (p.end > t) {
        char h1[12], h2[12];
        local_hm(p.start, h1, sizeof(h1));
        local_hm(p.end, h2, sizeof(h2));
        n += snprintf(body + n, sizeof(body) - n, "\nNext pass %s-%s%s\nRise %s  Peak %.0f° %s  Set %s", h1, h2,
                      p.visible ? " (visible)" : "", p.start_dir, p.max_el, p.max_dir, p.end_dir);
        break;
      }
  } else if (ui.sel_kind == K_ISS) {
    if (has_launch)
      n += snprintf(body + n, sizeof(body) - n, "\nLaunched %s (Zarya)", launched);
    if (const Pass *p = next_pass(t)) {
      char h1[12], h2[12];
      local_hm(p->start, h1, sizeof(h1));
      local_hm(p->end, h2, sizeof(h2));
      n += snprintf(body + n, sizeof(body) - n, "\nNext pass %s-%s%s\nRise %s  Peak %.0f° %s  Set %s", h1, h2,
                    p->visible ? " (visible)" : "", p->start_dir, p->max_el, p->max_dir, p->end_dir);
    }
  } else if (r.intl[0]) {
    n += snprintf(body + n, sizeof(body) - n, "\nNORAD %ld   %s", (long) r.id, r.intl);
  } else {
    n += snprintf(body + n, sizeof(body) - n, "\nNORAD %ld", (long) r.id);
  }
  if (ui.sel_kind != K_ISS && ui.sel_kind != K_CSS && has_launch)
    n += snprintf(body + n, sizeof(body) - n, "\nLaunched %s", launched);
  set_text_if(ui.card_title, title);
  if (ui.card_icon)
    set_text_if(ui.card_icon, card_icon_for(ui.sel_kind, r));
  card_align_icon();
  set_text_if(ui.card_body, body);
  if (ui.card_flag) {  // UI-34: flag just after the ISO code at the end of the title
    const lv_image_dsc_t *fl = r.cc[0] ? card_flag_for(r.cc) : nullptr;
    if (fl) {
      if (lv_image_get_src(ui.card_flag) != fl)
        lv_image_set_src(ui.card_flag, fl);
      lv_point_t sz;
      lv_text_get_size(&sz, title, lv_obj_get_style_text_font(ui.card_title, LV_PART_MAIN), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      lv_obj_update_layout(ui.card);
      const int room = lv_obj_get_content_width(ui.card_title) - flags::CARD_W - 6;
      lv_obj_align_to(ui.card_flag, ui.card_title, LV_ALIGN_LEFT_MID, std::min((int) sz.x, room) + 6, 0);
      lv_obj_remove_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // keep the card on the half of the disc away from the marker
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_obj_update_layout(ui.card);
  const int ch = lv_obj_get_height(ui.card), cw = lv_obj_get_width(ui.card);
  const int top = m->dy > ui.cy ? ui.cy - ch - 16 : ui.cy + 16;
  lv_obj_set_pos(ui.card, box.x1 + ui.cx - cw / 2, box.y1 + std::max(0, std::min(ui.size - ch, top)));
  const int ring = (ui.sel_kind == K_ISS || ui.sel_kind == K_CSS ? ISS_PX : 2 * mark_r()) + 10;
  lv_obj_set_size(ui.sel_ring, ring, ring);
  lv_obj_set_pos(ui.sel_ring, m->dx - ring / 2, m->dy - ring / 2);
  lv_obj_remove_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_to_index(ui.sel_ring, -1);
  arc_update(t);
}

// UI-36: the Sun or Moon card. The heavy part (rise/set, twilight, phases, path) is
// cached for 10 minutes; position and the Moon picture follow live values.
// UI-36c: the Sun/Moon card text, written on every refresh from the cached times
// (body_info_update) and the live position, units and clock format.
inline void day_change_text(double delta, char *buf, size_t n) {  // "gaining 5m/day"
  const double a = fabs(delta);
  const char *w = delta >= 0 ? "gaining" : "losing";
  if (a < 1)
    snprintf(buf, n, "%s", "no change today");
  else {  // UI-17a: the one duration format, "losing 6m/day", "gaining 45s/day"
    char d[16];
    fmt_dur(a < 60 ? a : 60.0 * floor(a / 60.0 + 0.5), d, sizeof(d));
    snprintf(buf, n, "%s %s/day", w, d);
  }
}

inline size_t body_text(double t, char *text, size_t n) {
  const bool moon = body.kind == K_MOON;
  const Config &c = config();
  size_t k = 0;
  auto add = [&](const char *fmt, auto... args) {
    if (k < n - 1)
      k += snprintf(text + k, n - k, "%s", k ? "\n" : "");
    if (k < n - 1)
      k += snprintf(text + k, n - k, fmt, args...);
    k = std::min(k, n - 1);
  };
  char a[16], b[16];
  auto hm = [](double tt, char *buf) {
    if (tt > 0)
      local_hm((int64_t) tt, buf, 16);
    else
      snprintf(buf, 16, "--");
  };
  const geo::AzEl p = moon ? ui.sm.moon : ui.sm.sun;
  add("Az %.0f\xC2\xB0 %s   El %.1f\xC2\xB0", p.az, compass(p.az), p.el);
  if (!moon) {
    if (body.ncross == 0) {
      add("%s all day", body.peak > 0 ? "Up" : "Down");
    } else {
      hm(body.rise, a);
      hm(body.set, b);
      add("Rise %s   Set %s", a, b);
    }
    hm(body.noon, a);
    add("Noon %s, %.0f\xC2\xB0 high", a, body.peak);
    {  // UI-36b: distance, as on the Moon and planet cards
      const double au = planets::sun_dist_au(astro::jd(t)), km = au * 149597870.7;
      add("Distance %.3f AU (%.1f million %s)", au, (ui.miles ? km / 1.609344 : km) / 1e6, ui.miles ? "mi" : "km");
    }
    if (body.day_len > 0) {  // UI-36c: "Daylight 11h 58m, losing 6m/day"
      char dc[32] = "";
      if (body.have_delta) {
        dc[0] = ',';
        dc[1] = ' ';
        day_change_text(body.day_delta, dc + 2, sizeof(dc) - 2);
      }
      char dl[16];
      fmt_dur(body.day_len, dl, sizeof(dl));  // UI-17a
      add("Daylight %s%s", dl, dc);
    }
    if (body.dawn > 0 || body.dusk > 0) {
      hm(body.dawn, a);
      hm(body.dusk, b);
      add("Dawn %s   Dusk %s", a, b);
    }
    if (body.gold0 > 0 && body.gold1 > body.gold0) {
      hm(body.gold0, a);
      hm(body.gold1, b);
      add("Golden hour %s-%s", a, b);
    }
    if (body.dark) {
      hm(body.dark0, a);
      hm(body.dark1, b);
      add("Dark %s-%s", a, b);
    } else {
      add("%s", "No dark sky tonight");
    }
    float kn, km;  // UI-37 / UI-38: plain words, the Kp number for reference
    aurora_update(t);
    if (aur.kp >= 0)
      add("Aurora %s, Kp %.0f: %s", aurora_word(aur.kp), aur.kp, aurora_outlook(aur.kp, body.dark));
    else if (kp_values(t, kn, km))
      add("Aurora %s, Kp %.0f: %s", aurora_word(std::max(kn, km)), std::max(kn, km),
          aurora_outlook(std::max(kn, km), body.dark));
    else
      add("%s", "Aurora: no forecast yet");
    if (ovation_fresh(t)) {  // UI-58
      char oc[40];
      ovation_text(oc, sizeof(oc));
      add("Aurora chance %s", oc);
    }
    if (wind_fresh(t))  // UI-55
      add("Solar wind %.0f km/s, Bz %+.0f nT%s", live.wind.speed, live.wind.bz,
          live.wind.bz <= WIND_BZ_SOUTH ? " (south)" : "");
  } else {
    const astro::SunMoon sm = astro::compute(t, c.lat, c.lon, c.alt_m);
    add("%s %.0f%%, %.1f days old", astro::phase_name(sm.moon_illum, sm.moon_elong), sm.moon_illum * 100.0f,
        body.prev_new > 0 ? (t - body.prev_new) / 86400.0 : 0.0);
    hm(body.rise, a);
    hm(body.set, b);
    add("Rise %s   Set %s", a, b);
    char f[16], nws[16];
    local_md(body.full, f, sizeof(f));
    local_md(body.next_new, nws, sizeof(nws));
    add("Full %s   New %s", f, nws);
    const double km = astro::moon_distance_km(t);
    const long dv = lround(dist((float) km) / 100.0) * 100;
    add("Distance %ld,%03ld %s%s", dv / 1000, dv % 1000, dist_unit(),
        (km < 360000 && sm.moon_illum > 0.9f) ? " (supermoon)" : "");
    if (const ev::Eclipse *e = next_eclipse(t)) {  // UI-51: the next one seen from here
      char md[16], nm[32];
      local_md(e->t_max, md, sizeof(md));
      snprintf(nm, sizeof(nm), "%s", ev::eclipse_name(e->type));
      for (char *q = nm; *q; q++)
        *q = (char) tolower(*q);
      if (char *q = strstr(nm, " eclipse"))
        *q = 0;  // "total lunar"
      if (e->t_max - t > 300 * 86400.0)
        add("Next eclipse %s '%02d: %s", md, (int) decimal_year(e->t_max) % 100, nm);
      else
        add("Next eclipse %s: %s", md, nm);
    }
  }
  return k;
}

inline void body_card_update(double t) {
  const bool moon = ui.sel_kind == K_MOON;
  if (t - ui.sel_since > CARD_TIMEOUT_S) {
    deselect();
    return;
  }
  body_info_update(ui.sel_kind, t);
  char text[720];
  size_t k = body_text(t, text, sizeof(text));
  // UI-36a: the myth behind the name, as on the planet cards
  if (k < sizeof(text) - 1)
    snprintf(text + k, sizeof(text) - k, "\n%s",
             moon ? "Luna to the Romans, Selene to the Greeks: a goddess who drove a silver chariot across "
                    "the night sky, sister of the Sun god."
                  : "Sol to the Romans, Helios to the Greeks: a god who drove a chariot of fire across the "
                    "sky each day, brother of the Moon.");
  set_text_if(ui.card_title, moon ? "Moon" : "Sun");
  set_text_if(ui.card_body, text);
  if (ui.card_flag)
    lv_obj_add_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_icon) {
    if (moon) {
      lv_obj_add_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
      set_text_if(ui.card_icon, "\xF3\xB0\x96\x99");  // weather-sunny
      lv_obj_remove_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
      card_align_icon();
    }
  }
  if (ui.card_moon) {
    if (moon) {
      static bool drawn_lr = false;
      const float kk = std::max(0.0f, std::min(1.0f, ui.sm.moon_illum));
      const bool lr = (ui.sm.moon_elong < 180.0f) != (config().lat < 0);
      if (ui.moon_drawn_k < 0 || fabsf(kk - ui.moon_drawn_k) > 0.004f || lr != drawn_lr) {
        ui.moon_drawn_k = kk;
        ui.fig_drawn = -1;
        drawn_lr = lr;
        render_moon(ui.card_moon, kk, lr ? 1.0f : -1.0f, 0.0f);
      }
      lv_obj_remove_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
    }
  }
  // place the card away from the body; ring it when it is up
  const bool up = moon ? ui.moon_shown : ui.sun_shown;
  const int half = moon ? MOON_PX / 2 : SUN_HALO_PX / 2;
  const int bx = (moon ? ui.moon_x : ui.sun_x) + half, by = (moon ? ui.moon_y : ui.sun_y) + half;
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_obj_update_layout(ui.card);
  const int ch = lv_obj_get_height(ui.card), cw = lv_obj_get_width(ui.card);
  const int top = !up ? (ui.size - ch) / 2 : by > ui.cy ? ui.cy - ch - 16 : ui.cy + 16;
  lv_obj_set_pos(ui.card, box.x1 + ui.cx - cw / 2, box.y1 + std::max(0, std::min(ui.size - ch, top)));
  if (up) {
    const int ring = 2 * half + 8;
    lv_obj_set_size(ui.sel_ring, ring, ring);
    lv_obj_set_pos(ui.sel_ring, bx - ring / 2, by - ring / 2);
    lv_obj_remove_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(ui.sel_ring, -1);
  } else {
    lv_obj_add_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  }
  arc_update(t);
}

// UI-40a: the planet card: where it is, how far, how bright, and the story of its name.
inline const char *planet_story(int p) {
  static const char *const S[planets::N_PLANETS] = {
      "Named for the Roman messenger of the gods (Greek Hermes), fast on winged feet: "
      "it circles the Sun in 88 days.",
      "Named for the Roman goddess of love and beauty (Greek Aphrodite). The brightest "
      "planet, and the hottest.",
      "Named for the Roman god of war (Greek Ares), for its blood-red colour. Its two "
      "moons are Fear and Dread.",
      "Named for the king of the Roman gods (Greek Zeus). The largest planet: 11 Earths "
      "across. Its big moons are Zeus's companions.",
      "Named for the Roman god of farming and time (Greek Kronos), Jupiter's father. "
      "Its rings are ice, 280,000 km wide.",
  };
  return S[p];
}
inline void planet_card_update(double t) {
  const int p = ui.sel_id;
  if (p < 0 || p >= planets::N_PLANETS || t - ui.sel_since > CARD_TIMEOUT_S || !ui.planet_shown[p]) {
    deselect();
    return;
  }
  const Config &c = config();
  planets::Pos ps;
  const geo::AzEl a = planets::horizontal(p, t, c.lat, c.lon, &ps);
  const double km = ps.dist_au * 149597870.7;
  const double far = ui.miles ? km / 1.609344 : km;
  const double light_min = ps.dist_au * 499.005 / 60.0;
  char light[24];
  fmt_dur(light_min * 60.0, light, sizeof(light));  // UI-17a
  char text[420];
  snprintf(text, sizeof(text),
           "Az %.0f\xC2\xB0 %s   El %.1f\xC2\xB0\n"
           "Distance %.2f AU (%.0f million %s)\n"
           "Light takes %s to reach us\n"
           "Magnitude %.1f%s\n"
           "%s",
           a.az, compass(a.az), a.el, ps.dist_au, far / 1e6, ui.miles ? "mi" : "km", light, ps.mag,
           ui.planet_visible[p] ? "   visible now" : "", planet_story(p));
  set_text_if(ui.card_title, planets::name(p));
  set_text_if(ui.card_body, text);
  if (ui.card_moon)
    lv_obj_add_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_icon)
    lv_obj_add_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_flag) {  // the planet's picture at the right end of the title line
    const lv_image_dsc_t *img = planet_card_dsc(p);
    if (lv_image_get_src(ui.card_flag) != img)
      lv_image_set_src(ui.card_flag, img);
    lv_obj_align(ui.card_flag, LV_ALIGN_TOP_RIGHT, CARD_ICON_DX, -2);
    lv_obj_remove_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
  }
  const int H = picons::SMALL / 2, bx = ui.planet_x[p] + H, by = ui.planet_y[p] + H;
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_obj_update_layout(ui.card);
  const int ch = lv_obj_get_height(ui.card), cw = lv_obj_get_width(ui.card);
  const int top = by > ui.cy ? ui.cy - ch - 16 : ui.cy + 16;
  lv_obj_set_pos(ui.card, box.x1 + ui.cx - cw / 2, box.y1 + std::max(0, std::min(ui.size - ch, top)));
  const int ring = 2 * H + 10;
  lv_obj_set_size(ui.sel_ring, ring, ring);
  lv_obj_set_pos(ui.sel_ring, bx - ring / 2, by - ring / 2);
  lv_obj_remove_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_to_index(ui.sel_ring, -1);
  arc_update(t);
}

// UI-63: the comet card: where it is, how bright, how far, when it is nearest the Sun and
// whether it comes back
inline int comet_slot(int idx) {
  for (int k = 0; k < Ui::MAX_COMETS; k++)
    if (ui.comet_idx[k] == idx)
      return k;
  return -1;
}
inline void comet_card_update(double t) {
  const int i = ui.sel_id, k = comet_slot(i);
  if (i < 0 || i >= (int) live.comet_list.size() || k < 0 || !ui.comet_shown[k] || t - ui.sel_since > CARD_TIMEOUT_S) {
    deselect();
    return;
  }
  const comets::El &c = live.comet_list[i];
  const comets::Pos p = comets::position(c, astro::jd(t));
  const geo::AzEl a = ui.comet_azel[k];
  const double km = p.d * 149597870.7, far = ui.miles ? km / 1.609344 : km;
  char peri[24], when[32];
  const double tp_unix = (c.tp - 2440587.5) * 86400.0;
  local_md(tp_unix, peri, sizeof(peri));
  const double dd = (tp_unix - t) / 86400.0;
  if (fabs(dd) < 1)
    snprintf(when, sizeof(when), "%s", "today");
  else if (dd > 0)
    snprintf(when, sizeof(when), "in %.0f days", dd);
  else
    snprintf(when, sizeof(when), "%.0f days ago", -dd);
  char orbit[64];
  if (c.e < 1) {
    const double yr = pow(c.q / (1 - c.e), 1.5);
    if (yr < 200)
      snprintf(orbit, sizeof(orbit), "Comes back every %.1f years", yr);
    else
      snprintf(orbit, sizeof(orbit), "Comes back in about %s years", yr < 1e5 ? "thousands of" : "millions of");
  } else {
    snprintf(orbit, sizeof(orbit), "%s", "One pass: it leaves the solar system");
  }
  const char *seen = p.mag <= 5.0f ? "naked eye" : p.mag <= 8.0f ? "binoculars" : "telescope";
  char text[420];
  snprintf(text, sizeof(text),
           "Az %.0f\xC2\xB0 %s   El %.1f\xC2\xB0%s\n"
           "Magnitude about %.1f (%s)\n"
           "From the Sun %.2f AU\n"
           "From us %.2f AU (%.0f million %s)\n"
           "Nearest the Sun %s, %s (%.2f AU)\n"
           "%s. Its tail points away from the Sun.",
           a.az, compass(a.az), a.el, ui.comet_visible[k] ? "   visible now" : "", p.mag, seen, p.r, p.d, far / 1e6,
           ui.miles ? "mi" : "km", peri, when, c.q, orbit);
  set_text_if(ui.card_title, c.name);
  set_text_if(ui.card_body, text);
  if (ui.card_moon)
    lv_obj_add_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_flag)
    lv_obj_add_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_icon) {
    set_text_if(ui.card_icon, ICON_COMET);
    lv_obj_remove_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
    card_align_icon();
  }
  const int bx = ui.comet_x[k], by = ui.comet_y[k];
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_obj_update_layout(ui.card);
  const int ch = lv_obj_get_height(ui.card), cw = lv_obj_get_width(ui.card);
  const int top = by > ui.cy ? ui.cy - ch - 16 : ui.cy + 16;
  lv_obj_set_pos(ui.card, box.x1 + ui.cx - cw / 2, box.y1 + std::max(0, std::min(ui.size - ch, top)));
  const int ring = COMET_PX + 12;
  lv_obj_set_size(ui.sel_ring, ring, ring);
  lv_obj_set_pos(ui.sel_ring, bx - ring / 2, by - ring / 2);
  lv_obj_remove_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_to_index(ui.sel_ring, -1);
  arc_update(t);
}

// UI-46: the constellation card: where it is, what the name means, its story, its
// brightest star and one sight worth finding (sky_lore.h)
constexpr uint32_t C_CONST_TITLE = 0xB8C4F0;
inline void const_card_update(double t) {
  const int i = ui.sel_id;
  if (i < 0 || i >= sky::N_NAMES || t - ui.sel_since > CARD_TIMEOUT_S) {
    deselect();
    return;
  }
  const sky::Name &nm = sky::NAMES[i];
  const Config &c = config();
  const float lst = (float) astro::wrap360(astro::gmst_deg(astro::jd(t)) + c.lon);
  const float la = (float) c.lat * geo::DEG;
  const Enu v = star_enu(nm.ra * (360.0f / 65536.0f), nm.dec * (90.0f / 32767.0f), lst, sinf(la), cosf(la));
  float az = atan2f(v.e, v.n) / geo::DEG;
  if (az < 0)
    az += 360.0f;
  const float el = asinf(std::max(-1.0f, std::min(1.0f, v.u))) / geo::DEG;
  char text[640];
  const lore::Lore *L = lore::find(nm.text);
  if (L)
    snprintf(text, sizeof(text), "Az %.0f\xC2\xB0 %s   El %.0f\xC2\xB0\n%s. %s\nBrightest: %s\nLook for: %s", az, compass(az),
             el, L->meaning, L->story, L->star, L->sight);
  else
    snprintf(text, sizeof(text), "Az %.0f\xC2\xB0 %s   El %.0f\xC2\xB0", az, compass(az), el);
  set_text_if(ui.card_title, nm.text);
  set_text_if(ui.card_body, text);
  if (ui.card_moon) {  // UI-46a: the constellation's own figure in the picture corner
    if (ui.fig_drawn != i) {
      render_cfig(ui.card_moon, i);
      ui.fig_drawn = i;
      ui.moon_drawn_k = -1;
    }
    lv_obj_align(ui.card_moon, LV_ALIGN_TOP_RIGHT, CARD_ICON_DX, -2);
    lv_obj_remove_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui.card_icon)
    lv_obj_add_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_flag)
    lv_obj_add_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  int ty = ui.cy;  // keep the card clear of the name tag
  for (const auto &nt : ui.names)
    if (nt.idx == i)
      ty = nt.y + nt.h / 2;
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_obj_update_layout(ui.card);
  const int ch = lv_obj_get_height(ui.card), cw = lv_obj_get_width(ui.card);
  const int top = ty > ui.cy ? ui.cy - ch - 16 : ui.cy + 16;
  lv_obj_set_pos(ui.card, box.x1 + ui.cx - cw / 2, box.y1 + std::max(0, std::min(ui.size - ch, top)));
  arc_update(t);
}

// UI-50 bright star / UI-47 meteor shower cards: a fixed point on the sky (RA/Dec)
inline void view_azel(float ra, float dec, double t, float &az, float &el) {
  const Config &c = config();
  const float lst = (float) astro::wrap360(astro::gmst_deg(astro::jd(t)) + c.lon);
  const float la = (float) c.lat * geo::DEG;
  const Enu v = star_enu(ra, dec, lst, sinf(la), cosf(la));
  az = atan2f(v.e, v.n) / geo::DEG;
  if (az < 0)
    az += 360.0f;
  el = asinf(std::max(-1.0f, std::min(1.0f, v.u))) / geo::DEG;
}
inline void sky_point_card_update(double t) {
  const bool star = ui.sel_kind == K_STAR, dso = ui.sel_kind == K_DSO;
  const int i = ui.sel_id;
  if (i < 0 || i >= (star ? ev::N_BRIGHT : dso ? ev::N_DSO : ev::N_SHOWERS) || t - ui.sel_since > CARD_TIMEOUT_S) {
    deselect();
    return;
  }
  char text[640];
  float az, el;
  const double tv = t + (ui.scrub_on ? ui.scrub_s : 0);
  if (dso) {  // UI-56
    const ev::DeepSky &o = ev::DSO[i];
    view_azel(o.ra, o.dec, tv, az, el);
    snprintf(text, sizeof(text), "Az %.0f\xC2\xB0 %s   El %.0f\xC2\xB0\n%s, magnitude %.1f\n%s light years, in %s\n%s",
             az, compass(az), el, o.kind, o.mag, o.dist, o.where, o.fact);
    char title[48];
    snprintf(title, sizeof(title), "%s (%s)", o.name, o.cat);
    set_text_if(ui.card_title, title);
  } else if (star) {
    const ev::BrightStar &b = ev::BRIGHT[i];
    view_azel(b.ra, b.dec, tv, az, el);
    snprintf(text, sizeof(text), "Az %.0f\xC2\xB0 %s   El %.0f\xC2\xB0\nMagnitude %.1f, %s light years\nIn %s\n%s", az,
             compass(az), el, b.mag, b.dist, b.where, b.fact);
    set_text_if(ui.card_title, b.name);
  } else {
    const ev::Shower &sh = ev::SHOWERS[i];
    view_azel(sh.ra, sh.dec, tv, az, el);
    char pk[16], a0[16], a1[16], hm[16];
    local_md(sev.peak[i], pk, sizeof(pk));
    local_hm((int64_t) sev.peak[i], hm, sizeof(hm));
    local_md(sev.peak[i] - sh.before * 86400.0, a0, sizeof(a0));
    local_md(sev.peak[i] + sh.after * 86400.0, a1, sizeof(a1));
    const float k = astro::compute(sev.peak[i], config().lat, config().lon, config().alt_m).moon_illum;
    snprintf(text, sizeof(text),
             "Radiant Az %.0f\xC2\xB0 %s   El %.0f\xC2\xB0\nPeak %s %s, up to %d an hour\nActive %s - %s\n"
             "Moon %.0f%% lit at the peak\nDust from %s. Meteors streak away from the radiant; "
             "best after midnight, when it is high.",
             az, compass(az), el, pk, hm, sh.zhr, a0, a1, k * 100.0f, sh.parent);
    set_text_if(ui.card_title, sh.name);
  }
  set_text_if(ui.card_body, text);
  if (ui.card_moon)
    lv_obj_add_flag(ui.card_moon, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_flag)
    lv_obj_add_flag(ui.card_flag, LV_OBJ_FLAG_HIDDEN);
  if (ui.card_icon) {
    set_text_if(ui.card_icon, star || dso ? "\xF3\xB0\xAB\xA2" : "\xF3\xB1\x9D\x81");  // star-four-points / star-shooting
    lv_obj_remove_flag(ui.card_icon, LV_OBJ_FLAG_HIDDEN);
    card_align_icon();
  }
  lv_obj_add_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);
  float x, y;
  project({az, el}, x, y);
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  lv_obj_update_layout(ui.card);
  const int ch = lv_obj_get_height(ui.card), cw = lv_obj_get_width(ui.card);
  const int top = y > ui.cy ? ui.cy - ch - 16 : ui.cy + 16;
  lv_obj_set_pos(ui.card, box.x1 + ui.cx - cw / 2, box.y1 + std::max(0, std::min(ui.size - ch, top)));
  arc_update(t);
}

// ================================================================== UI-57 "Point me to it"
// From a details card: a full-screen arrow that turns with the compass (or, without one,
// is relative to the map's top, the Heading setting) and says which way and how high.
struct Finder {
  lv_obj_t *root = nullptr, *arrow = nullptr, *title = nullptr, *turn = nullptr, *up = nullptr, *note = nullptr;
  int kind = -1;
  int32_t id = -1;
  float rel = 0, el = 0;  // drawn: bearing relative to the device's top, elevation
  bool have = false, compass = false;
};
inline Finder fnd;
constexpr float FIND_AHEAD_DEG = 8.0f;
inline bool finder_open() { return fnd.root != nullptr; }
// where the finder's object is now (false: not known any more)
inline bool finder_target(double t, float &az, float &el) {
  const Config &c = config();
  switch (fnd.kind) {
    case K_SUN:
    case K_MOON: {
      const astro::SunMoon sm = astro::compute(t, c.lat, c.lon, c.alt_m);
      const geo::AzEl a = fnd.kind == K_SUN ? sm.sun : sm.moon;
      az = a.az;
      el = a.el;
      return true;
    }
    case K_PLANET: {
      const geo::AzEl a = planets::horizontal(fnd.id, t, c.lat, c.lon);
      az = a.az;
      el = a.el;
      return true;
    }
    case K_COMET: {  // UI-63
      if (fnd.id < 0 || fnd.id >= (int) live.comet_list.size())
        return false;
      const double j = astro::jd(t);
      const comets::Pos p = comets::position(live.comet_list[fnd.id], j);
      if (!p.ok)
        return false;
      const geo::AzEl a = astro::horizontal(p.equ, c.lat, astro::rad(astro::wrap360(astro::gmst_deg(j) + c.lon)));
      az = a.az;
      el = a.el;
      return true;
    }
    case K_CONST:
      view_azel(sky::NAMES[fnd.id].ra * (360.0f / 65536.0f), sky::NAMES[fnd.id].dec * (90.0f / 32767.0f), t, az, el);
      return true;
    case K_STAR:
      view_azel(ev::BRIGHT[fnd.id].ra, ev::BRIGHT[fnd.id].dec, t, az, el);
      return true;
    case K_DSO:
      view_azel(ev::DSO[fnd.id].ra, ev::DSO[fnd.id].dec, t, az, el);
      return true;
    case K_SHOWER:
      view_azel(ev::SHOWERS[fnd.id].ra, ev::SHOWERS[fnd.id].dec, t, az, el);
      return true;
    default: {
      Marker *m = find_marker(fnd.kind, fnd.id);
      if (m == nullptr)
        return false;
      const geo::AzEl a = geo::azel(ui.obs, geo::sat_ecef(m->rec, t));
      az = a.az;
      el = a.el;
      return true;
    }
  }
}
inline void finder_arrow_cb(lv_event_t *e) {
  if (!fnd.have)
    return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(fnd.arrow, &a);
  const float cx = (a.x1 + a.x2) / 2.0f, cy = (a.y1 + a.y2) / 2.0f, R = (a.x2 - a.x1) / 2.0f - 4;
  lv_draw_arc_dsc_t ring;
  lv_draw_arc_dsc_init(&ring);
  ring.color = lv_color_hex(0x26314F);
  ring.width = 3;
  ring.center.x = (int32_t) cx;
  ring.center.y = (int32_t) cy;
  ring.radius = (uint16_t) R;
  ring.start_angle = 0;
  ring.end_angle = 360;
  lv_draw_arc(layer, &ring);
  const bool ahead = fabsf(fnd.rel) < FIND_AHEAD_DEG;
  const uint32_t col = fnd.el < 0 ? 0x7E8BB3 : ahead ? 0x4FD18B : 0x7FB2FF;
  const float t = fnd.rel * 0.0174533f, sn = sinf(t), cs = cosf(t);
  auto P = [&](float x, float y) {  // arrow space (up = -y) rotated about the centre
    lv_point_precise_t p;
    p.x = (lv_value_precise_t) (cx + x * cs - y * sn);
    p.y = (lv_value_precise_t) (cy + x * sn + y * cs);
    return p;
  };
  lv_draw_triangle_dsc_t d;
  lv_draw_triangle_dsc_init(&d);
  d.color = lv_color_hex(col);
  d.opa = LV_OPA_COVER;
  const float L = R * 0.78f, head = R * 0.42f, hw = R * 0.36f, sw = R * 0.12f;
  d.p[0] = P(0, -L);  // head
  d.p[1] = P(-hw, -L + head);
  d.p[2] = P(hw, -L + head);
  lv_draw_triangle(layer, &d);
  lv_draw_line_dsc_t sh;  // shaft: one thick line (two triangles leave a seam)
  lv_draw_line_dsc_init(&sh);
  sh.color = d.color;
  sh.width = (int32_t) (2 * sw);
  sh.p1 = P(0, -L + head - 2);
  sh.p2 = P(0, L * 0.8f);
  lv_draw_line(layer, &sh);
}
inline void finder_close() {
  if (fnd.root)
    lv_obj_delete(fnd.root);
  fnd = Finder();
}
inline void finder_close_cb(lv_event_t *) { finder_close(); }
inline lv_obj_t *finder_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int y) {
  lv_obj_t *l = lv_label_create(p);
  if (f)
    lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, 460);
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}
// heading: where the top of the display points (true north = 0), NAN without a compass
inline void finder_update(float heading) {
  if (!fnd.root)
    return;
  fnd.compass = !std::isnan(heading);
  const float h = fnd.compass ? heading : ui.heading;
  float az, el;
  char b[64];
  if (!finder_target(clock_now(), az, el)) {
    fnd.have = false;
    set_text_if(fnd.turn, "Out of view now");
    set_text_if(fnd.up, "");
    lv_obj_invalidate(fnd.arrow);
    return;
  }
  float rel = fmodf(az - h + 540.0f, 360.0f) - 180.0f;
  const bool moved = !fnd.have || fabsf(rel - fnd.rel) > 0.5f || fabsf(el - fnd.el) > 0.2f;
  fnd.have = true;
  fnd.rel = rel;
  fnd.el = el;
  if (fabsf(rel) < FIND_AHEAD_DEG)
    snprintf(b, sizeof(b), "Straight ahead");
  else
    snprintf(b, sizeof(b), "Turn %s %.0f\xC2\xB0", rel > 0 ? "right" : "left", fabsf(rel));
  set_text_if(fnd.turn, b);
  if (el < 0)
    snprintf(b, sizeof(b), "Below the horizon (%.0f\xC2\xB0)", el);
  else if (el > 80)
    snprintf(b, sizeof(b), "Straight up (%.0f\xC2\xB0)", el);
  else
    snprintf(b, sizeof(b), "Look up %.0f\xC2\xB0   (%s %.0f\xC2\xB0)", el, compass(az), az);
  set_text_if(fnd.up, b);
  set_text_if(fnd.note, fnd.compass ? "Hold the display flat, top edge pointing ahead"
                                    : "No compass: the arrow is relative to the top\nof the map (the Heading setting)");
  if (moved)
    lv_obj_invalidate(fnd.arrow);
}
inline void finder_open_for(int kind, int32_t id, const char *name) {
  finder_close();
  fnd.kind = kind;
  fnd.id = id;
  lv_obj_t *root = lv_obj_create(lv_layer_top());
  fnd.root = root;
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_set_style_bg_color(root, lv_color_hex(0x070B18), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
  const auto &w = ui.w;
  fnd.title = lv_label_create(root);
  if (w.title_font)
    lv_obj_set_style_text_font(fnd.title, w.title_font, 0);
  lv_obj_set_style_text_color(fnd.title, lv_color_hex(0xFF8A1F), 0);
  lv_label_set_long_mode(fnd.title, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(fnd.title, 350);
  char t[64];
  snprintf(t, sizeof(t), "Find: %s", name);
  lv_label_set_text(fnd.title, t);
  lv_obj_set_pos(fnd.title, 12, 14);
  lv_obj_t *b = lv_button_create(root);  // Close: top right, as on every other screen
  lv_obj_set_size(b, 96, 36);
  lv_obj_set_pos(b, 376, 4);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1A2547), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *bl = lv_label_create(b);
  if (w.title_font)
    lv_obj_set_style_text_font(bl, w.title_font, 0);
  lv_obj_set_style_text_color(bl, lv_color_hex(0xEEF2FF), 0);
  lv_label_set_text(bl, "Close");
  lv_obj_center(bl);
  lv_obj_add_event_cb(b, finder_close_cb, LV_EVENT_CLICKED, nullptr);
  fnd.arrow = lv_obj_create(root);
  lv_obj_remove_style_all(fnd.arrow);
  lv_obj_set_size(fnd.arrow, 280, 280);
  lv_obj_align(fnd.arrow, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_add_event_cb(fnd.arrow, finder_arrow_cb, LV_EVENT_DRAW_MAIN, nullptr);
  fnd.turn = finder_label(root, w.card_title_font ? w.card_title_font : w.title_font, 0xEEF2FF, 340);
  fnd.up = finder_label(root, w.card_font ? w.card_font : w.label_font, 0xC9D3F2, 372);
  fnd.note = finder_label(root, w.label_font, 0x7E8BB3, 420);
  finder_update(NAN);
}
inline void card_find_cb(lv_event_t *e) {
  lv_event_stop_bubbling(e);  // not a tap on the card (which closes it)
  if (ui.sel_kind < 0)
    return;
  char name[48];
  snprintf(name, sizeof(name), "%s", lv_label_get_text(ui.card_title));
  const int k = ui.sel_kind;
  const int32_t id = ui.sel_id;
  deselect();
  finder_open_for(k, id, name);
}

// ================================================================== UI-59..62 pictures
// Full screen, Close top right as on every other screen; tabs along the top for the Sun
// (NOAA GOES SUVI), the Moon (NASA Dial-A-Moon), the Earth (NOAA GOES GeoColor) and, when
// opened from a planet's card, that planet drawn on the device (UI-61). Photos are fetched
// when their tab opens, again after 10 min while open, with a bar and "520 of 1,150 KB".
constexpr int VIEW_PLANET = 10;  // sv.kind: 0..2 photos (net::ImgKind), VIEW_PLANET + p drawn
struct ImgView {
  lv_obj_t *root = nullptr, *img = nullptr, *msg = nullptr, *bar = nullptr, *cap1 = nullptr, *cap2 = nullptr;
  lv_obj_t *tab[4] = {}, *mlab[4] = {}, *east = nullptr, *west = nullptr;
  lv_obj_t *seg[2] = {};  // UI-62a: Globe / region, under the Earth picture
  lv_obj_t *credit = nullptr;  // UI-61b: the planet photo's credit, top left of the picture
  lv_obj_t *phase_mark = nullptr;  // UI-61g: the "Tonight's phase" switch
  lv_obj_t *flip[2] = {};  // UI-59c: < the picture before, > the latest (bottom corners of the picture)
  bool show_prev = false;
  int kind = net::IMG_SUN;
  int planet = -1;        // the planet tab (-1: none)
  lv_image_dsc_t dsc[2];  // alternated: a new source each update, so LVGL's image cache never shows old pixels
  int cur = 0;
  const void *dsc_px = nullptr;
  int32_t bar_val = -2;
  double drawn_at = 0;    // planet: last drawing (redrawn each minute: the moons move)
};
inline ImgView sv;
inline bool earth_regional = false;  // UI-62a: the Earth tab opens on the globe (4.5.31); the region is one tap away
inline const net::Region *my_region() { return net::region_for(config().lat, config().lon); }
inline int earth_kind() { return earth_regional && my_region() ? net::IMG_REGION : net::IMG_EARTH; }
inline bool planet_drawn = false;      // UI-61g: the drawn planet is only a fallback now (no photo)
inline bool planet_phase = true;       // UI-61g: "Tonight's phase" shading on the photo (kept between opens)
inline uint16_t *planet_px = nullptr;  // UI-61: the planet shown (photo with tonight's phase, or drawn)
inline int planet_px_of = -1;          // UI-61a: which planet's photo live.img_px[IMG_PLANET] holds
inline int planet_req_of = -1;         // the planet last asked for
struct ImgReq {
  double at = 0;      // last request (clock time)
  uint32_t seq = 0;   // live.img[k].seq when requested: still loading while unchanged
  bool asked = false;
};
inline ImgReq img_req[net::IMG_N];
constexpr double IMG_REFRESH_S = 600, IMG_RETRY_S = 90;
inline bool img_view_open() { return sv.root != nullptr; }
inline bool img_loading(int k) { return img_req[k].asked && live.img[k].seq == img_req[k].seq; }
inline void img_request(int k, double t) {
  ImgReq &q = img_req[k];
  if (!net::link_up || img_loading(k))
    return;
  const double wait = q.asked && !live.img[k].ok ? IMG_RETRY_S : IMG_REFRESH_S;
  if (q.asked && t - q.at < wait)
    return;
  if (!enqueue(k == net::IMG_SUN    ? JOB_SUNIMG
               : k == net::IMG_MOON ? JOB_MOONIMG
               : k == net::IMG_EARTH ? JOB_EARTHIMG
                                     : JOB_REGIONIMG))
    return;
  q.at = t;
  q.seq = live.img[k].seq;
  q.asked = true;
}
inline void img_view_close() {
  if (sv.root)
    lv_obj_delete(sv.root);
  lv_obj_t *const none[4] = {};
  sv.root = sv.img = sv.msg = sv.bar = sv.cap1 = sv.cap2 = sv.east = sv.west = sv.credit = nullptr;
  sv.seg[0] = sv.seg[1] = nullptr;
  sv.phase_mark = nullptr;
  sv.flip[0] = sv.flip[1] = nullptr;
  memcpy(sv.tab, none, sizeof(none));
  memcpy(sv.mlab, none, sizeof(none));
  sv.dsc_px = nullptr;
}
inline void img_view_close_cb(lv_event_t *) { img_view_close(); }
// "46%  -  520 of 1124 KB" (or the stage) for the picture being fetched
inline int img_progress(int k, char *line, size_t n) {  // returns percent, or -1 when not known
  if (!img_loading(k) || net::img_stage_kind != k || net::img_stage == net::STG_IDLE) {
    snprintf(line, n, "%s", "Waiting to start...");
    return -1;
  }
  const int32_t got = net::img_bytes, tot = net::img_total;
  switch (net::img_stage) {
    case net::STG_LIST:
      snprintf(line, n, "%s", "Finding the latest picture...");
      return -1;
    case net::STG_DECODE:
      snprintf(line, n, "%s", "Unpacking the picture...");
      return 100;
    default:
      if (tot > 0) {
        const int pc = (int) std::min<int64_t>(100, (int64_t) got * 100 / tot);
        snprintf(line, n, "%d%%  -  %ld of %ld KB", pc, (long) (got / 1024), (long) (tot / 1024));
        return pc;
      }
      snprintf(line, n, "%ld KB", (long) (got / 1024));
      return -1;
  }
}
// show `px` (IMG_PX square RGB565) in the picture; `fresh`: new pixels in the same buffer
inline void img_show(const uint16_t *px, bool fresh) {
  if (px == nullptr) {
    lv_obj_add_flag(sv.img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (sv.dsc_px != px) {
    for (auto &d : sv.dsc) {
      memset(&d, 0, sizeof(d));
      d.header.magic = LV_IMAGE_HEADER_MAGIC;
      d.header.cf = LV_COLOR_FORMAT_RGB565;
      d.header.w = net::IMG_PX;
      d.header.h = net::IMG_PX;
      d.header.stride = net::IMG_PX * 2;
      d.data_size = net::IMG_PX * net::IMG_PX * 2;
      d.data = (const uint8_t *) px;
    }
    sv.dsc_px = px;
    fresh = true;
  }
  if (fresh) {
    sv.cur ^= 1;  // same buffer, new pixels: a fresh source
    lv_image_set_src(sv.img, &sv.dsc[sv.cur]);
    lv_obj_invalidate(sv.img);
  }
  lv_obj_remove_flag(sv.img, LV_OBJ_FLAG_HIDDEN);
}
inline void img_bar(int32_t bv) {
  if (bv == sv.bar_val)
    return;
  sv.bar_val = bv;
  if (bv < 0) {
    lv_obj_add_flag(sv.bar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_bar_set_value(sv.bar, bv, LV_ANIM_OFF);
    lv_obj_remove_flag(sv.bar, LV_OBJ_FLAG_HIDDEN);
  }
}
// UI-61a: fetch the planet's photo once (it never changes); after a failure, again in 90 s
inline void planet_request(int p, double t) {
  ImgReq &q = img_req[net::IMG_PLANET];
  if (img_loading(net::IMG_PLANET) || (planet_px_of == p && live.img_px[net::IMG_PLANET]))  // built in (UI-61d)
    return;
  if (q.asked && planet_req_of == p && t - q.at < IMG_RETRY_S)
    return;
  net::planet_req = p;
  if (!enqueue(JOB_PLANETIMG))
    return;
  planet_req_of = p;
  q.at = t;
  q.seq = live.img[net::IMG_PLANET].seq;
  q.asked = true;
}
// moon letters on the root over Jupiter's strip, and the strip's E / W
inline void planet_marks(const pview::Marks &mk) {
  static const char *const L[4] = {"I", "E", "G", "C"};
  const int ox = (480 - net::IMG_PX) / 2, oy = 48;
  for (int k = 0; k < 4; k++) {
    if (!sv.mlab[k])
      continue;
    if (mk.n == 0 || !mk.shown[k]) {
      lv_obj_add_flag(sv.mlab[k], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    bool crowded = false;
    for (int j = 0; j < k; j++)
      crowded |= mk.shown[j] && abs(mk.x[j] - mk.x[k]) < 14 && abs(mk.y[j] - mk.y[k]) < 20;
    lv_label_set_text(sv.mlab[k], L[k]);
    lv_obj_set_pos(sv.mlab[k], ox + mk.x[k] - 5, oy + mk.y[k] + (crowded ? 6 : -24));
    lv_obj_remove_flag(sv.mlab[k], LV_OBJ_FLAG_HIDDEN);
  }
  const bool strip = mk.strip_y >= 0;
  for (lv_obj_t *o : {sv.east, sv.west})
    if (o) {
      if (strip)
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  if (strip) {
    lv_obj_set_pos(sv.east, ox + mk.strip_x0, oy + mk.strip_y + 14);
    lv_obj_set_pos(sv.west, ox + mk.strip_x1 - 14, oy + mk.strip_y + 14);
  }
}
// UI-61 / 61a: the planet: its NASA photo with tonight's phase (Mercury, Venus, Mars) or
// Jupiter's moons as they are now; drawn instead when the photo cannot be had
// UI-61e: the photos are stored at display size (sky_photos.h), so showing one is a plain
// 1:1 JPEG decode (~0.1 s) straight into its place, right here in the loop: no job, no
// queue behind downloads, no waiting message. The last one decoded is kept.
inline uint16_t *planet_src = nullptr;
inline int planet_src_of = -1;
inline bool planet_photo_ready(int p) {
  if (planet_src_of == p && planet_src)
    return true;
  const photos::Photo ph = photos::get(p);
  if (ph.data == nullptr || ph.w <= 0 || ph.w > net::IMG_PX || ph.h > net::IMG_PX)
    return false;
  if (planet_src == nullptr)
    planet_src = (uint16_t *) heap_caps_malloc(net::IMG_PX * net::IMG_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (planet_src == nullptr)
    return false;
  const uint16_t bg = (uint16_t) ((7 >> 3) << 11 | (11 >> 2) << 5 | (24 >> 3));
  for (int k = 0; k < net::IMG_PX * net::IMG_PX; k++)
    planet_src[k] = bg;
  const int ox = (net::IMG_PX - ph.w) / 2, oy = p == planets::JUPITER ? 6 : (net::IMG_PX - ph.h) / 2;
  const int64_t t0 = ui_us();
  if (const char *e = skyjpg::decode_copy(ph.data, ph.len, planet_src + (size_t) oy * net::IMG_PX + ox, net::IMG_PX,
                                          ph.w, ph.h)) {
    ESP_LOGW(UI_TAG, "planet %d photo: %s", p, e);
    planet_src_of = -1;
    return false;
  }
  ESP_LOGI(UI_TAG, "planet %d photo shown (%dx%d, %u bytes, %.0f ms)", p, ph.w, ph.h, (unsigned) ph.len,
           (ui_us() - t0) / 1000.0);
  planet_src_of = p;
  return true;
}
inline void planet_view_update(double t) {
  const int p = sv.kind - VIEW_PLANET;
  const int K = net::IMG_PLANET;
  const bool photo = !planet_drawn && planet_photo_ready(p);
  const bool loading = false;
  const bool drawn = !photo;
  const pview::View v = pview::compute(p, astro::jd(t));
  if ((photo || drawn) && (t - sv.drawn_at >= 60 || sv.drawn_at == 0)) {
    if (planet_px == nullptr)
      planet_px = (uint16_t *) heap_caps_malloc(net::IMG_PX * net::IMG_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (planet_px == nullptr)
      return;
    pview::Marks mk;
    if (photo) {
      memcpy(planet_px, planet_src, net::IMG_PX * net::IMG_PX * 2);
      pview::shade_photo(v, planet_px, net::IMG_PX, net::IMG_PX, planet_phase && p <= planets::MARS);  // phase: inner planets, Mars
      if (p == planets::JUPITER)
        pview::draw_strip(v, planet_px, net::IMG_PX, net::IMG_PX, mk, 0.80f * net::IMG_PX);
    } else {
      pview::render(v, planet_px, net::IMG_PX, net::IMG_PX, mk);
    }
    sv.drawn_at = t;
    img_show(planet_px, true);
    planet_marks(mk);
  }
  if (!photo && !drawn)
    lv_obj_add_flag(sv.img, LV_OBJ_FLAG_HIDDEN);
  char prog[48], m[128] = "";
  const int pc = loading ? img_progress(K, prog, sizeof(prog)) : -1;
  if (loading)
    snprintf(m, sizeof(m), "Preparing the photo\nof %s", planets::name(p));
  else if (!photo && !drawn)
    snprintf(m, sizeof(m), "%s", "Waiting for the photo...");
  set_text_if(sv.msg, m);
  img_bar(loading ? std::max(0, pc) : -1);
  char c1[112];
  const char *nm = planets::name(p);
  if (p == planets::SATURN)
    snprintf(c1, sizeof(c1), "%s: rings tilted %.1f\xC2\xB0 (%s face), %.1f\" across", nm, fabsf(v.earth_lat),
             v.earth_lat >= 0 ? "north" : "south", v.diam);
  else if (p == planets::JUPITER)
    snprintf(c1, sizeof(c1), "%s: %.1f\" across   I Io  E Europa  G Ganymede  C Callisto", nm, v.diam);
  else
    snprintf(c1, sizeof(c1), "%s: %.0f%% lit%s, %.1f\" across", nm, v.lit * 100,
             v.lit < 0.45f ? " (crescent)" : v.lit < 0.55f ? " (half)" : v.lit < 0.97f ? " (gibbous)" : "", v.diam);
  // UI-61b: the switch takes the second caption line; the credit sits on the picture
  char cr[64] = "";
  if (photo)
    snprintf(cr, sizeof(cr), "%s%s", net::planet_photo(p).credit, p == planets::SATURN ? ", 2004" : "");
  else if (drawn && !planet_drawn)
    snprintf(cr, sizeof(cr), "%s", "Photo unavailable: drawn");
  if (sv.credit)
    set_text_if(sv.credit, cr);
  set_text_if(sv.cap1, c1);
}
inline void img_view_update(double t);
// UI-59c: < and > between the picture before and the latest
// UI-59d: < steps back a frame at a time (the frame before is fetched when it isn't here:
// the Sun from NOAA's list, the Moon the hour before, the Earth and region 10 minutes
// before); > returns to the latest.
struct BackReq {
  bool asked = false;
  uint32_t seq = 0;  // live.back.seq when asked: still loading while unchanged
  int kind = -1;
};
inline BackReq back_req;
inline bool back_loading(int k) { return back_req.asked && back_req.kind == k && live.back.seq == back_req.seq; }
inline bool back_failed(int k) {
  return back_req.asked && back_req.kind == k && live.back.seq != back_req.seq && live.back_kind == k && !live.back.ok;
}
inline void img_flip_cb(lv_event_t *e) {
  const int i = (int) (intptr_t) lv_event_get_user_data(e), k = sv.kind;
  if (i == 1) {
    sv.show_prev = false;
  } else if (k < net::IMG_LIVE_N && !back_loading(k)) {
    const bool have_prev =
        live.img_prev_px[k] && live.img_prev[k].obs > 0 && live.img_prev[k].obs < live.img[k].obs;
    if (!sv.show_prev && have_prev) {
      sv.show_prev = true;
    } else {
      const double ref = sv.show_prev ? live.img_prev[k].obs : live.img[k].obs;
      if (ref > 0) {
        net::back_kind = k;
        net::back_before = ref;
        const uint32_t seq = live.back.seq;
        if (enqueue(JOB_BACK)) {
          back_req = {true, seq, k};
          sv.show_prev = true;
        }
      }
    }
  }
  sv.dsc_px = nullptr;  // re-point the picture
  img_view_update(clock_now());
}
inline void img_flip_state(bool avail, bool fetching = false) {
  for (int i = 0; i < 2; i++) {
    if (!sv.flip[i])
      continue;
    if (!avail) {
      lv_obj_add_flag(sv.flip[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(sv.flip[i], LV_OBJ_FLAG_HIDDEN);
    const bool usable = i == 0 ? !fetching : sv.show_prev;  // < always steps back; > back to the latest
    lv_obj_set_style_opa(sv.flip[i], usable ? LV_OPA_COVER : LV_OPA_40, 0);
    if (usable)
      lv_obj_add_flag(sv.flip[i], LV_OBJ_FLAG_CLICKABLE);
    else
      lv_obj_remove_flag(sv.flip[i], LV_OBJ_FLAG_CLICKABLE);
  }
}
inline void img_view_update(double t) {
  if (!sv.root)
    return;
  if (sv.kind >= VIEW_PLANET) {
    planet_view_update(t);
    return;
  }
  const int k = sv.kind;
  img_request(k, t);
  const bool live_kind = k < net::IMG_LIVE_N;
  if (!live_kind)
    sv.show_prev = false;
  const bool before = sv.show_prev;
  const bool fetching = live_kind && back_loading(k);  // UI-59d: an older frame on its way
  const bool loading = before ? fetching : img_loading(k);
  const net::ImageInfo &in = before ? live.img_prev[k] : live.img[k];
  uint16_t *px = before ? live.img_prev_px[k] : live.img_px[k];
  const bool have = px != nullptr;  // a picture has been decoded (it stays until a newer one)
  img_show(px, (before ? live.img_prev_new[k] : live.img_new[k]) || sv.dsc_px != px);
  (before ? live.img_prev_new[k] : live.img_new[k]) = false;
  img_flip_state(live_kind, fetching);
  // message and bar (only until the first picture; later updates show in the caption)
  char prog[48];
  const int pc = loading ? img_progress(k, prog, sizeof(prog)) : -1;
  char m[128] = "";
  if (!have) {
    if (loading)
      snprintf(m, sizeof(m), "Downloading the %s picture\nfrom %s\n\n%s", before ? "earlier" : "latest",
               k == net::IMG_MOON ? "NASA" : "NOAA", prog);
    else if (before)
      snprintf(m, sizeof(m), "%s", back_failed(k) && live.back.err[0] ? live.back.err : "No earlier picture");
    else
      snprintf(m, sizeof(m), "%s", !net::link_up ? "No network" : in.err[0] ? in.err : "Waiting for the picture...");
  }
  set_text_if(sv.msg, m);
  img_bar(!have && loading ? std::max(0, pc) : -1);
  char c1[112], c2[112], tail[48] = "";
  if (before && fetching) {
    snprintf(tail, sizeof(tail), " - getting an earlier one %d%%", std::max(0, pc));
  } else if (before && back_failed(k)) {
    snprintf(tail, sizeof(tail), "%s", " - none earlier");
  } else if (before) {
    snprintf(tail, sizeof(tail), "%s", " - earlier picture");
  } else if (have && k == net::IMG_SUN && in.shadow && !img_loading(k)) {
    snprintf(tail, sizeof(tail), "%s", " - GOES in Earth's shadow");  // UI-59: newer frames dark
  } else if (have && img_loading(k)) {
    if (pc >= 0)
      snprintf(tail, sizeof(tail), " - updating %d%%", pc);
    else
      snprintf(tail, sizeof(tail), "%s", " - updating");
  } else if (have && !in.ok && in.err[0]) {
    snprintf(tail, sizeof(tail), "%s", " - update failed");
  }
  char hm[12];
  if (k == net::IMG_MOON) {
    if (!std::isnan(in.phase)) {
      char dd[40];  // "372,667"
      const long v = lroundf(dist(in.dist_km));
      snprintf(dd, sizeof(dd), "%ld,%03ld", v / 1000, v % 1000);
      if (v >= 1000)
        snprintf(c1, sizeof(c1), "%.0f%% lit, %.1f days old, %s %s away", in.phase, in.age, dd, dist_unit());
      else
        snprintf(c1, sizeof(c1), "%.0f%% lit, %.1f days old", in.phase, in.age);
    } else {
      snprintf(c1, sizeof(c1), "%s", "NASA Dial-A-Moon");
    }
    if (have && in.obs > 0) {
      local_hm((int64_t) in.obs, hm, sizeof(hm));
      snprintf(c2, sizeof(c2), "NASA Dial-A-Moon for %s, %s up%s", hm, in.south_up ? "south" : "north", tail);
    } else {
      snprintf(c2, sizeof(c2), "%s", "Rendered hourly from Lunar Reconnaissance Orbiter maps");
    }
  } else {
    const bool earth = k == net::IMG_EARTH || k == net::IMG_REGION;
    if (k == net::IMG_SUN)
      snprintf(c1, sizeof(c1), "%.11s SUVI, 30.4 nm ultraviolet", in.src[0] ? in.src : "GOES");
    else if (k == net::IMG_REGION && my_region())
      snprintf(c1, sizeof(c1), "%s from %.11s: day in colour, night lights", my_region()->name, in.src[0] ? in.src : "GOES");
    else
      snprintf(c1, sizeof(c1), "%.11s GeoColor: day in colour, night lights", in.src[0] ? in.src : "GOES");
    if (earth && sv.seg[0]) {  // UI-62a: one caption line; the switch takes the second
      if (have && in.obs > 0) {
        char ago[16], c1b[112];
        local_hm((int64_t) in.obs, hm, sizeof(hm));
        fmt_dur(t - in.obs, ago, sizeof(ago));
        snprintf(c1b, sizeof(c1b), "%.11s %s, %s ago%s", in.src[0] ? in.src : "GOES", hm, ago, tail);
        if (k == net::IMG_REGION && my_region()) {
          char c1c[140];
          snprintf(c1c, sizeof(c1c), "%s: %s", my_region()->name, c1b);
          set_text_if(sv.cap1, c1c);
        } else {
          set_text_if(sv.cap1, c1b);
        }
      } else {
        set_text_if(sv.cap1, c1);
      }
      return;
    }
    if (have && in.obs > 0) {
      char ago[16];
      local_hm((int64_t) in.obs, hm, sizeof(hm));
      fmt_dur(t - in.obs, ago, sizeof(ago));
      snprintf(c2, sizeof(c2), "Taken %s, %s ago%s", hm, ago, tail);
    } else if (have) {
      snprintf(c2, sizeof(c2), "The latest picture%s", tail);
    } else {
      snprintf(c2, sizeof(c2), "%s", k == net::IMG_SUN ? "Bright: active regions   Edge: prominences"
                                                         : "Seen from 35,786 km above the equator");
    }
  }
  set_text_if(sv.cap1, c1);
  set_text_if(sv.cap2, c2);
}
inline void img_view_open_now(int kind, int planet = -2);
inline void img_seg_cb(lv_event_t *e) {  // UI-62a: Globe / region
  const int kind = (int) (intptr_t) lv_event_get_user_data(e);
  earth_regional = kind == net::IMG_REGION;
  if (kind != sv.kind)
    img_view_open_now(kind, sv.planet);
}
inline void img_view_update(double t);
inline void img_phase_cb(lv_event_t *e) {  // UI-61g: "Tonight's phase" on / off
  planet_phase = lv_obj_has_state((lv_obj_t *) lv_event_get_target(e), LV_STATE_CHECKED);
  sv.drawn_at = 0;  // redraw the photo now
  img_view_update(clock_now());
}
inline void img_tab_cb(lv_event_t *e) {
  const int kind = (int) (intptr_t) lv_event_get_user_data(e);
  if (kind != sv.kind)
    img_view_open_now(kind, sv.planet);
}
inline void img_view_open_now(int kind, int planet) {
  if (planet == -2)
    planet = kind >= VIEW_PLANET ? kind - VIEW_PLANET : -1;
  img_view_close();
  finder_close();
  sv.kind = kind;
  sv.planet = planet;
  sv.drawn_at = 0;
  const auto &w = ui.w;
  lv_obj_t *root = lv_obj_create(lv_layer_top());
  sv.root = root;
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_set_style_bg_color(root, lv_color_hex(0x070B18), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
  auto button = [&](int x, int wd, const char *text, bool on) {
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, wd, 36);
    lv_obj_set_pos(b, x, 4);
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? 0x24356A : 0x121A36), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    if (w.title_font)
      lv_obj_set_style_text_font(l, w.title_font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(on ? 0xFF8A1F : 0xC9D3F2), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
  };
  // tabs: Sun, Moon, Earth (+ the planet); Close at the top right as everywhere
  struct T {
    int kind, x, w;
    const char *name;
  };
  const T tabs[4] = {{net::IMG_SUN, 8, 80, "Sun"},
                     {net::IMG_MOON, 92, 80, "Moon"},
                     {kind == net::IMG_REGION || kind == net::IMG_EARTH ? kind : earth_kind(), 176, 84, "Earth"},
                     {planet >= 0 ? VIEW_PLANET + planet : -1, 264, 100, planet >= 0 ? planets::name(planet) : ""}};
  for (int i = 0; i < 4; i++) {
    if (tabs[i].kind < 0)
      continue;
    sv.tab[i] = button(tabs[i].x, tabs[i].w, tabs[i].name, tabs[i].kind == kind);
    lv_obj_add_event_cb(sv.tab[i], img_tab_cb, LV_EVENT_CLICKED, (void *) (intptr_t) tabs[i].kind);
  }
  lv_obj_t *b = button(376, 96, "Close", false);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1A2547), 0);
  lv_obj_add_event_cb(b, img_view_close_cb, LV_EVENT_CLICKED, nullptr);
  sv.img = lv_image_create(root);
  lv_obj_set_size(sv.img, net::IMG_PX, net::IMG_PX);
  lv_obj_set_pos(sv.img, (480 - net::IMG_PX) / 2, 48);
  lv_obj_add_flag(sv.img, LV_OBJ_FLAG_HIDDEN);
  sv.msg = finder_label(root, w.card_font ? w.card_font : w.label_font, 0xC9D3F2, 150);
  sv.bar = lv_bar_create(root);  // download progress, below the message
  lv_obj_set_size(sv.bar, 300, 10);
  lv_obj_align(sv.bar, LV_ALIGN_TOP_MID, 0, 236);
  lv_bar_set_range(sv.bar, 0, 100);
  lv_obj_set_style_bg_color(sv.bar, lv_color_hex(C_BAR_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sv.bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sv.bar, lv_color_hex(C_BAR), LV_PART_INDICATOR);  // UI-71 (was orange)
  lv_obj_set_style_radius(sv.bar, 5, LV_PART_MAIN);
  lv_obj_set_style_radius(sv.bar, 5, LV_PART_INDICATOR);
  lv_obj_add_flag(sv.bar, LV_OBJ_FLAG_HIDDEN);
  sv.bar_val = -2;
  if (kind == VIEW_PLANET + planets::JUPITER) {  // UI-61: moon letters and the strip's E / W
    for (auto &l : sv.mlab) {
      l = lv_label_create(root);
      if (w.label_font)
        lv_obj_set_style_text_font(l, w.label_font, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(0xC9D3F2), 0);
      lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t **o : {&sv.east, &sv.west}) {
      *o = lv_label_create(root);
      if (w.label_font)
        lv_obj_set_style_text_font(*o, w.label_font, 0);
      lv_obj_set_style_text_color(*o, lv_color_hex(0x7E8BB3), 0);
      lv_label_set_text(*o, o == &sv.east ? "E" : "W");
      lv_obj_add_flag(*o, LV_OBJ_FLAG_HIDDEN);
    }
  }
  sv.cap1 = finder_label(root, w.label_font, 0xC9D3F2, 414);
  sv.cap2 = finder_label(root, w.label_font, 0x7E8BB3, 442);
  sv.show_prev = false;
  if (kind < net::IMG_LIVE_N) {  // UI-59c: < before / > latest, in the picture's bottom corners
    for (int i = 0; i < 2; i++) {
      lv_obj_t *fb = lv_button_create(root);
      sv.flip[i] = fb;
      lv_obj_set_size(fb, 48, 48);
      lv_obj_set_pos(fb, i == 0 ? 66 : 366, 354);
      lv_obj_set_style_radius(fb, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_color(fb, lv_color_hex(0x24356A), 0);
      lv_obj_set_style_shadow_width(fb, 0, 0);
      lv_obj_set_style_pad_all(fb, 0, 0);
      lv_obj_t *l = lv_label_create(fb);
      if (w.title_font)
        lv_obj_set_style_text_font(l, w.title_font, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(0xEEF2FF), 0);
      lv_label_set_text(l, i == 0 ? "<" : ">");
      lv_obj_center(l);
      lv_obj_add_event_cb(fb, img_flip_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
      lv_obj_add_flag(fb, LV_OBJ_FLAG_HIDDEN);
    }
  }
  const bool pl = kind >= VIEW_PLANET;
  if (pl) {  // UI-61b: the photo's credit, top left of the picture
    sv.credit = lv_label_create(root);
    if (w.label_font)
      lv_obj_set_style_text_font(sv.credit, w.label_font, 0);
    lv_obj_set_style_text_color(sv.credit, lv_color_hex(0x7E8BB3), 0);
    lv_label_set_text(sv.credit, "");
    lv_obj_set_pos(sv.credit, 64, 52);
  }
  if (pl && kind - VIEW_PLANET <= planets::MARS) {  // UI-61g: "Tonight's phase" switch, lower right
    lv_obj_t *sw = lv_switch_create(root);  // as the settings switches (theme look)
    lv_obj_set_size(sw, 56, 28);
    lv_obj_set_pos(sw, 408, 442);  // lower right
    if (planet_phase)
      lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, img_phase_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    sv.phase_mark = sw;
    lv_obj_t *l = lv_label_create(root);
    if (w.label_font)
      lv_obj_set_style_text_font(l, w.label_font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC9D3F2), 0);
    lv_label_set_text(l, "Tonight's phase");
    lv_obj_align_to(l, sw, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  }
  if (pl)
    lv_obj_add_flag(sv.cap2, LV_OBJ_FLAG_HIDDEN);  // the caption is one line; the credit sits on the photo
  if (!pl && (kind == net::IMG_EARTH || kind == net::IMG_REGION) && my_region()) {  // UI-62a Globe | region
    const char *names[2] = {"Globe", my_region()->name};
    for (int i = 0; i < 2; i++) {
      const int want = i == 0 ? net::IMG_EARTH : net::IMG_REGION;
      lv_obj_t *sb = lv_button_create(root);
      sv.seg[i] = sb;
      lv_obj_set_size(sb, 128, 32);
      lv_obj_set_pos(sb, i == 0 ? 108 : 244, 442);
      const bool on = kind == want;
      lv_obj_set_style_bg_color(sb, lv_color_hex(on ? 0x24356A : 0x121A36), 0);
      lv_obj_set_style_shadow_width(sb, 0, 0);
      lv_obj_set_style_pad_all(sb, 0, 0);
      lv_obj_t *l = lv_label_create(sb);
      if (w.label_font)
        lv_obj_set_style_text_font(l, w.label_font, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(on ? 0xFF8A1F : 0xC9D3F2), 0);
      lv_label_set_text(l, names[i]);
      lv_obj_center(l);
      lv_obj_add_event_cb(sb, img_seg_cb, LV_EVENT_CLICKED, (void *) (intptr_t) want);
    }
    lv_obj_add_flag(sv.cap2, LV_OBJ_FLAG_HIDDEN);
  }
  img_view_update(clock_now());
}
inline void card_img_cb(lv_event_t *e) {
  lv_event_stop_bubbling(e);  // not a tap on the card (which closes it)
  const int kind = ui.sel_kind == K_MOON     ? (int) net::IMG_MOON
                   : ui.sel_kind == K_PLANET ? VIEW_PLANET + ui.sel_id
                                             : (int) net::IMG_SUN;
  deselect();
  img_view_open_now(kind);
}

// UI-36: the almanac Moon picture (lower right) opens the Moon card too
inline void moon_icon_click_cb(lv_event_t *e);

inline void select_object(int kind, int32_t id) {
  ui.sel_kind = kind;
  ui.sel_id = id;
  ui.sel_since = clock_now();
  static const uint32_t COL[13] = {C_LEO, 0x8FA8F0, C_ISS, C_GEO, C_SUN, C_MOON, 0, 0, 0xFFF1B8, C_METEOR, C_CSS, C_DSO, C_COMET};
  const uint32_t col = kind == K_PLANET ? C_PLANET[id] : kind == K_CONST ? C_CONST_TITLE : kind == K_INFO ? info_alert.col : COL[kind];
  lv_obj_set_style_text_color(ui.card_title, lv_color_hex(col), 0);
  if (ui.card_icon)
    lv_obj_set_style_text_color(ui.card_icon, lv_color_hex(col), 0);
  lv_obj_remove_flag(ui.card, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ui.card);
  card_update(ui.sel_since);
}

inline bool card_open() { return ui.sel_kind >= 0; }  // UI-24
// UI-72: after a minute untouched, what is open over the map closes (the picture viewer, Find,
// the time scrubber, a details card); the pages are handled in the YAML
inline void finder_close();
inline void img_view_close();
inline bool finder_open();
inline bool img_view_open();
inline void scrub_close();
inline void idle_close() {
  if (img_view_open())
    img_view_close();
  if (finder_open())
    finder_close();
  if (ui.scrub_on)
    scrub_close();
  if (card_open())
    deselect();
}

inline void moon_icon_click_cb(lv_event_t *e) {  // UI-36
  if (card_open())
    deselect();  // UI-24: with a card up, a touch only closes it
  else
    select_object(K_MOON, 0);
  lv_event_stop_bubbling(e);
}

inline void sky_click_cb(lv_event_t *e) {
  if (scrub_swallow_click) {  // UI-53: the release of the long press that opened the slider
    scrub_swallow_click = false;
    lv_event_stop_bubbling(e);
    return;
  }
  if (ui.scrub_on)
    ui.scrub_touched = clock_now();
  if (ui.sel_kind >= 0) {  // UI-24: with the card up, any touch only closes it
    deselect();
    lv_event_stop_bubbling(e);
    return;
  }
  lv_indev_t *in = lv_indev_active();
  if (in == nullptr)
    return;
  lv_point_t p;
  lv_indev_get_point(in, &p);
  lv_area_t box;
  lv_obj_get_coords(ui.w.sky, &box);
  const int x = p.x - box.x1, y = p.y - box.y1;
  Marker *best = nullptr;
  int kind = -1, best_d2 = TAP_R * TAP_R;
  auto consider = [&](Marker &m, int k, int bonus) {
    if (m.id < 0 || !m.up)
      return;
    const int dx = m.dx - x, dy = m.dy - y, d2 = dx * dx + dy * dy - bonus;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = &m;
      kind = k;
    }
  };
  // UI-36: the Sun and Moon, anywhere on their discs (a satellite right there still wins)
  int body_kind = -1;
  auto consider_body = [&](bool shown, int tlx, int tly, int half, int k) {
    if (!shown)
      return;
    const int dx = tlx + half - x, dy = tly + half - y, d2 = dx * dx + dy * dy - half * half / 2;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = nullptr;
      kind = k;
      body_kind = k;
    }
  };
  if (!ui.scrub_on) {  // UI-53: their cards are for now
    consider_body(ui.sun_shown, ui.sun_x, ui.sun_y, SUN_HALO_PX / 2, K_SUN);
    consider_body(ui.moon_shown, ui.moon_x, ui.moon_y, MOON_PX / 2, K_MOON);
  }
  // UI-40a: planets, a little easier to hit than their 14 px picture
  int planet_hit = -1;
  if (ui.planets_on)
    for (int p = 0; p < planets::N_PLANETS; p++) {
      if (!ui.planet_shown[p])
        continue;
      const int H = picons::SMALL / 2, dx = ui.planet_x[p] + H - x, dy = ui.planet_y[p] + H - y;
      const int d2 = dx * dx + dy * dy - 40;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = nullptr;
        kind = K_PLANET;
        body_kind = -1;
        planet_hit = p;
      }
    }
  // UI-63 comets: the head, as easy to hit as a planet
  int comet_hit = -1;
  for (int k = 0; k < Ui::MAX_COMETS; k++) {
    if (ui.comet_idx[k] < 0 || !ui.comet_shown[k])
      continue;
    const int dx = ui.comet_x[k] - x, dy = ui.comet_y[k] - y, d2 = dx * dx + dy * dy - 40;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = nullptr;
      kind = K_COMET;
      body_kind = -1;
      comet_hit = ui.comet_idx[k];
    }
  }
  // UI-50 named bright stars and the UI-47 meteor radiant: easy to hit, but a satellite
  // or planet on top of them wins
  int point_hit = -1, point_kind = -1;
  if (ui.stars_shown) {
    const double tv = clock_now() + (ui.scrub_on ? ui.scrub_s : 0);
    for (int i = 0; i < ev::N_BRIGHT; i++) {
      float az, el, sx, sy;
      view_azel(ev::BRIGHT[i].ra, ev::BRIGHT[i].dec, tv, az, el);
      if (el < 0)
        continue;
      project({az, el}, sx, sy);
      const int dx = (int) lroundf(sx) - x, dy = (int) lroundf(sy) - y, d2 = dx * dx + dy * dy + 10;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = nullptr;
        kind = K_STAR;
        body_kind = -1;
        point_hit = i;
        point_kind = K_STAR;
      }
    }
  }
  if (ui.stars_shown)  // UI-56 deep-sky objects
    for (const auto &p : ui.dso_pts) {
      const int dx = p.x - x, dy = p.y - y, d2 = dx * dx + dy * dy + 10;
      if (d2 < best_d2) {
        best_d2 = d2;
        best = nullptr;
        kind = K_DSO;
        body_kind = -1;
        point_hit = p.idx;
        point_kind = K_DSO;
      }
    }
  if (ui.radiant >= 0 && ui.radiant_up) {
    const int dx = ui.radiant_x - x, dy = ui.radiant_y - y, d2 = dx * dx + dy * dy;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = nullptr;
      kind = K_SHOWER;
      body_kind = -1;
      point_hit = ui.radiant;
      point_kind = K_SHOWER;
    }
  }
  consider(ui.css, K_CSS, 50);  // UI-52
  consider(ui.iss, K_ISS, 60);  // the ISS wins a close call
  for (auto &m : ui.sats)
    consider(m, K_SAT, 20);
  for (auto &m : ui.starlink)
    consider(m, K_STARLINK, 0);
  int32_t geo_id = -1;
  for (const auto &g : ui.geo_pts) {  // UI-28: GEO dots lose close calls
    const int dx = g.x - x, dy = g.y - y, d2 = dx * dx + dy * dy + 20;
    if (d2 < best_d2) {
      best_d2 = d2;
      best = nullptr;
      kind = K_GEO;
      geo_id = live.geo[g.idx].id;
    }
  }
  // UI-46: nothing else here: a constellation name tag under the finger
  int name_hit = -1;
  if (kind < 0)
    for (const auto &nt : ui.names) {
      if (nt.shown && x >= nt.x - 6 && x <= nt.x + nt.w + 6 && y >= nt.y - 6 && y <= nt.y + nt.h + 6) {
        name_hit = nt.idx;
        kind = K_CONST;
        break;
      }
    }
  if (kind == K_CONST && name_hit >= 0) {
    select_object(K_CONST, name_hit);
    lv_event_stop_bubbling(e);
  } else if (best == nullptr && body_kind >= 0 && kind == body_kind) {
    select_object(body_kind, 0);
    lv_event_stop_bubbling(e);
  } else if (best == nullptr && (kind == K_STAR || kind == K_SHOWER || kind == K_DSO) && point_hit >= 0 &&
             kind == point_kind) {
    select_object(kind, point_hit);
    lv_event_stop_bubbling(e);
  } else if (best == nullptr && kind == K_PLANET && planet_hit >= 0) {
    select_object(K_PLANET, planet_hit);
    lv_event_stop_bubbling(e);
  } else if (best == nullptr && kind == K_COMET && comet_hit >= 0) {
    select_object(K_COMET, comet_hit);  // UI-63
    lv_event_stop_bubbling(e);
  } else if (kind == K_GEO && geo_id >= 0) {
    select_object(K_GEO, geo_id);
    lv_event_stop_bubbling(e);
  } else if (best != nullptr) {
    select_object(kind, best->id);
    lv_event_stop_bubbling(e);
  }
  else if (ui.scrub_on)
    lv_event_stop_bubbling(e);  // UI-53: the page stays while scrubbing
  // else: bubbles to the page, which flips (UI-2)
}

inline void card_click_cb(lv_event_t *) { deselect(); }

inline void card_build() {
  lv_obj_t *sky = ui.w.sky;
  lv_obj_add_flag(sky, (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
  lv_obj_add_event_cb(sky, sky_click_cb, LV_EVENT_CLICKED, nullptr);

  ui.sel_ring = plain(sky);
  lv_obj_set_style_radius(ui.sel_ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ui.sel_ring, 2, 0);
  lv_obj_set_style_border_color(ui.sel_ring, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(ui.sel_ring, LV_OPA_COVER, 0);
  lv_obj_add_flag(ui.sel_ring, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *page = lv_obj_get_parent(sky);
  ui.card = lv_obj_create(page);
  lv_obj_remove_style_all(ui.card);
  lv_obj_set_size(ui.card, ui.w.card_font ? 352 : 316, LV_SIZE_CONTENT);  // wider for the 14 pt body
  lv_obj_set_style_bg_color(ui.card, lv_color_hex(0x101B3D), 0);
  lv_obj_set_style_bg_opa(ui.card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(ui.card, lv_color_hex(0x3A4E86), 0);
  lv_obj_set_style_border_width(ui.card, 1, 0);
  lv_obj_set_style_radius(ui.card, 10, 0);
  lv_obj_set_style_pad_all(ui.card, 10, 0);
  lv_obj_set_style_pad_row(ui.card, 6, 0);
  lv_obj_set_flex_flow(ui.card, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(ui.card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(ui.card, (lv_obj_flag_t) (LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN));
  lv_obj_add_event_cb(ui.card, card_click_cb, LV_EVENT_CLICKED, nullptr);
  ui.card_title = lv_label_create(ui.card);
  if (ui.w.title_font)
    lv_obj_set_style_text_font(ui.card_title, ui.w.card_title_font ? ui.w.card_title_font : ui.w.title_font, 0);
  lv_label_set_long_mode(ui.card_title, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(ui.card_title, lv_pct(100));
  if (ui.w.card_icon_font) {  // UI-30: type icon, top right, outside the flex column
    lv_obj_set_style_pad_right(ui.card_title, ui.w.card_icon_big ? 50 : 30, 0);
    ui.card_icon = lv_label_create(ui.card);
    lv_obj_add_flag(ui.card_icon, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_style_text_font(ui.card_icon, ui.w.card_icon_big ? ui.w.card_icon_big : ui.w.card_icon_font, 0);
    lv_obj_align(ui.card_icon, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_label_set_text(ui.card_icon, "");
  }
  ui.card_moon = lv_canvas_create(ui.card);  // UI-36: Moon phase picture, top right
  lv_obj_remove_style_all(ui.card_moon);
  lv_obj_add_flag(ui.card_moon, (lv_obj_flag_t) (LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
  lv_obj_remove_flag(ui.card_moon, LV_OBJ_FLAG_CLICKABLE);
  if (lv_draw_buf_t *mb = lv_draw_buf_create(40, 40, LV_COLOR_FORMAT_ARGB8888, 0)) {
    lv_draw_buf_clear(mb, nullptr);
    lv_canvas_set_draw_buf(ui.card_moon, mb);
  }
  lv_obj_align(ui.card_moon, LV_ALIGN_TOP_RIGHT, CARD_ICON_DX, -2);
  ui.card_flag = lv_image_create(ui.card);  // UI-34
  lv_obj_add_flag(ui.card_flag, (lv_obj_flag_t) (LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
  lv_obj_remove_flag(ui.card_flag, LV_OBJ_FLAG_CLICKABLE);
  ui.card_body = lv_label_create(ui.card);
  if (ui.w.label_font)
    lv_obj_set_style_text_font(ui.card_body, ui.w.card_font ? ui.w.card_font : ui.w.label_font, 0);
  lv_obj_set_style_text_color(ui.card_body, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_line_space(ui.card_body, 3, 0);
  lv_obj_set_width(ui.card_body, lv_pct(100));  // UI-40a: the planet story wraps
  lv_label_set_long_mode(ui.card_body, LV_LABEL_LONG_WRAP);
  // UI-57: "Find" at the bottom right of every card
  lv_obj_set_style_pad_bottom(ui.card, 52, 0);
  lv_obj_t *fb = lv_button_create(ui.card);
  card_find_btn = fb;
  lv_obj_add_flag(fb, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(fb, 104, 36);
  lv_obj_align(fb, LV_ALIGN_BOTTOM_RIGHT, 0, 42);
  lv_obj_set_style_bg_color(fb, lv_color_hex(0x24356A), 0);
  lv_obj_set_style_shadow_width(fb, 0, 0);
  lv_obj_t *fl = lv_label_create(fb);
  if (ui.w.title_font)
    lv_obj_set_style_text_font(fl, ui.w.title_font, 0);
  lv_obj_set_style_text_color(fl, lv_color_hex(0xEEF2FF), 0);
  lv_label_set_text(fl, "Find");
  lv_obj_center(fl);
  lv_obj_add_event_cb(fb, card_find_cb, LV_EVENT_CLICKED, nullptr);
  // UI-59/60: "Image" at the bottom left of the Sun and Moon cards
  lv_obj_t *sb = lv_button_create(ui.card);
  ui.card_img_btn = sb;
  lv_obj_add_flag(sb, (lv_obj_flag_t) (LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
  lv_obj_set_size(sb, 104, 36);  // same size as Find
  lv_obj_align(sb, LV_ALIGN_BOTTOM_LEFT, 0, 42);
  lv_obj_set_style_bg_color(sb, lv_color_hex(0x24356A), 0);
  lv_obj_set_style_shadow_width(sb, 0, 0);
  lv_obj_t *sl = lv_label_create(sb);
  if (ui.w.title_font)
    lv_obj_set_style_text_font(sl, ui.w.title_font, 0);
  lv_obj_set_style_text_color(sl, lv_color_hex(0xEEF2FF), 0);
  lv_label_set_text(sl, "Image");
  lv_obj_center(sl);
  ui.card_img_lbl = sl;
  lv_obj_add_event_cb(sb, card_img_cb, LV_EVENT_CLICKED, nullptr);
}

// ================================================================== schedule (DATA-4..6)
#ifdef SAT_HOST_TEST
inline bool host_net_up = true;  // host tests: simulated Wi-Fi state
#endif
inline bool network_up() {
#ifdef SAT_HOST_TEST
  return host_net_up;
#else
  return esphome::network::is_connected();
#endif
}

inline void schedule(double t) {
  // NET-8: only the element download uses the network, and only while Wi-Fi is up;
  // the positions are maths and carry on without it.
  net::link_up = network_up();
  if (!ui.started) {
    ui.started = true;
    ui.next_elem = t;
    ui.next_iss = ui.next_sat = t + 1;
    ui.next_starlink = t + STARLINK_OFFSET;
  }
  if (!net::link_up) {
    ui.net_down = true;
  } else if (ui.net_down) {
    ui.net_down = false;
    ui.next_elem = t;  // try a pending download as soon as Wi-Fi is back
  }
  auto due = [&](double &next, double period, Job j) {
    if (t < next)
      return;
    if (!enqueue(j)) {
      ESP_LOGW(UI_TAG, "job queue full; retrying next tick");
      return;  // ARCH-4
    }
    next += period;
    if (next <= t)
      next = t + period;  // fell behind (e.g. after an upload): don't burst
  };
  if (net::link_up)
    due(ui.next_elem, ELEM_PERIOD, JOB_ELEMENTS);
  if (!clock_valid())
    return;  // positions need the time
  // fresh element sets: recompute everything now rather than at the next period
  if (live.status.loaded != ui.loaded_seen) {
    ui.loaded_seen = live.status.loaded;
    ui.next_sat = ui.next_iss = ui.next_starlink = t;
  }
  if (ui.sats_on || ui.meo_on || ui.geo_on || ui.debris_on)  // UI-22/28/35: hidden layers are not computed
    due(ui.next_sat, SAT_PERIOD, JOB_ABOVE);
  if (ui.starlink_on && config().starlink_radius > 0)
    due(ui.next_starlink, STARLINK_PERIOD, JOB_STARLINK);
  due(ui.next_iss, ISS_PERIOD, JOB_ISS);
  // DATA-6: new passes once the one shown has ended, or when the observer moved
  if (!live.passes.empty()) {
    const Pass &first = live.passes.front();
    if (first.end < t && ui.pass_refetched_for != first.end) {
      ui.pass_refetched_for = first.end;
      ui.want_pass = true;
    }
  }
  // DATA-6a: no pass list (the first search ran before the ISS elements or the clock
  // were there, or 4 days held no pass): search again, at most every 10 min
  if (live.passes.empty() && live.status.have_iss && clock_valid() && t >= ui.pass_retry_at) {
    ui.pass_retry_at = t + PASS_RETRY_S;
    ui.want_pass = true;
  }
  // UI-52: Tiangong's passes come with the ISS's; the same two triggers for its list
  if (!live.css_passes.empty()) {
    const Pass &first = live.css_passes.front();
    if (first.end < t && ui.css_refetched_for != first.end) {
      ui.css_refetched_for = first.end;
      ui.want_pass = true;
    }
  } else if (live.css_ok && clock_valid() && t >= ui.css_pass_retry_at) {
    ui.css_pass_retry_at = t + PASS_RETRY_S;
    ui.want_pass = true;
  }
  if (ui.want_pass && enqueue(JOB_PASS))
    ui.want_pass = false;
}

// ================================================================== entry points
// HW-7: software-reset the ST7701 before ESPHome initialises it. The board has no
// panel reset pin and ESPHome's init sequence has no SWRESET, so after a warm reboot
// (every OTA install) the panel sometimes ignored the init and stayed black until a
// power cycle. Bit-bangs one 9-bit 3-wire SPI command (D/C=0, 0x01, MSB first,
// sampled on the rising edge) and waits the datasheet's 120 ms. Must run before the
// SPI bus is set up (on_boot priority above BUS).
inline void panel_soft_reset(int cs, int sck, int mosi) {
#ifndef SAT_HOST_TEST
  const gpio_num_t pins[3] = {(gpio_num_t) cs, (gpio_num_t) sck, (gpio_num_t) mosi};
  for (gpio_num_t p : pins) {
    gpio_reset_pin(p);
    gpio_set_direction(p, GPIO_MODE_OUTPUT);
  }
  gpio_set_level(pins[0], 1);
  gpio_set_level(pins[1], 0);
  esp_rom_delay_us(10);
  gpio_set_level(pins[0], 0);
  const uint16_t word = 0x001;  // 9 bits: D/C = 0 (command), then SWRESET 0x01
  for (int bit = 8; bit >= 0; bit--) {
    gpio_set_level(pins[2], (word >> bit) & 1);
    esp_rom_delay_us(2);
    gpio_set_level(pins[1], 1);
    esp_rom_delay_us(2);
    gpio_set_level(pins[1], 0);
  }
  esp_rom_delay_us(2);
  gpio_set_level(pins[0], 1);
  esp_rom_delay_us(120000);
  ESP_LOGI(UI_TAG, "panel software reset sent");
#endif
}

inline void setup(const Config &cfg, const Widgets &w) {
  // UI-22: the layer switches were restored (set_layers) before setup; net_init copies
  // the whole Config, so carry them over or a hidden layer is downloaded at boot anyway.
  Config c = cfg;
  c.sats_on = ui.sats_on;
  c.starlink_on = ui.starlink_on;
  c.meo_on = ui.meo_on;
  c.geo_on = ui.geo_on;
  c.debris_on = ui.debris_on;  // UI-35: a restored "debris off" must survive net_init's copy
  ui.w = w;
  ui.obs.set((float) c.lat, (float) c.lon, (float) c.alt_m);
  if (!net_init(c))
    ESP_LOGE(UI_TAG, "fetch task not running; the map will stay empty");
  ui_build();
  card_build();
  scrub_build();  // UI-53
  list_build();
  if (ui.w.moon_img) {  // UI-36: the almanac Moon opens the Moon card
    lv_obj_add_flag(ui.w.moon_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ui.w.moon_img, 12);
    lv_obj_add_event_cb(ui.w.moon_img, moon_icon_click_cb, LV_EVENT_CLICKED, nullptr);
  }
  update_declination(clock_now());  // HW-9a
  if (ui.w.status) {
    lv_obj_set_width(ui.w.status, 340);
    lv_label_set_long_mode(ui.w.status, LV_LABEL_LONG_WRAP);
  }
  ui.ready = true;
  const double t = clock_now();
  draw_sun_moon(t, t);
  reassert_order();
  draw_hud(t);
  draw_list(t);
}

// UI-47: the radiant of a shower near its peak, at the time drawn
inline void update_radiant(double tv) {
  const int r = ui.sky_alerts ? radiant_shower(tv) : -1;
  int x = 0, y = 0;
  bool up = false;
  if (r >= 0) {
    float az, el, fx, fy;
    view_azel(ev::SHOWERS[r].ra, ev::SHOWERS[r].dec, tv, az, el);
    up = el >= 0;
    if (up) {
      project({az, el}, fx, fy);
      x = (int) lroundf(fx);
      y = (int) lroundf(fy);
    }
  }
  if (r == ui.radiant && up == ui.radiant_up && (!up || (x == ui.radiant_x && y == ui.radiant_y)))
    return;
  auto inval = [&](int ix, int iy) {
    lv_area_t box;
    lv_obj_get_coords(ui.w.sky, &box);
    const lv_area_t a = {box.x1 + ix - 102, box.y1 + iy - 12, box.x1 + ix + 102, box.y1 + iy + 12};
    lv_obj_invalidate_area(ui.w.sky, &a);
  };
  if (ui.radiant >= 0 && ui.radiant_up)
    inval(ui.radiant_x, ui.radiant_y);
  ui.radiant = r;
  ui.radiant_up = up;
  ui.radiant_x = x;
  ui.radiant_y = y;
  if (r >= 0 && up)
    inval(x, y);
}

// UI-53 time scrub: a long press on the map opens a slider under it; the sky (stars,
// constellations, planets, Sun, Moon, meteor radiants) is drawn up to 24 h ahead.
// Satellites are hidden meanwhile; "Now" or 90 s untouched goes back.
constexpr double SCRUB_IDLE_S = 90, SCRUB_STEP_S = 600;
constexpr int SCRUB_STEPS = 144;  // 24 h in 10-minute steps
inline void redraw_all(double t);
inline void scrub_label_update() {
  if (ui.scrub_label == nullptr)
    return;
  const double t = clock_now(), tv = t + ui.scrub_s;
  char hm[16], w[16], b[64];
  local_hm((int64_t) tv, hm, sizeof(hm));
  day_word(t, tv, w, sizeof(w));
  const int m = (int) lround(ui.scrub_s / 60.0);
  if (m == 0)
    snprintf(b, sizeof(b), "Sky now, %s  -  drag to look ahead", hm);
  else
  {
    char d[16];
    fmt_dur(m * 60.0, d, sizeof(d));  // UI-17a
    snprintf(b, sizeof(b), "Sky at %s %s  (+%s)", hm, w, d);
  }
  set_text_if(ui.scrub_label, b);
}
inline void scrub_hide_markers() {
  auto hide = [](Marker &m) {
    if (m.dot == nullptr)
      return;
    set_shown(m.dot, m.shown, false);
    place_label(m, 0, 0, false);
    m.up = false;
  };
  for (auto &m : ui.sats)
    hide(m);
  for (auto &m : ui.starlink)
    hide(m);
  hide(ui.iss);
  hide(ui.css);
}
inline void scrub_view_changed() {
  ui.star_next = 0;
  ui.planet_next = 0;
  lv_obj_invalidate(ui.w.sky);
}
inline void scrub_open() {
  if (ui.scrub_panel == nullptr || ui.scrub_on)
    return;
  deselect();
  ui.scrub_on = true;
  ui.scrub_s = 0;
  ui.scrub_touched = clock_now();
  lv_slider_set_value(ui.scrub_slider, 0, LV_ANIM_OFF);
  lv_obj_remove_flag(ui.scrub_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(ui.scrub_panel);
  scrub_hide_markers();
  scrub_label_update();
  scrub_view_changed();
}
inline void scrub_close() {
  if (!ui.scrub_on)
    return;
  ui.scrub_on = false;
  ui.scrub_s = 0;
  if (ui.scrub_panel)
    lv_obj_add_flag(ui.scrub_panel, LV_OBJ_FLAG_HIDDEN);
  deselect();
  scrub_view_changed();
  redraw_all(clock_now());
}
inline void scrub_long_press_cb(lv_event_t *e) {
  if (ui.scrub_on || ui.sel_kind >= 0)
    return;
  scrub_open();
  scrub_swallow_click = true;  // the release that ends this press is not a tap
  lv_event_stop_bubbling(e);
}
inline void scrub_slider_cb(lv_event_t *e) {
  const int v = lv_slider_get_value((lv_obj_t *) lv_event_get_target(e));
  ui.scrub_s = v * SCRUB_STEP_S;
  ui.scrub_touched = clock_now();
  scrub_label_update();
  scrub_view_changed();
}
inline void scrub_now_cb(lv_event_t *e) {
  scrub_close();
  lv_event_stop_bubbling(e);
}
inline void scrub_build() {
  lv_obj_t *page = lv_obj_get_parent(ui.w.sky);
  if (page == nullptr)
    return;
  lv_obj_add_event_cb(ui.w.sky, scrub_long_press_cb, LV_EVENT_LONG_PRESSED, nullptr);
  lv_obj_t *p = lv_obj_create(page);
  ui.scrub_panel = p;
  lv_obj_remove_style_all(p);
  lv_obj_set_size(p, 480, 62);
  lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(p, lv_color_hex(0x101B3D), 0);
  lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(p, lv_color_hex(0x3A4E86), 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_border_side(p, LV_BORDER_SIDE_TOP, 0);
  lv_obj_add_flag(p, (lv_obj_flag_t) (LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE));  // taps stop here
  lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  ui.scrub_label = lv_label_create(p);
  if (ui.w.label_font)
    lv_obj_set_style_text_font(ui.scrub_label, ui.w.card_font ? ui.w.card_font : ui.w.label_font, 0);
  lv_obj_set_style_text_color(ui.scrub_label, lv_color_hex(C_TEXT), 0);
  lv_obj_set_pos(ui.scrub_label, 14, 6);
  ui.scrub_slider = lv_slider_create(p);
  lv_obj_set_size(ui.scrub_slider, 360, 12);
  lv_obj_set_pos(ui.scrub_slider, 20, 38);
  lv_slider_set_range(ui.scrub_slider, 0, SCRUB_STEPS);
  lv_obj_set_ext_click_area(ui.scrub_slider, 14);
  lv_obj_set_style_bg_color(ui.scrub_slider, lv_color_hex(0x22305A), LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui.scrub_slider, lv_color_hex(0x5A7BD8), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(ui.scrub_slider, lv_color_hex(0xDCE4FF), LV_PART_KNOB);
  lv_obj_add_event_cb(ui.scrub_slider, scrub_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_size(b, 64, 40);
  lv_obj_set_pos(b, 402, 12);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1A2547), 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *bl = lv_label_create(b);
  if (ui.w.label_font)
    lv_obj_set_style_text_font(bl, ui.w.title_font ? ui.w.title_font : ui.w.label_font, 0);
  lv_obj_set_style_text_color(bl, lv_color_hex(C_TEXT), 0);
  lv_label_set_text(bl, "Now");
  lv_obj_center(bl);
  lv_obj_add_event_cb(b, scrub_now_cb, LV_EVENT_CLICKED, nullptr);
}

namespace rocket {
bool active();  // sky_rocket.h (UI-69): the launch animation is on screen
}

inline void tick() {
  if (!ui.ready || ui.ota)
    return;  // BUILD-6
  // UI-69b: no satellite/sky work while the launch animation plays; the redraws it causes
  // would compete with the animation's frames. The next tick after it ends catches up
  // (positions are computed for the current time, so nothing is lost but up to 2 s).
  if (rocket::active())
    return;
  const double t = clock_now();
  if (clock_valid())
    schedule(t);
  if (clock_valid() && (ui.decl_at < 1.7e9 || t - ui.decl_at > 86400))
    update_declination(t);  // HW-9a: once the clock is set, then daily (secular change)
  uint8_t fresh = drain();
  if (fresh & (L_KP | L_EXTRA))
    aur.until = 0;  // UI-38: new forecast (UI-58: or nowcast)
  img_view_update(t);  // UI-59/60
  // UI-22: a layer switched off while its fetch was in flight: drop the result
  if ((fresh & L_SAT) && !ui.sats_on && !ui.meo_on && !ui.debris_on) {
    live.sats.clear();
    live.sat_total = 0;
  }
  if ((fresh & L_GEO) && !ui.geo_on) {
    live.geo.clear();
    live.geo_total = 0;
  }
  if (fresh & L_GEO)
    geo_project(t);  // UI-28
  if ((fresh & L_STARLINK) && !ui.starlink_on) {
    live.starlink.clear();
    live.starlink_total = 0;
  }

  if (ui.scrub_on && t - ui.scrub_touched > SCRUB_IDLE_S)
    scrub_close();  // UI-53
  const double tv = ui.scrub_on ? t + ui.scrub_s : t;  // UI-53: the time the sky is drawn for
  draw_sun_moon(t, tv);  // before the ISS so its sunlit state uses this tick's Sun
  night_update();     // UI-45: follows the Sun
  update_starmap(tv);  // UI-21: before the markers, whose tags outrank constellation names
  update_planets(tv);  // UI-40
  update_comets(tv);   // UI-63
  update_radiant(tv);  // UI-47
  align_step(t);      // UI-41: one day of the look-ahead per tick
  sky_events_step(t);  // UI-47..51
  if (fresh & L_SAT)
    restyle_sats(t);
  if (fresh & L_STARLINK) {
    restyle_starlink(t);
    find_trains();  // UI-42
  }
  if (fresh & L_ISS) {
    restyle_iss(t);
    restyle_css(t);  // UI-52
  }
  // motion for layers that were not just restyled
  if (!(fresh & L_STARLINK))
    for (auto &m : ui.starlink)
      if (m.id >= 0)
        draw_marker(m, nullptr, sl_px(), t, false, starlink_min_el());
  if (!(fresh & L_SAT))
    for (auto &m : ui.sats)
      if (m.id >= 0)
        draw_marker(m, nullptr, sat_px(), t, false);
  if (!(fresh & L_SAT))
    declutter_labels();
  if (!(fresh & L_ISS) && ui.iss.id >= 0)
    draw_marker(ui.iss, nullptr, ISS_PX, t, false);
  if (!(fresh & L_ISS) && ui.css.id >= 0)
    draw_marker(ui.css, nullptr, ISS_PX, t, false);  // UI-52
  if (ui.scrub_on)
    scrub_hide_markers();  // UI-53
  update_iss_state(t);
  update_trails(t);  // UI-26
  update_trains();   // UI-42
  card_update(t);    // UI-24
  if (fresh & L_PASS)
    arc_update(t);

  count_starlink(t);  // satellites cross the cone edge between fetches
  draw_hud(t);
  if (fresh & (L_SAT | L_STARLINK | L_ISS | L_GEO))
    draw_list(t);  // PERF-6: list text on fresh data only
}

// NET-7: SNTP starts at boot, before Wi-Fi, and its first requests fail; lwIP then
// backs off (15 s doubling to 150 s), so a slow Wi-Fi join could leave the clock
// unset for minutes. Called on Wi-Fi connect to ask again at once.
inline void sntp_kick() {
#ifndef SAT_HOST_TEST
  if (esp_sntp_enabled())
    esp_sntp_restart();
  else
    esp_sntp_init();  // keeps the servers and interval the SNTP component set up
#endif
}
// NET-8: no SNTP traffic without Wi-Fi: stopped at boot until the first connect,
// and again whenever Wi-Fi drops.
inline void sntp_hold() {
#ifndef SAT_HOST_TEST
  if (esp_sntp_enabled() && !network_up())
    esp_sntp_stop();
#endif
}

inline void ota_begin() {
  ui.ota = true;
  net_pause(true);
}
inline void ota_error() {
  ui.ota = false;
  net_pause(false);
}

// Redraw every marker, the Sun and the Moon at once, dropping MOTION-5 offsets
// (a snap). Used when the view itself changes: heading or observer.
inline void redraw_all(double t) {
  auto redraw = [&](Marker &m, int px, float min_el = 0.0f) {
    if (m.id < 0)
      return;
    m.offx = m.offy = 0;
    m.px = m.py = INT_MIN;
    m.lx = m.ly = INT_MIN;
    draw_marker(m, nullptr, px, t, false, min_el);
  };
  for (auto &m : ui.starlink)
    redraw(m, sl_px(), starlink_min_el());
  for (auto &m : ui.sats)
    redraw(m, sat_px());
  declutter_labels();
  geo_project(t);  // UI-28
  redraw(ui.iss, ISS_PX);
  redraw(ui.css, ISS_PX);  // UI-52
  update_iss_state(t);
  ui.sun_x = ui.sun_y = ui.moon_x = ui.moon_y = INT_MIN;
  ui.map_moon_k = -1;
  draw_sun_moon(t, t + (ui.scrub_on ? ui.scrub_s : 0));
  update_planets(t + (ui.scrub_on ? ui.scrub_s : 0), true);  // UI-40
  update_comets(t + (ui.scrub_on ? ui.scrub_s : 0), true);   // UI-63
  ui.star_next = 0;  // UI-21: the view changed
  update_starmap(t);
  update_trails(t);
  if (ui.sel_kind >= 0) {
    ui.arc_shown = false;  // re-project the pass arc
    ui.arc.clear();
    card_update(t);
  }
}

// UI-15: rotate the map. Safe before setup (the value is kept and applied there).
// UI-39: LEO cone (the Starlink layer), degrees from overhead, 15-90 in 5° steps.
// Applied at once: the published Starlink list covers the whole sky (DATA-3).
constexpr int CONE_MIN = 15, CONE_MAX = 90;
inline int cone_snap(int v) {
  v = (v + 2) / 5 * 5;
  return std::max(CONE_MIN, std::min(CONE_MAX, v));
}
inline void set_starlink_radius(int r) {
  r = cone_snap(r);
  if (config().starlink_radius == r)
    return;
  net_set_starlink_radius(r);
  if (!ui.ready)
    return;
  ESP_LOGI(UI_TAG, "LEO cone %d° (above %d° elevation)", r, 90 - r);
  const double t = clock_now();
  restyle_starlink(t);
  ui.next_starlink = t;  // a fresh scan straight away
}
// Settings slider readout (snapped to what Save will store)
inline void cone_label(lv_obj_t *lbl, int v) {
  char b[8];
  snprintf(b, sizeof(b), "%d\xC2\xB0", cone_snap(v));
  lv_label_set_text(lbl, b);
}
// UI-16a: settings tabs. which 0 = Display, 1 = Location, 2 = Celestial
constexpr uint32_t C_TAB_ON = 0x2D5BD0, C_TAB_OFF = 0x1A2547;
inline lv_obj_t *settings_title = nullptr;  // the settings header (YAML settings_title)
// UI-16a: five icon-only tabs (Display, Location, Celestial, Satellites, Alerts), each a panel; one shown
inline void settings_tab(int which, lv_obj_t *p0, lv_obj_t *p1, lv_obj_t *p2, lv_obj_t *p3, lv_obj_t *p4, lv_obj_t *t0,
                         lv_obj_t *t1, lv_obj_t *t2, lv_obj_t *t3, lv_obj_t *t4) {
  lv_obj_t *const P[5] = {p0, p1, p2, p3, p4}, *const T[5] = {t0, t1, t2, t3, t4};
  static const char *const NAME[5] = {"DISPLAY", "LOCATION", "CELESTIAL", "SATELLITES", "ALERTS"};
  if (settings_title && which >= 0 && which < 5)
    lv_label_set_text(settings_title, NAME[which]);  // the header names the open tab
  for (int k = 0; k < 5; k++) {
    if (k == which)
      lv_obj_remove_flag(P[k], LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(P[k], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(T[k], lv_color_hex(k == which ? C_TAB_ON : C_TAB_OFF), 0);
  }
}
// UI-40: the Planets switch gets Saturn's picture (MDI 7.4 has no planet glyph)
inline void settings_planet_icon(lv_obj_t *parent, int x, int y, lv_obj_t *alerts, int ax, int ay) {
  static lv_obj_t *img = nullptr;
  if (img != nullptr || parent == nullptr)
    return;
  img = lv_image_create(parent);
  lv_obj_remove_style_all(img);
  lv_image_set_src(img, planet_dsc(planets::SATURN, true));
  lv_obj_set_pos(img, x, y);
  lv_obj_t *al = lv_image_create(alerts ? alerts : parent);  // UI-41a: Planet alerts gets the alignment picture
  lv_obj_remove_style_all(al);
  lv_image_set_src(al, align_dsc());
  lv_obj_set_pos(al, ax, ay);
}

inline void set_heading(float deg) {
  if (std::isnan(deg))
    return;
  deg = fmodf(deg, 360.0f);
  if (deg < 0)
    deg += 360.0f;
  if (fabsf(deg - ui.heading) < 0.01f)
    return;
  ui.heading = deg;
  if (!ui.ready)
    return;
  ESP_LOGI(UI_TAG, "map heading %.0f°", deg);
  place_compass_tags();
  redraw_all(clock_now());
}

// UI-16: move the observer. Safe before setup. Markers are re-projected at once and
// everything location-dependent (satellites overhead, ISS, passes) is fetched again.
inline void draw_coords();
inline void set_observer(double lat, double lon) {
  if (std::isnan(lat) || std::isnan(lon))
    return;
  lat = std::max(-90.0, std::min(90.0, lat));
  lon = std::max(-180.0, std::min(180.0, lon));
  const Config &c = config();
  if (fabs(lat - c.lat) < 5e-6 && fabs(lon - c.lon) < 5e-6)
    return;
  net_set_observer(lat, lon);
  ui.obs.set((float) lat, (float) lon, (float) c.alt_m);
  update_declination(clock_now());  // HW-9a: declination follows the location
  if (!ui.ready)
    return;
  ESP_LOGI(UI_TAG, "observer %.4f, %.4f", lat, lon);
  const double t = clock_now();
  live.passes.clear();
  ui.pass_refetched_for = 0;
  ui.want_pass = true;
  ui.alm_next = 0;  // UI-20: new rise/set times
  ui.icon_illum = -1;
  redraw_all(t);
  if (ui.started)
    ui.next_sat = ui.next_iss = ui.next_starlink = t;
  draw_hud(t);
  draw_list(t);
  draw_coords();
}

// UI-22: satellite and Starlink layers. Off: markers freed, nothing computed and no
// downloads for that layer. On: downloaded if needed and computed straight away.
// Safe before setup.
// UI-35: rocket bodies and debris on or off (the net task filters them; no downloads change)
inline void set_debris(bool on) {
  if (ui.debris_on == on)
    return;
  ui.debris_on = on;
  net_set_debris(on);
  if (!ui.ready)
    return;
  ESP_LOGI(UI_TAG, "debris %s", on ? "on" : "off");
  const double t = clock_now();
  ui.next_sat = t;  // the next job applies the filter
  if (on)
    ui.next_elem = t;  // the visual list may not be loaded while LEO/MEO are off
  if (!on && !ui.sats_on && !ui.meo_on) {  // nothing left in the satellite layer
    live.sats.clear();
    live.sat_total = 0;
    restyle_sats(t);
  }
}

inline void set_layers(bool sats, bool starlink, bool meo, bool geo) {
  starlink = starlink && sats;  // UI-28: Starlink is part of LEO
  const bool sats_was = ui.sats_on, sl_was = ui.starlink_on, meo_was = ui.meo_on, geo_was = ui.geo_on;
  ui.sats_on = sats;
  ui.starlink_on = starlink;
  ui.meo_on = meo;
  ui.geo_on = geo;
  net_set_layers(sats, starlink, meo, geo);
  if (!ui.ready)
    return;
  const double t = clock_now();
  if ((sats && !sats_was) || (starlink && !sl_was) || (meo && !meo_was) || (geo && !geo_was))
    ui.next_elem = t;  // download a group that was skipped while its class was off
  if (sats != sats_was || meo != meo_was || geo != geo_was) {
    if (!sats && !meo && !ui.debris_on) {
      live.sats.clear();
      live.sat_total = 0;
      restyle_sats(t);  // frees every marker
    }
    if (!geo) {
      live.geo.clear();
      live.geo_total = 0;
      geo_project(t);
    }
    ui.next_sat = t;  // the next job applies the class filter
  }
  if (starlink != sl_was) {
    if (!starlink) {
      live.starlink.clear();
      live.starlink_total = 0;
      restyle_starlink(t);
    } else {
      ui.next_starlink = t;
    }
  }
  if (sats != sats_was || starlink != sl_was || meo != meo_was || geo != geo_was) {
    ESP_LOGI(UI_TAG, "layers: LEO %s, Starlink %s, MEO %s, GEO %s", sats ? "on" : "off", starlink ? "on" : "off",
             meo ? "on" : "off", geo ? "on" : "off");
    draw_hud(t);
    draw_list(t);
  }
}

// UI-21: star map on/off, and whether it waits for the end of civil twilight.
inline void set_stars(bool on, bool after_dusk) {
  ui.stars_on = on;
  ui.stars_dusk = after_dusk;
  if (ui.ready)
    update_starmap(clock_now());
}

// UI-26
inline void set_trails(bool on) {
  if (ui.trails_on == on)
    return;
  ui.trails_on = on;
  if (ui.ready)
    update_trails(clock_now());  // clears or draws every trail
}

inline void set_miles(bool on) {  // UI-29
  if (ui.miles == on)
    return;
  ui.miles = on;
  if (ui.ready) {
    const double t = clock_now();
    draw_list(t);
    card_update(t);
  }
}

inline void set_24h(bool on) {
  if (ui.h24 == on)
    return;
  ui.h24 = on;
  if (ui.ready)
    draw_hud(clock_now());
}

// ================================================================== settings page (UI-16)
constexpr uint32_t C_FIELD_EDGE = 0x3A4E86;
constexpr uint32_t C_ERROR = 0xFF5A5A;

// One coordinate in degrees-minutes-seconds form (UI-16): three fields and a
// hemisphere button (N/S or E/W), shown instead of the decimal field in DMS mode.
struct DmsFields {
  lv_obj_t *deg = nullptr, *min = nullptr, *sec = nullptr, *hemi = nullptr;  // hemi: the button's label
  bool negative = false;                                                      // S or W
  const char *pos = "N", *neg = "S";
};
struct SettingsForm {
  lv_obj_t *heading = nullptr, *lat = nullptr, *lon = nullptr, *h24 = nullptr, *status = nullptr;
  lv_obj_t *dms_switch = nullptr;
  lv_obj_t *box_dd[2] = {}, *box_dms[2] = {};  // [0] latitude row, [1] longitude row
  DmsFields dms_f[2];
  bool dms = false;  // coordinate entry format; persisted by the YAML
};
struct SettingsValues {
  int heading = 0;
  double lat = 0, lon = 0;
  bool h24 = true;
};
inline SettingsForm form;
inline SettingsValues form_values;  // the last values settings_parse() accepted

// HW-8/HW-9: fields driven by a GPS or a compass are read-only on the settings page,
// and Save leaves them alone.
inline bool lock_loc = false, lock_head = false;

inline void field_enable(lv_obj_t *o, bool on) {
  if (o == nullptr)
    return;
  if (on) {
    lv_obj_remove_state(o, LV_STATE_DISABLED);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
  } else {
    lv_obj_add_state(o, LV_STATE_DISABLED);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(o, LV_OPA_50, 0);
  }
}
inline void settings_apply_locks() {
  field_enable(form.heading, !lock_head);
  field_enable(form.lat, !lock_loc);
  field_enable(form.lon, !lock_loc);
  for (auto &f : form.dms_f) {
    field_enable(f.deg, !lock_loc);
    field_enable(f.min, !lock_loc);
    field_enable(f.sec, !lock_loc);
    if (f.hemi != nullptr)
      field_enable(lv_obj_get_parent(f.hemi), !lock_loc);  // the N/S, E/W button
  }
}
inline void set_locks(bool loc, bool head) {
  if (loc == lock_loc && head == lock_head)
    return;
  lock_loc = loc;
  lock_head = head;
  settings_apply_locks();
}
// Status-line text for the settings page when something is locked.
inline void settings_lock_note(char *b, size_t n) {
  b[0] = 0;
  if (lock_loc && lock_head)
    snprintf(b, n, "Location from GPS, heading from compass");
  else if (lock_loc)
    snprintf(b, n, "Location from GPS");
  else if (lock_head)
    snprintf(b, n, "Heading from compass");
}

inline void settings_bind(lv_obj_t *heading, lv_obj_t *lat, lv_obj_t *lon, lv_obj_t *h24, lv_obj_t *status) {
  form.heading = heading;
  form.lat = lat;
  form.lon = lon;
  form.h24 = h24;
  form.status = status;
}

// Whole-string number parse; false for empty input or trailing junk.
inline bool parse_num(const char *s, double lo, double hi, double &out) {
  if (s == nullptr || *s == 0)
    return false;
  char *end = nullptr;
  const double v = strtod(s, &end);
  if (end == s || *end != 0 || std::isnan(v) || v < lo || v > hi)
    return false;
  out = v;
  return true;
}

inline void settings_bind_dms(lv_obj_t *dms_switch, lv_obj_t *lat_dd_box, lv_obj_t *lat_dms_box, lv_obj_t *lat_d,
                              lv_obj_t *lat_m, lv_obj_t *lat_s, lv_obj_t *lat_hemi, lv_obj_t *lon_dd_box,
                              lv_obj_t *lon_dms_box, lv_obj_t *lon_d, lv_obj_t *lon_m, lv_obj_t *lon_s,
                              lv_obj_t *lon_hemi) {
  form.dms_switch = dms_switch;
  form.box_dd[0] = lat_dd_box;
  form.box_dms[0] = lat_dms_box;
  form.box_dd[1] = lon_dd_box;
  form.box_dms[1] = lon_dms_box;
  form.dms_f[0] = {lat_d, lat_m, lat_s, lat_hemi, false, "N", "S"};
  form.dms_f[1] = {lon_d, lon_m, lon_s, lon_hemi, false, "E", "W"};
}

inline void show(lv_obj_t *o, bool on) {
  if (o == nullptr)
    return;
  if (on)
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Decimal / DMS switch. Only swaps which row is visible; values are converted
// from the settings in use when the page opens (settings_fill).
inline void settings_set_dms(bool on) {
  form.dms = on;
  for (int i = 0; i < 2; i++) {
    show(form.box_dd[i], !on);
    show(form.box_dms[i], on);
  }
  if (form.dms_switch != nullptr) {
    if (on)
      lv_obj_add_state(form.dms_switch, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(form.dms_switch, LV_STATE_CHECKED);
  }
  draw_coords();
}

inline void set_hemi(DmsFields &f, bool negative) {
  f.negative = negative;
  if (f.hemi != nullptr)
    lv_label_set_text(f.hemi, negative ? f.neg : f.pos);
}
inline void settings_toggle_hemi(int which) {
  if (which == 0 || which == 1)
    set_hemi(form.dms_f[which], !form.dms_f[which].negative);
}

// Decimal degrees -> whole degrees, whole minutes, seconds to 0.1".
inline void to_dms(double v, int &d, int &m, double &sec) {
  double a = fabs(v);
  d = (int) floor(a);
  double rem = (a - d) * 60.0;
  m = (int) floor(rem);
  sec = round((rem - m) * 60.0);  // whole seconds (3-decimal coordinates)
  if (sec >= 60.0) {
    sec = 0;
    m++;
  }
  if (m >= 60) {
    m = 0;
    d++;
  }
}

// UI-23: the observer's position, top centre, in the format chosen on the settings
// page (decimal or degrees-minutes-seconds).
inline void draw_coords() {
  if (ui.w.coords == nullptr)
    return;
  const Config &c = config();
  char b[48];
  const char ns = c.lat < 0 ? 'S' : 'N', ew = c.lon < 0 ? 'W' : 'E';
  if (form.dms) {
    int d[2], m[2];
    double sec[2];
    to_dms(c.lat, d[0], m[0], sec[0]);
    to_dms(c.lon, d[1], m[1], sec[1]);
    snprintf(b, sizeof(b), "%d°%02d'%02d\"%c  %d°%02d'%02d\"%c", d[0], m[0], (int) sec[0], ns, d[1], m[1], (int) sec[1], ew);
  } else {
    snprintf(b, sizeof(b), "%.3f°%c  %.3f°%c", fabs(c.lat), ns, fabs(c.lon), ew);
  }
  set_text_if(ui.w.coords, b);
}

// DMS fields -> signed decimal degrees. Minutes may carry decimals when seconds is
// empty or zero (degrees + decimal minutes, the usual GPS style).
inline bool parse_dms(const DmsFields &f, double max_deg, double &out, const char *&why) {
  double d = 0, m = 0, sec = 0;
  why = "must be degrees, minutes and optional seconds";
  if (!parse_num(lv_textarea_get_text(f.deg), 0, max_deg, d) || d != floor(d))
    return false;
  const char *ms = lv_textarea_get_text(f.min);
  if (ms != nullptr && *ms != 0 && !parse_num(ms, 0, 59.999999, m))
    return false;
  const char *ss = lv_textarea_get_text(f.sec);
  if (ss != nullptr && *ss != 0 && !parse_num(ss, 0, 59.999999, sec))
    return false;
  if (m != floor(m) && sec > 0) {
    why = "use decimal minutes or seconds, not both";
    return false;
  }
  const double v = d + m / 60.0 + sec / 3600.0;
  if (v > max_deg) {
    why = max_deg > 90 ? "must be 180 or less" : "must be 90 or less";
    return false;
  }
  out = f.negative ? -v : v;
  return true;
}

inline void mark_field(lv_obj_t *ta, bool bad) {
  if (ta != nullptr)
    lv_obj_set_style_border_color(ta, lv_color_hex(bad ? C_ERROR : C_FIELD_EDGE), 0);
}

// on_load of the settings page: show the values in use now.
inline void settings_fill() {
  if (form.heading == nullptr)
    return;
  char b[24];
  const Config &c = config();
  snprintf(b, sizeof(b), "%d", (int) lroundf(ui.heading) % 360);
  lv_textarea_set_text(form.heading, b);
  snprintf(b, sizeof(b), "%.3f", c.lat);
  lv_textarea_set_text(form.lat, b);
  snprintf(b, sizeof(b), "%.3f", c.lon);
  lv_textarea_set_text(form.lon, b);
  const double coords[2] = {c.lat, c.lon};
  for (int i = 0; i < 2; i++) {
    DmsFields &f = form.dms_f[i];
    if (f.deg == nullptr)
      continue;
    int d, m;
    double sec;
    to_dms(coords[i], d, m, sec);
    snprintf(b, sizeof(b), "%d", d);
    lv_textarea_set_text(f.deg, b);
    snprintf(b, sizeof(b), "%d", m);
    lv_textarea_set_text(f.min, b);
    snprintf(b, sizeof(b), "%.0f", sec);
    lv_textarea_set_text(f.sec, b);
    set_hemi(f, coords[i] < 0);
    for (lv_obj_t *ta : {f.deg, f.min, f.sec})
      mark_field(ta, false);
  }
  settings_set_dms(form.dms);
  if (ui.h24)
    lv_obj_add_state(form.h24, LV_STATE_CHECKED);
  else
    lv_obj_remove_state(form.h24, LV_STATE_CHECKED);
  for (lv_obj_t *ta : {form.heading, form.lat, form.lon})
    mark_field(ta, false);
  char note[64];
  settings_lock_note(note, sizeof(note));
  lv_label_set_text(form.status, note);
  settings_apply_locks();
}

// Save button: validate every field, flag the bad ones, keep the result in
// form_values. Returns true when everything is valid.
inline bool settings_parse() {
  if (form.heading == nullptr)
    return false;
  SettingsValues v;
  double h = 0;
  const bool ok_h = lock_head || (parse_num(lv_textarea_get_text(form.heading), 0, 359, h) && h == floor(h));
  if (lock_head)
    h = (int) lroundf(ui.heading) % 360;
  bool ok_lat, ok_lon;
  char msg[80];
  msg[0] = 0;
  if (lock_loc) {  // HW-8: GPS owns the location
    ok_lat = ok_lon = true;
    v.lat = config().lat;
    v.lon = config().lon;
  } else if (form.dms && form.dms_f[0].deg != nullptr) {
    const char *why_lat = "", *why_lon = "";
    ok_lat = parse_dms(form.dms_f[0], 90, v.lat, why_lat);
    ok_lon = parse_dms(form.dms_f[1], 180, v.lon, why_lon);
    for (int i = 0; i < 2; i++) {
      const bool bad = i == 0 ? !ok_lat : !ok_lon;
      for (lv_obj_t *ta : {form.dms_f[i].deg, form.dms_f[i].min, form.dms_f[i].sec})
        mark_field(ta, bad);
    }
    if (!ok_lat)
      snprintf(msg, sizeof(msg), "Latitude %s", why_lat);
    else if (!ok_lon)
      snprintf(msg, sizeof(msg), "Longitude %s", why_lon);
  } else {
    ok_lat = parse_num(lv_textarea_get_text(form.lat), -90, 90, v.lat);
    ok_lon = parse_num(lv_textarea_get_text(form.lon), -180, 180, v.lon);
    mark_field(form.lat, !ok_lat);
    mark_field(form.lon, !ok_lon);
    if (!ok_lat)
      snprintf(msg, sizeof(msg), "Latitude must be -90 to 90");
    else if (!ok_lon)
      snprintf(msg, sizeof(msg), "Longitude must be -180 to 180");
  }
  mark_field(form.heading, !ok_h);
  if (!ok_h)
    snprintf(msg, sizeof(msg), "Heading must be a whole number, 0 to 359");
  lv_label_set_text(form.status, msg);
  if (!(ok_h && ok_lat && ok_lon))
    return false;
  v.heading = (int) h;
  if (!lock_loc) {
    v.lat = round(v.lat * 1000.0) / 1000.0;  // 3 decimals (~100 m)
    v.lon = round(v.lon * 1000.0) / 1000.0;
  }
  v.h24 = lv_obj_has_state(form.h24, LV_STATE_CHECKED);
  form_values = v;
  return true;
}

inline float heading() { return ui.heading; }

inline void show_notice(const char *text, double secs) {
  snprintf(notice_text, sizeof(notice_text), "%s", text);
  notice_until = clock_valid() ? clock_now() + secs : 1e18;
  if (!clock_valid() && secs <= 0)
    notice_until = 0;
  if (ui.ready)
    draw_hud(clock_now());
}

// ---- Home Assistant accessors (ARCH-9)
inline float iss_el() { return ui.iss_valid ? ui.iss_azel.el : NAN; }
inline float iss_az() { return ui.iss_valid ? ui.iss_azel.az : NAN; }
inline bool iss_up() { return ui.iss_valid && ui.iss_azel.el >= 0; }
inline float sat_count() { return ui.started && ui.sats_on ? (float) live.sat_total : NAN; }
inline float starlink_count() {
  return ui.started && ui.starlink_on && config().starlink_radius > 0 ? (float) ui.starlink_in_cone : NAN;
}
inline std::string next_pass_text() {
  const Pass *p = next_pass(clock_now());
  if (p == nullptr)
    return live.status.have_iss ? "none in 4 days" : "unknown";
  char hm[12], b[64];
  local_hm(p->start, hm, sizeof(hm));
  snprintf(b, sizeof(b), "%s  peak %.0f° %s%s", hm, p->max_el, p->max_dir, p->visible ? " (visible)" : "");
  return b;
}
// UI-54: the next launch, for Home Assistant
inline std::string next_launch_text() {
  const double t = clock_now();
  for (const auto &l : live.launches) {
    if (!launch_pending(l) || l.net < t - 900)
      continue;
    char md[16], hm[16], b[160];
    local_md(l.net, md, sizeof(md));
    local_hm((int64_t) l.net, hm, sizeof(hm));
    snprintf(b, sizeof(b), "%s %s %s, %s", l.name, md, hm, short_site(l.where));
    return b;
  }
  return live.launches.empty() ? "unknown" : "none scheduled";
}
inline float wind_bz() { return wind_fresh(clock_now()) ? live.wind.bz : NAN; }  // UI-55
inline float wind_speed() { return wind_fresh(clock_now()) ? live.wind.speed : NAN; }
// Diagnostics (NET-9)
inline std::string data_status() {
  char b[160];
  status_text(clock_now(), b, sizeof(b));
  for (char *c = b; *c; c++)
    if (*c == '\n')
      *c = ' ';
  return b;
}
inline float data_age_h() {
  return live.status.data_time > 0 && clock_valid() ? (float) ((clock_now() - live.status.data_time) / 3600.0)
                                                       : NAN;
}
inline float tracked_sats() { return (float) live.status.n_sats; }
inline float tracked_starlink() { return (float) live.status.n_starlink; }

}  // namespace sat
