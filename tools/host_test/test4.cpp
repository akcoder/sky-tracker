#ifndef HOST_DIR
#define HOST_DIR "."   // this folder (test data)
#endif
#ifndef OUT_DIR
#define OUT_DIR "out"  // renders and logs
#endif
#include <functional>
// Host test for rev 4: on-board SGP4 / Starlink model vs skyfield, the element download
// pipeline, passes, and the new UI (tap card, pass arc, trails, table list, status line).
#define SAT_HOST_TEST
#include "lvgl.h"
#include <ArduinoJson.h>
#include <map>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream>

namespace sat { double sat_host_now = 0; }
using sat::sat_host_now;
int host_tasks_created = 0;
size_t host_psram_allocs = 0;
int host_warnings = 0;
std::map<std::string, std::string> host_http_bodies;
std::string host_last_url;

#include "sat_tracker.h"
#define SKY_IMPL
#include "sky_about.h"
#include "sky_sdfw.h"
#include "sky_update.h"
#include "sky_rocket.h"
#include "sky_tz.h"
extern "C" const lv_font_t mono16, mono18, mono24;

static int failures = 0;
#define CHECK(cond, ...)                                                   \
  do {                                                                     \
    if (!(cond)) {                                                         \
      failures++;                                                          \
      printf("FAIL %s:%d ", __FILE__, __LINE__);                           \
      printf(__VA_ARGS__);                                                 \
      printf("\n");                                                        \
    }                                                                      \
  } while (0)

