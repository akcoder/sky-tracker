#ifndef HOST_DIR
#define HOST_DIR "."   // this folder (test data)
#endif
#ifndef OUT_DIR
#define OUT_DIR "out"  // renders and logs
#endif
// Host test for sat_tracker.h / sat_net.h against real LVGL 9.5 and ArduinoJson 7.4.3.
// VER-1 (maths vs skyfield/pyephem vectors) and VER-2 (drawing + fetch parsing).
#define SAT_HOST_TEST
#include "lvgl.h"
#include <ArduinoJson.h>
#include <map>
#include <string>
#include <chrono>
#include <fstream>
#include <sstream>
#include <random>

namespace sat { double sat_host_now = 0; }
using sat::sat_host_now;
int host_tasks_created = 0;
size_t host_psram_allocs = 0;
int host_warnings = 0;
std::map<std::string, std::string> host_http_bodies;
std::string host_last_url;

#include "sat_tracker.h"
static const int SAT_PX_HALF = sat::SAT_PX / 2;

static int failures = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      failures++;                                \
      printf("FAIL %s:%d ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

// ------------------------------------------------------------------ display
static uint16_t fb[480 * 480];
static long flushed_px = 0;
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px) {
  const uint16_t *p = (const uint16_t *) px;
  flushed_px += (long) (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
  for (int y = a->y1; y <= a->y2; y++)
    for (int x = a->x1; x <= a->x2; x++)
      fb[y * 480 + x] = *p++;
  lv_display_flush_ready(d);
}
static void save_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6 480 480 255\n");
  for (int i = 0; i < 480 * 480; i++) {
    uint16_t c = (uint16_t) ((fb[i] >> 8) | (fb[i] << 8));  // ESPHome lv_conf renders byte-swapped RGB565
    unsigned char rgb[3] = {(unsigned char) ((c >> 11) << 3), (unsigned char) (((c >> 5) & 63) << 2),
                            (unsigned char) ((c & 31) << 3)};
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

// ------------------------------------------------------------------ canned N2YO bodies
static std::string above_json(JsonArrayConst sats, const char *key) {
  JsonDocument d;
  d["info"]["category"] = "x";
  d["info"]["transactionscount"] = 1;
  d["info"]["satcount"] = sats.size();
  JsonArray a = d["above"].to<JsonArray>();
  for (JsonObjectConst s : sats) {
    JsonObject o = a.add<JsonObject>();
    o["satid"] = s["id"];
    o["satname"] = s["name"];
    o["intDesignator"] = "2020-001A";
    o["launchDate"] = "2020-01-01";
    o["satlat"] = s[key]["lat"];
    o["satlng"] = s[key]["lon"];
    o["satalt"] = s[key]["alt_km"];
  }
  std::string out;
  serializeJson(d, out);
  return out;
}
static std::string positions_json(JsonObjectConst p, double t) {
  JsonDocument d;
  d["info"]["satname"] = "SPACE STATION";
  d["info"]["satid"] = 25544;
  JsonObject o = d["positions"].add<JsonObject>();
  o["satlatitude"] = p["lat"];
  o["satlongitude"] = p["lon"];
  o["sataltitude"] = p["alt_km"];
  o["azimuth"] = p["az"];
  o["elevation"] = p["el"];
  o["ra"] = 0;
  o["dec"] = 0;
  o["timestamp"] = (int64_t) t;
  o["eclipsed"] = false;
  std::string out;
  serializeJson(d, out);
  return out;
}

static void run_jobs() {  // stands in for the task
  uint8_t job;
  while (xQueueReceive(sat::net::queue, &job, 0) == pdTRUE)
    if (!sat::net::paused)
      sat::net::run_job(job);
}

int main() {
  std::ifstream fin(HOST_DIR "/vectors.json");
  std::stringstream ss;
  ss << fin.rdbuf();
  JsonDocument V;
  deserializeJson(V, ss.str());
  const double LAT = V["observer"]["lat"], LON = V["observer"]["lon"], ALT = V["observer"]["alt"];
  sat::geo::Observer obs;
  obs.set(LAT, LON, ALT);

  // ============ VER-1a Sun and Moon vs pyephem
  {
    double sun_max = 0, moon_max = 0, phase_max = 0;
    for (JsonObject v : V["sunmoon"].as<JsonArray>()) {
      auto r = sat::astro::compute(v["t"].as<double>(), LAT, LON, ALT);
      const float se = v["sun_el"], me = v["moon_el"];
      if (se > 0.5) { double e = sat::geo::separation(r.sun, {v["sun_az"], se}); if (e > 0.1 && getenv("DBG")) printf("sun t=%.0f ephem %.2f/%.2f mine %.2f/%.2f\n", v["t"].as<double>(), v["sun_az"].as<double>(), se, r.sun.az, r.sun.el); sun_max = std::max<double>(sun_max, e); }
      if (me > 0.5) moon_max = std::max<double>(moon_max, sat::geo::separation(r.moon, {v["moon_az"], me}));
      phase_max = std::max<double>(phase_max, fabs(r.moon_illum - v["moon_phase"].as<double>()));
    }
    printf("Sun  max error vs pyephem: %.3f deg\n", sun_max);
    printf("Moon max error vs pyephem: %.3f deg (topocentric), illumination %.3f\n", moon_max, phase_max);
    CHECK(sun_max < 0.1, "sun error %.3f", sun_max);
    CHECK(moon_max < 0.5, "moon error %.3f", moon_max);
    CHECK(phase_max < 0.03, "phase error %.3f", phase_max);
  }

  // ============ VER-1b one geometry path: sub-point -> az/el vs skyfield
  JsonArray tr = V["iss_truth"];
  {
    double mx = 0;
    for (JsonObject p : tr) {
      sat::SatRec r;
      sat::net::unit_from_latlon(p["lat"], p["lon"], r.p);
      r.alt_km = p["alt_km"];
      auto a = sat::geo::azel(obs, sat::geo::sat_ecef(r, p["t"]));
      mx = std::max<double>(mx, sat::geo::separation(a, {p["az"], p["el"]}));
    }
    printf("ISS az/el from sub-point, max error vs skyfield: %.3f deg (%u samples)\n", mx, (unsigned) tr.size());
    CHECK(mx < 0.1, "geometry error %.3f", mx);
  }

  // ============ VER-1c dead reckoning (MOTION-1..3) at ISS (15 s) and /above (90 s) cadence
  for (int cadence : {15, 90}) {
    sat::net::history.clear();
    const int step = cadence / 2;  // truth samples are 2 s apart
    double mx = 0, mx_first = 0;
    sat::SatRec cur;
    bool have = false;
    for (size_t i = 0; i < tr.size(); i++) {
      JsonObject p = tr[i];
      const double t = p["t"];
      if (i % step == 0) {
        sat::SatRec r;
        r.id = 25544;
        r.t_fix = t;
        r.alt_km = p["alt_km"];
        sat::net::unit_from_latlon(p["lat"], p["lon"], r.p);
        sat::net::apply_history(r);
        if (i == 0) CHECK(!r.has_vel, "first sighting must have no velocity (MOTION-3)");
        cur = r;
        have = true;
      }
      if (!have || !cur.has_vel) continue;
      auto a = sat::geo::azel(obs, sat::geo::sat_ecef(cur, t));
      const double e = sat::geo::separation(a, {p["az"], p["el"]});
      mx = std::max(mx, e);
    }
    printf("Extrapolation at %2d s fetch cadence, max error vs skyfield: %.3f deg\n", cadence, mx);
    CHECK(mx < (cadence == 15 ? 0.1 : 0.6), "extrapolation error %.3f at %d s", mx, cadence);
  }
  {  // 30 random objects: fixes at T0 and T0+90, predict T0+180 (90 s past the last fix)
    double mx = 0;
    for (JsonObject s : V["sky"].as<JsonArray>()) {
      sat::net::history.clear();
      sat::SatRec r;
      for (const char *k : {"a", "b"}) {
        r = sat::SatRec();
        r.id = s["id"];
        r.t_fix = V["T0"].as<double>() + (k[0] == 'a' ? 0 : 90);
        r.alt_km = s[k]["alt_km"];
        sat::net::unit_from_latlon(s[k]["lat"], s[k]["lon"], r.p);
        sat::net::apply_history(r);
      }
      auto a = sat::geo::azel(obs, sat::geo::sat_ecef(r, V["T0"].as<double>() + 180));
      if (s["c"]["el"].as<double>() > 0) mx = std::max<double>(mx, sat::geo::separation(a, {s["c"]["az"], s["c"]["el"]}));
    }
    printf("Random LEO/MEO/GEO, 90 s past last fix, max error: %.3f deg\n", mx);
    CHECK(mx < 1.0, "sky extrapolation %.3f", mx);
  }
  {  // MOTION-2 cap
    sat::SatRec r;
    r.has_vel = true;
    r.rate = 0.001f;
    r.axis[0] = 1; r.axis[1] = 0; r.axis[2] = 0;
    r.p[0] = 0; r.p[1] = 1; r.p[2] = 0;
    r.t_fix = 1000;
    auto a = sat::geo::sat_ecef(r, 1000 + 150), b = sat::geo::sat_ecef(r, 1000 + 5000);
    CHECK(sat::geo::norm(sat::geo::sub(a, b)) < 1e-3, "extrapolation not capped at 150 s");
  }
  {  // ISS sunlit vs pyephem eclipsed
    int n = 0, agree = 0;
    sat::SatRec r;
    for (JsonObject e : V["eclipse"].as<JsonArray>()) {
      sat::net::unit_from_latlon(e["lat"], e["lon"], r.p);
      r.alt_km = e["alt_km"];
      auto sm = sat::astro::compute(e["t"], LAT, LON, ALT);
      bool lit = sat::astro::sunlit(sat::geo::sat_ecef(r, e["t"]), sm.sun_ecef_dir);
      n++;
      agree += lit == e["sunlit"].as<bool>();
    }
    printf("ISS sunlit state agrees with pyephem: %d/%d\n", agree, n);
    CHECK(agree >= n - 2, "sunlit mismatch %d/%d", agree, n);
  }

  // ============ VER-2 pipeline on real LVGL
  lv_init();
  lv_display_t *disp = lv_display_create(480, 480);
  alignas(LV_DRAW_BUF_ALIGN) static uint16_t draw_buf[480 * 480 / 4];  // PERF-4: 25%
  lv_display_set_buffers(disp, draw_buf, nullptr, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  lv_obj_t *page1 = lv_obj_create(nullptr), *page2 = lv_obj_create(nullptr);
  for (auto *p : {page1, page2}) {
    lv_obj_set_style_bg_color(p, lv_color_hex(0x070B1A), 0);
    lv_obj_set_style_text_color(p, lv_color_hex(0xC9D3F2), 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  }
  const lv_font_t *f12 = &lv_font_montserrat_12, *f16 = &lv_font_montserrat_16;
  label(page1, 10, 4, "SKY TRACKER", f16, 0xFF8A1F);
  sat::Widgets w;
  w.countdown = label(page1, 10, 24, "", f12, 0x7E8BB3);
  w.counts = label(page1, 380, 34, "", f12, 0x7E8BB3);
  w.clock = label(page1, 400, 4, "--:--", f16, 0xC9D3F2);
  w.sky = lv_obj_create(page1);
  lv_obj_set_pos(w.sky, 56, 58);
  lv_obj_set_size(w.sky, 368, 368);
  lv_obj_set_style_radius(w.sky, 184, 0);
  lv_obj_set_style_bg_color(w.sky, lv_color_hex(sat::SKY_BG), 0);
  lv_obj_set_style_border_width(w.sky, 0, 0);
  lv_obj_set_style_pad_all(w.sky, 0, 0);
  w.foot_iss = label(page1, 10, 432, "", f16, 0xC9D3F2);
  w.foot_pass = label(page1, 10, 454, "", f16, 0xFFD54A);
  w.list = label(page2, 8, 6, "", f16, 0xC9D3F2);
  lv_obj_set_style_text_line_space(w.list, 2, 0);
  w.label_font = f12;
  w.coords = label(page1, 0, 0, "", f12, 0x7E8BB3);
  lv_obj_align(w.coords, LV_ALIGN_TOP_MID, 0, 7);
  w.alm_sun = label(page1, 10, 412, "", f12, 0xFFD54A);
  w.alm_moon = label(page1, 0, 0, "", f12, 0xDCE2EE);
  lv_obj_align(w.alm_moon, LV_ALIGN_TOP_RIGHT, -10, 396);
  w.alm_phase = label(page1, 0, 0, "", f12, 0x7E8BB3);
  lv_obj_align(w.alm_phase, LV_ALIGN_TOP_RIGHT, -10, 412);
  static lv_draw_buf_t moon_buf;
  static uint8_t moon_px[LV_DRAW_BUF_SIZE(32, 32, LV_COLOR_FORMAT_ARGB8888)];
  lv_draw_buf_init(&moon_buf, 32, 32, LV_COLOR_FORMAT_ARGB8888, 0, moon_px, sizeof(moon_px));
  lv_draw_buf_set_flag(&moon_buf, LV_IMAGE_FLAGS_MODIFIABLE);
  w.moon_img = lv_canvas_create(page1);
  lv_canvas_set_draw_buf(w.moon_img, &moon_buf);
  lv_obj_align(w.moon_img, LV_ALIGN_TOP_RIGHT, -10, 360);
  lv_screen_load(page1);

  const double T0 = V["T0"];
  JsonArray sky = V["sky"], sl = V["starlink"];
  // ISS truth near peak for the map
  JsonObject iss_a = tr[150], iss_b = tr[158];
  host_http_bodies["/above/61.5814/-149.4394/100/90/1/"] = above_json(sky, "a");
  host_http_bodies["/above/61.5814/-149.4394/100/90/52/"] = above_json(sl, "a");
  host_http_bodies["/positions/25544/"] = positions_json(iss_a, T0);
  {
    const int64_t s = (int64_t) T0 + 7200;
    char b[512];
    snprintf(b, sizeof(b),
             "{\"info\":{\"satid\":25544,\"satname\":\"SPACE STATION\",\"transactionscount\":1,\"passescount\":1},"
             "\"passes\":[{\"startAz\":250,\"startAzCompass\":\"WSW\",\"startEl\":10,\"startUTC\":%lld,\"maxAz\":200,"
             "\"maxAzCompass\":\"SSW\",\"maxEl\":15.5,\"maxUTC\":%lld,\"endAz\":140,\"endAzCompass\":\"SE\",\"endEl\":10,"
             "\"endUTC\":%lld,\"mag\":-1.2,\"duration\":420,\"startVisibility\":%lld}]}",
             (long long) s, (long long) s + 210, (long long) s + 420, (long long) s);
    host_http_bodies["/visualpasses/25544/"] = b;
  }

  sat::Config c;
  c.lat = LAT;
  c.lon = LON;
  c.alt_m = ALT;
  c.key = "SECRETKEY";
  sat_host_now = T0;
  sat::setup(c, w);
  CHECK(host_tasks_created == 1, "task not created");

  sat::tick();                   // schedules SAT + ISS
  CHECK(sat::ui.started, "scheduler did not start");
  run_jobs();
  sat_host_now += 2; sat::tick();  // drains SAT + ISS
  CHECK(host_last_url.find("SECRETKEY") != std::string::npos, "key missing from URL");
  int bound = 0;
  for (auto &m : sat::ui.sats) bound += m.id >= 0;
  printf("After first satellite fetch: %d satellite markers bound, total %d\n", bound, sat::live.sat_total);
  CHECK(bound == (int) sky.size(), "bound %d vs %u", bound, (unsigned) sky.size());
  // markers where skyfield says they are (no offset on first sighting)
  {
    double worst = 0;
    for (auto &m : sat::ui.sats) {
      if (m.id < 0) continue;
      for (JsonObject s : sky)
        if (s["id"] == m.id) {
          // truth position at T0 + 2 (tick time) is between a and b; use the device's own record
          auto a = sat::geo::azel(obs, sat::geo::sat_ecef(m.rec, T0));
          worst = std::max<double>(worst, sat::geo::separation(a, {s["a"]["az"], s["a"]["el"]}));
        }
    }
    CHECK(worst < 0.1, "marker vs truth %.3f", worst);
  }

  // run 3 minutes of ticks; at T0+90 serve 'b' in SHUFFLED order (MOTION-4), at +180 'c'
  std::map<int32_t, int> slot_before;
  for (size_t i = 0; i < sat::ui.sats.size(); i++)
    if (sat::ui.sats[i].id >= 0) slot_before[sat::ui.sats[i].id] = (int) i;
  std::mt19937 rng(3);
  JsonDocument shuffled;
  JsonArray sh = shuffled.to<JsonArray>();
  {
    std::vector<int> idx(sky.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = (int) i;
    std::shuffle(idx.begin(), idx.end(), rng);
    for (int i : idx) sh.add(sky[i]);
  }
  host_http_bodies["/above/61.5814/-149.4394/100/90/1/"] = above_json(sh, "b");
  host_http_bodies["/above/61.5814/-149.4394/100/90/52/"] = above_json(sl, "b");
  double max_tick_ms = 0;
  float max_jump = 0;
  std::map<int32_t, std::pair<int, int>> lastpos;
  for (int k = 0; k < 90; k++) {
    sat_host_now += 2;
    if (sat_host_now >= T0 + 180) host_http_bodies["/above/61.5814/-149.4394/100/90/1/"] = above_json(sky, "c");
    host_http_bodies["/positions/25544/"] = positions_json(tr[std::min<int>(150 + (k + 1), (int) tr.size() - 1)], sat_host_now);
    auto t0 = std::chrono::steady_clock::now();
    sat::tick();
    run_jobs();
    lv_timer_handler();
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    max_tick_ms = std::max(max_tick_ms, ms);
    for (auto &m : sat::ui.sats) {
      if (m.id < 0 || !m.shown) continue;
      auto it = lastpos.find(m.id);
      if (it != lastpos.end() && sat_host_now > T0 + 100) { float j = hypotf(m.px - it->second.first, m.py - it->second.second); if (j > 25 && getenv("DBG")) printf("leap id=%d t=+%.0f %.0f px off=%.1f,%.1f alt=%.0f\n", m.id, sat_host_now - T0, j, m.offx, m.offy, m.rec.alt_km); max_jump = std::max<float>(max_jump, j); }
      lastpos[m.id] = {m.px, m.py};
    }
  }
  int moved = 0;
  for (size_t i = 0; i < sat::ui.sats.size(); i++)
    if (sat::ui.sats[i].id >= 0 && slot_before.count(sat::ui.sats[i].id) && slot_before[sat::ui.sats[i].id] != (int) i) moved++;
  printf("After shuffled refetches: %d markers changed satellite (must be 0); largest per-tick move once every marker has velocity: %.1f px\n", moved, max_jump);
  CHECK(moved == 0, "MOTION-4 violated");
  CHECK(max_jump < 25, "a marker leapt %.1f px in one tick", max_jump);
  printf("Host tick+render worst case: %.2f ms\n", max_tick_ms);

  // z-order (UI-6)
  const int sun_i = lv_obj_get_index(sat::ui.sun), moon_i = lv_obj_get_index(sat::ui.moon);
  int min_marker = INT_MAX;
  for (auto &m : sat::ui.starlink) min_marker = std::min<int>(min_marker, lv_obj_get_index(m.dot));
  for (auto &m : sat::ui.sats) min_marker = std::min<int>(min_marker, lv_obj_get_index(m.dot));
  CHECK(sun_i < min_marker && moon_i < min_marker, "sun/moon above a marker");
  CHECK(lv_obj_get_index(sat::ui.iss.dot) == (int) lv_obj_get_child_count(w.sky) - 1, "ISS not on top");
  // UI-7 hidden below horizon
  for (auto &m : sat::ui.sats)
    if (m.id >= 0) {
      auto a = sat::geo::azel(obs, sat::geo::sat_ecef(m.rec, sat_host_now));
      CHECK((a.el >= 0) == !lv_obj_has_flag(m.dot, LV_OBJ_FLAG_HIDDEN), "visibility wrong for %d", m.id);
    }

  // offsets decay (MOTION-5)
  {
    float off = 0;
    for (auto &m : sat::ui.sats) off = std::max(off, hypotf(m.offx, m.offy));
    printf("Largest residual MOTION-5 offset: %.2f px\n", off);
  }


  if (getenv("BENCH")) {
    lv_screen_load(page1);
    lv_refr_now(disp);
    // replay 20 real motion ticks (markers extrapolating) and measure what gets flushed
    long px_total = 0; double ms_total = 0; long worst = 0;
    for (int k = 0; k < 20; k++) {
      sat_host_now += 2;
      sat::tick();
      flushed_px = 0;
      auto a = std::chrono::steady_clock::now();
      lv_refr_now(disp);
      ms_total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
      px_total += flushed_px; worst = std::max(worst, flushed_px);
    }
    int moving = 0; for (auto &m : sat::ui.sats) moving += m.id >= 0; for (auto &m : sat::ui.starlink) moving += m.id >= 0;
    printf("BENCH inv_buf=%d markers=%d  avg flushed %6ld px/tick (worst %6ld, full screen 230400)  avg %.2f ms\n",
           (int) LV_INV_BUF_SIZE, moving, px_total / 20, worst, ms_total / 20);
    return 0;
  }
  lv_refr_now(disp);
  save_ppm(HOST_DIR "/page1.ppm");
  lv_screen_load(page2);
  lv_refr_now(disp);
  save_ppm(HOST_DIR "/page2.ppm");
  printf("list page:\n%s\n", lv_label_get_text(w.list));
  printf("footer: %s | %s\n", lv_label_get_text(w.foot_iss), lv_label_get_text(w.foot_pass));
  printf("countdown: %s\n", lv_label_get_text(w.countdown));
  {
    int lines = 1;
    for (const char *p = lv_label_get_text(w.list); *p; p++) lines += *p == '\n';
    CHECK(lines <= 20, "list has %d lines", lines);
  }

  {  // DATA-3 / UI-4: Starlink tracked whole-sky, drawn in the cone, always with velocity
    const double T0s = V["T0"];
    sat::net::history.clear();
    sat_host_now = T0s;
    host_http_bodies["/above/61.5814/-149.4394/100/90/52/"] = above_json(sl, "a");
    sat::net::run_job(sat::JOB_STARLINK);
    CHECK(host_last_url.find("/100/90/52/") != std::string::npos, "starlink not fetched whole-sky");
    sat_host_now = T0s + 90;
    host_http_bodies["/above/61.5814/-149.4394/100/90/52/"] = above_json(sl, "b");
    sat::net::run_job(sat::JOB_STARLINK);
    sat::tick();  // drains and restyles
    const float min_el = sat::starlink_min_el();
    int bound = 0, no_vel = 0, entered = 0, wrong_vis = 0;
    for (auto &m : sat::ui.starlink) {
      if (m.id < 0) continue;
      bound++;
      if (!m.rec.has_vel) no_vel++;
      for (JsonObject o : sl)
        if (o["id"] == m.id && o["a"]["el"].as<float>() < min_el && o["b"]["el"].as<float>() >= min_el) entered++;
      const float el = sat::geo::azel(sat::ui.obs, sat::geo::sat_ecef(m.rec, sat_host_now)).el;
      if ((el >= min_el) != m.shown) wrong_vis++;
    }
    printf("Starlink: %d bound, %d entered the cone between fetches, %d without velocity, %d in cone\n", bound, entered, no_vel,
           sat::ui.starlink_in_cone);
    CHECK(bound > 0 && no_vel == 0, "%d of %d Starlink markers have no velocity", no_vel, bound);
    CHECK(wrong_vis == 0, "%d Starlink markers shown outside / hidden inside the cone", wrong_vis);
    // they move between fetches: 20 s later every bound marker has shifted
    std::map<int32_t, std::pair<int, int>> pos;
    for (auto &m : sat::ui.starlink) if (m.id >= 0) pos[m.id] = {m.px, m.py};
    int still = 0;
    for (int k = 0; k < 10; k++) { sat_host_now += 2; sat::tick(); }
    for (auto &m : sat::ui.starlink)
      if (m.id >= 0 && m.shown && pos.count(m.id) && pos[m.id] == std::make_pair(m.px, m.py)) still++;
    CHECK(still == 0, "%d Starlink markers did not move in 20 s", still);
    // accuracy against skyfield: position predicted 90 s past the second fix vs truth 'c'
    double worst = 0;
    for (auto &m : sat::ui.starlink) {
      if (m.id < 0) continue;
      for (JsonObject o : sl)
        if (o["id"] == m.id && o["c"]["el"].as<double>() > 0)
          worst = std::max<double>(worst, sat::geo::separation(sat::geo::azel(obs, sat::geo::sat_ecef(m.rec, T0s + 180)), {o["c"]["az"], o["c"]["el"]}));
    }
    printf("Starlink extrapolated 90 s past last fix, max error vs skyfield: %.2f deg\n", worst);
    CHECK(worst < 1.0, "starlink extrapolation %.2f", worst);
  }
  // DATA-8 oversize body and N2YO error body
  {
    int before = host_warnings;
    std::string big(120 * 1024, ' ');
    big[0] = '{';
    host_http_bodies["/above/61.5814/-149.4394/100/90/1/"] = big;
    auto n0 = sat::live.sats.size();
    sat::net::run_job(sat::JOB_ABOVE);
    CHECK(host_warnings == before + 1, "oversize not warned");
    CHECK(!(sat::net::pending.fresh & sat::L_SAT) && sat::live.sats.size() == n0, "oversize was parsed");
    host_http_bodies["/above/61.5814/-149.4394/100/90/1/"] = "{\"error\":\"Invalid API key\"}";
    sat::net::run_job(sat::JOB_ABOVE);
    CHECK(host_warnings == before + 2, "error body not warned");
  }
  // BUILD-6 OTA pause
  {
    sat::ota_begin();
    const size_t q = sat::net::queue->q.size();
    sat_host_now += 200;
    sat::tick();
    CHECK(sat::net::queue->q.size() == 0 && q == 0, "jobs queued during OTA");
    sat::ota_error();
  }
  // starlink disabled
  {
    sat::net::cfg.starlink_radius = 0;
    host_last_url.clear();
    sat::net::run_job(sat::JOB_STARLINK);
    CHECK(host_last_url.empty(), "starlink fetched while disabled");
  }
  {  // moon phase render: one waxing and one waning-ish case with the Moon up
    lv_screen_load(page1);
    int k = 0;
    for (JsonObject v : V["sunmoon"].as<JsonArray>()) {
      double ph = v["moon_phase"], me = v["moon_el"];
      if (me > 20 && ((k == 0 && ph > 0.2 && ph < 0.45) || (k == 1 && ph > 0.6 && ph < 0.85))) {
        sat_host_now = v["t"];
        sat::draw_sun_moon(sat_host_now);
        lv_refr_now(disp);
        char p[64]; snprintf(p, sizeof(p), HOST_DIR "/moon%d.ppm", k);
        save_ppm(p);
        printf("moon%d: illum %.2f el %.1f az %.0f, sun el %.1f az %.0f; moon px %d,%d shadow off %d,%d\n", k, sat::ui.sm.moon_illum,
               sat::ui.sm.moon.el, sat::ui.sm.moon.az, sat::ui.sm.sun.el, sat::ui.sm.sun.az, sat::ui.moon_x, sat::ui.moon_y, sat::ui.shadow_x, sat::ui.shadow_y);
        if (++k == 2) break;
      }
    }
  }
  {  // UI-15 heading
    sat::set_heading(90);
    float x, y;
    sat::project({90, 0}, x, y);
    CHECK(fabsf(x - sat::ui.cx) < 1 && fabsf(y - (sat::ui.cy - sat::ui.radius)) < 1, "east not at top with heading 90 (%.1f,%.1f)", x, y);
    sat::project({0, 0}, x, y);
    CHECK(x < sat::ui.cx - sat::ui.radius + 1, "north not at left with heading 90");
    lv_obj_update_layout(w.sky);
    lv_area_t na; lv_obj_get_coords(sat::ui.compass_tags[0], &na);
    lv_area_t sa; lv_obj_get_coords(w.sky, &sa);
    CHECK(na.x2 - sa.x1 < sat::ui.cx / 2, "N tag not on the left");
    // every shown marker must sit where the rotated projection says
    int bad = 0;
    for (auto &m : sat::ui.sats) if (m.id >= 0 && m.shown) {
      auto a = sat::geo::azel(obs, sat::geo::sat_ecef(m.rec, sat_host_now));
      sat::project(a, x, y);
      if (abs((int) lroundf(x) - SAT_PX_HALF - m.px) > 1 || abs((int) lroundf(y) - SAT_PX_HALF - m.py) > 1) bad++;
    }
    CHECK(bad == 0, "%d markers not rotated", bad);
    sat::set_heading(135);
    lv_screen_load(page1); lv_refr_now(disp); save_ppm(HOST_DIR "/heading135.ppm");
    sat::set_heading(400);
    CHECK(fabsf(sat::heading() - 40) < 0.01f, "heading not wrapped");
    sat::set_heading(0);
    printf("Heading rotation checks done\n");
  }
  {  // UI-17 time format
    char b[16];
    sat::set_24h(false);
    sat::local_hm(1790000700 - (1790000700 % 86400) + 13 * 3600 + 5 * 60, b, sizeof(b));
    CHECK(strcmp(b, "1:05 PM") == 0, "12h afternoon: %s", b);
    sat::local_hm(1790000700 - (1790000700 % 86400) + 7 * 60, b, sizeof(b));
    CHECK(strcmp(b, "12:07 AM") == 0, "12h midnight: %s", b);
    sat::set_24h(true);
    sat::local_hm(1790000700 - (1790000700 % 86400) + 13 * 3600 + 5 * 60, b, sizeof(b));
    CHECK(strcmp(b, "13:05") == 0, "24h: %s", b);
  }
  lv_obj_t *page3 = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(page3, lv_color_hex(0x070B1A), 0);
  {  // UI-16 settings page, laid out as in the YAML
    auto ta = [&](int x, int y, const char *acc, int maxlen) {
      lv_obj_t *t = lv_textarea_create(page3);
      lv_obj_set_pos(t, x, y); lv_obj_set_size(t, 150, 42);
      lv_textarea_set_one_line(t, true); lv_textarea_set_accepted_chars(t, acc); lv_textarea_set_max_length(t, maxlen);
      lv_obj_set_style_bg_color(t, lv_color_hex(0x0E1836), 0); lv_obj_set_style_border_width(t, 2, 0);
      lv_obj_set_style_text_font(t, f16, 0); lv_obj_set_style_text_color(t, lv_color_hex(0xC9D3F2), 0);
      return t;
    };
    label(page3, 16, 16, "SETTINGS", f16, 0xFF8A1F);
    lv_obj_t *bc = lv_button_create(page3); lv_obj_set_pos(bc, 240, 6); lv_obj_set_size(bc, 110, 40); lv_obj_center(label(bc, 0, 0, "Cancel", f16, 0xFFFFFF));
    lv_obj_t *bs = lv_button_create(page3); lv_obj_set_pos(bs, 360, 6); lv_obj_set_size(bs, 110, 40); lv_obj_center(label(bs, 0, 0, "Save", f16, 0xFFFFFF));
    label(page3, 16, 64, "Heading", f16, 0xC9D3F2);  lv_obj_t *th = ta(190, 54, "0123456789", 3);  label(page3, 350, 66, "deg (0 = N up)", f12, 0x7E8BB3);
    label(page3, 16, 110, "Latitude", f16, 0xC9D3F2); lv_obj_t *tla = ta(190, 100, "-.0123456789", 8); label(page3, 350, 112, "N +  S -", f12, 0x7E8BB3);
    label(page3, 16, 156, "Longitude", f16, 0xC9D3F2); lv_obj_t *tlo = ta(190, 146, "-.0123456789", 9); label(page3, 350, 158, "E +  W -", f12, 0x7E8BB3);
    label(page3, 16, 200, "Time format", f16, 0xC9D3F2); label(page3, 190, 200, "12h", f16, 0xC9D3F2);
    lv_obj_t *sw = lv_switch_create(page3); lv_obj_set_pos(sw, 232, 194); lv_obj_set_size(sw, 60, 30);
    label(page3, 302, 200, "24h", f16, 0xC9D3F2);
    label(page3, 16, 242, "Brightness", f16, 0xC9D3F2);
    lv_obj_t *sl = lv_slider_create(page3); lv_obj_set_pos(sl, 200, 244); lv_obj_set_size(sl, 250, 14); lv_slider_set_range(sl, 1, 100); lv_slider_set_value(sl, 70, LV_ANIM_OFF);
    lv_obj_t *st = label(page3, 16, 272, "", f12, 0xFF5A5A); lv_obj_set_width(st, 448);
    lv_obj_t *kb = lv_keyboard_create(page3); lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER); lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0); lv_obj_set_size(kb, 480, 188);
    lv_keyboard_set_textarea(kb, tla);
    sat::settings_bind(th, tla, tlo, sw, st);
    sat::settings_fill();
    CHECK(strcmp(lv_textarea_get_text(tla), "61.5814") == 0, "lat fill %s", lv_textarea_get_text(tla));
    CHECK(strcmp(lv_textarea_get_text(tlo), "-149.4394") == 0, "lon fill %s", lv_textarea_get_text(tlo));
    CHECK(lv_obj_has_state(sw, LV_STATE_CHECKED), "24h switch not filled");
    struct { const char *h, *la, *lo; bool ok; } cases[] = {
        {"270", "-33.8688", "151.2093", true}, {"360", "10", "10", false}, {"12", "95", "10", false},
        {"12", "10", "-180.5", false}, {"", "10", "10", false}, {"12", "1.2.3", "10", false}, {"0", "-90", "180", true}};
    for (auto &c : cases) {
      lv_textarea_set_text(th, c.h); lv_textarea_set_text(tla, c.la); lv_textarea_set_text(tlo, c.lo);
      bool ok = sat::settings_parse();
      CHECK(ok == c.ok, "parse %s/%s/%s gave %d", c.h, c.la, c.lo, ok);
      CHECK(ok == (lv_label_get_text(st)[0] == 0), "status text wrong for %s/%s/%s", c.h, c.la, c.lo);
    }
    lv_textarea_set_text(th, "270"); lv_textarea_set_text(tla, "-33.86884"); lv_textarea_set_text(tlo, "151.2093");
    lv_obj_remove_state(sw, LV_STATE_CHECKED);
    CHECK(sat::settings_parse() && sat::form_values.heading == 270 && fabs(sat::form_values.lat + 33.8688) < 1e-9 && !sat::form_values.h24, "values wrong");
    lv_textarea_set_text(tla, "61.5814"); lv_textarea_set_text(tlo, "-149.4394"); lv_textarea_set_text(th, "0");
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_screen_load(page3); lv_refr_now(disp); save_ppm(HOST_DIR "/settings.ppm");
    lv_screen_load(page1);
  }
  {  // UI-16 DMS entry, laid out as in the YAML
    lv_obj_t *p = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x070B1A), 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    auto ta = [&](lv_obj_t *par, int x, int y, int w, const char *acc, int maxlen) {
      lv_obj_t *t = lv_textarea_create(par);
      lv_obj_set_pos(t, x, y); lv_obj_set_size(t, w, 36);
      lv_textarea_set_one_line(t, true); lv_textarea_set_accepted_chars(t, acc); lv_textarea_set_max_length(t, maxlen);
      lv_obj_set_style_bg_color(t, lv_color_hex(0x0E1836), 0); lv_obj_set_style_border_width(t, 2, 0);
      lv_obj_set_style_border_color(t, lv_color_hex(0x3A4E86), 0); lv_obj_set_style_pad_all(t, 6, 0);
      lv_obj_set_style_text_font(t, f16, 0); lv_obj_set_style_text_color(t, lv_color_hex(0xC9D3F2), 0);
      return t;
    };
    auto box = [&](int y) { lv_obj_t *b = lv_obj_create(p); lv_obj_remove_style_all(b); lv_obj_set_pos(b, 190, y); lv_obj_set_size(b, 284, 36);
                            lv_obj_remove_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE)); return b; };
    auto btn = [&](lv_obj_t *par, int x, int y, int w, int h, uint32_t c, const char *t) {
      lv_obj_t *b = lv_button_create(par); lv_obj_set_pos(b, x, y); lv_obj_set_size(b, w, h); lv_obj_set_style_bg_color(b, lv_color_hex(c), 0);
      lv_obj_set_style_shadow_width(b, 0, 0); lv_obj_t *l = label(b, 0, 0, t, f16, 0xFFFFFF); lv_obj_center(l); return l; };
    label(p, 16, 16, "SETTINGS", f16, 0xFF8A1F);
    btn(p, 240, 6, 110, 38, 0x1A2547, "Cancel"); btn(p, 360, 6, 110, 38, 0x2D5BD0, "Save");
    label(p, 16, 60, "Heading", f16, 0xC9D3F2); lv_obj_t *th = ta(p, 190, 52, 90, "0123456789", 3); label(p, 290, 62, "deg (0 = N up)", f12, 0x7E8BB3);
    label(p, 16, 100, "Coordinates", f16, 0xC9D3F2); label(p, 190, 100, "Dec", f16, 0xC9D3F2);
    lv_obj_t *swd = lv_switch_create(p); lv_obj_set_pos(swd, 232, 94); lv_obj_set_size(swd, 56, 28); label(p, 298, 100, "DMS", f16, 0xC9D3F2);
    lv_obj_t *bd[2], *bm[2], *tdd[2], *d[2], *m[2], *sc[2], *hl[2];
    const char *names[2] = {"Latitude", "Longitude"}, *hints[2] = {"N +  S -", "E +  W -"}, *hem[2] = {"N", "E"};
    for (int i = 0; i < 2; i++) {
      int y = i == 0 ? 132 : 174;
      label(p, 16, y + 8, names[i], f16, 0xC9D3F2);
      bd[i] = box(y); tdd[i] = ta(bd[i], 0, 0, 150, "-.0123456789", i ? 9 : 8); label(bd[i], 160, 10, hints[i], f12, 0x7E8BB3);
      bm[i] = box(y); d[i] = ta(bm[i], 0, 0, 52, "0123456789", 3); label(bm[i], 54, 4, "\xC2\xB0", f16, 0xC9D3F2);
      m[i] = ta(bm[i], 66, 0, 62, "0123456789.", 6); label(bm[i], 130, 4, "'", f16, 0xC9D3F2);
      sc[i] = ta(bm[i], 140, 0, 62, "0123456789.", 4); label(bm[i], 204, 4, "\"", f16, 0xC9D3F2);
      hl[i] = btn(bm[i], 222, 0, 56, 36, 0x1A2547, hem[i]);
    }
    label(p, 16, 222, "Time format", f16, 0xC9D3F2); label(p, 190, 222, "12h", f16, 0xC9D3F2);
    lv_obj_t *sw = lv_switch_create(p); lv_obj_set_pos(sw, 232, 216); lv_obj_set_size(sw, 56, 28); label(p, 298, 222, "24h", f16, 0xC9D3F2);
    label(p, 16, 258, "Brightness", f16, 0xC9D3F2);
    lv_obj_t *sl = lv_slider_create(p); lv_obj_set_pos(sl, 200, 262); lv_obj_set_size(sl, 250, 14); lv_slider_set_range(sl, 1, 100); lv_slider_set_value(sl, 70, LV_ANIM_OFF);
    lv_obj_t *st = label(p, 16, 292, "", f12, 0xFF5A5A); lv_obj_set_width(st, 448);
    lv_obj_t *kb = lv_keyboard_create(p); lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER); lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0); lv_obj_set_size(kb, 480, 164);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x0E1836), 0); lv_obj_set_style_bg_color(kb, lv_color_hex(0x1A2547), LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(0xC9D3F2), LV_PART_ITEMS); lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_keyboard_set_textarea(kb, m[0]);
    sat::settings_bind(th, tdd[0], tdd[1], sw, st);
    sat::settings_bind_dms(swd, bd[0], bm[0], d[0], m[0], sc[0], hl[0], bd[1], bm[1], d[1], m[1], sc[1], hl[1]);
    sat::form.dms = true;
    sat::settings_fill();   // current observer 61.5814, -149.4394
    CHECK(!strcmp(lv_textarea_get_text(d[0]), "61") && !strcmp(lv_textarea_get_text(m[0]), "34") && !strcmp(lv_textarea_get_text(sc[0]), "53.0"),
          "lat DMS fill %s %s %s", lv_textarea_get_text(d[0]), lv_textarea_get_text(m[0]), lv_textarea_get_text(sc[0]));
    CHECK(!strcmp(lv_textarea_get_text(d[1]), "149") && !strcmp(lv_textarea_get_text(m[1]), "26") && !strcmp(lv_textarea_get_text(sc[1]), "21.8"),
          "lon DMS fill %s %s %s", lv_textarea_get_text(d[1]), lv_textarea_get_text(m[1]), lv_textarea_get_text(sc[1]));
    CHECK(!strcmp(lv_label_get_text(hl[0]), "N") && !strcmp(lv_label_get_text(hl[1]), "W"), "hemispheres %s %s", lv_label_get_text(hl[0]), lv_label_get_text(hl[1]));
    CHECK(lv_obj_has_flag(bd[0], LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(bm[0], LV_OBJ_FLAG_HIDDEN), "DMS rows not shown");
    lv_screen_load(p); lv_refr_now(disp); save_ppm(HOST_DIR "/settings_dms.ppm");
    // round trip: fill then parse gives the same 4-decimal values
    CHECK(sat::settings_parse() && fabs(sat::form_values.lat - 61.5814) < 1e-9 && fabs(sat::form_values.lon + 149.4394) < 1e-9,
          "DMS round trip %.6f %.6f", sat::form_values.lat, sat::form_values.lon);
    // degrees + decimal minutes, southern hemisphere
    lv_textarea_set_text(d[0], "33"); lv_textarea_set_text(m[0], "52.128"); lv_textarea_set_text(sc[0], "");
    sat::settings_toggle_hemi(0);
    lv_textarea_set_text(d[1], "151"); lv_textarea_set_text(m[1], "12"); lv_textarea_set_text(sc[1], "33.5");
    sat::settings_toggle_hemi(1);
    CHECK(sat::settings_parse() && fabs(sat::form_values.lat + 33.8688) < 1e-9 && fabs(sat::form_values.lon - 151.2093) < 1e-9,
          "DM/DMS parse %.6f %.6f (%s)", sat::form_values.lat, sat::form_values.lon, lv_label_get_text(st));
    struct { const char *d, *m, *s; bool ok; } bad[] = {{"91", "0", "0", false}, {"90", "0", "1", false}, {"45", "60", "", false},
                                                       {"45", "30.5", "10", false}, {"", "1", "1", false}, {"45", "", "", true}, {"90", "0", "0", true}};
    for (auto &c : bad) {
      lv_textarea_set_text(d[0], c.d); lv_textarea_set_text(m[0], c.m); lv_textarea_set_text(sc[0], c.s);
      bool ok = sat::settings_parse();
      CHECK(ok == c.ok, "DMS lat %s %s %s gave %d (%s)", c.d, c.m, c.s, ok, lv_label_get_text(st));
    }
    // DD mode still works through the same Save path
    sat::settings_set_dms(false);
    CHECK(!lv_obj_has_flag(bd[0], LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(bm[0], LV_OBJ_FLAG_HIDDEN), "DD rows not shown");
    lv_textarea_set_text(tdd[0], "61.5814"); lv_textarea_set_text(tdd[1], "-149.4394"); lv_textarea_set_text(th, "0");
    CHECK(sat::settings_parse(), "DD parse after toggle");
    // edge: carry 59.96" -> next minute
    int dd, mm; double ss; sat::to_dms(10.0 + 59.0 / 60 + 59.96 / 3600, dd, mm, ss);
    CHECK(dd == 11 && mm == 0 && ss == 0, "carry %d %d %.1f", dd, mm, ss);
    lv_screen_load(page1);
    printf("DMS checks done\n");
  }
  {  // UI-16 moving the observer
    const double now = sat_host_now;
    sat::set_observer(-33.8688, 151.2093);
    CHECK(fabs(sat::config().lat + 33.8688) < 1e-9 && fabs(sat::config().lon - 151.2093) < 1e-9, "cfg not updated");
    CHECK(sat::ui.next_sat <= now + 0.01 && sat::ui.next_iss <= now + 0.01 && sat::ui.next_pass <= now + 0.01, "no immediate refetch");
    CHECK(sat::live.passes.empty(), "old passes kept");
    int bad = 0;
    for (auto &m : sat::ui.sats) if (m.id >= 0 && m.shown) {
      float x, y; sat::project(sat::geo::azel(sat::ui.obs, sat::geo::sat_ecef(m.rec, now)), x, y);
      if (abs((int) lroundf(x) - SAT_PX_HALF - m.px) > 1 || abs((int) lroundf(y) - SAT_PX_HALF - m.py) > 1) bad++;
    }
    CHECK(bad == 0, "%d markers not re-projected", bad);
    sat::net::run_job(sat::JOB_ISS);
    CHECK(host_last_url.find("/-33.8688/151.2093/") != std::string::npos, "task used old location: %s", host_last_url.substr(0, 90).c_str());
    sat::set_observer(61.5814, -149.4394);
    printf("Settings, time format and observer checks done\n");
  }
  {  // UI-20 almanac vs ephem (vectors.json "alm", UTC)
    sat::ui.alm_next = 0;
    sat::draw_hud(sat_host_now);
    printf("Almanac at %.0f: '%s' | '%s' | '%s'\n", sat_host_now, lv_label_get_text(w.alm_sun), lv_label_get_text(w.alm_moon), lv_label_get_text(w.alm_phase));
    printf("ALM sun %.0f %d moon %.0f %d\n", sat::ui.sun_ev.t, sat::ui.sun_ev.rise, sat::ui.moon_ev.t, sat::ui.moon_ev.rise);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; i++) { sat::astro::Event a, b; sat::astro::next_events(sat_host_now + i * 3600, 61.5814, -149.4394, 100, a, b); }
    printf("next_events: %.2f ms each (host)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 20);
    lv_refr_now(disp); save_ppm(HOST_DIR "/page1_alm.ppm");
    // phase strip: 8 phases side by side on a blank screen
    lv_obj_t *strip = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, 480, 48);
    lv_obj_set_style_bg_color(strip, lv_color_hex(0x070B1A), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_t *old_img = w.moon_img;
    const float ks[8] = {0.01f, 0.2f, 0.5f, 0.8f, 1.0f, 0.8f, 0.5f, 0.2f};
    for (int i = 0; i < 8; i++) {
      static lv_draw_buf_t bufs[8];
      static uint8_t px[8][LV_DRAW_BUF_SIZE(32, 32, LV_COLOR_FORMAT_ARGB8888)];
      lv_draw_buf_init(&bufs[i], 32, 32, LV_COLOR_FORMAT_ARGB8888, 0, px[i], sizeof(px[i]));
      lv_draw_buf_set_flag(&bufs[i], LV_IMAGE_FLAGS_MODIFIABLE);
      lv_obj_t *cv = lv_canvas_create(strip);
      lv_canvas_set_draw_buf(cv, &bufs[i]);
      lv_obj_set_pos(cv, 8 + i * 40, 8);
      sat::ui.w.moon_img = cv;
      sat::ui.icon_illum = -1;
      sat::draw_moon_icon(ks[i], i < 4, false);
    }
    sat::ui.w.moon_img = old_img;
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp); save_ppm(HOST_DIR "/moon_strip.ppm");
    lv_obj_delete(strip); lv_obj_invalidate(lv_screen_active());
  }
  {  // UI-18 owner country codes, UI-19 overlapping tags
    std::vector<int32_t> ids;
    for (JsonObject o : sky) ids.push_back(o["id"].as<int32_t>());
    CHECK(ids.size() >= 4, "need 4 sats");
    char b[512];
    snprintf(b, sizeof(b), "[{\"NORAD_CAT_ID\":%d,\"OWNER\":\"US\",\"OBJECT_NAME\":\"X\"},{\"NORAD_CAT_ID\":%d,\"OWNER\":\"PRC\"},"
             "{\"NORAD_CAT_ID\":%d,\"OWNER\":\"ESA\"}]", ids[0], ids[1], ids[2]);
    host_http_bodies["GROUP=visual"] = b;
    snprintf(b, sizeof(b), "[{\"NORAD_CAT_ID\":%d,\"OWNER\":\"CIS\"}]", ids[3]);
    char key[32]; snprintf(key, sizeof(key), "CATNR=%d&", ids[3]);
    host_http_bodies[key] = b;
    host_http_bodies["/above/61.5814/-149.4394/100/90/1/"] = above_json(sky, "c");
    sat::net::owner_bulk_next = 0; sat::net::owner_backoff = 0;
    sat::net::run_job(sat::JOB_ABOVE);   // bulk before the swap, CATNR lookups after
    CHECK(sat::net::owners.count(ids[3]) && strcmp(sat::net::owners[ids[3]].data(), "RUS") == 0, "CATNR lookup");
    sat::net::run_job(sat::JOB_ABOVE);   // the looked-up code rides on this fetch
    sat_host_now += 2; sat::tick();
    const char *want[] = {"USA", "CHN", "ESA", "RUS"};
    for (int i = 0; i < 4; i++) {
      const sat::Marker &m = sat::ui.sats[sat::ui.sat_slot[ids[i]]];
      const char *txt = lv_label_get_text(m.label);
      CHECK(strstr(txt, want[i]) != nullptr, "label '%s' lacks %s", txt, want[i]);
      if (i == 0) printf("Label with owner: '%s'\n", txt);
    }
    // UI-19: two visible dots on top of each other -> only the lower keeps its tag
    std::vector<sat::Marker *> vis;
    for (auto &m : sat::ui.sats) if (m.id >= 0 && m.up) vis.push_back(&m);
    CHECK(vis.size() >= 2, "need 2 visible");
    sat::Marker &a = *vis[0], &bb = *vis[1];
    const int adx = a.dx, ady = a.dy;
    for (auto &m : sat::ui.sats) if (m.id >= 0 && m.up && &m != &a && &m != &bb) { m.dx = 5000 + m.id % 1000 * 20; m.dy = 5000; }  // clear the field
    sat::declutter_labels();
    CHECK(!lv_obj_has_flag(a.label, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(bb.label, LV_OBJ_FLAG_HIDDEN), "apart: tag hidden");
    a.dx = bb.dx + 3; a.dy = bb.dy + 2;
    sat::Marker &hi = a.el > bb.el ? a : bb, &lo = a.el > bb.el ? bb : a;
    sat::declutter_labels();
    CHECK(lv_obj_has_flag(hi.label, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(lo.label, LV_OBJ_FLAG_HIDDEN),
          "overlap: higher (%.1f) tag shown or lower (%.1f) hidden", hi.el, lo.el);
    // dots apart but tags on top of each other: the lower keeps its tag
    a.dx = bb.dx; a.dy = bb.dy + 8;
    sat::declutter_labels();
    CHECK(lv_obj_has_flag(hi.label, LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_flag(lo.label, LV_OBJ_FLAG_HIDDEN), "tag overlap");
    a.dx = adx; a.dy = ady;
    sat_host_now += 2; sat::tick();  // real positions again
    printf("Owner codes and overlap checks done\n");
  }
  {  // UI-21 star map: positions vs ephem, layer switches, preview
    struct Ref { const char *n; float ra, dec, az, el; } refs[] = {
        {"Polaris", 37.95f, 89.26f, 358.71f, 61.47f}, {"Vega", 279.23f, 38.78f, 37.06f, 17.51f},
        {"Sirius", 101.29f, -16.72f, 223.10f, 3.82f}, {"Betelgeuse", 88.79f, 7.41f, 244.35f, 21.40f}};
    const double tt = 1793294009;
    const float lst = (float) sat::astro::wrap360(sat::astro::gmst_deg(sat::astro::jd(tt)) - 149.4394);
    const float sl = sinf(61.5814f * sat::geo::DEG), cl = cosf(61.5814f * sat::geo::DEG);
    for (auto &r : refs) {
      int best = -1; float bd = 1e9;
      for (int i = 0; i < sat::sky::N_STARS; i++) {
        const float dr = (sat::star_ra(sat::sky::STARS[i]) - r.ra) * cosf(r.dec * sat::geo::DEG), dd = sat::star_dec(sat::sky::STARS[i]) - r.dec;
        if (dr * dr + dd * dd < bd) { bd = dr * dr + dd * dd; best = i; }
      }
      CHECK(bd < 0.01f, "%s not in catalogue", r.n);
      auto v = sat::star_enu(sat::star_ra(sat::sky::STARS[best]), sat::star_dec(sat::sky::STARS[best]), lst, sl, cl);
      sat::geo::AzEl a{(float) fmod(atan2f(v.e, v.n) / sat::geo::DEG + 360, 360), asinf(v.u) / sat::geo::DEG};
      const float sep = sat::geo::separation(a, {r.az, r.el});
      printf("%-10s az %6.2f el %5.2f  (ephem %6.2f %5.2f)  off %.2f°\n", r.n, a.az, a.el, r.az, r.el, sep);
      CHECK(sep < 0.6f, "%s off by %.2f°", r.n, sep);
    }
    // dusk rule: at T0 the Sun is up or in twilight here -> hidden with after-dusk on
    sat::set_stars(true, true);
    sat_host_now += 2; sat::tick();
    printf("Sun el %.1f, stars shown %d\n", sat::ui.sm.sun.el, sat::ui.stars_shown);
    CHECK(sat::ui.sm.sun.el > sat::DUSK_SUN_EL ? !sat::ui.stars_shown : sat::ui.stars_shown, "dusk rule");
    sat::set_stars(true, false);
    CHECK(sat::ui.stars_shown && !sat::ui.star_pts.empty() && !sat::ui.star_segs.empty(), "stars not shown");
    int nnames = 0; for (auto &nt : sat::ui.names) nnames += nt.shown;
    printf("Constellation names shown: %d of %zu\n", nnames, sat::ui.names.size());
    CHECK(nnames > 3, "too few names");
    auto t0 = std::chrono::steady_clock::now();
    sat::draw_starmap(sat_host_now);
    printf("draw_starmap: %.2f ms (host)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp); save_ppm(HOST_DIR "/page1_stars.ppm");
    // UI-22: satellites off -> no /above requests, markers freed, no countdown line
    sat::set_layers(false, true);
    int bound = 0; for (auto &m : sat::ui.sats) bound += m.id >= 0;
    CHECK(bound == 0, "%d sat markers still bound", bound);
    host_last_url.clear();
    for (int k = 0; k < 60; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
    CHECK(host_last_url.find("/above/61.5814/-149.4394/100/90/1/") == std::string::npos, "satellite fetch while off");
    const char *cd = lv_label_get_text(w.countdown);
    CHECK(strstr(cd, "Satellites") == nullptr && strstr(lv_label_get_text(w.counts), "SAT") == nullptr, "countdown '%s'", cd);
    sat::set_layers(false, false);
    sat_host_now += 2; sat::tick();
    CHECK(lv_label_get_text(w.countdown)[0] == 0, "countdown not empty: '%s'", lv_label_get_text(w.countdown));
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp); save_ppm(HOST_DIR "/page1_stars_only.ppm");
    sat::set_layers(true, true);
    CHECK(sat::ui.next_sat <= sat_host_now + 0.01, "no immediate refetch on");
    sat::set_stars(true, true);
    sat::draw_coords();
    printf("Coords header: '%s'\n", lv_label_get_text(w.coords));
    CHECK(strstr(lv_label_get_text(w.coords), "61.5814") != nullptr, "coords header");
    sat::form.dms = true; sat::draw_coords();
    printf("Coords header (DMS): '%s'\n", lv_label_get_text(w.coords));
    lv_obj_invalidate(lv_screen_active()); lv_refr_now(disp); save_ppm(HOST_DIR "/page1_coords.ppm");
    sat::form.dms = false; sat::draw_coords();
    printf("Star map and layer checks done\n");
  }
  {  // NET-8: no fetches while Wi-Fi is down; refresh on reconnect
    sat::host_net_up = false;
    host_last_url.clear();
    for (int k = 0; k < 60; k++) { sat_host_now += 2; sat::tick(); run_jobs(); }
    CHECK(host_last_url.empty(), "request while Wi-Fi down: %s", host_last_url.substr(0, 60).c_str());
    CHECK(!sat::net::link_up, "link_up still set");
    sat::host_net_up = true;
    sat_host_now += 2; sat::tick();
    CHECK(sat::net::link_up, "link not up after reconnect");
    run_jobs();
    CHECK(!host_last_url.empty(), "nothing fetched after reconnect");
    printf("Wi-Fi gating checks done\n");
  }
  printf("PSRAM allocations: %zu, warnings logged: %d\n", host_psram_allocs, host_warnings);
  printf(failures ? "\n%d FAILURES\n" : "\nALL CHECKS PASSED\n", failures);
  return failures ? 1 : 0;
}