static uint16_t fb[480 * 480];
static long g_flush_px = 0;
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  g_flush_px += (long) lv_area_get_size(a);
  const uint16_t *p = (const uint16_t *) px;
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++) fb[y * 480 + x] = *p++;
  lv_display_flush_ready(d);
}
static void save_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6 480 480 255\n");
  for (int i = 0; i < 480 * 480; i++) {
    uint16_t c = (uint16_t) ((fb[i] >> 8) | (fb[i] << 8));
    unsigned char rgb[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2), (unsigned char) ((c & 31) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}
static lv_obj_t *label(lv_obj_t *p, int x, int y, const char *t, const lv_font_t *f, uint32_t col) {
  lv_obj_t *l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  return l;
}
static std::string slurp(const char *path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream b;
  b << f.rdbuf();
  return b.str();
}
static void run_jobs() {
  uint8_t job;
  while (xQueueReceive(sat::net::queue, &job, 0) == pdTRUE) {
    sat::net::queued.fetch_and(~(1u << job));  // as task_main does (ARCH-4)
    if (!sat::net::paused) sat::net::run_job(job);
  }
}

extern "C" const lv_font_t mdi14, mdi20, mdi40;
int main() {
  std::ifstream fin(HOST_DIR "/vectors4.json");
  std::stringstream ss;
  ss << fin.rdbuf();
  JsonDocument V;
  deserializeJson(V, ss.str());
  const double T0 = V["T0"], LAT = V["observer"]["lat"], LON = V["observer"]["lon"], ALT = V["observer"]["alt"];
  sat::geo::Observer obs;
  obs.set(LAT, LON, ALT);
  printf("sizeof(elsetrec) %zu, SlElem %zu, SatRec %zu, Pass %zu\n", sizeof(sat::sgp4::elsetrec), sizeof(sat::net::SlElem),
         sizeof(sat::SatRec), sizeof(sat::Pass));

  // ============ VER-1: element parsing and propagation vs skyfield
  std::map<int32_t, sat::net::SgpSat> sgp;
  std::map<int32_t, sat::net::SlElem> sl;
  for (const char *grp : {"iss", "sat", "starlink"}) {
    std::string csv = slurp(V["csv"][grp].as<const char *>());
    sat::net::LineSplitter ls;
    int n = 0;
    ls.on_line = [&](char *line) {
      sat::net::Omm o;
      if (!sat::net::parse_omm(line, o)) return;
      n++;
      if (strcmp(grp, "starlink") == 0) { CHECK(sat::net::make_sl(o, sl[o.id]), "sl init"); }
      else { CHECK(sat::net::make_sgp(o, sgp[o.id]), "sgp4init %d", o.id); }
    };
    ls.feed(csv.data(), csv.size());
    ls.finish();
    printf("%-8s %d rows parsed\n", grp, n);
  }
  double sgp_max = 0, sl_max[8] = {0};
  for (JsonObject o : V["truth"].as<JsonArray>()) {
    const int32_t id = o["id"];
    const bool is_sl = strcmp(o["kind"], "starlink") == 0;
    int k = 0;
    for (JsonObject p : o["pts"].as<JsonArray>()) {
      const double t = T0 + p["dt"].as<double>();
      double x[3] = {0, 0, 0};
      if (is_sl) sat::net::sl_ecef(sl[id], t, x);
      else CHECK(sat::net::sgp_ecef(sgp[id], t, x), "sgp4 %d", id);
      const sat::geo::AzEl a = sat::net::azel_of(obs, x);
      if (p["el"].as<double>() > -5) {
        const double e = sat::geo::separation(a, {p["az"], p["el"]});
        if (is_sl) sl_max[k] = std::max(sl_max[k], e);
        else sgp_max = std::max(sgp_max, e);
      }
      k++;
    }
  }
  printf("SGP4 (bright sats + ISS) max error vs skyfield: %.4f deg\n", sgp_max);
  CHECK(sgp_max < 0.02, "sgp4 error %.3f", sgp_max);
  const int offs[8] = {0, 10, 30, 90, 600, 3600, 6 * 3600, 20 * 3600};
  for (int k = 0; k < 8; k++) printf("  Starlink J2 model at +%6ds: max %.3f deg\n", offs[k], sl_max[k]);
  CHECK(sl_max[7] < 1.0, "starlink model error %.3f", sl_max[7]);
  {  // fix + dead reckoning: fix at T0, predict T0+10 s, +30 s and +90 s
    double mx = 0;
    for (JsonObject o : V["truth"].as<JsonArray>()) {
      if (strcmp(o["kind"], "starlink") == 0) continue;
      auto &s = sgp[o["id"].as<int32_t>()];
      sat::SatRec r;
      if (!sat::net::sgp_rec(s, T0, r, obs)) continue;
      for (JsonObject p : o["pts"].as<JsonArray>()) {
        const double dt = p["dt"];
        if (dt > 90 || p["el"].as<double>() < 0) continue;
        const auto a = sat::geo::azel(obs, sat::geo::sat_ecef(r, T0 + dt));
        mx = std::max<double>(mx, sat::geo::separation(a, {p["az"], p["el"]}));
      }
    }
    printf("Dead reckoning up to 90 s past a fix: max %.3f deg\n", mx);
    CHECK(mx < 0.3, "dead reckoning %.3f", mx);
  }
  {  // timing
    auto t0 = std::chrono::steady_clock::now();
    double x[3] = {0, 0, 0};
    for (int i = 0; i < 10000; i++) sat::net::sgp_ecef(sgp[25544], T0 + i, x);
    auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; i++) sat::net::sl_ecef(sl.begin()->second, T0 + i, x);
    auto t2 = std::chrono::steady_clock::now();
    auto t3 = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; i++) { sat::net::SgpSat tmp; sat::net::Omm o{}; o.n_revday = 15.06; o.ecc = 0.0002; o.incl = 53; o.epoch = T0; o.id = 1; o.bstar = 1e-4; sat::net::make_sgp(o, tmp); }
    printf("host: sgp4init %.2f us\n", std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t3).count() / 1000);
    printf("host: sgp4 %.2f us, starlink model %.2f us per call\n",
           std::chrono::duration<double, std::micro>(t1 - t0).count() / 10000,
           std::chrono::duration<double, std::micro>(t2 - t1).count() / 10000);
  }

  // ============ VER-2 pipeline on real LVGL
  lv_init();
  lv_display_t *disp = lv_display_create(480, 480);
  alignas(LV_DRAW_BUF_ALIGN) static uint16_t draw_buf[480 * 480 / 8];
  lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);
  lv_obj_t *page1 = lv_obj_create(nullptr), *page2 = lv_obj_create(nullptr);
  for (auto *p : {page1, page2}) {
    lv_obj_set_style_bg_color(p, lv_color_hex(sat::PAGE_BG), 0);
    lv_obj_set_style_text_color(p, lv_color_hex(0xC9D3F2), 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  }
  const lv_font_t *f12 = &lv_font_montserrat_12, *f16 = &lv_font_montserrat_16;
  label(page1, 10, 4, "SKY TRACKER", f16, 0xFF8A1F);
  sat::Widgets w;
  w.status = label(page1, 10, 27, "", f12, 0x7E8BB3);
  w.counts = label(page1, 0, 0, "", f12, 0x7E8BB3);
  lv_obj_align(w.counts, LV_ALIGN_TOP_RIGHT, -10, 31);
  w.clock = label(page1, 0, 0, "--:--", f16, 0xC9D3F2);
  lv_obj_align(w.clock, LV_ALIGN_TOP_RIGHT, -10, 1);
  w.sky = lv_obj_create(page1);
  lv_obj_set_pos(w.sky, 56, 58);
  lv_obj_set_size(w.sky, 368, 368);
  lv_obj_set_style_radius(w.sky, 184, 0);
  lv_obj_set_style_bg_color(w.sky, lv_color_hex(sat::SKY_BG), 0);
  lv_obj_set_style_border_width(w.sky, 0, 0);
  lv_obj_set_style_pad_all(w.sky, 0, 0);
  w.foot_iss = label(page1, 10, 432, "", f16, 0xC9D3F2);
  w.foot_pass = label(page1, 10, 454, "", f16, 0xFFD54A);
  w.list = lv_table_create(page2);
  lv_obj_set_pos(w.list, 10, 8);
  lv_obj_set_style_text_font(w.list, f12, LV_PART_ITEMS);
  lv_obj_set_style_pad_all(w.list, 4, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(w.list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(w.list, LV_OPA_TRANSP, LV_PART_ITEMS);
  lv_obj_set_style_border_width(w.list, 0, 0);
  lv_obj_set_style_border_color(w.list, lv_color_hex(0x22305A), LV_PART_ITEMS);
  lv_obj_set_style_border_width(w.list, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_side(w.list, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
  w.label_font = f12;
  w.title_font = f16;
  w.card_font = &lv_font_montserrat_14;  // stands in for mono14
  w.card_title_font = f16;               // (no 18 pt Montserrat in the host LVGL)
  w.alert_font = &lv_font_montserrat_16;  // stands in for mono15
  w.card_icon_big = &mdi40;
  w.icon_font = &mdi14;
  w.card_icon_font = &mdi20;
  {  // almanac Moon picture, lower right (UI-20 / UI-36)
    static uint8_t mbuf[32 * 32 * 4];
    lv_obj_t *cv = lv_canvas_create(page1);
    lv_canvas_set_buffer(cv, mbuf, 32, 32, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(cv, 438, 360);
    w.moon_img = cv;
  }
  lv_screen_load(page1);

  // canned CelesTrak bodies
  host_http_bodies["CATNR=25544&FORMAT=csv"] = slurp(V["csv"]["iss"].as<const char *>());
  host_http_bodies["CATNR=48274&FORMAT=csv"] = slurp(HOST_DIR "/csv4_css.csv");  // UI-52
  host_http_bodies["GROUP=visual&FORMAT=csv"] = slurp(V["csv"]["sat"].as<const char *>());
  host_http_bodies["GROUP=starlink&FORMAT=csv"] = slurp(V["csv"]["starlink"].as<const char *>());
  host_http_bodies["GROUP=gps-ops&FORMAT=csv"] = slurp(HOST_DIR "/csv4_gps.csv");
  host_http_bodies["GROUP=galileo&FORMAT=csv"] = slurp(HOST_DIR "/csv4_gal.csv");
  host_http_bodies["GROUP=glo-ops&FORMAT=csv"] = slurp(HOST_DIR "/csv4_glo.csv");
  host_http_bodies["GROUP=beidou&FORMAT=csv"] = slurp(HOST_DIR "/csv4_bds.csv");
  host_http_bodies["GROUP=geo&FORMAT=csv"] = slurp(HOST_DIR "/csv4_geo.csv");
  host_http_bodies["satcat/records.php?GROUP=visual"] = "[{\"NORAD_CAT_ID\":694,\"OWNER\":\"US\"},{\"NORAD_CAT_ID\":40003,\"OWNER\":\"PRC\"}]";

  {  // UI-37: NOAA Kp in the current object form, 3 days back and 3 ahead, a Kp 5 peak ahead
    std::string js = "[";
    const int64_t t0 = ((int64_t) T0 / 10800) * 10800;
    for (int i = -24; i <= 24; i++) {
      const int64_t tt = t0 + i * 10800;
      time_t tv = (time_t) tt; struct tm g; gmtime_r(&tv, &g);
      char row[160];
      const float kp = i == 4 ? 5.33f : 2.0f + 0.33f * (i & 1);
      snprintf(row, sizeof(row), "%s{\"time_tag\":\"%04d-%02d-%02dT%02d:00:00\",\"kp\":%.2f,\"observed\":\"%s\",\"noaa_scale\":null}",
               i == -24 ? "" : ",", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, kp, i > 0 ? "predicted" : "observed");
      js += row;
    }
    js += "]";
    host_http_bodies["noaa-planetary-k-index-forecast.json"] = js;
  }
  sat::Config c;
  c.lat = LAT;
  c.lon = LON;
  c.alt_m = ALT;
  sat_host_now = T0;
  // REAL_SKY=1: the real sky now, from CelesTrak data cached by ./fetch_celestrak.py (no checks)
  const bool real_sky = getenv("REAL_SKY") != nullptr;
  if (real_sky) {
    const struct { const char *key, *file; } RS[] = {
        {"CATNR=25544&FORMAT=csv", "iss.csv"},         {"CATNR=48274&FORMAT=csv", "css.csv"},
        {"GROUP=visual&FORMAT=csv", "visual.csv"},     {"GROUP=starlink&FORMAT=csv", "starlink.csv"},
        {"GROUP=gps-ops&FORMAT=csv", "gps-ops.csv"},   {"GROUP=galileo&FORMAT=csv", "galileo.csv"},
        {"GROUP=glo-ops&FORMAT=csv", "glo-ops.csv"},   {"GROUP=beidou&FORMAT=csv", "beidou.csv"},
        {"GROUP=geo&FORMAT=csv", "geo.csv"},           {"satcat/records.php?GROUP=visual", "satcat_visual.json"}};
    for (const auto &r : RS) {
      const std::string body = slurp((std::string(HOST_DIR "/cache/celestrak/") + r.file).c_str());
      if (body.empty()) {
        printf("REAL_SKY: no cache/celestrak/%s - run ./fetch_celestrak.py first\n", r.file);
        return 1;
      }
      host_http_bodies[r.key] = body;
    }
    sat_host_now = (double) time(nullptr);
  }
  sat::setup(c, w);
  sat::tick();  // queues the element download
  run_jobs();   // downloads, computes passes
  if (real_sky) {
    for (int k = 0; k < 4; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
    sat::tick();
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/real_sky.ppm");
    printf("REAL_SKY: ISS %d, %zu satellites, %zu Starlink -> " OUT_DIR "/real_sky.ppm\n", (int) sat::net::have_iss,
           sat::net::sats.size(), sat::net::starlink.size());
    return 0;
  }
  CHECK(sat::net::have_iss && sat::net::sats.size() == 60 && sat::net::starlink.size() == 600, "elements %d/%zu/%zu",
        sat::net::have_iss, sat::net::sats.size(), sat::net::starlink.size());
  for (int k = 0; k < 3; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
  sat_host_now += 2; sat::tick();
  int bound = 0, bound_sl = 0;
  for (auto &m : sat::ui.sats) bound += m.id >= 0;
  for (auto &m : sat::ui.starlink) bound_sl += m.id >= 0;
  printf("Markers bound: %d satellites (%d overhead), %d Starlink (%d in cone of %d overhead), ISS %s\n", bound,
         sat::live.sat_total, bound_sl, sat::ui.starlink_in_cone, sat::live.starlink_total, sat::ui.iss.id >= 0 ? "yes" : "no");
  CHECK(bound > 0 && sat::ui.iss.id == 25544, "nothing bound");
  // markers where skyfield says they are (T0 + elapsed)
  {
    double mx = 0;
    const double t = sat_host_now;
    for (auto &m : sat::ui.sats) {
      if (m.id < 0 || !m.up || !sgp.count(m.id)) continue;  // GNSS markers have no skyfield vector
      auto a = sat::geo::azel(obs, sat::geo::sat_ecef(m.rec, t));
      auto &s = sgp[m.id];
      double x[3] = {0, 0, 0};
      sat::net::sgp_ecef(s, t, x);
      mx = std::max<double>(mx, sat::geo::separation(a, sat::net::azel_of(obs, x)));
    }
    printf("Drawn satellites vs direct SGP4 at draw time: max %.3f deg\n", mx);
    CHECK(mx < 0.1, "marker error %.3f", mx);
  }
  // passes vs skyfield
  {
    JsonArray P = V["passes"];
    CHECK(sat::live.passes.size() >= 2, "passes %zu", sat::live.passes.size());
    double worst = 0;
    for (size_t i = 0; i < sat::live.passes.size() && i < P.size(); i++) {
      const auto &p = sat::live.passes[i];
      worst = std::max({worst, fabs(p.start - P[i]["rise"].as<double>()), fabs(p.end - P[i]["set"].as<double>()),
                        fabs(p.max - P[i]["max"].as<double>())});
      printf("Pass %zu: rise %+.0f s  max %+.0f s (%.1f° vs %.1f°)  set %+.0f s  %s  %s->%s->%s\n", i,
             p.start - P[i]["rise"].as<double>(), p.max - P[i]["max"].as<double>(), p.max_el,
             P[i]["max_el"].as<double>(), p.end - P[i]["set"].as<double>(), p.visible ? "visible" : "not visible",
             p.start_dir, p.max_dir, p.end_dir);
    }
    CHECK(worst < 20, "pass timing off by %.0f s", worst);
  }
  char st[200];
  sat::status_text(sat_host_now, st, sizeof(st));
  printf("Status: %s\n", st);
  CHECK(strstr(st, "Orbital data updated") != nullptr, "status '%s'", st);

  // ---- UI-28 orbit classes / UI-29 debris
  {
    using namespace sat::net;
    using namespace sat;
    auto cls_of = [](const char *name, double n, double e) {
      Omm o{};
      snprintf(o.name, sizeof(o.name), "%s", name);
      o.epoch = sat_host_now; o.n_revday = n; o.ecc = e; o.incl = 50; o.id = 99999;
      SgpSat s;
      make_sgp(o, s);
      return std::make_pair((int) s.cls, s.debris);
    };
    auto t4b = cls_of("TITAN 4B R/B", 15.10646547, 0.00407311);
    auto mol = cls_of("MOLNIYA 1-93", 2.006, 0.72);
    auto geo = cls_of("INTELSAT 21", 1.00272, 0.0002);
    auto gps = cls_of("GPS BIIF-5 (PRN 30)", 2.0056, 0.005);
    auto deb = cls_of("COSMOS 2251 DEB", 14.3, 0.002);
    CHECK(t4b.first == C_CLS_LEO && t4b.second, "Titan 4B R/B: class %d debris %d", t4b.first, t4b.second);
    CHECK(mol.first == C_CLS_HEO && !mol.second, "Molniya class %d", mol.first);
    CHECK(geo.first == C_CLS_GEO, "GEO class %d", geo.first);
    CHECK(gps.first == C_CLS_MEO, "GPS class %d", gps.first);
    CHECK(deb.second, "DEB not flagged");
    CHECK(meo_sats.size() == 37, "GNSS list %zu", meo_sats.size());
    int tags[4] = {0, 0, 0, 0};
    for (auto &s : meo_sats) tags[0] += s.tag == 'G', tags[1] += s.tag == 'E', tags[2] += s.tag == 'R', tags[3] += s.tag == 'C';
    CHECK(tags[0] == 12 && tags[1] == 9 && tags[2] == 8 && tags[3] == 8, "tags G%d E%d R%d C%d", tags[0], tags[1], tags[2], tags[3]);
    int meo_rec = 0, sq = 0;
    for (auto &r : sat::live.sats) meo_rec += r.cls == C_CLS_MEO;
    for (auto &m : sat::ui.sats) sq += m.id >= 0 && m.style == sat::MS_SQUARE;
    printf("MEO records above: %d, square markers: %d\n", meo_rec, sq);
    CHECK(meo_rec > 0 && sq > 0, "no MEO markers");
    CHECK(geo_sats.empty() && sat::live.geo.empty(), "GEO downloaded while off");
    // GEO on
    sat::set_layers(true, true, true, true);
    sat_host_now += 2; sat::tick(); run_jobs(); sat_host_now += 2; sat::tick(); run_jobs(); sat::tick();
    printf("GEO: %zu listed, %zu above, %zu dots\n", geo_sats.size(), sat::live.geo.size(), sat::ui.geo_pts.size());
    CHECK(geo_sats.size() == 40 && !sat::ui.geo_pts.empty(), "GEO pipeline %zu/%zu", geo_sats.size(), sat::ui.geo_pts.size());
    // tap a GEO dot far from other markers -> card says GEO
    const auto &gp = sat::ui.geo_pts[0];
    sat::select_object(sat::K_GEO, sat::live.geo[gp.idx].id);
    const char *body = lv_label_get_text(sat::ui.card_body);
    CHECK(strstr(body, "Orbit GEO") != nullptr, "GEO card: %s", body);
    CHECK(strstr(body, " km") != nullptr && strstr(body, "km/s") != nullptr, "km card: %s", body);
    sat::set_miles(true);
    body = lv_label_get_text(sat::ui.card_body);
    { float h = 0; const char *hp = strstr(body, "Height ");
      CHECK(hp && sscanf(hp, "Height %f mi", &h) == 1 && h > 21000 && h < 23000 && strstr(body, "mi/s") && !strstr(body, " km"),
            "miles card: %s", body); }
    const char *hcell = lv_table_get_cell_value(sat::ui.w.list, 1, 6);
    CHECK(strstr(hcell, " mi") != nullptr, "miles list: %s", hcell);
    sat::set_miles(false);
    CHECK(strstr(lv_table_get_cell_value(sat::ui.w.list, 1, 6), " km") != nullptr, "back to km");
    sat::deselect();
    // MEO off: no MEO records next round
    sat::set_layers(true, true, false, true);
    for (int k = 0; k < 6; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
    int meo_left = 0;
    for (auto &r : sat::live.sats) meo_left += r.cls == C_CLS_MEO;
    CHECK(meo_left == 0, "MEO still listed: %d", meo_left);
    sat::set_layers(true, true, true, false);
    for (int k = 0; k < 3; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
    CHECK(sat::ui.geo_pts.empty(), "GEO dots after off: %zu", sat::ui.geo_pts.size());
    // list has the class column
    CHECK(!strcmp(lv_table_get_cell_value(sat::ui.w.list, 0, 3), "CL"), "list header");
    CHECK(!strcmp(lv_table_get_cell_value(sat::ui.w.list, 1, 0), sat::ICON_ISS), "list ISS icon");
    CHECK(!strcmp(lv_table_get_cell_value(sat::ui.w.list, 2, 0), sat::ICON_SUN), "list Sun icon");
    // UI-35: a debris object (R/B) is drawn as a trash can when the switch is on
    {
      int k = -1;
      for (size_t i = 0; i < sat::net::sats.size() && k < 0; i++)
        if (sat::net::sats[i].cls == sat::C_CLS_LEO)
          for (auto &m : sat::ui.sats)
            if (m.id == sat::net::sats[i].id && m.up) { k = (int) i; break; }
      { int leo = 0, up = 0; for (auto &r : sat::live.sats) leo += r.cls == sat::C_CLS_LEO; for (auto &m : sat::ui.sats) up += m.id >= 0 && m.up;
        printf("debris test setup: live %zu (LEO %d), markers up %d, net sats %zu, sats_on %d\n", sat::live.sats.size(), leo, up, sat::net::sats.size(), sat::ui.sats_on); }
      CHECK(k >= 0, "no LEO satellite up to turn into debris");
      if (k >= 0) {
        auto &ss = sat::net::sats[k];
        const bool was = ss.debris;
        ss.debris = true;
        sat::set_debris(false); for (int n = 0; n < 3; n++) { sat_host_now += 2; sat::tick(); run_jobs(); }
        sat::set_debris(true);  for (int n = 0; n < 3; n++) { sat_host_now += 2; sat::tick(); run_jobs(); }
        sat::Marker *mk = nullptr;
        for (auto &m : sat::ui.sats) if (m.id == ss.id) mk = &m;
        int rec = 0; for (auto &r : sat::live.sats) rec += r.id == ss.id && r.debris;
        printf("debris test: id %d rec %d marker %s style %d label '%s' hidden %d dot hidden %d\n", (int) ss.id, rec,
               mk ? "yes" : "no", mk ? mk->style : -1, mk ? lv_label_get_text(mk->label) : "",
               mk ? lv_obj_has_flag(mk->label, LV_OBJ_FLAG_HIDDEN) : -1, mk ? lv_obj_has_flag(mk->dot, LV_OBJ_FLAG_HIDDEN) : -1);
        CHECK(rec == 1 && mk && mk->style == sat::MS_DEBRIS && !lv_obj_has_flag(mk->label, LV_OBJ_FLAG_HIDDEN) &&
              !strcmp(lv_label_get_text(mk->label), "\xF3\xB0\xA9\xB9"), "debris not drawn");
        // the reported case: LEO off, Debris on -> the debris still shows, other LEO do not
        sat::set_layers(false, false, true, false);
        for (int n = 0; n < 3; n++) { sat_host_now += 2; sat::tick(); run_jobs(); }
        int deb = 0, leo_payload = 0;
        for (auto &r : sat::live.sats) { deb += r.debris; leo_payload += !r.debris && r.cls == sat::C_CLS_LEO; }
        sat::Marker *mk2 = nullptr;
        for (auto &m : sat::ui.sats) if (m.id == ss.id) mk2 = &m;
        printf("LEO off + debris on: %d debris, %d LEO payloads, marker %s\n", deb, leo_payload, mk2 ? "yes" : "no");
        CHECK(deb == 1 && leo_payload == 0 && mk2 && !lv_obj_has_flag(mk2->label, LV_OBJ_FLAG_HIDDEN), "debris with LEO off");
        sat::set_layers(true, true, true, false);
        for (int n = 0; n < 3; n++) { sat_host_now += 2; sat::tick(); run_jobs(); }
        ss.debris = was;
      }
    }
  }

  // run 60 s with trails on
  for (int k = 0; k < 30; k++) { sat_host_now += 2; sat::tick(); run_jobs(); lv_timer_handler(); }
  int trails = 0;
  for (auto &m : sat::ui.sats) trails += m.tr_area.x2 >= m.tr_area.x1;
  printf("Trails drawn: %d\n", trails);
  CHECK(trails > 0, "no trails");

  // tap the ISS (or the highest satellite if the ISS is down)
  {
    sat::Marker *target = sat::ui.iss.up ? &sat::ui.iss : nullptr;
    int kind = sat::K_ISS;
    if (!target)
      for (auto &m : sat::ui.sats) if (m.id >= 0 && m.up && (!target || m.el > target->el)) { target = &m; kind = sat::K_SAT; }
    CHECK(target != nullptr, "nothing to tap");
    if (target) {
      sat::select_object(kind, target->id);
      CHECK(!lv_obj_has_flag(sat::ui.card, LV_OBJ_FLAG_HIDDEN), "card hidden");
      printf("Card: %s\n%s\n", lv_label_get_text(sat::ui.card_title), lv_label_get_text(sat::ui.card_body));
    }
    // force an ISS selection for the arc even when it is below the horizon
    sat::ui.sel_kind = sat::K_ISS;
    sat::arc_update(sat_host_now);
    printf("ISS pass arc points: %zu\n", sat::ui.arc.size());
    CHECK(sat::ui.arc.size() >= 2, "no pass arc");
    if (!sat::ui.iss.up) sat::ui.sel_kind = kind;
  }
  sat::set_layers(true, true, true, true);  // render with every class on
  for (int k = 0; k < 3; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
  sat::tick();
  lv_obj_invalidate(lv_screen_active());
  lv_refr_now(disp);
  save_ppm(OUT_DIR "/renders/r4_map.ppm");
  CHECK(!strcmp(lv_label_get_text(sat::ui.card_icon), "\xF3\xB0\x91\xB1"), "card icon (satellite-variant)");
  for (auto &m : sat::ui.sats)
    if (m.id >= 0 && m.up && m.rec.cls == sat::C_CLS_LEO) {
      m.rec.debris = true;  // pretend: render a debris card
      sat::select_object(sat::K_SAT, m.id);
      CHECK(!strcmp(lv_label_get_text(sat::ui.card_icon), "\xF3\xB0\xA9\xB9"), "debris card icon");
      lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r4_debris.ppm");
      break;
    }
  sat::deselect();
  sat::draw_list(sat_host_now);
  lv_screen_load(page2);
  lv_refr_now(disp);
  save_ppm(OUT_DIR "/renders/r4_list.ppm");
  lv_screen_load(page1);

  // error: CelesTrak refuses; the old data stays and the status says so
  {
    sat::net::sats_loaded = sat::net::iss_loaded = sat::net::starlink_loaded = sat_host_now - 13 * 3600;
    host_http_bodies["CATNR=25544&FORMAT=csv"] = "HTTP403";
    sat::ui.next_elem = 0;
    sat_host_now += 2; sat::tick(); run_jobs();
    sat_host_now += 2; sat::tick();
    sat::status_text(sat_host_now, st, sizeof(st));
    printf("Status after 403: %s\n", st);
    CHECK(strstr(st, "403") && strstr(st, "retrying in 1h 59m"), "status '%s'", st);
    CHECK(sat::net::have_iss && !sat::net::sats.empty(), "old data dropped");
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r4_error.ppm");
  }
  // offline: no downloads, positions continue
  {
    sat::host_net_up = false;
    host_last_url.clear();
    sat::ui.next_elem = 0;
    sat::net::status.next_try = 0;
    for (int k = 0; k < 30; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
    CHECK(host_last_url.empty(), "download while offline: %s", host_last_url.c_str());
    CHECK(sat::ui.iss.id >= 0, "positions stopped offline");
    sat::host_net_up = true;
  }
  // toggles
  sat::set_trails(false);
  int left = 0;
  for (auto &m : sat::ui.sats) left += m.tr_area.x2 >= m.tr_area.x1;
  CHECK(left == 0, "trails left after switch-off: %d", left);
  // UI-40..45: planets, alerts, alignments, Starlink train, horizon dimming, night mode
  {
    sat::night_install();
    sat::host_tz_s = -8 * 3600;  // AKDT for the renders
    const auto saved_status = sat::live.status;
    const double t0 = sat_host_now;
    double best_t = t0;
    int best_n = -1;
    for (int k = 0; k < 96; k++) {  // the next 24 h: the moment with the most planets in view
      const double tt = t0 + k * 900.0;
      sat::ui.sm = sat::astro::compute(tt, sat::config().lat, sat::config().lon, 0);
      sat::update_planets(tt, true);
      int n = 0;
      for (int p = 0; p < sat::planets::N_PLANETS; p++) n += sat::ui.planet_visible[p] ? 2 : sat::ui.planet_shown[p];
      if (k % 8 == 0) printf("scan %.0f sun %.1f sat el %.1f shown %d on %d\n", tt, sat::ui.sm.sun.el, sat::ui.planet_azel[4].el, sat::ui.planet_shown[4], sat::ui.planets_on);
      if (n > best_n) { best_n = n; best_t = tt; }
    }
    sat_host_now = best_t;
    sat::ui.planet_next = 0;
    for (int k = 0; k < 3; k++) { sat::tick(); run_jobs(); sat_host_now += 1; }
    sat::align_scan = sat::AlignScan{};
    for (int k = 0; k <= sat::ALIGN_DAYS; k++) sat::align_step(sat_host_now);
    printf("planets at %.0f:", sat_host_now);
    for (int p = 0; p < sat::planets::N_PLANETS; p++)
      printf(" %s el %.0f mag %.1f%s", sat::planets::name(p), sat::ui.planet_azel[p].el, sat::ui.planet_mag[p], sat::ui.planet_visible[p] ? "*" : "");
    printf("\nconj %d %s-%s %.2f at %.0f; parade %d n=%d at %.0f\n", sat::conj_ev.valid, sat::conj_ev.valid ? sat::planets::name(sat::conj_ev.a) : "",
           sat::conj_ev.valid ? sat::planets::name(sat::conj_ev.b) : "", sat::conj_ev.sep, sat::conj_ev.t, sat::parade_ev.valid, sat::parade_ev.count, sat::parade_ev.t);
    CHECK(sat::parade_ev.valid && sat::parade_ev.count >= 4, "no planet parade found in 60 days");
    CHECK(sat::conj_ev.valid && sat::conj_ev.a == sat::planets::MARS && sat::conj_ev.b == sat::planets::JUPITER && sat::conj_ev.sep < 1.5f,
          "Mars-Jupiter conjunction not found");
    // a synthetic Starlink train behind the highest Starlink in view
    sat::Marker *hi = nullptr;
    for (auto &m : sat::ui.starlink) if (m.id >= 0 && m.up && m.el > 15 && (!hi || m.el > hi->el)) hi = &m;
    CHECK(hi != nullptr, "no Starlink high enough for the train");
    if (hi) {
      sat::net::SlElem base{};
      for (auto &e : sat::net::starlink) if (e.id == hi->id) base = e;
      for (int k = 1; k <= 8; k++) {
        sat::net::SlElem e = base;
        e.id = 900000 + k;
        e.num = 9000 + k;
        e.launch = 26150;
        e.m0 -= k * 0.009;
        sat::net::starlink.push_back(e);
      }
      for (auto &e : sat::net::starlink) if (e.id == hi->id) e.launch = 26150;
      { int nn = 0; for (auto &e : sat::net::starlink) nn += e.launch == 26150; printf("net cars %d of %zu\n", nn, sat::net::starlink.size()); }
      sat::net::do_starlink(); sat::tick();
      int tagged = 0, pub = 0;
      for (auto &m : sat::ui.starlink) tagged += m.id >= 0 && m.rec.launch == 26150;
      for (auto &r : sat::live.starlink) pub += r.launch == 26150;
      printf("train cars: published %d, bound %d (base el %.0f)\n", pub, tagged, hi->el);
    }
    printf("trains: %d (%zu cars)\n", sat::ui.n_trains, sat::ui.n_trains ? sat::ui.trains[0].idx.size() : (size_t) 0);
    CHECK(sat::ui.n_trains >= 1 && sat::ui.trains[0].idx.size() >= 8, "train not found");
    // a visible ISS pass in 12 minutes, for its alert
    sat::Pass fake;
    fake.start = (int64_t) sat_host_now + 720; fake.max = fake.start + 300; fake.end = fake.start + 600;
    fake.max_el = 64; fake.visible = true;
    strcpy(fake.start_dir, "NW"); strcpy(fake.max_dir, "NE"); strcpy(fake.end_dir, "SE");
    const auto saved_passes = sat::live.passes;
    sat::live.passes.insert(sat::live.passes.begin(), fake);
    sat::live.status.error[0] = 0;
    sat::Alert al[sat::MAX_ALERTS];
    const int na = sat::collect_alerts(sat_host_now, al, sat::MAX_ALERTS);
    for (int k = 0; k < na; k++) printf("alert %d: %s\n", k, al[k].text);
    CHECK(na >= 3, "only %d alerts", na);
    {
      char lt[24];
      CHECK(sat::launch_text(sat::ISS_LAUNCHED, lt, sizeof(lt)) && !strcmp(lt, "20 Nov 1998"), "launch text %s", lt);
      CHECK(sat::net::launch_from_text("1998-11-20") == sat::ISS_LAUNCHED, "launch parse %u", sat::net::launch_from_text("1998-11-20"));
      sat::Marker *mk = nullptr;
      for (auto &m : sat::ui.sats) if (m.id >= 0 && m.up && m.el > 20) { mk = &m; break; }
      if (mk) {
        mk->rec.launched = sat::net::launch_from_text("2024-01-03");
        sat::select_object(sat::K_SAT, mk->id);
        printf("sat card:\n%s\n", lv_label_get_text(sat::ui.card_body));
        CHECK(strstr(lv_label_get_text(sat::ui.card_body), "Launched 3 Jan 2024") != nullptr, "sat launch line");
        lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
        save_ppm(OUT_DIR "/renders/r5_satcard.ppm");
        sat::deselect();
      }
    }
    const double base_t = ceil(sat_host_now / (6.0 * na)) * 6.0 * na;
    char fn[80];
    for (int k = 0; k < na; k++) {
      sat_host_now = base_t + 6.0 * k + 1;
      sat::live.status.error[0] = 0;
      sat::tick(); sat::draw_hud(sat_host_now);
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      snprintf(fn, sizeof(fn), OUT_DIR "/renders/r5_alert%d.ppm", k);
      save_ppm(fn);
      printf("status %d: %s\n", k, lv_label_get_text(sat::ui.w.status));
    }
    // planet card (the brightest planet in view)
    int pc = -1;
    for (int p = 0; p < sat::planets::N_PLANETS; p++) if (sat::ui.planet_shown[p] && (pc < 0 || sat::ui.planet_mag[p] < sat::ui.planet_mag[pc])) pc = p;
    if (pc >= 0) {
      sat::select_object(sat::K_PLANET, pc);
      printf("planet card %s:\n%s\n", sat::planets::name(pc), lv_label_get_text(sat::ui.card_body));
      CHECK(strstr(lv_label_get_text(sat::ui.card_body), "Greek") != nullptr, "planet card");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r5_planetcard.ppm");
      sat::deselect();
    }
    // UI-46: a constellation card
    for (const auto &nt : sat::ui.names) if (nt.shown) {
      sat::select_object(sat::K_CONST, nt.idx);
      printf("constellation card %s:\n%s\n", sat::sky::NAMES[nt.idx].text, lv_label_get_text(sat::ui.card_body));
      CHECK(strstr(lv_label_get_text(sat::ui.card_body), "Brightest:") != nullptr, "constellation card");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r5_constcard.ppm");
      for (const char *want : {"Orion", "Ursa Major", "Cassiopeia", "Cygnus", "Andromeda", "Lyra"})
        for (int q = 0; q < sat::sky::N_NAMES; q++) if (!strcmp(sat::sky::NAMES[q].text, want)) {
          sat::select_object(sat::K_CONST, q);
          lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
          char fn[96]; snprintf(fn, sizeof(fn), OUT_DIR "/renders/r5_cc_%c%c.ppm", want[0], want[1]); save_ppm(fn); { lv_area_t cc; lv_obj_get_coords(sat::ui.card,&cc); printf("CCBOX %c%c %d %d %d\n", want[0], want[1], (int)cc.x1,(int)cc.y1,(int)cc.x2); }
        }
      sat::select_object(sat::K_MOON, 0); lv_refr_now(disp); save_ppm(OUT_DIR "/renders/r5_moonafter.ppm");
      sat::select_object(sat::K_CONST, nt.idx); lv_refr_now(disp);
      { lv_area_t c1,c2,c3; lv_obj_get_coords(sat::ui.card,&c1); lv_obj_get_coords(sat::ui.card_title,&c2); lv_obj_get_coords(sat::ui.card_icon,&c3);
        printf("card %d-%d title %d-%d icon %d-%d (%d)\n",(int)c1.x1,(int)c1.x2,(int)c2.x1,(int)c2.x2,(int)c3.x1,(int)c3.x2,(int)lv_obj_get_width(sat::ui.card_icon)); }
      sat::deselect();
      break;
    }
    {  // every rank 1-2 name has its lore
      int miss = 0;
      for (int i = 0; i < sat::sky::N_NAMES; i++)
        if (sat::sky::NAMES[i].rank <= sat::NAME_MAX_RANK && !sat::lore::find(sat::sky::NAMES[i].text)) { printf("no lore: %s\n", sat::sky::NAMES[i].text); miss++; }
      CHECK(miss == 0, "%d constellations without lore", miss);
    }
    {  // UI-63 comets (JPL rows, faint, plus one bright made up for the test) and UI-64 Milky Way
      std::string cj = slurp(HOST_DIR "/fixtures/comet_real.json");  // a JPL SBDB answer (fixture)
      if (cj.find("\"data\":[") == std::string::npos) printf("SKIP UI-63 comets: no comet_real.json fixture\n"); else {
      char row[256];
      const double jd_now = sat_host_now / 86400.0 + 2440587.5;
      int node = 60, argp = 120, incl = 140;  // turn the made-up orbit until the comet stands well up now
      bool found = false;
      for (int in : {140, 100, 60, 30}) for (int wp = 0; wp < 360 && !found; wp += 30) for (int nd = 0; nd < 360 && !found; nd += 10) {
        sat::comets::El tc{};
        tc.q = 0.9; tc.e = 1.0005; tc.tp = jd_now + 15; tc.om = nd; tc.w = wp; tc.i = in; tc.M1 = 4.5f; tc.K1 = 10;
        const auto pp = sat::comets::position(tc, jd_now);
        const double lst = sat::astro::rad(sat::astro::wrap360(sat::astro::gmst_deg(jd_now) + sat::config().lon));
        const auto aa = sat::astro::horizontal(pp.equ, sat::config().lat, lst);
        if (aa.el > 20 && aa.el < 70 && pp.mag < 5.0 && pp.d > 0.3) { node = nd; argp = wp; incl = in; found = true; }
      }
      printf("test comet orbit found %d: node %d w %d i %d\n", found, node, argp, incl);
      snprintf(row, sizeof(row), "[\"     C/2026 Z9 (Testfield)\",\"1.0005\",\"0.9\",\"%.4f\",\"%d\",\"%d\",\"%d\",\"4.5\",\"10\"],",
               jd_now + 15, node, argp, incl);
      cj.insert(cj.find("\"data\":[") + 8, row);
      host_http_bodies["sbdb_query.api"] = cj;
      host_last_url.clear();
      sat::net::comet_next = 0;
      sat::net::do_comets(sat_host_now);
      printf("comet url %s\n", host_last_url.c_str());
      sat::tick();
      sat::update_comets(sat_host_now, true);
      int shown = 0;
      for (int k = 0; k < sat::Ui::MAX_COMETS; k++) if (sat::ui.comet_idx[k] >= 0) {
        const auto &c = sat::live.comet_list[sat::ui.comet_idx[k]];
        printf("comet slot %d: %s [%s] mag %.1f az %.0f el %.1f shown %d vis %d head %d,%d tail %d,%d\n", k, c.name, c.tag,
               sat::ui.comet_mag[k], sat::ui.comet_azel[k].az, sat::ui.comet_azel[k].el, sat::ui.comet_shown[k],
               sat::ui.comet_visible[k], sat::ui.comet_x[k], sat::ui.comet_y[k], sat::ui.comet_tx[k], sat::ui.comet_ty[k]);
        shown += sat::ui.comet_shown[k];
      }
      CHECK(sat::live.comet_list.size() == 39 && host_last_url.find("sbdb_query.api") != std::string::npos &&
            host_last_url.find("%7CRG%7C") != std::string::npos, "comet list (%zu)", sat::live.comet_list.size());
      CHECK(shown == 1 && sat::ui.comet_idx[1] < 0 && !strcmp(sat::live.comet_list[sat::ui.comet_idx[0]].tag, "Testfield"),
            "one bright comet on the map");
      printf("mw spans %zu bands %zu cells %zu\n", sat::ui.mw_spans.size(), sat::ui.mw_band.size(), sat::ui.mw_cells.size());
      CHECK(sat::ui.mw_spans.size() > 200, "milky way spans");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r13_mw_comet.ppm");
      // tap the comet: its card
      sat::select_object(sat::K_COMET, sat::ui.comet_idx[0]);
      printf("comet card %s:\n%s\n", lv_label_get_text(sat::ui.card_title), lv_label_get_text(sat::ui.card_body));
      CHECK(strstr(lv_label_get_text(sat::ui.card_body), "Nearest the Sun") && sat::card_open(), "comet card");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r13_comet_card.ppm");
      sat::deselect();
      sat::Alert al[sat::MAX_ALERTS];
      const int na = sat::collect_alerts(sat_host_now, al, sat::MAX_ALERTS);
      bool ca = false;
      for (int i = 0; i < na; i++) { ca |= strstr(al[i].text, "Comet Testfield visible") != nullptr; if (strstr(al[i].text, "Comet")) printf("  alert: %s\n", al[i].text); }
      CHECK(ca, "comet alert");
      // Milky Way off: no spans; back on
      sat::set_milky_way(false); sat_host_now += 1; sat::tick();
      CHECK(sat::ui.mw_spans.empty(), "milky way off");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r13_no_mw.ppm");
      sat::set_milky_way(true); sat_host_now += 1; sat::tick();
      sat::set_comets(false);
      CHECK(!sat::ui.comet_shown[0], "comets off");
      sat::set_comets(true);
      CHECK(sat::ui.comet_shown[0], "comets on again");
    } }
    // night mode
    sat::set_night(true);
    sat::tick();
    CHECK(sat::ui.night_active, "night mode not active in the dark");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r5_night.ppm");
    sat::set_night(false);
    sat::tick();
    sat::live.passes = saved_passes;
    sat::live.status = saved_status;
    sat::host_tz_s = 0;
    sat_host_now = t0; sat::tick(); run_jobs(); sat::tick();
  }
  // ---- DATA-10 flash cache round trip
  {
    using namespace sat::net;
    host_part_enabled = true;
    // make sure every group has something to save
    printf("cache before: iss %d sats %zu starlink %zu owners %zu\n", have_iss, sats.size(), starlink.size(), owners.size());
    cache_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "skydata");
    const size_t ns = sats.size(), nl = starlink.size(), no = owners.size();
    const int32_t id0 = ns ? sats[0].id : 0; const double m0 = nl ? starlink[0].m0 : 0; const double iss_ep = iss.epoch;
    const double sl_t = sats_loaded, ll_t = starlink_loaded;
    cache_save_a(); cache_save_b();
    const size_t nm = meo_sats.size(), ng = geo_sats.size();
    cache_save_list(CACHE_C_OFF, CACHE_C_MAX, meo_sats, meo_loaded, "GNSS");
    cache_save_list(CACHE_D_OFF, CACHE_D_MAX, geo_sats, geo_loaded, "GEO");
    meo_sats.clear(); geo_sats.clear();
    double ml = 0, gl = 0;
    CHECK(cache_load_list(CACHE_C_OFF, CACHE_C_MAX, meo_sats, ml, "GNSS") && meo_sats.size() == nm && meo_sats[0].tag == 'G', "GNSS cache %zu", meo_sats.size());
    CHECK(cache_load_list(CACHE_D_OFF, CACHE_D_MAX, geo_sats, gl, "GEO") && geo_sats.size() == ng && geo_sats[0].cls == sat::C_CLS_GEO, "GEO cache %zu", geo_sats.size());
    sats.clear(); starlink.clear(); owners.clear(); have_iss = false; iss = SgpSat{}; sats_loaded = starlink_loaded = iss_loaded = 0;
    cfg.sats_on = cfg.starlink_on = true;
    cache_load();
    CHECK(have_iss && iss.epoch == iss_ep, "cache ISS epoch %f vs %f", iss.epoch, iss_ep);
    CHECK(sats.size() == ns && (!ns || sats[0].id == id0), "cache sats %zu vs %zu", sats.size(), ns);
    CHECK(starlink.size() == nl && (!nl || starlink[0].m0 == m0), "cache starlink %zu vs %zu", starlink.size(), nl);
    CHECK(owners.size() == no, "cache owners %zu vs %zu", owners.size(), no);
    CHECK(sats_loaded == sl_t && starlink_loaded == ll_t, "cache load times");
    // a damaged byte in the payload must be caught
    host_flash[CACHE_B_OFF + CACHE_HDR + 5] ^= 0x55;
    starlink.clear(); cache_load();
    CHECK(starlink.empty(), "damaged Starlink cache was loaded");
    // a changed satellite group drops the satellites but keeps the ISS
    cfg.sat_group = "stations"; sats.clear(); cache_load();
    CHECK(sats.empty() && have_iss, "group change: sats %zu", sats.size());
    // DATA-12: a 403 hold survives a reboot
    status.next_try = 0; hold_save(sat_host_now + 7200); status.next_try = 0; cache_load();
    CHECK(status.next_try == sat_host_now + 7200, "hold not carried over: %f", status.next_try);
    status.next_try = 0;
    // UI-54a: the launch list survives a reboot, and holds off the next download
    {
      const double ln0 = launch_next;
      sat::pvector<LaunchRec> lv(3);
      snprintf(lv[0].name, sizeof(lv[0].name), "Falcon Heavy | NROL-97");
      lv[0].net = sat_host_now + 1400;
      lv[2].net = sat_host_now + 9000;
      launch_cache_save(lv, sat_host_now - 600);
      pending.launches.clear();
      launch_next = 0;
      launch_cache_load();
      CHECK(pending.launches.size() == 3 && !strcmp(pending.launches[0].name, "Falcon Heavy | NROL-97") &&
            launch_next == sat_host_now - 600 + LAUNCH_EVERY_S, "launch cache (%zu)", pending.launches.size());
      host_flash[CACHE_LAUNCH_OFF + 40] ^= 0x55;
      pending.launches.clear();
      launch_cache_load();
      CHECK(pending.launches.empty(), "damaged launch cache was loaded");
      launch_next = ln0;
    }
    // DATA-10: a layer switched on after boot comes from flash, with no download first
    {
      const auto jc0 = jc;
      meo_sats.clear(); geo_sats.clear(); meo_loaded = geo_loaded = 0;
      jc.meo_on = jc.geo_on = true; jc.starlink_on = false; jc.sats_on = jc.debris_on = false;
      host_last_url.clear();
      do_elements();
      CHECK(meo_sats.size() == nm && geo_sats.size() == ng && meo_loaded == ml && geo_loaded == gl,
            "lazy fill: meo %zu geo %zu", meo_sats.size(), geo_sats.size());
      CHECK(host_last_url.empty(), "lazy fill downloaded first: %s", host_last_url.c_str());
      jc = jc0;
    }
    printf("cache: %zu sats, %zu starlink, %zu owners round-tripped\n", ns, nl, no);
  }
  // ---- preview: dots vs all icons, at the ISS's next culmination
  {
    sat::deselect();
    const sat::Pass *p = nullptr;
    for (const auto &x : sat::live.passes) {
      if (x.end <= sat_host_now) continue;
      if (!p) p = &x;
      const auto sm = sat::astro::compute((double) x.max, sat::config().lat, sat::config().lon, 0);
      printf("pass %lld max_el %.0f moon el %.0f\n", (long long) x.max, x.max_el, sm.moon.el);
      if (sm.moon.el > 5) { p = &x; break; }
    }
    if (p) sat_host_now = (double) p->max;
    for (int k = 0; k < 4; k++) { sat_host_now += 1; sat::tick(); run_jobs(); }
    sat::tick();
    printf("preview: ISS up %d el %.1f, Moon el %.1f illum %.2f\n", sat::ui.iss.up, sat::ui.iss.el,
           sat::ui.sm.moon.el, sat::ui.sm.moon_illum);
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/preview_icons.ppm");

    // map Moon at several phases and Sun directions (UI-6), for the preview strip
    lv_obj_t *cv = lv_canvas_create(sat::ui.w.sky);
    lv_draw_buf_t *mb = lv_draw_buf_create(sat::MOON_PX, sat::MOON_PX, LV_COLOR_FORMAT_ARGB8888, 0);
    lv_canvas_set_draw_buf(cv, mb);
    const float ks[8] = {0.03f, 0.15f, 0.35f, 0.5f, 0.65f, 0.85f, 0.97f, 0.5f};
    const float angs[8] = {0, 0.3f, 0.6f, 1.57f, 2.4f, 3.14f, 3.6f, -0.8f};
    FILE *f = fopen(OUT_DIR "/renders/moon_map_strip.raw", "wb");
    for (int i = 0; i < 8; i++) {
      lv_draw_buf_clear(mb, nullptr);
      sat::render_moon(cv, ks[i], cosf(angs[i]), sinf(angs[i]));
      for (int y = 0; y < sat::MOON_PX; y++)
        fwrite((uint8_t *) mb->data + y * mb->header.stride, 4, sat::MOON_PX, f);
    }
    fclose(f);
    lv_obj_delete(cv);
  }
  // UI-6: Sun just above the horizon stays wholly inside the ring
  {
    double t0 = sat_host_now, tt = t0;
    for (int k = 0; k < 24 * 60; k++) {
      tt = t0 + k * 60.0;
      const auto sm = sat::astro::compute(tt, sat::config().lat, sat::config().lon, 0);
      if (sm.sun.el > 0.2f && sm.sun.el < 1.5f) break;
    }
    sat_host_now = tt; sat::tick(); run_jobs(); sat::tick();
    sat::draw_sun_moon(sat_host_now, sat_host_now);
    lv_obj_update_layout(sat::ui.w.sky);
    const int cx = lv_obj_get_x(sat::ui.sun) + sat::SUN_HALO_PX / 2 - sat::ui.cx;
    const int cy = lv_obj_get_y(sat::ui.sun) + sat::SUN_HALO_PX / 2 - sat::ui.cy;
    const float d = sqrtf((float) (cx * cx + cy * cy));
    printf("sun el %.2f az %.0f, centre %.1f px from zenith (ring %d)\n", sat::ui.sm.sun.el, sat::ui.sm.sun.az, d, sat::ui.radius);
    CHECK(d + sat::SUN_HALO_PX / 2 <= sat::ui.radius + 1.5f, "sun pokes out: %.1f", d);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r4_sunedge.ppm");
    sat_host_now = t0; sat::tick(); run_jobs(); sat::tick();
  }
  // HW-9a: declination from the location (WMM2025, checked against pygeomag)
  {
    const auto f = sat::wmm::compute(61.581, -149.439, 0.1, 2026.73);
    CHECK(fabs(f.decl - 14.169907) < 1e-4 && fabs(f.incl - 74.286076) < 1e-4, "WMM %.6f %.6f", f.decl, f.incl);
    const auto s2 = sat::wmm::compute(-33.9, 151.2, 0.0, 2025.0);
    printf("WMM: Wasilla %.2f°, Sydney %.2f°; device decl %.2f, maglat %.1f\n", f.decl, s2.decl, sat::declination(), sat::ui.maglat);
    CHECK(fabs(sat::declination() - f.decl) < 0.3, "device declination %.2f", sat::declination());
  }
  // UI-37: Kp parsed in both forms
  {
    sat::pvector<sat::net::KpPt> k;
    CHECK(sat::net::parse_kp(host_http_bodies["noaa-planetary-k-index-forecast.json"].c_str(), k) == 49 && k[30].predicted && !k[10].predicted, "Kp object form %zu", k.size());
    const char *old = "[[\"time_tag\",\"kp\",\"observed\",\"noaa_scale\"],[\"2026-09-19 00:00:00\",\"2.33\",\"observed\",null],[\"2026-09-27 03:00:00\",\"4.67\",\"predicted\",\"G1\"]]";
    CHECK(sat::net::parse_kp(old, k) == 2 && fabsf(k[1].kp - 4.67f) < 1e-3 && k[1].predicted && !k[0].predicted, "Kp array form %zu", k.size());
    float n, m;
    CHECK(sat::kp_values(sat_host_now, n, m) && m >= n && sat::live.kp.size() == 49, "Kp values %.2f %.2f (live %zu)", n, m, sat::live.kp.size());
  }
  // UI-41b: solstices and equinoxes (USNO: 2026-06-21 08:24, 2026-09-23 00:05, 2026-12-21 20:50, 2027-03-20 20:24 UTC)
  {
    const double want[4] = {1782030240, 1790121900, 1797886200, 1805574240};
    double t = 1780000000;
    for (int i = 0; i < 4; i++) {
      sat::SeasonEv e = sat::next_season(t);
      printf("season kind %d at %.0f (want %.0f, %+.0f min)\n", e.kind, e.t, want[i], (e.t - want[i]) / 60);
      CHECK(fabs(e.t - want[i]) < 40 * 60, "season %d off", i);
      t = e.t + 86400;
    }
    sat::Alert al[sat::MAX_ALERTS];
    const int n = sat::collect_alerts(1797886200 - 2 * 86400, al, sat::MAX_ALERTS);
    bool found = false;
    for (int i = 0; i < n; i++) { printf("alert: %s\n", al[i].text); if (strstr(al[i].text, "Winter solstice")) found = true; }
    CHECK(found, "solstice alert");
  }
  // UI-36: Sun and Moon cards, the path arc, the almanac Moon tap
  {
    sat::select_object(sat::K_SUN, 0);
    const char *sb = lv_label_get_text(sat::ui.card_body);
    printf("Sun card:\n%s\n", sb);
    CHECK(strstr(sb, "Noon") && strstr(sb, "Aurora ") && strstr(sb, "Dark"), "sun card: %s", sb);
    CHECK(!strcmp(lv_label_get_text(sat::ui.card_title), "Sun"), "sun title");
    CHECK(sat::body.path.size() > 10 && !sat::body.ticks.empty(), "sun path %zu/%zu", sat::body.path.size(), sat::body.ticks.size());
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r4_suncard.ppm");
    { auto t0=std::chrono::steady_clock::now(); for(int q=0;q<50;q++){ sat::body.until=0; sat::body_info_update(sat::K_SUN, sat_host_now); }
      double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/50;
      auto t1=std::chrono::steady_clock::now(); for(int q=0;q<2000;q++){ volatile auto r=sat::astro::compute(sat_host_now+q*60, 61.6, -149.4, 100); (void)r; }
      double cu=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t1).count()/2000;
      auto t2=std::chrono::steady_clock::now(); for(int q=0;q<50;q++){ sat::body.until=0; sat::body_info_update(sat::K_MOON, sat_host_now); }
      double mu=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t2).count()/50;
      printf("BENCH sun %.0f us, moon %.0f us, compute %.2f us -> sun ~%.0f computes, moon ~%.0f\n", us, mu, cu, us/cu, mu/cu); }
    sat::set_miles(true);
    CHECK(strstr(lv_label_get_text(sat::ui.card_body), "million mi") != nullptr, "sun card miles: %s", lv_label_get_text(sat::ui.card_body));
    sat::set_miles(false);
    CHECK(strstr(lv_label_get_text(sat::ui.card_body), "million km") != nullptr, "sun card km");
    sat::deselect();
    lv_obj_send_event(sat::ui.w.moon_img, LV_EVENT_CLICKED, nullptr);
    CHECK(sat::ui.sel_kind == sat::K_MOON, "moon icon tap: %d", sat::ui.sel_kind);
    const char *mb = lv_label_get_text(sat::ui.card_body);
    printf("Moon card:\n%s\n", mb);
    CHECK(strstr(mb, "days old") && strstr(mb, "Full") && strstr(mb, "Distance"), "moon card: %s", mb);
    CHECK(!lv_obj_has_flag(sat::ui.card_moon, LV_OBJ_FLAG_HIDDEN), "moon picture hidden");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r4_mooncard.ppm");
    lv_obj_send_event(sat::ui.w.moon_img, LV_EVENT_CLICKED, nullptr);  // a touch with the card up closes it
    CHECK(!sat::card_open(), "moon card still open");
    // phase times: new Moon 2026-09-11 03:27 UTC, full 2026-09-26 16:49 UTC (USNO)
    const double nm = sat::astro::moon_phase_time(1789000000.0, 0, 1), fm = sat::astro::moon_phase_time(1789000000.0, 180, 1);
    printf("next new %.0f (want ~1789097220), full %.0f (want ~1790441340)\n", nm, fm);
    CHECK(fabs(nm - 1789097220.0) < 3 * 3600 && fabs(fm - 1790441340.0) < 3 * 3600, "phase times off");
  }
  // UI-38: aurora notice on the status line and the northern glow
  {
    const double t0 = sat_host_now;
    double tt = t0;  // a dark evening hour
    for (int k = 0; k < 48; k++) {
      tt = t0 + k * 1800.0;
      if (sat::astro::compute(tt, sat::config().lat, sat::config().lon, 0).sun.el < -14) break;
    }
    auto saved = sat::live.kp;
    const auto saved_status = sat::live.status;
    sat::live.status.error[0] = 0;  // an error would take the line (and does, by design)
    for (auto &k : sat::live.kp) k.kp = 5.0f;
    sat_host_now = tt; sat::aur.until = 0; sat::draw_hud(tt);
    // alerts rotate once a minute: find the aurora one among them, and put it on the line
    static sat::Alert aal[sat::MAX_ALERTS];
    auto aurora_alert = [&]() -> const sat::Alert * {
      const int na = sat::collect_alerts(tt, aal, sat::MAX_ALERTS);
      for (int i = 0; i < na; i++) if (strstr(aal[i].text, "Aurora")) return &aal[i];
      return nullptr;
    };
    const sat::Alert *aa = aurora_alert();
    sat::alert_override = aa;
    sat::draw_hud(tt);
    const char *st = lv_label_get_text(sat::ui.w.status);
    printf("aurora line: %s (dark %d, kp %.1f)\n", st, sat::aur.dark_now, sat::aur.kp);
    CHECK(sat::aur.show && sat::aur.likely && sat::aur.dark_now && strstr(st, "Aurora likely until") && strstr(st, "strong"), "aurora notice: %s", st);
    CHECK(sat::ui.aurora_icon && !lv_obj_has_flag(sat::ui.aurora_icon, LV_OBJ_FLAG_HIDDEN), "aurora icon hidden");
    sat::tick(); run_jobs(); sat::tick();
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r4_aurora_likely.ppm");
    for (auto &k : sat::live.kp) k.kp = 3.0f;  // low in the north only
    sat::alert_override = nullptr;
    sat::aur.until = 0; sat::draw_hud(tt);
    aa = aurora_alert();
    printf("faint aurora alert: %s\n", aa ? aa->text : "(none)");
    CHECK(sat::aur.show && !sat::aur.likely && aa == nullptr, "UI-38b: no alert for a faint aurora (%s)", aa ? aa->text : "");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r4_aurora_possible.ppm");
    for (auto &k : sat::live.kp) k.kp = 0.3f;  // nothing
    sat::aur.until = 0; sat::draw_hud(tt);
    printf("Kp 0.3 line: %s\n", lv_label_get_text(sat::ui.w.status));
    CHECK(!sat::aur.show && !strstr(lv_label_get_text(sat::ui.w.status), "Aurora"), "notice at Kp 0.3");  // other glyph alerts may hold the icon (4.5.15)
    sat::live.kp = saved;
    sat::live.status = saved_status;
    sat::aur.until = 0;
    sat_host_now = t0; sat::tick(); run_jobs(); sat::tick();
  }
  {  // UI-27: debug page orbital lines
    char ob[400];
    sat::orbital_debug(ob, sizeof(ob), sat_host_now);
    printf("%s", ob);
    CHECK(strstr(ob, "ORBITAL DATA\n  newest") && strstr(ob, "decl 14.2"), "orbital debug: %s", ob);
  }
  // UI-35: "debris off" restored before setup survives net_init (boot order)
  CHECK(sat::net::cfg.debris_on == sat::ui.debris_on, "net debris %d vs ui %d", sat::net::cfg.debris_on, sat::ui.debris_on);
  // UI-35: debris off drops R/B and DEB objects from the computed set
  {
    sat::set_debris(false);
    for (int k = 0; k < 3; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
    int deb = 0;
    for (auto &r : sat::live.sats) deb += r.debris;
    CHECK(deb == 0, "debris still listed: %d", deb);
    sat::set_debris(true);
    for (int k = 0; k < 3; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
  }
  // UI-24: with the card up, a tap anywhere on the sky only closes it
  // UI-34: owner flag after the ISO code
  {
    sat::Marker *mk = nullptr;
    for (auto &m : sat::ui.sats) if (m.id >= 0 && m.up) { mk = &m; break; }
    CHECK(mk != nullptr, "no marker for the flag card");
    if (mk) {
      strcpy(mk->rec.cc, "USA");
      sat::select_object(sat::K_SAT, mk->id);
      CHECK(!lv_obj_has_flag(sat::ui.card_flag, LV_OBJ_FLAG_HIDDEN), "no card flag for USA");
      lv_obj_invalidate(lv_screen_active());
      lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r4_flagcard.ppm");
      strcpy(mk->rec.cc, "ESA");  // ESA gets the EU flag
      sat::card_update(sat_host_now);
      CHECK(!lv_obj_has_flag(sat::ui.card_flag, LV_OBJ_FLAG_HIDDEN), "no flag for ESA");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r4_esacard.ppm");
      strcpy(mk->rec.cc, "ISS");  // multinational: no flag
      sat::card_update(sat_host_now);
      CHECK(lv_obj_has_flag(sat::ui.card_flag, LV_OBJ_FLAG_HIDDEN), "flag shown for ISS");
      CHECK(sat::card_open(), "card not open");
      sat::deselect();
      CHECK(!sat::card_open(), "card still open");
    }
  }
  // ARCH-4: repeated requests for a waiting job merge into one queued run
  {
    uint8_t j; while (xQueueReceive(sat::net::queue, &j, 0) == pdTRUE) sat::net::queued.fetch_and(~(1u << j));
    for (int k = 0; k < 30; k++) { sat::enqueue(sat::JOB_ISS); sat::enqueue(sat::JOB_ABOVE); }
    CHECK(uxQueueMessagesWaiting(sat::net::queue) == 2, "queued %u", (unsigned) uxQueueMessagesWaiting(sat::net::queue));
    run_jobs();
  }
  // ---- 4.5.15: sky events, bright stars, time scrub, Tiangong
  {
    const double save_now = sat_host_now;
    auto settle = [&](double tt) {
      sat_host_now = tt;
      sat::sev = sat::SkyEvents();
      for (int k = 0; k < 120; k++) sat::eclipse_step(tt);
      sat::sky_events_step(tt);
      sat::tick(); run_jobs(); sat::tick();
    };
    auto alerts = [&](double tt, const char *want) {
      sat::Alert al[sat::MAX_ALERTS];
      const int n = sat::collect_alerts(tt, al, sat::MAX_ALERTS);
      bool f = false;
      for (int i = 0; i < n; i++) { printf("  alert: %s\n", al[i].text); if (strstr(al[i].text, want)) f = true; }
      return f;
    };
    // Geminids: peak ~2026-12-14 (radiant up in the evening)
    const double gem = 1797141600;  // 2026-12-13 06:00 UTC
    settle(gem);
    printf("Geminids peak at %.0f\n", sat::sev.peak[7]);
    CHECK(fabs(sat::sev.peak[7] - 1797256266) < 12 * 3600, "Geminid peak %.0f", sat::sev.peak[7]);
    CHECK(alerts(gem, "Geminids peak"), "Geminid alert");
    const auto st_save = sat::live.status;
    sat::live.status.error[0] = 0;
    sat::live.status.data_time = sat::live.status.loaded = gem - 600;
    for (int k = 0; k < 20; k++) {  // rotate to the shower alert and render it
      sat::draw_hud(gem + 6 * k);
      if (strstr(lv_label_get_text(sat::ui.w.status), "Geminids")) break;
    }
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r6_alert.ppm");
    sat::live.status = st_save;
    CHECK(sat::ui.radiant == 7 && sat::ui.radiant_up, "radiant %d up %d", sat::ui.radiant, sat::ui.radiant_up);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r6_radiant.ppm");
    sat::select_object(sat::K_SHOWER, 7);
    printf("shower card:\n%s\n", lv_label_get_text(sat::ui.card_body));
    CHECK(strstr(lv_label_get_text(sat::ui.card_body), "Phaethon") != nullptr, "shower card");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r6_showercard.ppm");
    sat::deselect();
    // a bright star card (Capella is up on a December night)
    sat::select_object(sat::K_STAR, 3);
    printf("star card:\n%s\n", lv_label_get_text(sat::ui.card_body));
    CHECK(strstr(lv_label_get_text(sat::ui.card_body), "Auriga") != nullptr, "star card");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r6_starcard.ppm");
    sat::deselect();
    // time scrub: +6 h
    sat::scrub_open();
    CHECK(sat::ui.scrub_on && !lv_obj_has_flag(sat::ui.scrub_panel, LV_OBJ_FLAG_HIDDEN), "scrub open");
    lv_slider_set_value(sat::ui.scrub_slider, 36, LV_ANIM_OFF);
    lv_obj_send_event(sat::ui.scrub_slider, LV_EVENT_VALUE_CHANGED, nullptr);
    sat::tick();
    int shown = 0;
    for (auto &m : sat::ui.sats) shown += m.id >= 0 && m.shown;
    printf("scrub: %s, markers shown %d\n", lv_label_get_text(sat::ui.scrub_label), shown);
    CHECK(shown == 0 && fabs(sat::ui.scrub_s - 6 * 3600) < 1, "scrub view");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r6_scrub.ppm");
    sat::scrub_close();
    sat::tick();
    shown = 0;
    for (auto &m : sat::ui.sats) shown += m.id >= 0 && m.shown;
    CHECK(!sat::ui.scrub_on && shown > 0, "scrub closed, markers %d", shown);
    // total lunar eclipse 2028-12-31 16:52 UTC, seen from here (07:16-08:28 local)
    const double ecl = 1861815600;  // 2028-12-30 19:00 UTC
    settle(ecl);
    CHECK(alerts(ecl, "Total lunar eclipse tomorrow"), "eclipse alert");
    // full Moon names: 2026-10-26 is the Hunter's Moon
    settle(1792900000);
    printf("full: %s at %.0f super %d\n", sat::sev.full_name, sat::sev.full_t, sat::sev.full_super);
    CHECK(!strcmp(sat::sev.full_name, "Hunter's Moon"), "hunter's moon: %s", sat::sev.full_name);
    // Moon near a planet or star in the next 18 h, over the next month
    int nconj = 0;
    for (double tt = save_now; tt < save_now + 30 * 86400.0; tt += 12 * 3600.0) {
      sat::sev.conj_next_at = 0;
      sat::moon_conj_update(tt);
      if (sat::sev.conj_with) {
        nconj++;
        char w[32]; sat::when_text(tt, sat::sev.conj_t, w, sizeof(w));
        printf("  conj %.0f: Moon near %s %s, %.1f deg\n", tt, sat::sev.conj_with, w, sat::sev.conj_sep);
      }
    }
    CHECK(nconj > 0, "no Moon conjunctions in a month");
    // UI-52 Tiangong: passes from Tokyo (from 61.6 N it barely clears the horizon)
    {
      const auto jc0 = sat::net::jc;
      sat::pvector<sat::Pass> v;
      sat::net::find_passes(sat::net::css, v);
      printf("Tiangong passes here: %zu\n", v.size());
      sat::net::jc.lat = 35.68; sat::net::jc.lon = 139.69;
      sat::net::find_passes(sat::net::css, v);
      printf("Tiangong passes from Tokyo: %zu (first peak %.0f)\n", v.size(), v.empty() ? 0.0 : v[0].max_el);
      CHECK(!v.empty(), "no Tiangong passes from 35N");
      sat::net::jc = jc0;
      CHECK(sat::ui.css.id == 48274, "Tiangong marker id %d", (int) sat::ui.css.id);
    }
    sat_host_now = save_now;
    sat::sev = sat::SkyEvents();
    sat::tick();
  }
  // ---- 4.5.19: SatNOGS fallback, solar wind, launches, deep-sky objects, finder
  {
    // DATA-13 TLE -> elements (values checked against python-sgp4 2.27)
    sat::net::Omm o;
    CHECK(sat::net::tle_to_omm("0 ISS (ZARYA)", "1 25544U 98067A   26271.14212772  .00006053  00000-0  11922-3 0  9996",
                               "2 25544  51.6312 150.5592 0007113 196.8918 163.1834 15.48675883587710", o), "tle parse");
    printf("TLE: %s %s epoch %.3f ndot %.3g bstar %.5g n %.8f e %.7f\n", o.name, o.intl, o.epoch, o.ndot, o.bstar, o.n_revday, o.ecc);
    CHECK(fabs(o.epoch - 1790565879.835) < 0.01 && fabs(o.bstar - 1.1922e-4) < 1e-9 && fabs(o.ndot - 6.053e-5) < 1e-10 &&
          fabs(o.n_revday - 15.48675883) < 1e-8 && fabs(o.ecc - 7.113e-4) < 1e-9 && !strcmp(o.intl, "1998-067A") &&
          !strcmp(o.name, "ISS (ZARYA)"), "tle values");
    sat::net::SgpSat sg;
    CHECK(sat::net::make_sgp(o, sg), "tle sgp4 init");
    // the fallback path: CelesTrak refusing, SatNOGS answering
    host_http_bodies["norad_cat_id=25544"] = "[{\"tle0\":\"0 ISS (ZARYA)\",\"tle1\":\"1 25544U 98067A   26271.14212772  .00006053  00000-0  11922-3 0  9996\",\"tle2\":\"2 25544  51.6312 150.5592 0007113 196.8918 163.1834 15.48675883587710\",\"norad_cat_id\":25544}]";
    const double iss_ep = sat::net::iss.epoch;
    sat::net::fallback_next = 0;
    sat::net::iss_loaded = 0;
    const bool fb = sat::net::stations_fallback(sat_host_now);
    printf("fallback: %d, ISS epoch %.0f -> %.0f\n", fb, iss_ep, sat::net::iss.epoch);
    CHECK(sat::net::iss.epoch >= iss_ep && sat::net::iss_loaded == sat_host_now, "SatNOGS fallback");
    // UI-55 solar wind: newest first, an inactive source mixed in
    std::string mag = "[", wind = "[";
    for (int i = 0; i < 40; i++) {
      char b[400];
      snprintf(b, sizeof(b), "%s{\"time_tag\": \"2026-09-26T0%d:%02d:00\", \"active\": %s, \"source\": \"ACE\", \"bt\": 9.5, \"bz_gsm\": %s}",
               i ? "," : "", 5 - i / 60, 59 - i % 60, i % 3 == 2 ? "false" : "true", i % 3 == 2 ? "null" : "-8.0");
      mag += b;
      snprintf(b, sizeof(b), "%s{\"time_tag\": \"2026-09-26T0%d:%02d:00\", \"active\": true, \"proton_speed\": 520.0, \"proton_density\": 4.2}",
               i ? "," : "", 5 - i / 60, 59 - i % 60);
      wind += b;
    }
    mag += "]"; wind += "]";
    host_http_bodies["rtsw_mag_1m.json"] = mag;
    host_http_bodies["rtsw_wind_1m.json"] = wind;
    // UI-54 launches: one from Kodiak in 5 h, one from Florida in 40 min, one flown
    char lj[2400];
    auto iso = [](double t, char *b) { time_t tt = (time_t) t; struct tm g; gmtime_r(&tt, &g); strftime(b, 32, "%Y-%m-%dT%H:%M:%SZ", &g); };
    char t1[32], t2[32], t3[32];
    iso(sat_host_now + 5 * 3600, t1); iso(sat_host_now + 40 * 60, t2); iso(sat_host_now - 3 * 3600, t3);
    snprintf(lj, sizeof(lj), "{\"count\":3,\"results\":["
      "{\"name\":\"Minotaur IV | NROL-174\",\"net\":\"%s\",\"status\":{\"abbrev\":\"Go\"},\"pad\":{\"latitude\":\"57.435\",\"longitude\":\"-152.337\",\"location\":{\"name\":\"Pacific Spaceport Complex, Alaska, USA\"}},\"rocket\":{\"configuration\":{\"name\":\"Minotaur IV\"}},\"mission\":{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":[1]}}}}}}}}},"
      "{\"name\":\"Falcon 9 | Starlink 12-5\",\"net\":\"%s\",\"status\":{\"abbrev\":\"Go\"},\"pad\":{\"latitude\":28.56,\"longitude\":-80.577,\"location\":{\"name\":\"Cape Canaveral SFS, FL, USA\"}},\"rocket\":{\"configuration\":{\"name\":\"Falcon 9\"}}},"
      "{\"name\":\"Electron | Test\",\"net\":\"%s\",\"status\":{\"abbrev\":\"Success\"},\"pad\":{\"latitude\":-39.26,\"longitude\":177.86,\"location\":{\"name\":\"Mahia, NZ\"}},\"rocket\":{\"configuration\":{\"name\":\"Electron\"}}}]}", t1, t2, t3);
    host_http_bodies["launches/upcoming"] = lj;
    sat::net::wind_next = sat::net::launch_next = 0;
    sat::net::do_wind(sat_host_now);
    sat::net::do_launches(sat_host_now);
    sat::net::pending.wind.t = sat_host_now - 60;  // the canned samples are old; pretend they are new
    sat::tick();
    printf("wind: Bz %.1f Bt %.1f v %.0f n %.1f; launches %zu\n", sat::live.wind.bz, sat::live.wind.bt, sat::live.wind.speed,
           sat::live.wind.density, sat::live.launches.size());
    CHECK(fabs(sat::live.wind.bz + 8.0f) < 0.01f && fabs(sat::live.wind.speed - 520) < 0.1f && sat::live.launches.size() == 3,
          "wind/launches");
    CHECK(sat::wind_warning(sat_host_now), "wind warning");
    sat::Alert al[sat::MAX_ALERTS];
    const int na = sat::collect_alerts(sat_host_now, al, sat::MAX_ALERTS);
    bool kod = false, fl = false;
    for (int i = 0; i < na; i++) {
      printf("  alert: %s\n", al[i].text);
      kod |= strstr(al[i].text, "Minotaur IV from Pacific Spaceport Complex in 5h") != nullptr;
      fl |= strstr(al[i].text, "Falcon 9 launches in 40m") != nullptr;
    }
    CHECK(kod && fl, "launch alerts");
    {  // UI-54 a launch 2 days out alerts too; UI-54c space events within 3 days
      char ej[1600], te1[32], te2[32], te3[32];
      iso(sat_host_now + 2 * 86400 + 3600, te1); iso(sat_host_now + 5 * 86400, te2); iso(sat_host_now + 26 * 3600, te3);
      snprintf(ej, sizeof(ej), "{\"count\":3,\"results\":["
        "{\"name\":\"SpaceX Crew-12 Crew Dragon Undocking\",\"date\":\"%s\",\"type\":{\"id\":8,\"name\":\"Spacecraft Undocking\"},\"date_precision\":{\"abbrev\":\"MIN\"},\"location\":\"International Space Station\"},"
        "{\"name\":\"Far Away Docking\",\"date\":\"%s\",\"type\":{\"id\":2,\"name\":\"Docking\"},\"date_precision\":{\"abbrev\":\"MIN\"}},"
        "{\"name\":\"Vague Month Event\",\"date\":\"%s\",\"type\":{\"id\":16,\"name\":\"Orbital Insertion\"},\"date_precision\":{\"abbrev\":\"M\"}}]}",
        te1, te2, te3);
      host_http_bodies["events/upcoming"] = ej;
      sat::net::event_next = 0;
      sat::net::do_events(sat_host_now);
      sat::tick();
      CHECK(sat::live.events.size() == 3, "events parsed (%zu)", sat::live.events.size());
      const int n2 = sat::collect_alerts(sat_host_now, al, sat::MAX_ALERTS);
      bool und = false, far = false, vague = false;
      for (int i = 0; i < n2; i++) {
        printf("  alert: %s\n", al[i].text);
        und |= strstr(al[i].text, "SpaceX Crew-12 Crew Dragon Undocking in 2d 1h") != nullptr;
        far |= strstr(al[i].text, "Far Away") != nullptr;
        vague |= strstr(al[i].text, "Vague") != nullptr;
      }
      CHECK(und && !far && !vague, "event alerts: within 3 days, exact times only");
      const auto st0 = sat::live.status;
      sat::live.status.error[0] = 0;  // the test data's CelesTrak error would hide the alerts
      sat::live.status.loaded = sat_host_now - 60;
      for (int k = 0; k < 12; k++) {  // a render of each alert as the status line rotates
        sat::draw_hud(sat_host_now + k * sat::ALERT_ROTATE_S);
        if (strstr(lv_label_get_text(sat::ui.w.status), "Undocking")) {
          lv_obj_invalidate(lv_screen_active());
          lv_refr_now(disp);
          save_ppm(OUT_DIR "/renders/r16_event_alert.ppm");
          // UI-41d: a tap on the alert opens its details card
          lv_obj_send_event(sat::ui.w.status, LV_EVENT_CLICKED, nullptr);
          printf("event details: %s\n%s\n", lv_label_get_text(sat::ui.card_title), lv_label_get_text(sat::ui.card_body));
          CHECK(sat::ui.sel_kind == sat::K_INFO && strstr(lv_label_get_text(sat::ui.card_body), "SpaceX Crew-12 Crew Dragon Undocking") &&
                strstr(lv_label_get_text(sat::ui.card_body), "International Space Station"), "event alert tap: details card");
          lv_obj_invalidate(lv_screen_active());
          lv_refr_now(disp);
          save_ppm(OUT_DIR "/renders/r16_event_details.ppm");
          sat::deselect();
          break;
        }
      }
      for (int k = 0; k < 12; k++) {  // and a launch alert
        sat::draw_hud(sat_host_now + k * sat::ALERT_ROTATE_S);
        if (strstr(lv_label_get_text(sat::ui.w.status), "launches in") || strstr(lv_label_get_text(sat::ui.w.status), " from ")) {
          lv_obj_send_event(sat::ui.w.status, LV_EVENT_CLICKED, nullptr);
          printf("launch details: %s\n%s\n", lv_label_get_text(sat::ui.card_title), lv_label_get_text(sat::ui.card_body));
          CHECK(sat::ui.sel_kind == sat::K_INFO && strstr(lv_label_get_text(sat::ui.card_body), "Status:") &&
                strstr(lv_label_get_text(sat::ui.card_body), "From "), "launch alert tap: details card");
          lv_obj_invalidate(lv_screen_active());
          lv_refr_now(disp);
          save_ppm(OUT_DIR "/renders/r16_launch_details.ppm");
          sat::deselect();
          break;
        }
      }
      {  // a planet alert opens the planet's own card; no alert: the tap is not taken
        sat::Alert pa = {};
        snprintf(pa.text, sizeof(pa.text), "Jupiter visible - SE, 30\xC2\xB0 up");
        int shown_p = -1;  // one on the map (planet alerts only come for those)
        for (int q = 0; q < sat::planets::N_PLANETS && shown_p < 0; q++)
          if (sat::ui.planet_shown[q]) shown_p = q;
        pa.kind = sat::K_PLANET, pa.id = shown_p < 0 ? 0 : shown_p, pa.col = 0xFFFFFF;
        sat::shown_alert = pa;
        sat::shown_alert_ok = true;
        lv_obj_send_event(sat::ui.w.status, LV_EVENT_CLICKED, nullptr);
        CHECK(shown_p < 0 || (sat::ui.sel_kind == sat::K_PLANET && sat::ui.sel_id == shown_p), "planet alert tap: planet card (%d)", shown_p);
        sat::deselect();
        sat::shown_alert_ok = false;
        lv_obj_send_event(sat::ui.w.status, LV_EVENT_CLICKED, nullptr);
        CHECK(!sat::card_open(), "no alert: tap ignored");
      }
      {  // UI-41e: launch and docking alerts each have their own switch
        auto count = [&](const char *what) {
          static sat::Alert xa[sat::MAX_ALERTS];
          const int nx = sat::collect_alerts(sat_host_now, xa, sat::MAX_ALERTS);
          int c = 0;
          for (int i = 0; i < nx; i++) c += strstr(xa[i].text, what) != nullptr;
          return c;
        };
        const int l0 = count("launches in") + count(" from "), e0 = count("Undocking");
        sat::set_alert_kinds(true, false, true);
        const int l1 = count("launches in") + count(" from "), e1 = count("Undocking");
        sat::set_alert_kinds(true, true, false);
        const int l2 = count("launches in") + count(" from "), e2 = count("Undocking");
        sat::set_alert_kinds(true, true, true);
        CHECK(l0 > 0 && e0 > 0 && l1 == 0 && e1 == e0 && l2 == l0 && e2 == 0, "alert kinds %d/%d %d/%d %d/%d", l0, e0, l1, e1, l2, e2);
      }
      {  // UI-41f: the Moon's alerts follow Lunar, not Sky events
        auto counts = [&](int &moon, int &other) {
          static sat::Alert xa[sat::MAX_ALERTS];
          // half a day before the next full Moon (sev.full_t), when its alert is up
          const double tm = sat::sev.full_t > sat_host_now ? sat::sev.full_t - 43200 : sat_host_now;
          const int nx = sat::collect_alerts(tm, xa, sat::MAX_ALERTS);
          moon = other = 0;
          for (int i = 0; i < nx; i++) (xa[i].col == sat::C_MOON ? moon : other)++;
        };
        int m0, o0, m1, o1, m2, o2;
        counts(m0, o0);
        sat::set_lunar_alerts(false);
        counts(m1, o1);
        sat::set_lunar_alerts(true);
        sat::set_sky_alerts(false);
        counts(m2, o2);
        sat::set_sky_alerts(true);
        printf("lunar alerts: on %d/%d, Lunar off %d/%d, Sky events off %d/%d (moon/other)\n", m0, o0, m1, o1, m2, o2);
        CHECK(m0 > 0 && m1 == 0 && o1 == o0 && m2 == m0, "Lunar switch");
      }
      {  // UI-52b: Space stations off hides both stations and their list rows
        sat::set_stations(false);
        CHECK(!sat::ui.iss.up && !sat::ui.css.up && !sat::ui.iss.shown, "stations hidden");
        sat::set_stations(true);
        CHECK(sat::ui.iss.id < 0 || sat::ui.iss.up || sat::ui.iss_azel.el < 0, "ISS back");
      }
      if (getenv("ALERT_INV")) {  // what an alert switch redraws
        static std::vector<lv_area_t> inv;
        lv_display_add_event_cb(disp, [](lv_event_t *e) {
            if (const lv_area_t *ar = (const lv_area_t *) lv_event_get_param(e)) inv.push_back(*ar);
          }, LV_EVENT_INVALIDATE_AREA, nullptr);
        double tt = sat_host_now;
        sat::draw_hud(tt);
        lv_refr_now(disp);
        for (int k = 1; k <= 6; k++) {
          inv.clear();
          g_flush_px = 0;
          tt += sat::ALERT_ROTATE_S;
          sat::draw_hud(tt);
          auto a0 = std::chrono::steady_clock::now();
          lv_refr_now(disp);
          const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a0).count();
          printf("ALERT_INV switch to \"%s\": %ld px flushed, %.0f us\n", lv_label_get_text(sat::ui.w.status), g_flush_px, us);
          for (const auto &ar : inv)
            printf("   inval %d,%d-%d,%d (%d px)\n", (int) ar.x1, (int) ar.y1, (int) ar.x2, (int) ar.y2, (int) lv_area_get_size(&ar));
        }
      }
      sat::live.status = st0;
    }
    printf("next launch: %s\n", sat::next_launch_text().c_str());
    // UI-54b: Launch Library refuses (429): RocketLaunch.Live's free feed fills in
    {
      const std::string ll = host_http_bodies["launches/upcoming"];
      host_http_bodies["launches/upcoming"] = "HTTP429";
      char ta[32], tb[32];
      auto isom = [](double t, char *b) { time_t tt = (time_t) t; struct tm g; gmtime_r(&tt, &g); strftime(b, 32, "%Y-%m-%dT%H:%MZ", &g); };
      isom(sat_host_now + 3 * 3600, ta);
      isom(sat_host_now + 20 * 60, tb);
      char rj[1600];
      snprintf(rj, sizeof(rj), "{\"valid_auth\":false,\"count\":3,\"result\":["
        "{\"name\":\"NROL-97\",\"t0\":\"%s\",\"win_open\":null,\"sort_date\":\"1\",\"result\":-1,\"vehicle\":{\"name\":\"Falcon Heavy\"},\"pad\":{\"location\":{\"name\":\"Kennedy Space Center\",\"state\":\"FL\",\"country\":\"United States\"}}},"
        "{\"name\":\"Kodiak Test\",\"t0\":null,\"win_open\":\"%s\",\"sort_date\":\"1\",\"result\":-1,\"vehicle\":{\"name\":\"Minotaur IV\"},\"pad\":{\"location\":{\"name\":\"Pacific Spaceport Complex\",\"state\":\"AK\",\"country\":\"United States\"}}},"
        "{\"name\":\"Someday\",\"t0\":null,\"win_open\":null,\"sort_date\":\"%.0f\",\"result\":-1,\"vehicle\":{\"name\":\"Neutron\"},\"pad\":{\"location\":{\"name\":\"Wallops Island\",\"state\":\"VA\",\"country\":\"United States\"}}}]}",
        tb, ta, sat_host_now + 40 * 86400);
      host_http_bodies["rocketlaunch.live/json/launches/next"] = rj;
      sat::net::launch_next = 0;
      sat::net::do_launches(sat_host_now);
      sat::tick();
      const auto &L = sat::live.launches;
      printf("backup launches %zu: %s / %s / %s\n", L.size(), L.size() > 0 ? L[0].name : "", L.size() > 1 ? L[1].name : "",
             L.size() > 2 ? L[2].status : "");
      CHECK(L.size() == 3 && !strcmp(L[0].name, "Falcon Heavy | NROL-97") && !strcmp(L[0].status, "Go") &&
                fabs(L[0].net - (floor((sat_host_now + 20 * 60) / 60) * 60)) < 1 && !strcmp(L[0].where, "Kennedy Space Center, FL") &&
                fabs(L[1].lat - 57.43f) < 0.01f && !strcmp(L[2].status, "TBD") && sat::net::launch_next > sat_host_now + 3600,
            "launches from RocketLaunch.Live when Launch Library refuses");
      host_http_bodies["launches/upcoming"] = ll;
      host_http_bodies.erase("rocketlaunch.live/json/launches/next");
      sat::net::launch_next = 0;
      sat::net::do_launches(sat_host_now);
      sat::tick();
    }
    sat::draw_list(sat_host_now);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    // UI-56 deep-sky card and the map ring
    for (int i = 0; i < sat::ev::N_DSO; i++) if (!strcmp(sat::ev::DSO[i].cat, "M31")) {
      sat::select_object(sat::K_DSO, i);
      printf("DSO card: %s\n%s\n", lv_label_get_text(sat::ui.card_title), lv_label_get_text(sat::ui.card_body));
      CHECK(strstr(lv_label_get_text(sat::ui.card_title), "Andromeda Galaxy (M31)"), "dso card");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r7_dsocard.ppm");
    }
    printf("DSO on the map: %zu\n", sat::ui.dso_pts.size());
    // UI-57 finder for Jupiter: heading pointing at it -> straight ahead
    sat::deselect();
    sat::finder_open_for(sat::K_PLANET, 3, "Jupiter");
    float az, el;
    CHECK(sat::finder_target(sat_host_now, az, el), "finder target");
    sat::finder_update(az - 60);  // display pointing 60 deg left of it
    printf("finder: %s / %s\n", lv_label_get_text(sat::fnd.turn), lv_label_get_text(sat::fnd.up));
    CHECK(strstr(lv_label_get_text(sat::fnd.turn), "Turn right 60"), "finder turn");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r7_finder.ppm");
    sat::finder_update(az + 3);
    CHECK(strstr(lv_label_get_text(sat::fnd.turn), "Straight ahead"), "finder ahead");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r7_finder_ahead.ppm");
    sat::finder_close();
    sat::live.launches.clear();
    sat::live.wind = sat::net::SolarWind();
  }
  {  // ---- 4.5.20: UI-58 OVATION aurora chance, UI-59 SUVI Sun image
    const double day_now = sat_host_now;
    const auto &cfg0 = sat::config();
    auto mkjson = [&](double t, int peak) {
      auto pct = [&](double la) { return std::max(0.0, peak - 8.0 * fabs(la - 66.0)); };
      std::string oj = "{\"Observation Time\": \"";
      char ts[40];
      { time_t tt = (time_t) (t - 1200); struct tm g; gmtime_r(&tt, &g); strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &g); }
      oj += ts; oj += "\", \"Forecast Time\": \"";
      { time_t tt = (time_t) (t + 1800); struct tm g; gmtime_r(&tt, &g); strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &g); }
      oj += ts; oj += "\", \"Data Format\": \"[Longitude, Latitude, Aurora]\", \"coordinates\": [";
      for (int lo = 0; lo < 360; lo++)
        for (int la = -90; la <= 90; la++) {
          char b[32];
          snprintf(b, sizeof(b), "%s[%d, %d, %d]", lo || la > -90 ? ", " : "", lo, la, (int) lround(pct(la)));
          oj += b;
        }
      return oj + "], \"type\": \"MultiPoint\"}";
    };
    auto pctw = [](double la) { return std::max(0.0, 60.0 - 8.0 * fabs(la - 66.0)); };
    sat::net::jc = sat::net::cfg;
    // daylight: no download
    const bool day = sat::astro::compute(day_now, cfg0.lat, cfg0.lon, cfg0.alt_m).sun.el > -6;
    host_http_bodies["ovation_aurora_latest.json"] = mkjson(day_now, 60);
    host_last_url.clear();
    sat::net::ovation_next = 0;
    sat::net::do_ovation(day_now);
    printf("OVATION in daylight (%d): url '%s'\n", day, host_last_url.c_str());
    if (day)
      CHECK(host_last_url.find("ovation") == std::string::npos, "no OVATION download in daylight");
    {  // a daylight hour: never fetched
      double tl = day_now;
      while (tl < day_now + 86400 && sat::astro::compute(tl, cfg0.lat, cfg0.lon, cfg0.alt_m).sun.el < 5) tl += 600;
      host_last_url.clear();
      sat::net::ovation_next = 0;
      sat::net::do_ovation(tl);
      CHECK(host_last_url.empty(), "daylight: no OVATION download (%s)", host_last_url.c_str());
    }
    // the next dark hour
    double td = day_now;
    while (td < day_now + 86400 && sat::astro::compute(td, cfg0.lat, cfg0.lon, cfg0.alt_m).sun.el > -14) td += 600;
    sat_host_now = td;
    sat::tick();
    printf("dark at +%.1f h, Kp gate %.2f\n", (td - day_now) / 3600, sat::net::kp_gate);
    // quiet night: a weak oval, no wind warning, Kp below the gate -> 75 min
    sat::net::wind_seen = sat::net::SolarWind();
    const float gate_was = sat::net::kp_gate;
    sat::net::kp_gate = 9;  // whatever the canned Kp says, quiet
    host_http_bodies["ovation_aurora_latest.json"] = mkjson(td, 8);
    sat::net::ov_seen = sat::net::AuroraChance();
    sat::net::ovation_next = 0;
    sat::net::do_ovation(td);
    CHECK(!sat::net::ovation_active_last && fabs(sat::net::ovation_next - td - sat::net::OVATION_QUIET_S) < 1, "quiet: 75 min");
    host_last_url.clear();
    sat::net::do_ovation(td + 600);
    CHECK(host_last_url.empty(), "quiet: not due after 10 min");
    // a solar wind warning 20 min later wakes it at once
    sat::net::wind_seen.t = td + 1200; sat::net::wind_seen.bz = -12; sat::net::wind_seen.speed = 600;
    host_http_bodies["ovation_aurora_latest.json"] = mkjson(td, 60);
    sat::net::do_ovation(td + 1260);
    printf("wake-up: url '%s', active %d, next in %.0f min\n", host_last_url.c_str(), sat::net::ovation_active_last,
           (sat::net::ovation_next - td - 1260) / 60);
    CHECK(host_last_url.find("ovation") != std::string::npos && sat::net::ovation_active_last &&
          fabs(sat::net::ovation_next - td - 1260 - sat::net::OVATION_ACTIVE_S) < 1, "wind warning wakes the aurora fetch");
    sat::net::kp_gate = gate_was;
    sat::net::wind_seen = sat::net::SolarWind();
    sat::tick();
    const double olat = sat::net::cfg.lat;
    const double fy = olat - floor(olat);
    const double want = lround(pctw(floor(olat))) * (1 - fy) + lround(pctw(floor(olat) + 1)) * fy;
    char oc[48];
    sat::ovation_text(oc, sizeof(oc));
    printf("OVATION: here %.1f (want %.1f) view %.0f az %.0f: \"%s\"\n", sat::live.ovation.here, want,
           sat::live.ovation.view, sat::live.ovation.view_az, oc);
    CHECK(fabs(sat::live.ovation.here - want) < 0.05 && sat::live.ovation.view == 60 && !strcmp(oc + strlen(oc) - 5, "low N") &&
          sat::aurora_chance() > 0, "ovation chance");
    // the nowcast alert
    {
      sat::aur.until = 0;
      sat::aurora_update(td);
      sat::Alert al2[sat::MAX_ALERTS];
      const int n2 = sat::collect_alerts(td, al2, sat::MAX_ALERTS);
      bool now_alert = false;
      for (int i = 0; i < n2; i++) {
        printf("  alert @dark: %s\n", al2[i].text);
        now_alert |= strstr(al2[i].text, "Aurora likely now - 25% overhead, 60% low N") != nullptr;
      }
      printf("aurora @dark: dark %d show %d likely %d nowcast %d kp %.1f\n", sat::aur.dark_now, sat::aur.show, sat::aur.likely,
             sat::aur.nowcast, sat::aur.kp);
      CHECK(sat::aur.dark_now && now_alert, "ovation nowcast alert");
    }
    // the Sun card: chance line, "Sun image" button, the picture requested
    using sat::net::IMG_SUN;
    using sat::net::IMG_MOON;
    sat::select_object(sat::K_SUN, 0);
    printf("Sun card:\n%s\n", lv_label_get_text(sat::ui.card_body));
    CHECK(strstr(lv_label_get_text(sat::ui.card_body), "Aurora chance ") && !lv_obj_has_flag(sat::ui.card_img_btn, LV_OBJ_FLAG_HIDDEN) &&
          !strcmp(lv_label_get_text(sat::ui.card_img_lbl), "Image") && sat::img_req[IMG_SUN].asked, "sun card chance + button");
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r8_suncard.ppm");
    // the viewer while the picture downloads (the host runs jobs at once: hold that state)
    auto hold_loading = [](int k, uint8_t stage, int32_t got, int32_t tot) {
      sat::img_req[k].asked = true;
      sat::img_req[k].seq = sat::live.img[k].seq;
      sat::img_req[k].at = sat_host_now;
      sat::net::img_stage_kind = (uint8_t) k;
      sat::net::img_stage = stage;
      sat::net::img_bytes = got;
      sat::net::img_total = tot;
    };
    sat::deselect();
    sat::img_view_open_now(IMG_SUN);
    hold_loading(IMG_SUN, sat::net::STG_DOWNLOAD, 520 * 1024, 1124 * 1024);
    sat::img_view_update(sat_host_now);
    lv_refr_now(disp);
    printf("sun view (loading): \"%s\" bar %d\n", lv_label_get_text(sat::sv.msg), (int) lv_bar_get_value(sat::sv.bar));
    CHECK(strstr(lv_label_get_text(sat::sv.msg), "46%  -  520 of 1124 KB") && lv_bar_get_value(sat::sv.bar) == 46 &&
          !lv_obj_has_flag(sat::sv.bar, LV_OBJ_FLAG_HIDDEN), "sun view progress");
    save_ppm(OUT_DIR "/renders/r8_sunload.ppm");
    sat::net::img_stage = sat::net::STG_IDLE;
    // GOES SUVI 304: the frame list names each exposure; the newest that shows the Sun is used
    auto suvi_name = [](double t) { char b[96]; time_t tt = (time_t) t; struct tm g; gmtime_r(&tt, &g);
      strftime(b, sizeof(b), "or_suvi-l2-ci304_g19_s%Y%m%dT%H%M%SZ_e%Y%m%dT%H%M%SZ_v1-0-2.png", &g); return std::string(b); };
    auto suvi_list = [&](std::initializer_list<double> ts) { std::string j = "[";
      for (double t : ts) j += std::string(j.size() > 1 ? "," : "") + "{\"url\":\"/images/animations/suvi/primary/304/" + suvi_name(t) + "\"}";
      host_http_bodies["suvi-primary-304.json"] = j + "]"; };
    const double t3 = floor(sat_host_now - 900), t1 = floor(sat_host_now - 420), t2 = floor(sat_host_now - 60), t4 = floor(sat_host_now - 20);
    const std::string good = slurp(HOST_DIR "/fixtures/suvi_good.png"), dark = slurp(HOST_DIR "/fixtures/suvi_dark.png");
    host_http_bodies[suvi_name(t3)] = good;
    host_http_bodies[suvi_name(t1)] = good;
    suvi_list({t3, t1});
    sat::net::do_image(IMG_SUN);
    printf("progress after: %ld of %ld\n", (long) sat::net::img_bytes, (long) sat::net::img_total);
    CHECK(sat::net::img_total > 100000 && sat::net::img_bytes == sat::net::img_total && sat::net::img_stage == sat::net::STG_IDLE,
          "download progress counted");
    sat::tick();
    const auto &si = sat::live.img[IMG_SUN];
    printf("sun image: ok %d obs %.0f (want %.0f) src %s err '%s' px %p\n", si.ok, si.obs, t1, si.src, si.err,
           (void *) sat::live.img_px[IMG_SUN]);
    CHECK(si.ok && sat::live.img_px[IMG_SUN] && fabs(si.obs - t1) < 1 && !strcmp(si.src, "GOES-19") && !si.shadow,
          "sun image decoded");
    if (sat::live.img_px[IMG_SUN]) {
      const uint16_t mid = sat::live.img_px[IMG_SUN][180 * 360 + 180];
      CHECK((mid >> 11) > 20, "sun image centre is bright (%04x)", mid);
    }
    sat::img_view_update(sat_host_now);
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
    printf("sun view: \"%s\" / \"%s\"\n", lv_label_get_text(sat::sv.cap1), lv_label_get_text(sat::sv.cap2));
    CHECK(strstr(lv_label_get_text(sat::sv.cap1), "GOES-19 SUVI, 30.4 nm") && strstr(lv_label_get_text(sat::sv.cap2), "Taken ") &&
          !lv_obj_has_flag(sat::sv.img, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(sat::sv.bar, LV_OBJ_FLAG_HIDDEN), "sun view shows it");
    save_ppm(OUT_DIR "/renders/r8_sunview.ppm");
    // an update in progress: the caption says so
    hold_loading(IMG_SUN, sat::net::STG_DOWNLOAD, 30 * 1024, 70 * 1024);
    sat::img_view_update(sat_host_now);
    CHECK(strstr(lv_label_get_text(sat::sv.cap2), "updating 42%"), "sun update progress in caption (%s)", lv_label_get_text(sat::sv.cap2));
    sat::net::img_stage = sat::net::STG_IDLE;
    // the same frame again: not downloaded
    {
      const uint32_t sq = sat::live.img[IMG_SUN].seq;
      host_last_url.clear();
      sat::net::do_image(IMG_SUN);
      sat::tick();
      CHECK(sat::live.img[IMG_SUN].ok && sat::live.img[IMG_SUN].seq == sq + 1 && !sat::live.img_new[IMG_SUN] &&
            host_last_url.find(".json") != std::string::npos, "sun image repeat skipped");
    }
    {  // UI-59c/d: a newer frame keeps the one before; < past it takes an older frame from the list
      host_http_bodies[suvi_name(t2)] = good;
      suvi_list({t3, t1, t2});
      sat::net::do_image(IMG_SUN);
      sat::tick();
      sat::img_view_update(sat_host_now);
      CHECK(sat::live.img_prev_px[IMG_SUN] && fabs(sat::live.img_prev[IMG_SUN].obs - t1) < 1 &&
            fabs(sat::live.img[IMG_SUN].obs - t2) < 1 && !lv_obj_has_flag(sat::sv.flip[0], LV_OBJ_FLAG_HIDDEN), "sun history kept");
      lv_obj_send_event(sat::sv.flip[0], LV_EVENT_CLICKED, nullptr);
      printf("before: \"%s\"\n", lv_label_get_text(sat::sv.cap2));
      CHECK(sat::sv.show_prev && strstr(lv_label_get_text(sat::sv.cap2), "7m ago - earlier picture"), "flip to the one before");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r11_sunbefore.ppm");
      lv_obj_send_event(sat::sv.flip[0], LV_EVENT_CLICKED, nullptr);  // one more: from the list
      run_jobs();
      sat::tick();
      sat::img_view_update(sat_host_now);
      printf("sun two back: \"%s\" prev obs %.0f want %.0f\n", lv_label_get_text(sat::sv.cap2), sat::live.img_prev[IMG_SUN].obs, t3);
      CHECK(fabs(sat::live.img_prev[IMG_SUN].obs - t3) < 1 && strstr(lv_label_get_text(sat::sv.cap2), "earlier picture"), "sun earlier from the list");
      lv_obj_send_event(sat::sv.flip[1], LV_EVENT_CLICKED, nullptr);
      CHECK(!sat::sv.show_prev && strstr(lv_label_get_text(sat::sv.cap2), "1m ago"), "flip back to the latest");
    }
    {  // UI-59: GOES in Earth's shadow: the newest frame is dark; the last good one stays, captioned
      host_http_bodies[suvi_name(t4)] = dark;
      suvi_list({t3, t1, t2, t4});
      const uint32_t sq = sat::live.img[IMG_SUN].seq;
      sat::net::do_image(IMG_SUN);
      sat::tick();
      sat::img_view_update(sat_host_now);
      printf("sun shadow: \"%s\" obs %.0f\n", lv_label_get_text(sat::sv.cap2), sat::live.img[IMG_SUN].obs);
      CHECK(sat::live.img[IMG_SUN].seq == sq + 1 && fabs(sat::live.img[IMG_SUN].obs - t2) < 1 && sat::live.img[IMG_SUN].shadow &&
            strstr(lv_label_get_text(sat::sv.cap2), "GOES in Earth's shadow"), "dark frame skipped, shadow captioned");
    }
    sat::img_view_close();
    // ---- UI-60 the Moon: NASA Dial-A-Moon
    {
      const int64_t hr = (int64_t) floor(sat_host_now / 3600) * 3600;
      char key[48];
      { time_t tt = (time_t) hr; struct tm g; gmtime_r(&tt, &g); strftime(key, sizeof(key), "api/dialamoon/%Y-%m-%dT%H:00", &g); }
      host_http_bodies[key] = "{\"image\":{\"id\":1,\"url\":\"https://svs.gsfc.nasa.gov/vis/a000000/a005500/a005587/frames/730x730_1x1_30p/moon.6501.jpg\","
                              "\"width\":730,\"height\":730},\"image_highres\":{\"url\":\"x.tif\"},\"su_image\":{\"url\":\"https://svs.gsfc.nasa.gov/vis/a000000/a005500/a005588/frames/730x730_1x1_30p/moon.6501.jpg\"},"
                              "\"time\":\"2026-09-28T20:00\",\"phase\":94.27,\"obscuration\":0,\"age\":17.69,\"diameter\":1923.2,\"distance\":372667,\"posangle\":340.21}";
      host_http_bodies["a005587/frames/730x730_1x1_30p/moon.6501.jpg"] = slurp(HOST_DIR "/fixtures/moon_test.jpg");
      sat::select_object(sat::K_MOON, 0);
      CHECK(!lv_obj_has_flag(sat::ui.card_img_btn, LV_OBJ_FLAG_HIDDEN) && !strcmp(lv_label_get_text(sat::ui.card_img_lbl), "Image"),
            "moon card button");
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r9_mooncard.ppm");
      sat::deselect();
      sat::img_view_open_now(IMG_MOON);
      hold_loading(IMG_MOON, sat::net::STG_DOWNLOAD, 41 * 1024, 107 * 1024);
      sat::img_view_update(sat_host_now);
      lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r9_moonload.ppm");
      sat::net::img_stage = sat::net::STG_IDLE;
      sat::net::do_image(IMG_MOON);
      sat::tick();
      const auto &mi = sat::live.img[IMG_MOON];
      printf("moon image: ok %d err '%s' phase %.1f age %.1f dist %.0f obs %.0f (hr %lld) su %d\n", mi.ok, mi.err, mi.phase, mi.age,
             mi.dist_km, mi.obs, (long long) hr, mi.south_up);
      CHECK(mi.ok && sat::live.img_px[IMG_MOON] && fabs(mi.phase - 94.27f) < 0.01f && mi.obs == (double) hr && !mi.south_up,
            "moon image decoded");
      sat::img_view_update(sat_host_now);
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      printf("moon view: \"%s\" / \"%s\"\n", lv_label_get_text(sat::sv.cap1), lv_label_get_text(sat::sv.cap2));
      CHECK(strstr(lv_label_get_text(sat::sv.cap1), "94% lit, 17.7 days old, 372,667 km away") && strstr(lv_label_get_text(sat::sv.cap2), "north up"),
            "moon view captions");
      save_ppm(OUT_DIR "/renders/r9_moonview.ppm");
      host_last_url.clear();
      sat::net::do_image(IMG_MOON);  // same hour: the JPEG is not fetched again
      CHECK(host_last_url.find("dialamoon") != std::string::npos, "moon repeat skipped (%s)", host_last_url.c_str());
      host_http_close_kept = true;  // 4.5.33: the server dropped the kept connection meanwhile
      sat::net::do_image(IMG_MOON);
      sat::tick();
      CHECK(!host_http_close_kept && sat::live.img[IMG_MOON].ok, "moon: a closed kept connection is replaced (%s)",
            sat::live.img[IMG_MOON].err);
      sat::img_view_close();
    }
    // ---- UI-62 the Earth (GOES GeoColor), frames named by scan time (UI-59d)
    {
      const double f10 = floor(sat_host_now / 600.0) * 600.0;  // this 10-minute frame is not up yet (404)
      char st1[32], st2[32];
      sat::net::star_stamp(f10 - 600, st1, sizeof(st1));
      sat::net::star_stamp(f10 - 1200, st2, sizeof(st2));
      host_http_bodies[std::string(st1) + "_GOES18-ABI-FD-GEOCOLOR-678x678.jpg"] = slurp(HOST_DIR "/fixtures/earth_test.jpg");
      sat::earth_regional = false;
      sat::img_view_open_now(sat::net::IMG_EARTH, -1);
      hold_loading(sat::net::IMG_EARTH, sat::net::STG_DOWNLOAD, 180 * 1024, 460 * 1024);
      sat::img_view_update(sat_host_now);
      CHECK(strstr(lv_label_get_text(sat::sv.msg), "39%  -  180 of 460 KB") && sat::sv.tab[2] && !sat::sv.tab[3], "earth loading + tabs");
      sat::net::img_stage = sat::net::STG_IDLE;
      host_last_url.clear();
      sat::net::do_image(sat::net::IMG_EARTH);
      sat::tick();
      const auto &ei = sat::live.img[sat::net::IMG_EARTH];
      printf("earth image: ok %d err '%s' src %s obs %.0f (want %.0f) url %s\n", ei.ok, ei.err, ei.src, ei.obs,
             f10 - 600, host_last_url.c_str());
      CHECK(ei.ok && sat::live.img_px[sat::net::IMG_EARTH] && !strcmp(ei.src, "GOES-18") && fabs(ei.obs - (f10 - 600)) < 1 &&
            host_last_url.find(st1) != std::string::npos, "earth image decoded (newest frame that exists)");
      sat::img_view_update(sat_host_now);
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      printf("earth view: \"%s\" / \"%s\"\n", lv_label_get_text(sat::sv.cap1), lv_label_get_text(sat::sv.cap2));
      CHECK(strstr(lv_label_get_text(sat::sv.cap1), "GOES-18 ") && strstr(lv_label_get_text(sat::sv.cap1), " ago") &&
            sat::sv.seg[0] && sat::sv.seg[1] && !strcmp(lv_label_get_text(lv_obj_get_child(sat::sv.seg[1], 0)), "Alaska"), "earth captions + switch");
      {  // UI-62b: the corners fade into the page colour
        const uint16_t cpx = sat::live.img_px[sat::net::IMG_EARTH][0];
        CHECK(cpx == (uint16_t) ((7 >> 3) << 11 | (11 >> 2) << 5 | (24 >> 3)), "earth corner masked (%04x)", cpx);
      }
      save_ppm(OUT_DIR "/renders/r10_earth.ppm");
      const uint32_t sq = sat::live.img[sat::net::IMG_EARTH].seq;
      host_last_url.clear();
      const int opens0 = host_http_opens, inits0 = host_http_inits;
      sat::net::do_image(sat::net::IMG_EARTH);  // the newest there is is showing: no download
      sat::tick();
      printf("earth repeat: opens %d inits %d last '%s'\n", host_http_opens - opens0, host_http_inits - inits0, host_last_url.c_str());
      CHECK(sat::live.img[sat::net::IMG_EARTH].seq == sq + 1 && sat::live.img[sat::net::IMG_EARTH].ok && !sat::live.img_new[sat::net::IMG_EARTH] &&
            host_http_opens - opens0 == 1 && host_http_inits - inits0 == 0,
            "earth repeat skipped (one 404 on the kept connection)");
      // UI-59d: < fetches the frame 10 minutes before
      host_http_bodies[std::string(st2) + "_GOES18-ABI-FD-GEOCOLOR-678x678.jpg"] = slurp(HOST_DIR "/fixtures/earth_test.jpg");
      CHECK(!lv_obj_has_flag(sat::sv.flip[0], LV_OBJ_FLAG_HIDDEN), "earth < shown");
      lv_obj_send_event(sat::sv.flip[0], LV_EVENT_CLICKED, nullptr);
      CHECK(sat::sv.show_prev && sat::back_loading(sat::net::IMG_EARTH), "earth < asks for the earlier frame");
      run_jobs();
      sat::tick();
      sat::img_view_update(sat_host_now);
      printf("earth earlier: \"%s\" prev obs %.0f\n", lv_label_get_text(sat::sv.cap1), sat::live.img_prev[sat::net::IMG_EARTH].obs);
      CHECK(sat::live.img_prev_px[sat::net::IMG_EARTH] && fabs(sat::live.img_prev[sat::net::IMG_EARTH].obs - (f10 - 1200)) < 1 &&
            strstr(lv_label_get_text(sat::sv.cap1), "earlier picture"), "earth earlier frame shown");
      lv_obj_send_event(sat::sv.flip[0], LV_EVENT_CLICKED, nullptr);  // and one more: none there
      run_jobs();
      sat::tick();
      sat::img_view_update(sat_host_now);
      CHECK(strstr(lv_label_get_text(sat::sv.cap1), "none earlier"), "earth: no frame before that");
      lv_obj_send_event(sat::sv.flip[1], LV_EVENT_CLICKED, nullptr);
      CHECK(!sat::sv.show_prev, "earth > back to the latest");
      {  // UI-59e: one shared "before" buffer (the Sun's went to the Earth) and one work buffer
        int held = 0;
        for (int k = 0; k < sat::net::IMG_N; k++) held += sat::live.img_prev_px[k] != nullptr;
        CHECK(held == 1 && sat::live.img_prev_px[sat::net::IMG_EARTH] && sat::live.img_prev[IMG_SUN].obs <= 0 &&
              sat::net::img_work_owner == -1, "one picture-before buffer, work buffer free");
      }
      // ---- UI-62a the region (Alaska for the test location), label strip cropped
      host_http_bodies["_GOES18-ABI-ak-GEOCOLOR-500x500.jpg"] = slurp(HOST_DIR "/fixtures/region_test.jpg");
      sat::earth_regional = true;  // as the Alaska button does
      sat::img_view_open_now(sat::net::IMG_REGION, -1);
      host_last_url.clear();
      sat::net::do_image(sat::net::IMG_REGION);
      sat::tick();
      sat::img_view_update(sat_host_now);
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      const auto &ri = sat::live.img[sat::net::IMG_REGION];
      printf("region: ok %d err '%s' url %s cap \"%s\"\n", ri.ok, ri.err, host_last_url.c_str(), lv_label_get_text(sat::sv.cap1));
      CHECK(ri.ok && host_last_url.find("SECTOR/ak/GEOCOLOR/") != std::string::npos && host_last_url.find("-ak-GEOCOLOR-500x500.jpg") != std::string::npos &&
            strstr(lv_label_get_text(sat::sv.cap1), "Alaska: GOES-18 ") && sat::earth_kind() == sat::net::IMG_REGION, "region image");
      save_ppm(OUT_DIR "/renders/r10_region.ppm");
      sat::img_view_close();
    }
    // ---- UI-61 planets drawn on the device, opened from their cards
    host_http_bodies["Venus-real_color.jpg"] = slurp(HOST_DIR "/fixtures/venus_test.jpg");
    host_http_bodies["500px-Jupiter_and_its_shrunken"] = slurp(HOST_DIR "/fixtures/jupiter_test.jpg");
    host_http_bodies["500px-Saturn_from_Cassini"] = slurp(HOST_DIR "/fixtures/saturn_test.jpg");
    for (int p = 0; p < 5; p++) {  // (for the browser preview: tonight's view values)
      const auto v = sat::pview::compute(p, sat::astro::jd((double) time(nullptr)));
      printf("PVIEW %d phase %.2f lit %.4f limb %.2f blat %.2f diam %.2f\n", p, v.phase_deg, v.lit, v.limb_pa, v.earth_lat, v.diam);
    }
    sat::img_req[sat::net::IMG_PLANET] = {};  // (earlier card views asked before these existed and got 404s)
    for (int p : {sat::planets::JUPITER, sat::planets::SATURN, sat::planets::VENUS, sat::planets::MERCURY}) {
      sat::select_object(sat::K_PLANET, p);
      CHECK(!lv_obj_has_flag(sat::ui.card_img_btn, LV_OBJ_FLAG_HIDDEN), "planet card Image button");
      sat::deselect();
      sat::img_view_open_now(sat::VIEW_PLANET + p);
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      printf("planet view %d: tab %s | \"%s\" / \"%s\"\n", p, sat::sv.tab[3] ? lv_label_get_text(lv_obj_get_child(sat::sv.tab[3], 0)) : "-",
             lv_label_get_text(sat::sv.cap1), lv_label_get_text(sat::sv.cap2));
      run_jobs();
      sat::tick();  // the photo reaches the loop
      sat::sv.drawn_at = 0;
      sat::img_view_update(sat_host_now);
      lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
      printf("   -> \"%s\" / \"%s\" photo_of %d req_of %d asked %d ok %d err '%s' px %p url %s\n", lv_label_get_text(sat::sv.cap1), lv_label_get_text(sat::sv.credit), sat::planet_px_of,
             sat::planet_req_of, sat::img_req[sat::net::IMG_PLANET].asked, sat::live.img[sat::net::IMG_PLANET].ok,
             sat::live.img[sat::net::IMG_PLANET].err, (void *) sat::live.img_px[sat::net::IMG_PLANET], host_last_url.c_str()); printf("   seq live %u req %u pend %u fresh %d\n", (unsigned) sat::live.img[4].seq, (unsigned) sat::img_req[4].seq, (unsigned) sat::net::pending.img[4].seq, (int) sat::net::pending.img[4].fresh);
      if (p == sat::planets::MERCURY && !sat::photos::get(p).data)  // no photo to be had: drawn instead
        CHECK(strstr(lv_label_get_text(sat::sv.credit), "Photo unavailable") && sat::planet_px, "planet drawn fallback");
      else
        CHECK(sat::sv.tab[3] && !strcmp(lv_label_get_text(lv_obj_get_child(sat::sv.tab[3], 0)), sat::planets::name(p)) &&
              lv_label_get_text(sat::sv.credit)[0] && lv_obj_has_flag(sat::sv.cap2, LV_OBJ_FLAG_HIDDEN) &&
              sat::planet_src_of == p, "planet photo view");
      char fnp[64];
      snprintf(fnp, sizeof(fnp), OUT_DIR "/renders/r10_planet%d.ppm", p);
      save_ppm(fnp);
      if (p <= sat::planets::MARS) {  // UI-61g: "Tonight's phase" off and on again
        CHECK(sat::sv.phase_mark && lv_obj_has_state(sat::sv.phase_mark, LV_STATE_CHECKED), "phase switch on");
        lv_obj_remove_state(sat::sv.phase_mark, LV_STATE_CHECKED);
        lv_obj_send_event(sat::sv.phase_mark, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp);
        CHECK(!sat::planet_phase, "phase off: full photo");
        snprintf(fnp, sizeof(fnp), OUT_DIR "/renders/r12_nophase%d.ppm", p);
        save_ppm(fnp);
        lv_obj_add_state(sat::sv.phase_mark, LV_STATE_CHECKED);
        lv_obj_send_event(sat::sv.phase_mark, LV_EVENT_VALUE_CHANGED, nullptr);
        CHECK(sat::planet_phase, "phase on again");
      } else {
        CHECK(sat::sv.phase_mark == nullptr, "no phase box for %d", p);
      }
      if (p == sat::planets::JUPITER) {
        int shown = 0;
        for (auto *l : sat::sv.mlab) shown += !lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN);
        CHECK(shown >= 2, "jupiter moon letters (%d)", shown);
        // switch tab to the Moon and back: the planet tab stays
        sat::img_view_open_now(sat::net::IMG_MOON, sat::sv.planet);
        CHECK(sat::sv.tab[3] && sat::sv.kind == sat::net::IMG_MOON, "planet tab kept on switching");
      }
      sat::img_view_close();
    }
    {  // UI-61d: the photos are built in: Venus again with no network body at all
      host_http_bodies.erase("Venus-real_color.jpg");
      host_last_url.clear();
      sat::net::img_have[sat::net::IMG_PLANET] = -1;
      sat::img_req[sat::net::IMG_PLANET] = {};
      sat::planet_src_of = -1;
      const double keep_now = sat_host_now;
      if (getenv("VENUS_NOW")) sat_host_now = (double) time(nullptr);
      sat::img_view_open_now(sat::VIEW_PLANET + sat::planets::VENUS);
      run_jobs();
      sat::tick();
      sat::sv.drawn_at = 0;
      sat::img_view_update(sat_host_now);
      if (getenv("VENUS_NOW")) { lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp); save_ppm(OUT_DIR "/venus_now.ppm"); sat_host_now = keep_now; }
      printf("venus built in: \"%s\" url '%s'\n", lv_label_get_text(sat::sv.credit), host_last_url.c_str());
      CHECK(sat::planet_src_of == sat::planets::VENUS && host_last_url.empty() && !strcmp(lv_label_get_text(sat::sv.credit), "NASA Mariner 10"),
            "venus photo built in, no download");
      sat::img_view_close();
    }
    sat::live.ovation = sat::net::AuroraChance();
    sat::net::ov_seen = sat::net::AuroraChance();
    sat_host_now = day_now;
    sat::aur.until = 0;
    sat::tick();
  }
  // ---- UI-65 the logo; UI-66 firmware from a microSD card
  {
    const lv_image_dsc_t *ld = sat::logo_dsc();
    CHECK(ld && ld->header.w == 200 && ld->header.cf == LV_COLOR_FORMAT_RGB565A8, "logo unpacked");
    {  // the boot page's logo and wordmark, on the real panel size (the rest of that page is YAML)
      lv_obj_t *scr = lv_obj_create(nullptr);
      lv_obj_set_style_bg_color(scr, lv_color_hex(0), 0);
      lv_screen_load(scr);
      lv_obj_t *box = lv_obj_create(scr);
      lv_obj_remove_style_all(box);
      lv_obj_set_size(box, 200, 200);
      lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 30);
      CHECK(sat::logo_show(box, 200) != nullptr, "logo shown");
      lv_obj_t *box2 = lv_obj_create(scr);
      lv_obj_remove_style_all(box2);
      lv_obj_set_size(box2, 112, 112);
      lv_obj_set_pos(box2, 12, 300);
      sat::logo_show(box2, 112);
      lv_refr_now(disp);
      save_ppm(OUT_DIR "/renders/r12_logo.ppm");
    }
    namespace sd = sat::sdfw;
    sd::init("4.5.38", &mono16, &mono16, &mono16);
    const std::string dir = OUT_DIR "/sdcard";
    system(("rm -rf " + dir + " && mkdir -p " + dir).c_str());
    auto img = [&](const char *name, size_t n, char fill) {
      std::string b(n, fill);
      b[0] = (char) 0xE9;
      memcpy(&b[32 + 112], "Oct  2 2026", 11);
      std::ofstream(dir + "/" + name, std::ios::binary) << b;
    };
    img("sky_tracker_firmware_4.5.9.bin", 300000, 'a');
    img("SKY_TRACKER_FIRMWARE_4.10.0.bin", 400000, 'b');  // newest by version order, any case
    std::ofstream(dir + "/sky_tracker_firmware_junk.bin") << "not firmware";
    std::ofstream(dir + "/notes.txt") << "hi";
    auto boot = [&]() { sd::state = sd::S_NONE; sd::declined = 0; sd::boot_probe(); sd::ui_update(); };  // RAM starts clean
    auto probe = [&]() { sd::ui_update(); };
    sd::host_dir.clear();
    boot();
    CHECK(sd::state == sd::S_NONE && !sd::sui.root, "no card, no prompt");
    sd::host_dir = dir;
    boot();
    printf("sd offer: %s ver %s size %u built %s\n", sd::offer.file, sd::offer.ver, (unsigned) sd::offer.size, sd::offer.built);
    CHECK(sd::state == sd::S_OFFER && !strcmp(sd::offer.ver, "4.10.0") && sd::offer.size == 400000 && sd::sui.root &&
          !strcmp(lv_label_get_text(sd::sui.l1), "On the card: 4.10.0") && !strcmp(lv_label_get_text(sd::sui.l2), "Installed: 4.5.38"),
          "card offers the newest firmware");
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r12_sd_prompt.ppm");
    lv_obj_send_event(sd::sui.btn[0], LV_EVENT_CLICKED, nullptr);  // Not now
    probe();
    CHECK(!sd::sui.root && sd::declined == sd::offer.key, "not now: not asked again this boot");
    boot();
    CHECK(sd::state == sd::S_OFFER && sd::sui.root, "next boot: asked again");
    sd::close_ui();
    sd::host_same_build = true;  // the same build as running: never offered
    boot();
    CHECK(sd::state == sd::S_CARD && !sd::sui.root, "the running build is not offered");
    sd::host_same_build = false;
    boot();
    lv_obj_send_event(sd::sui.btn[1], LV_EVENT_CLICKED, nullptr);  // Update
    CHECK(sd::state == sd::S_FLASHING && !strcmp(lv_label_get_text(sd::sui.title), "Updating firmware"), "update starts");
    sd::done_bytes = 180000;
    sd::ui_update();
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r12_sd_progress.ppm");
    printf("sd progress: \"%s\"\n", lv_label_get_text(sd::sui.l1));
    CHECK(strstr(lv_label_get_text(sd::sui.l1), "45%"), "progress shown");
    run_jobs();
    sd::ui_update();
    CHECK(sd::state == sd::S_DONE && sd::host_flashed.size() == 400000 && sd::host_flashed[1] == 'b' &&
          !strcmp(lv_label_get_text(sd::sui.title), "Update installed"), "written, restarting");
    sat_host_now += 2;
    sd::ui_update();
    CHECK(sd::host_restarts == 1, "restarted");
    sd::close_ui();
    sd::state = sd::S_NONE;
    sd::host_dir.clear();
  }
  // ---- UI-68 internet updates: Settings > Updates, and the hourly check's offer
  {
    namespace up = sat::upd;
    up::init(nullptr, &mono16, &mono16, &mono16);
    up::host = up::HostUpdate();
    up::host.current = "4.6.0";
    up::host.state = 1;
    up::check_now();
    CHECK(up::host.checks == 1 && up::ui_.mode == up::M_CHECKING, "check started");
    sat_host_now += 2;
    up::tick();
    CHECK(up::ui_.mode == up::M_LATEST && strstr(lv_label_get_text(up::ui_.l1), "4.6.0 is the latest"), "up to date");
    lv_obj_send_event(up::ui_.btn[0], LV_EVENT_CLICKED, nullptr);
    CHECK(!up::ui_.root, "closed");
    // UI-68a: the map page's gear and the update icon beside it, as the YAML builds them
    lv_screen_load(page1);
    auto mkbtn = [&](int x, const char *sym, uint32_t col) {
      lv_obj_t *b = lv_button_create(page1);
      lv_obj_set_size(b, 46, 46);
      lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, x, -2);
      lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
      lv_obj_set_style_shadow_width(b, 0, 0);
      lv_obj_set_style_border_width(b, 0, 0);
      lv_obj_t *l = lv_label_create(b);
      lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
      lv_label_set_text(l, sym);
      lv_obj_center(l);
      return b;
    };
    mkbtn(-2, LV_SYMBOL_SETTINGS, 0xB8BEC9);
    lv_obj_t *ub = mkbtn(-50, LV_SYMBOL_DOWNLOAD, 0xFFB547);
    up::set_button(ub);
    up::tick();
    CHECK(lv_obj_has_flag(ub, LV_OBJ_FLAG_HIDDEN), "no icon while up to date");
    up::host.state = 2;
    up::host.latest = "4.6.1";
    up::tick();  // the hourly check found 4.6.1: an icon, no prompt
    CHECK(!up::ui_.root && !lv_obj_has_flag(ub, LV_OBJ_FLAG_HIDDEN), "hourly find: icon only");
    lv_obj_invalidate(page1);
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r15_update_icon.ppm");
    up::offer_now();  // the icon tapped
    CHECK(up::ui_.mode == up::M_AVAILABLE && strstr(lv_label_get_text(up::ui_.l1), "4.6.1"), "icon opens the prompt");
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r13_update.ppm");
    lv_obj_send_event(up::ui_.btn[0], LV_EVENT_CLICKED, nullptr);  // Not now
    up::tick();
    CHECK(!up::ui_.root && !lv_obj_has_flag(ub, LV_OBJ_FLAG_HIDDEN), "not now: closed, the icon stays");
    up::tick();
    CHECK(!up::ui_.root, "never opens by itself");
    up::check_now();  // asked again from Settings: offered again
    sat_host_now += 1;
    up::tick();
    CHECK(up::ui_.mode == up::M_AVAILABLE, "settings check offers it");
    lv_obj_send_event(up::ui_.btn[1], LV_EVENT_CLICKED, nullptr);  // Update
    CHECK(up::host.performs == 1 && up::ui_.mode == up::M_INSTALLING, "install started");
    up::host.has_progress = true;
    up::host.progress = 37;
    up::tick();
    CHECK(strstr(lv_label_get_text(up::ui_.l1), "37%"), "install progress");
    CHECK(lv_obj_has_flag(ub, LV_OBJ_FLAG_HIDDEN), "no icon while installing");
    up::close();
    up::set_button(nullptr);
    lv_obj_delete(ub);
    // an OLDER release is never offered (ESPHome calls any difference "available")
    up::host = up::HostUpdate();
    up::declined.clear();
    up::host.current = "4.6.10";
    up::host.latest = "4.6.9";
    up::host.state = 2;
    up::tick();
    CHECK(!up::ui_.root && up::host.performs == 0, "older release not offered");
    up::check_now();
    sat_host_now += 2;
    up::tick();
    CHECK(up::ui_.mode == up::M_LATEST, "older release: up to date");
    up::close();
    up::host.latest = "4.6.11";
    up::tick();
    up::offer_now();
    CHECK(up::ui_.mode == up::M_AVAILABLE, "newer release offered");
    up::close();
  }
  // ---- UI-70 time zone picker
  {
    namespace tz = sat::tz;
    tz::set_global(tz::ZONES[10]);  // HA's zone: UTC, say
    tz::set(2);                      // Alaska
    CHECK(tz::same(tz::get_global(), tz::ZONES[2]) && tz::get_global().std_offset_seconds == 9 * 3600, "Alaska applied");
    tz::set_global(tz::ZONES[10]);  // an HA time sync puts its zone back...
    tz::tick();
    CHECK(tz::same(tz::get_global(), tz::ZONES[2]), "...and the choice wins again");
    tz::set(0);
    CHECK(tz::same(tz::get_global(), tz::ZONES[10]), "Auto: back to HA's zone");
    tz::set_global(tz::ZONES[3]);
    tz::tick();
    CHECK(tz::same(tz::get_global(), tz::ZONES[3]), "Auto leaves HA's zone alone");
    // the Display tab as the YAML lays it out (rows 42 px apart, the zone row on top)
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0), 0);
    lv_screen_load(scr);
    auto lab = [&](int x, int y, const char *t, const lv_font_t *f, uint32_t col) {
      lv_obj_t *l = lv_label_create(scr);
      lv_obj_set_style_text_font(l, f, 0);
      lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
      lv_label_set_text(l, t);
      lv_obj_set_pos(l, x, y);
      return l;
    };
    auto swi = [&](int x, int y, bool on) {
      lv_obj_t *w = lv_switch_create(scr);
      lv_obj_set_pos(w, x, y);
      lv_obj_set_size(w, 56, 28);
      if (on) lv_obj_add_state(w, LV_STATE_CHECKED);
    };
    lab(16, 16, "SETTINGS", &mono16, 0xFF8A1F);
    lab(16, 66, "Display    Location    Celestial", &mono16, 0xC9D3F2);
    const int Y = 100;
    lab(16, Y + 16, "Time zone", &mono16, 0xC9D3F2);
    lab(128, Y + 14, "\xF3\xB0\x87\xA7", &mdi20, 0x7EE0B0);  // earth
    lv_obj_t *dd = lv_dropdown_create(scr);
    lv_obj_set_pos(dd, 190, Y + 8);
    lv_obj_set_size(dd, 260, 36);
    lv_dropdown_set_options(dd, "Auto (from HA)\nHawaii\nAlaska\nPacific\nMountain\nArizona\nCentral\nEastern\nAtlantic\nNewfoundland\nUTC\nLondon\nCentral Europe\nEastern Europe\nMoscow\nIndia\nChina\nJapan\nSydney\nAuckland");
    lv_dropdown_set_selected(dd, 2);
    lv_obj_set_style_text_font(dd, &mono16, 0);
    lv_obj_set_style_text_color(dd, lv_color_hex(0xEEF2FF), 0);
    lv_obj_set_style_bg_color(dd, lv_color_hex(0x0E1836), 0);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x3A4E86), 0);
    lv_obj_set_style_border_width(dd, 2, 0);
    lv_obj_set_style_pad_all(dd, 6, 0);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(dd, lv_color_hex(0x7E8BB3), LV_PART_INDICATOR);
    lab(16, Y + 58, "Time format", &mono16, 0xC9D3F2);
    lab(190, Y + 58, "12h", &mono16, 0xC9D3F2);
    swi(232, Y + 52, false);
    lab(298, Y + 58, "24h", &mono16, 0xC9D3F2);
    lab(16, Y + 100, "Brightness", &mono16, 0xC9D3F2);
    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_set_pos(sl, 200, Y + 104);
    lv_obj_set_size(sl, 250, 14);
    lv_slider_set_value(sl, 70, LV_ANIM_OFF);
    lab(16, Y + 142, "Auto bright", &mono16, 0xC9D3F2);
    swi(232, Y + 136, true);
    lab(298, Y + 144, "dims after sunset", &mono16, 0x7E8BB3);
    lab(16, Y + 184, "Miles", &mono16, 0xC9D3F2);
    swi(232, Y + 178, true);
    lab(16, Y + 226, "Night mode", &mono16, 0xC9D3F2);
    lab(128, Y + 224, "\xF3\xB0\x96\x94", &mdi20, 0xFF6B6B);  // weather-night
    swi(232, Y + 220, false);
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r17_tz.ppm");
    lv_dropdown_open(dd);
    if (lv_obj_t *list = lv_dropdown_get_list(dd)) {
      lv_obj_set_style_text_font(list, &mono16, 0);
      lv_obj_set_style_text_color(list, lv_color_hex(0xEEF2FF), 0);
      lv_obj_set_style_bg_color(list, lv_color_hex(0x0E1836), 0);
      lv_obj_set_style_border_color(list, lv_color_hex(0x3A4E86), 0);
      lv_obj_set_style_max_height(list, 300, 0);
    }
    lv_refr_now(disp);
    save_ppm(OUT_DIR "/renders/r17_tz_open.ppm");
    lv_dropdown_close(dd);
    lv_screen_load(page1);
    lv_obj_delete(scr);
  }
  // ---- UI-69 launch animation at a launch's T-0
  {
    namespace rk = sat::rocket;
    rk::init(&mono18);
    lv_screen_load(page1);
    CHECK(std::all_of(rk::st.img_buf, rk::st.img_buf + rk::FLAMES, [](lv_draw_buf_t *b) { return b != nullptr; }), "rocket drawn, every flame");
    if (getenv("ROCKET_GIF")) {  // the flames side by side, 3x, for review
      FILE *f = fopen(OUT_DIR "/flames.ppm", "wb");
      fprintf(f, "P6 %d %d 255\n", rk::FLAMES * rk::IW * 3, rk::IH * 3);
      for (int y = 0; y < rk::IH * 3; y++)
        for (int i = 0; i < rk::FLAMES; i++)
          for (int x = 0; x < rk::IW * 3; x++) {
            const uint8_t *p = rk::st.img_buf[i]->data + (y / 3) * rk::st.img_buf[i]->header.stride + (x / 3) * 4;
            const int a = p[3];  // over the map's navy
            unsigned char c[3] = {(unsigned char) ((p[2] * a + 0x0E * (255 - a)) / 255), (unsigned char) ((p[1] * a + 0x18 * (255 - a)) / 255), (unsigned char) ((p[0] * a + 0x36 * (255 - a)) / 255)};
            fwrite(c, 1, 3, f);
          }
      fclose(f);
    }
    while (lv_obj_get_child_count(lv_layer_top()) > 0)
      lv_obj_delete(lv_obj_get_child(lv_layer_top(), 0));
    sat::live.launches.clear();
    sat::net::LaunchRec lr;
    snprintf(lr.name, sizeof(lr.name), "Falcon 9 | Starlink Group 12-7");
    snprintf(lr.rocket, sizeof(lr.rocket), "Falcon 9");
    snprintf(lr.status, sizeof(lr.status), "Go");
    lr.net = sat::clock_now() + 30;
    sat::live.launches.push_back(lr);
    rk::check();
    CHECK(!rk::playing(), "not before T-0");
    sat_host_now += 31;
    rk::check();
    CHECK(rk::playing() && !strcmp(lv_label_get_text(rk::st.label), "Falcon 9 \xC2\xB7 Starlink Group 12-7"), "plays at T-0");
    const float us[4] = {0.05f, 0.3f, 0.55f, 0.8f};
    float done = 0;
    for (int f = 0; f < 4; f++) {
      // step through in 33 ms frames so the smoke trail builds as on the device
      while (done < us[f] * rk::DUR_S) {
        sat_host_now += 0.033;
        done += 0.033f;
        rk::frame();
      }
      lv_refr_now(disp);
      char fn[64];
      snprintf(fn, sizeof(fn), OUT_DIR "/renders/r14_rocket%d.ppm", f);
      save_ppm(fn);
    }
    sat_host_now += rk::DUR_S;
    rk::frame();
    CHECK(!rk::playing() && lv_obj_get_child_count(lv_layer_top()) == 0, "cleans up after itself");
    rk::check();
    CHECK(!rk::playing(), "once per launch");
    if (getenv("ROCKET_PERF")) {  // dirty pixels and render time per 33 ms frame
      static int big_n = 0;
      if (getenv("ROCKET_INV")) lv_display_add_event_cb(disp, [](lv_event_t *e) {
          const lv_area_t *ar = (const lv_area_t *) lv_event_get_param(e);
          if (ar && lv_area_get_size(ar) > 15000 && big_n++ < 30) printf("  big inval %d,%d-%d,%d (%d px)\n", (int) ar->x1, (int) ar->y1, (int) ar->x2, (int) ar->y2, (int) lv_area_get_size(ar));
        }, LV_EVENT_INVALIDATE_AREA, nullptr);
      {
        std::function<int(lv_obj_t *)> cnt = [&](lv_obj_t *o) { int n = 1; for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) n += cnt(lv_obj_get_child(o, i)); return n; };
        int vis = 0; std::function<void(lv_obj_t *)> cv = [&](lv_obj_t *o) { if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return; vis++; for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) cv(lv_obj_get_child(o, i)); };
        cv(lv_screen_active());
        printf("ROCKET_PERF objects on the map page: %d (visible %d)\n", cnt(lv_screen_active()), vis);
      }
      rk::play("Falcon 9 | Starlink Group 12-7");
      lv_refr_now(disp);
      long tot = 0, mx = 0; double us = 0, umx = 0; int n = 0;
      while (rk::playing()) {
        sat_host_now += 0.016;  // the device's 16 ms refresh; REFR_START runs frame()
        g_flush_px = 0;
        auto a = std::chrono::steady_clock::now();
        lv_refr_now(disp);
        double d = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - a).count();
        if (!rk::playing()) break;
        if (getenv("ROCKET_SPIKES") && d > 1500) printf("  spike n=%d t=%.2fs px=%ld us=%.0f\n", n, n * 0.016, g_flush_px, d);
        tot += g_flush_px; mx = std::max(mx, g_flush_px); us += d; umx = std::max(umx, d); n++;
      }
      printf("ROCKET_PERF (16 ms refresh) frames %d  avg px %ld (%.0f%% screen)  max px %ld  avg %.0f us  max %.0f us\n", n, tot / n, 100.0 * tot / n / 230400, mx, us / n, umx);
    }
    if (getenv("ROCKET_GIF")) {  // the README animation: the real code, frame by frame
      system("rm -rf " OUT_DIR "/rgif && mkdir -p " OUT_DIR "/rgif");
      if (sat::ui.w.status)  // not the test data's CelesTrak error
        lv_label_set_text(sat::ui.w.status, "Falcon 9 lifting off now - Cape Canaveral SFS");
      rk::play("Falcon 9 | Starlink Group 12-7");
      for (int f = 0; rk::playing(); f++) {
        sat_host_now += 0.033;  // a 30 fps GIF; each refresh runs frame() itself
        lv_refr_now(disp);
        if (!rk::playing())
          break;
        char fn[80];
        snprintf(fn, sizeof(fn), OUT_DIR "/rgif/f%03d.ppm", f);
        save_ppm(fn);
      }
    }
    if (getenv("BOOT_GIF")) {  // UI-69k: the launch over a mock of the boot screen (wifi_page)
      lv_obj_t *old = lv_screen_active();
      lv_obj_t *scr = lv_obj_create(nullptr);
      lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
      lv_screen_load(scr);
      lv_obj_t *box = lv_obj_create(scr);
      lv_obj_remove_style_all(box);
      lv_obj_set_size(box, 200, 200);
      lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 46);
      sat::logo_show(box, 200);
      auto lbl = [&](const char *t, const lv_font_t *f, uint32_t c, int y) {
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, t);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, 380);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
      };
      lbl("Sky Tracker", &mono18, 0xFFFFFF, 266);  // (the harness mono24 has caps only)
      lbl("Connecting to Wi-Fi", &mono18, 0xFFFFFF, 306);
      lbl("Home Network", &mono16, 0x9AA6C8, 334);
      lbl("GPS: connected, searching\nCompass: connected (QMC5883L)", &mono16, 0x7E8BB3, 372);
      system("rm -rf " OUT_DIR "/bgif && mkdir -p " OUT_DIR "/bgif");
      rk::play_boot();
      CHECK(rk::playing() && rk::st.banner == nullptr, "boot launch: plays, no banner");
      for (int f = 0; rk::playing(); f++) {
        sat_host_now += 0.033;
        lv_refr_now(disp);
        if (!rk::playing())
          break;
        char fn[80];
        snprintf(fn, sizeof(fn), OUT_DIR "/bgif/f%03d.ppm", f);
        save_ppm(fn);
      }
      lv_screen_load(old);
      lv_obj_delete(scr);
    }
    {  // the banner keeps a long name on one line inside its border
      rk::play("Falcon 9 Block 5 | SDA Tranche 1 Transport Layer A");
      lv_refr_now(disp);
      lv_obj_update_layout(rk::st.banner);
      lv_area_t lb, bb;
      lv_obj_get_coords(rk::st.label, &lb);
      lv_obj_get_coords(rk::st.banner, &bb);
      printf("banner label %d..%d in %d..%d: '%s'\n", (int) lb.y1, (int) lb.y2, (int) bb.y1, (int) bb.y2, lv_label_get_text(rk::st.label));
      CHECK(lb.y1 >= bb.y1 + 2 && lb.y2 <= bb.y2 - 2, "banner text inside its border");
      if (getenv("ROCKET_GIF")) save_ppm(OUT_DIR "/banner_long.ppm");
      rk::stop();
    }
    rk::play_now();
    CHECK(rk::playing(), "button plays it");
    CHECK(sat::rocket::active(), "sat::tick pauses while it plays (UI-69b)");
    lv_obj_send_event(rk::st.catcher, LV_EVENT_CLICKED, nullptr);
    CHECK(!rk::playing() && lv_obj_get_child_count(lv_layer_top()) == 0, "a tap skips it");
    {  // UI-69f undocking at the ISS at its time
      sat::live.events.clear();
      sat::net::EventRec e;
      snprintf(e.name, sizeof(e.name), "SpaceX Crew-12 Crew Dragon Undocking");
      snprintf(e.type, sizeof(e.type), "Spacecraft Undocking");
      e.exact = true;
      e.iss = true;
      e.t = sat::clock_now() - 3;
      sat::live.events.push_back(e);
      rk::check();
      CHECK(rk::playing() && rk::st.undock && !strcmp(lv_label_get_text(rk::st.label), "SpaceX Crew-12 Crew Dragon Undocking"), "undocking plays at its time");
      if (getenv("UNDOCK_GIF")) {
        system("rm -rf " OUT_DIR "/ugif && mkdir -p " OUT_DIR "/ugif");
        rk::play_scene(true, e.name);
        for (int f = 0; rk::playing(); f++) {
          sat_host_now += 0.033;
          lv_refr_now(disp);
          if (!rk::playing()) break;
          char fn[80];
          snprintf(fn, sizeof(fn), OUT_DIR "/ugif/f%03d.ppm", f);
          save_ppm(fn);
        }
      }
      sat_host_now += 7;
      rk::frame();
      CHECK(!rk::playing() && lv_obj_get_child_count(lv_layer_top()) == 0, "undocking cleans up");
      rk::check();
      CHECK(!rk::playing(), "undocking once");
      rk::play_undock_now();
      CHECK(rk::playing() && rk::st.undock, "undocking button");
      rk::stop();
      sat::live.events.clear();
    }
    rk::set_enabled(false);
    sat::live.launches[0].net = sat::clock_now() - 5;
    snprintf(sat::live.launches[0].name, sizeof(sat::live.launches[0].name), "Electron | Test");
    rk::check();
    CHECK(!rk::playing(), "switch off: no animation");
    rk::set_enabled(true);
  }
  if (getenv("ALERT_ICONS")) {  // UI-41c review: alert icon placement, each mode, several alert kinds
    lv_screen_load(page1);
    sat::Alert al[6] = {};
    auto mk = [&](int i, const char *t, uint32_t c, const char *g, const lv_image_dsc_t *im) {
      snprintf(al[i].text, sizeof(al[i].text), "%s", t); al[i].col = c; al[i].glyph = g; al[i].img = im; };
    mk(0, "Falcon 9 launches in 1d 2h - Cape Canaveral SFS", sat::C_LAUNCH, "\xF3\xB1\x93\x9E", nullptr);
    mk(1, "SpaceX Crew-12 Crew Dragon Undocking in 2d 21h", sat::C_EVENT, "\xF3\xB1\x8E\x83", nullptr);
    mk(2, "Aurora possible tonight - Kp 5", sat::C_AURORA, "\xF3\xB1\xAE\xB9", nullptr);
    mk(3, "Venus visible - W, 12\xC2\xB0 up", sat::C_PLANET[sat::planets::VENUS], nullptr, sat::planet_dsc(sat::planets::VENUS, true));
    mk(4, "Comet C/2025 A1 visible - mag 5.2, NE 30\xC2\xB0 up", sat::C_COMET, sat::ICON_COMET, nullptr);
    mk(5, "ISS visible pass 06:44 in 2h 10m", sat::C_ISS, "\xF3\xB1\x8E\x83", nullptr);
    for (int mode = 0; mode < 4; mode++)
      for (int i = 0; i < 6; i++) {
        sat::alert_icon_align = mode;
        sat::alert_override = &al[i];
        sat::draw_hud(sat::clock_now());
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
        char fn[80];
        snprintf(fn, sizeof(fn), OUT_DIR "/ai_%d_%d.ppm", mode, i);
        save_ppm(fn);
      }
    sat::alert_override = nullptr;
    sat::alert_icon_align = 1;
  }
  printf("PSRAM allocations: %zu\n", host_psram_allocs);
  printf(failures ? "\n%d FAILURES\n" : "\nALL CHECKS PASSED\n", failures);
  return failures ? 1 : 0;
}
