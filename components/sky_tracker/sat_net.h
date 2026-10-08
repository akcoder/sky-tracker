// sat_net.h — Sky Tracker data task (requirements rev 4, ARCH-1..9, DATA-1..9)
//
// Orbits come from CelesTrak element sets (OMM CSV), downloaded at most every 12 h,
// and positions are computed on the device: SGP4/SDP4 for the bright satellites and
// the ISS, a J2 secular model for Starlink (thousands of near-circular orbits). No
// per-position API calls, so no key and no rate limit.
//
// Everything in this file except enqueue(), drain(), net_pause(), net_set_*() and
// net_init() runs on the "sat_net" FreeRTOS task. It never touches LVGL, ESPHome
// components or sensors (ARCH-6); ESP_LOGx is the one exception.
//
// Flow:  loop --enqueue(Job)--> queue --> task: download / propagate -> staging
//        task --swap under mutex--> pending --drain() on the loop tick--> loop copies
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <functional>
#include <strings.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

#include "esphome/core/log.h"
#include <ArduinoJson.h>

#include "sky_math.h"
#include "sky_png.h"
#include "sky_jpg.h"
#include "sgp4.h"
#include "sky_comets.h"
#include "sky_photos.h"
#ifdef SAT_HOST_TEST
#include <chrono>
inline int64_t net_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
#else
#include "esp_timer.h"
inline int64_t net_us() { return esp_timer_get_time(); }
#endif

namespace sat {

// ------------------------------------------------------------------ shared
// Wall-clock UTC seconds. Safe from any task (FAIL-7: the only clock global).
#ifdef SAT_HOST_TEST
extern double sat_host_now;  // host test harness clock
inline double clock_now() { return sat_host_now; }
#else
inline double clock_now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (double) tv.tv_sec + tv.tv_usec * 1e-6;
}
#endif
inline bool clock_valid() { return clock_now() > 1.7e9; }

// ESPHome builds with CONFIG_SPIRAM_USE_CAPS_ALLOC, so plain malloc never reaches
// PSRAM. Element sets, record lists and marker pools use this allocator (ARCH-8).
template<class T> struct PsramAlloc {
  using value_type = T;
  PsramAlloc() = default;
  template<class U> PsramAlloc(const PsramAlloc<U> &) {}
  T *allocate(size_t n) {
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr)
      p = malloc(n * sizeof(T));
    if (p == nullptr)
      abort();
    return static_cast<T *>(p);
  }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
  template<class U> bool operator==(const PsramAlloc<U> &) const { return true; }
  template<class U> bool operator!=(const PsramAlloc<U> &) const { return false; }
};
template<class T> using pvector = std::vector<T, PsramAlloc<T>>;
// FAIL-12: PsramAlloc aborts when PSRAM has no block big enough, and a vector that grows needs
// its new block while it still holds the old one. The four element lists (satellites, GNSS,
// GEO, Starlink) are therefore reserved at their caps when the data task starts, while PSRAM
// is in one piece, and then refilled in place: the cache is read straight into them and a
// download clears and refills the list it replaces (the data task alone owns them), so they
// never allocate again. room_for() is the guard: a list without room is grown only if PSRAM
// has a free block that size (plus 32 KB); otherwise the caller skips, keeping what it has.
// (4.6.21 aborted building the GEO list after Starlink: 2.5 MB free, none of it in one piece.)
template<class T, class A> inline bool room_for(std::vector<T, A> &v, size_t n, const char *what, bool quiet = false);
// FAIL-12b: room for one more row in a list being filled in place: grows by a quarter (from at
// least `start`) when PSRAM has the block, else no (the row is dropped)
template<class T, class A> inline bool one_more(std::vector<T, A> &v, size_t cap, size_t start, const char *what) {
  if (v.size() >= cap)
    return false;
  if (v.size() < v.capacity())
    return true;
  return room_for(v, std::min(cap, std::max(start, v.capacity() + v.capacity() / 4 + 16)), what, true);
}
// FAIL-12b: append, growing the vector only by a margin and only when PSRAM has the block
// (room_for); without it the item is dropped. Buffers grow to the most they have needed and stay.
template<class T, class A> inline bool push_room(std::vector<T, A> &v, const T &x, size_t cap, const char *what) {
  if (v.size() >= cap)
    return false;
  if (v.size() == v.capacity() && !room_for(v, std::min(cap, v.capacity() + v.capacity() / 4 + 16), what, true))
    return false;
  v.push_back(x);
  return true;
}
template<class T, class A> inline bool room_for(std::vector<T, A> &v, size_t n, const char *what, bool quiet) {
  if (v.capacity() >= n)
    return true;
  const size_t need = n * sizeof(T), big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (big < need + 32 * 1024) {
    if (!quiet)
      ESP_LOGW("sat_net", "%s: PSRAM has no free %u KB block (largest %u KB): skipped", what, (unsigned) (need / 1024),
               (unsigned) (big / 1024));
    return false;
  }
  v.reserve(n);
  return true;
}

enum Layer : uint8_t { L_SAT = 1, L_STARLINK = 2, L_ISS = 4, L_PASS = 8, L_STATUS = 16, L_GEO = 32, L_KP = 64, L_EXTRA = 128 };  // L_EXTRA: solar wind, launches
enum Job : uint8_t { JOB_ABOVE = 0, JOB_STARLINK = 1, JOB_ISS = 2, JOB_PASS = 3, JOB_ELEMENTS = 4, JOB_SUNIMG = 5, JOB_MOONIMG = 6, JOB_EARTHIMG = 7, JOB_REGIONIMG = 8, JOB_PLANETIMG = 9, JOB_ALLSKYIMG = 10,
                     JOB_SD = 11, JOB_SDFLASH = 12 };  // UI-66 (sky_sdfw.h, through extra_job)
inline void (*extra_job)(uint8_t job) = nullptr;  // UI-66: jobs served by headers included later
enum Kind : uint8_t { K_SAT = 0, K_STARLINK = 1, K_ISS = 2, K_GEO = 3 };
// UI-52: China's space station (Tianhe core; the docked modules share its orbit)
constexpr int32_t CSS_ID = 48274, CSS_WENTIAN = 53239, CSS_MENGTIAN = 54216;
// UI-28 orbit classes. HEO (highly elliptical) is shown on the card as HEO and is
// switched with MEO.
enum OrbitClass : uint8_t { C_CLS_LEO = 0, C_CLS_MEO = 1, C_CLS_GEO = 2, C_CLS_HEO = 3 };

// One satellite as published to the loop. Carries its own motion (ARCH-7): the
// position at time t is p rotated about `axis` by rate*(t - t_fix) (MOTION-1).
struct SatRec {
  int32_t id = 0;
  char name[26] = {0};
  char cc[6] = {0};         // UI-18: owner country (ISO 3166 alpha-3 or CelesTrak's code); "" = unknown
  char intl[12] = {0};      // international designator, e.g. 1998-067A
  uint8_t kind = K_SAT;
  float alt_km = 0;         // altitude at the fix
  float alt_rate = 0;       // km/s
  double t_fix = 0;         // UTC seconds of the fix
  float p[3] = {0, 0, 1};     // unit vector of the sub-satellite point (lat/lon on a sphere)
  float axis[3] = {0, 0, 1};  // unit rotation axis of the great-circle arc
  float rate = 0;             // rad/s along the arc
  bool has_vel = false;
  float speed_kms = 0;      // inertial speed (UI-24 details)
  float period_min = 0, incl_deg = 0;
  uint8_t cls = C_CLS_LEO;  // UI-28
  bool debris = false;      // UI-29: rocket body or debris (drawn as a trash can)
  char tag = 0;             // UI-28: GNSS constellation letter (G/E/R/C) for MEO markers
  uint16_t launch = 0;      // UI-42: Starlink launch key (SlElem::launch)
  uint16_t launched = 0;    // UI-24b: launch date, days since 1957-01-01 (0 = unknown)
};

struct PassPt {
  float az, el;
};
constexpr int PASS_PTS = 24;
struct Pass {
  int64_t start = 0, max = 0, end = 0;  // UTC seconds: rise, culmination, set (0° horizon)
  float max_el = 0;
  char start_dir[4] = {0}, max_dir[4] = {0}, end_dir[4] = {0};
  bool visible = false;                 // ISS sunlit while the sky is dark (Sun < -6°)
  PassPt path[PASS_PTS];                // rise to set, for the tap-to-show arc (UI-24)
  uint8_t npath = 0;
};

// What the user is told about the data (UI-25). Written by the task.
struct DataStatus {
  double loaded = 0;          // last successful element download, UTC seconds (0 = never)
  double oldest_epoch = 0;    // oldest element-set epoch in use, UTC seconds (log only)
  double data_time = 0;       // UI-25/DATA-3: download time of the oldest group in use
  int n_sats = 0, n_starlink = 0;
  bool have_iss = false;
  char error[80] = {0};       // last download failure, "" after a success
  double next_try = 0;        // when the next download attempt is due
};

// Settings shared by the loop and the task (the task reads a snapshot, `jc`).
struct Config {
  double lat = 0, lon = 0, alt_m = 0;
  int starlink_radius = 70;
  std::string sat_group = "visual";  // CelesTrak GP group for the satellite layer
  bool sats_on = true, starlink_on = true;  // sats_on = the LEO switch (UI-28)
  bool meo_on = true, geo_on = false;
  bool debris_on = true;  // UI-35: rocket bodies and debris
};

inline const char *compass(float az) {
  static const char *const P[16] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  int i = (int) floorf((az + 11.25f) / 22.5f) % 16;
  if (i < 0)
    i += 16;
  return P[i];
}

namespace net {

static const char *const TAG = "sat_net";

inline double ELEM_REFRESH_S = 12 * 3600.0;  // DATA-1: element sets are good for days; 12 h keeps them fresh
// TEST-1: where the CelesTrak files come from. Release builds use celestrak.org; a debug build
// can point at tools/host_test/celestrak_server.py (cached copies, no 403 rule) and shorten
// ELEM_REFRESH_S to download again and again.
inline std::string celestrak_base = "https://celestrak.org";
inline std::string ct_url(const char *path) { return celestrak_base + path; }
constexpr double MAX_ELEM_AGE_S = 10 * 86400.0; // DATA-9: older element sets (decayed, lost) are dropped
constexpr double RETRY_S = 10 * 60.0;           // after a failed download
constexpr double RETRY_403_S = 2 * 3600.0;       // CelesTrak 403s repeat downloads within 2 h; retrying sooner risks a block
constexpr size_t JSON_BUF = 96 * 1024;          // SATCAT owner list (~52 KB)
constexpr size_t SAT_CAP = 400;                 // element sets kept for the satellite layer
constexpr size_t STARLINK_CAP = 16000;
constexpr size_t MEO_CAP = 200;                  // UI-28: GNSS lists, ~130 today
constexpr size_t GEO_CAP = 740;                  // geosynchronous, ~560 today (cache region D)          // ~11,000 today
constexpr size_t STARLINK_PUB_CAP = 700;        // DATA-3: published per scan (whole sky)
constexpr float RISING_EL = -2.0f;              // publish slightly below the horizon so risers appear promptly
constexpr uint32_t STACK = 20480;               // ARCH-8, internal RAM (TLS + SGP4 locals)

inline Config cfg;  // written only by the loop, under `mutex`
inline Config jc;   // task-only snapshot of cfg, taken under `mutex` at the start of each job
inline QueueHandle_t queue = nullptr;
inline SemaphoreHandle_t mutex = nullptr;
inline volatile bool paused = false;   // BUILD-6
inline volatile bool link_up = false;  // NET-8: written by the loop each tick; no downloads while false

// ---- pending slot: written by the task, drained by the loop, guarded by `mutex`
// UI-37: one 3-hour planetary Kp value from NOAA SWPC (observed, estimated or predicted)
struct KpPt {
  int64_t t = 0;  // UTC seconds, start of the 3 h interval
  float kp = 0;
  bool predicted = false;
};

// UI-55: solar wind at L1 (NOAA SWPC real-time, the active spacecraft), 15-minute means
struct SolarWind {
  double t = 0;              // UTC seconds of the newest sample (0 = none)
  float bz = NAN, bt = NAN;  // nT, GSM
  float speed = NAN, density = NAN;  // km/s, protons/cm3
};
// UI-54: an upcoming launch (Launch Library 2)
struct LaunchRec {
  char name[64] = "";     // "Falcon 9 | Starlink Group 12-5"
  char rocket[24] = "";   // "Falcon 9"
  char where[48] = "";    // pad location
  char status[8] = "";    // "Go", "TBD", "Hold", "Success"...
  char cc[4] = "";        // UI-54d: the launching country (ISO 3166 alpha-3, from the pad; "ESA" for Kourou)
  double net = 0;         // UTC seconds
  float lat = NAN, lon = NAN;
  bool webcast = false;   // UI-76: Launch Library says its webcast is live
};
// UI-54c: an upcoming space event (Launch Library 2 events: dockings, undockings, EVAs...)
struct EventRec {
  char name[72] = "";     // "SpaceX Crew-12 Crew Dragon Undocking"
  char type[28] = "";     // "Spacecraft Undocking"
  double t = 0;           // UTC seconds
  bool exact = false;     // time known to the hour or better
  bool iss = false;       // UI-69f: at the International Space Station (its "location")
  char cc[4] = "";        // UI-54d: the spacecraft's country, from its name (Dragon -> USA...)
};
// UI-73: NOAA SWPC space weather (products/alerts.json): the newest notice of each kind that
// is still current
enum SwxKind : uint8_t { SWX_WATCH = 0, SWX_STORM = 1, SWX_WARN = 2, SWX_FLARE = 3, SWX_PROTON = 4, SWX_KINDS = 5 };
struct SwxRec {
  uint8_t kind = 0;
  uint8_t level = 0;  // WATCH: G; STORM, WARN: Kp; FLARE: R (0: none); PROTON: S
  char cls[8] = "";   // FLARE: "M6.7"
  double issued = 0;  // UTC seconds
  double t = 0;       // WATCH: the predicted day (UTC midnight); FLARE: its maximum; else issued
  double until = 0;   // shown until
};
// UI-74: a re-entry expected within two days (CelesTrak's decaying list): the time from how
// fast the orbit is shrinking; the highest pass over the observer in the window
struct DecayRec {
  char name[26] = "";
  char intl[12] = "";
  int32_t id = 0;
  double est = 0;       // UTC seconds
  float unc_h = 0;      // +/- hours
  bool rb = false, deb = false;  // rocket body, debris (else a satellite)
  double over_t = 0;    // the highest pass above the horizon before the estimate (0: none)
  float over_el = -90;
  char over_dir[4] = "";
};
// UI-54d: the country a flag is shown for. A launch: its pad's country, but the launching
// nation where the site is someone else's (Kourou: ESA; Baikonur: Russia). An event: from the
// spacecraft in its name (Launch Library gives events no agency in list or normal mode).
inline const char *launch_cc(const char *pad) {
  if (!strcmp(pad, "GUF"))
    return "ESA";
  if (!strcmp(pad, "KAZ"))
    return "RUS";
  return pad;
}
inline const char *rll_cc(const char *country) {  // RocketLaunch.Live gives the country's name
  static const struct { const char *name, *cc; } C[] = {
      {"United States", "USA"}, {"China", "CHN"}, {"Russia", "RUS"}, {"Kazakhstan", "RUS"}, {"French Guiana", "ESA"},
      {"India", "IND"}, {"Japan", "JPN"}, {"New Zealand", "NZL"}, {"South Korea", "KOR"}, {"Iran", "IRN"},
      {"North Korea", "PRK"}, {"Israel", "ISR"}, {"Australia", "AUS"}, {"United Kingdom", "GBR"}, {"Norway", "NOR"},
      {"Brazil", "BRA"}, {"Sweden", "SWE"}};
  for (const auto &c : C)
    if (!strcmp(country, c.name))
      return c.cc;
  return "";
}
inline const char *event_cc(const char *name) {
  static const struct { const char *word, *cc; } W[] = {
      {"Dragon", "USA"}, {"SpaceX", "USA"}, {"Crew-", "USA"}, {"Cygnus", "USA"}, {"Starliner", "USA"}, {"Dream Chaser", "USA"},
      {"Axiom", "USA"}, {"Ax-", "USA"}, {"NG-", "USA"}, {"Orion", "USA"}, {"Artemis", "USA"},
      {"Soyuz", "RUS"}, {"Progress", "RUS"},
      {"Shenzhou", "CHN"}, {"Tianzhou", "CHN"}, {"Mengzhou", "CHN"}, {"Tiangong", "CHN"},
      {"HTV", "JPN"}, {"Kounotori", "JPN"}, {"Gaganyaan", "IND"}};
  for (const auto &w : W)
    if (strstr(name, w.word))
      return w.cc;
  return "";
}
// UI-58: NOAA OVATION aurora nowcast at the observer (percent chance of visible aurora)
struct AuroraChance {
  double obs = 0, fc = 0;  // UTC seconds: model input time, the time it forecasts (0 = none)
  float here = NAN;        // overhead
  float view = NAN;        // the most within AURORA_VIEW_KM (seen low in the sky that way)
  float view_az = NAN;     // true bearing of that cell
};
// UI-59/60: live pictures, decoded to IMG_PX x IMG_PX RGB565 in the shared img_work_buf:
//   IMG_SUN  NOAA GOES SUVI 30.4 nm (1280 px PNG, ~1.1 MB)
//   IMG_MOON NASA SVS Dial-A-Moon (730 px JPEG, ~110 KB, one frame per hour)
//   IMG_EARTH NOAA GOES-18 or GOES-19 GeoColor full disk (678 px JPEG, ~450 KB, every 10 min)
//   IMG_REGION the observer's region from the same satellite (500-600 px JPEG, ~250 KB, UI-62a)
//   IMG_ALLSKY UAF's Poker Flat all-sky camera (UI-75: 514x600 JPEG, ~50 KB, while it is dark)
//   IMG_PLANET a NASA photo of the planet asked for (Wikimedia Commons, ~15-55 KB, UI-61a)
enum ImgKind : uint8_t { IMG_SUN = 0, IMG_MOON = 1, IMG_EARTH = 2, IMG_REGION = 3, IMG_ALLSKY = 4, IMG_PLANET = 5, IMG_N = 6 };
constexpr int IMG_LIVE_N = 5;  // the live kinds (Sun .. all-sky): streamed in as they arrive (UI-59f)
// UI-61a: public-domain NASA photos, 500 px thumbnails from Wikimedia Commons (baseline JPEG)
struct PlanetPhoto {
  const char *path, *credit;
  int w, h;
};
inline const PlanetPhoto &planet_photo(int p) {
  static const PlanetPhoto P[5] = {
      {"thumb/4/4a/Mercury_in_true_color.jpg/500px-Mercury_in_true_color.jpg", "NASA MESSENGER", 500, 500},
      {"e/e5/Venus-real_color.jpg", "NASA Mariner 10", 480, 480},
      {"thumb/0/02/OSIRIS_Mars_true_color.jpg/500px-OSIRIS_Mars_true_color.jpg", "ESA Rosetta", 500, 500},
      {"thumb/2/2b/Jupiter_and_its_shrunken_Great_Red_Spot.jpg/500px-Jupiter_and_its_shrunken_Great_Red_Spot.jpg",
       "NASA Hubble", 500, 500},
      {"thumb/e/e3/Saturn_from_Cassini_Orbiter_%282004-10-06%29.jpg/500px-Saturn_from_Cassini_Orbiter_%282004-10-06%29.jpg",
       "NASA Cassini", 500, 256},
  };
  return P[p];
}
inline volatile int planet_req = -1;  // the planet whose photo JOB_PLANETIMG fetches
// UI-62a: NOAA STAR GeoColor sectors (square frames); the first whose box holds the observer
struct Region {
  const char *code, *sat, *name;
  int px;                             // frame size
  float lat0, lat1, lon0, lon1;       // box, degrees (lon east positive)
};
inline const Region *region_for(double lat, double lon) {
  static const Region R[] = {
      {"ak", "GOES18", "Alaska", 500, 50, 75, -180, -128},  {"hi", "GOES18", "Hawaii", 600, 15, 25, -165, -150},
      {"pnw", "GOES18", "Pacific NW", 600, 40, 52, -130, -110}, {"psw", "GOES18", "Pacific SW", 600, 30, 40, -128, -110},
      {"nr", "GOES19", "N Rockies", 600, 40, 50, -110, -100},   {"sr", "GOES19", "S Rockies", 600, 30, 40, -110, -100},
      {"umv", "GOES19", "Upper Miss.", 600, 38, 50, -100, -88}, {"smv", "GOES19", "Lower Miss.", 600, 28, 38, -100, -90},
      {"cgl", "GOES19", "Great Lakes", 600, 38, 48, -88, -78},  {"ne", "GOES19", "Northeast", 600, 38, 48, -78, -65},
      {"se", "GOES19", "Southeast", 600, 24, 38, -90, -75},     {"pr", "GOES19", "Puerto Rico", 600, 15, 21, -70, -62},
  };
  for (const auto &r : R)
    if (lat >= r.lat0 && lat <= r.lat1 && lon >= r.lon0 && lon <= r.lon1)
      return &r;
  return nullptr;
}
// UI-62b: the pictures float on the page: black sky becomes the page colour (0x070B18) and a
// soft circular edge (smoothstep from r0 to r1, pixels from the centre) fades the square away.
constexpr uint32_t PAGE_BG = 0x070B18;
inline void round_mask_row(uint16_t *row, int y, int n, float r0, float r1) {  // one row of round_mask
  const float br = (PAGE_BG >> 16) & 0xFF, bg = (PAGE_BG >> 8) & 0xFF, bb = PAGE_BG & 0xFF, c = n / 2.0f;
  for (int x = 0; x < n; x++) {
    const float dx = x + 0.5f - c, dy = y + 0.5f - c, d = sqrtf(dx * dx + dy * dy);
    float k = d <= r0 ? 1.0f : d >= r1 ? 0.0f : (r1 - d) / (r1 - r0);
    k = k * k * (3 - 2 * k);
    uint16_t &p = row[x];
    const float r = std::max<float>((p >> 11) * 255 / 31, br), g = std::max<float>(((p >> 5) & 63) * 255 / 63, bg),
                b = std::max<float>((p & 31) * 255 / 31, bb);
    const int R = (int) (r * k + br * (1 - k) + 0.5f), G = (int) (g * k + bg * (1 - k) + 0.5f),
              B = (int) (b * k + bb * (1 - k) + 0.5f);
    p = (uint16_t) ((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3));
  }
}
inline void round_mask(uint16_t *px, int n, float r0, float r1) {
  for (int y = 0; y < n; y++)
    round_mask_row(px + (size_t) y * n, y, n, r0, r1);
}
constexpr int IMG_PX = 360;
constexpr float SUN_CROP = 0.75f;  // SUVI's field is ~1.6 solar diameters: the middle 3/4 fills the view
struct ImageInfo {
  double obs = 0;       // UTC seconds of the exposure / the hour rendered (0: unknown)
  char src[12] = "";    // "GOES-19"
  bool ok = false;      // the last attempt succeeded
  bool fresh = false;   // new pixels in img_work_buf, not yet copied by the loop
  char err[48] = "";
  uint32_t seq = 0;     // attempts finished (the loop waits for this to move)
  float phase = NAN, age = NAN, dist_km = NAN;  // Moon: % lit, days, km (Dial-A-Moon)
  bool south_up = false;                         // Moon: the southern-hemisphere view
  int planet = -1;                               // IMG_PLANET: which planet
  bool shadow = false;                           // Sun: newer frames dark (the satellite in Earth's shadow)
};
// Progress of the picture being fetched: written by the task, read by the loop for display
// only (single words; a torn read shows a stale number for one frame at worst).
enum ImgStage : uint8_t { STG_IDLE, STG_LIST, STG_DOWNLOAD, STG_DECODE };
inline volatile uint8_t img_stage = STG_IDLE, img_stage_kind = IMG_SUN;
inline volatile int32_t img_bytes = 0, img_total = -1;  // -1: size not known (chunked)
// UI-59f: the live pictures are shown as they arrive: rows of img_work_buf finished so far (-1:
// not streaming one, img_stage_kind says which); the rows below hold the picture showing
// (img_shown, set by the loop when it copies one)
inline volatile int img_prog_rows = -1;
inline const uint16_t *volatile img_shown[IMG_N] = {};
inline volatile bool dl_track = false;                   // http_stream reports into img_bytes/img_total
struct Pending {
  uint8_t fresh = 0;
  pvector<KpPt> kp;  // UI-37
  pvector<SatRec> sats, starlink, geo;
  int sat_total = 0, starlink_total = 0, geo_total = 0;
  SatRec iss;
  bool iss_ok = false;
  pvector<Pass> passes;
  SatRec css;  // UI-52
  bool css_ok = false;
  pvector<Pass> css_passes;
  SolarWind wind;                 // UI-55
  pvector<LaunchRec> launches;    // UI-54
  pvector<EventRec> events;       // UI-54c
  pvector<comets::El> comet_list;     // UI-63 (empty: nothing new)
  bool comets_new = false;
  pvector<SwxRec> swx;            // UI-73
  bool swx_new = false;
  pvector<DecayRec> decays;       // UI-74
  bool decays_new = false;
  AuroraChance ovation;           // UI-58
  ImageInfo img[IMG_N];           // UI-59/60
  DataStatus status;
};
inline Pending pending;

// ---- task-only state (ARCH-7)
struct SgpSat {
  int32_t id = 0;
  char name[26] = {0};
  char intl[12] = {0};
  double epoch = 0;  // UTC seconds
  float period_min = 0, incl_deg = 0;
  uint8_t cls = C_CLS_LEO;
  bool debris = false;
  char tag = 0;
  sgp4::elsetrec rec;
};
// Starlink: J2 secular propagation from mean elements (DATA-3). ~72 bytes each.
struct SlElem {
  int32_t id;
  int32_t num;            // the number in "STARLINK-1234"
  double epoch;           // UTC seconds
  double m0, mdot;        // rad, rad/s
  double raan0, raan_dot; // rad, rad/s
  double argp0, argp_dot; // rad, rad/s
  float a_km, e, incl;    // km, -, rad
  float ndd;              // rad/s^2 (MEAN_MOTION_DOT term)
  uint16_t launch;        // UI-42: launch key from the designator, YY*1000 + launch number (0 = unknown)
};
inline pvector<SgpSat> sats;
inline SgpSat iss;
inline bool have_iss = false;
inline SgpSat css;  // UI-52
inline bool have_css = false;
inline double css_loaded = 0, css_retry_at = 0;
inline pvector<SlElem> starlink;
inline pvector<SgpSat> meo_sats, geo_sats;  // UI-28: GNSS and geosynchronous lists
inline double meo_loaded = 0, geo_loaded = 0;
inline DataStatus status;
inline double sats_loaded = 0, starlink_loaded = 0, iss_loaded = 0;  // UTC seconds of each group's last load
inline char *json_buf = nullptr;

// ArduinoJson documents live in PSRAM (ARCH-8).
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
  }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
};
inline PsramAllocator psram_alloc;

// ================================================================== HTTP
struct HttpResult {
  int status = 0;     // HTTP status, or 0 when the connection failed
  bool ok = false;    // 200 and the whole body read
  long bytes = 0;
  bool reused = false;  // NET-12: came over the kept connection
};

// GET `url` and hand the body to `sink` in chunks as it arrives (nothing is held in
// full, so a 1.7 MB Starlink list needs no buffer). `what` labels the log.
// UI-62: the Last-Modified header of the latest http_stream() reply ("" when none); below
inline char http_last_modified[40] = "";
inline esp_err_t http_event(esp_http_client_event_t *evt);
// NET-12: one kept-alive connection, reused while the next request goes to the same host
// (the Sun's frame list and its picture; the Earth's 404 steps; JPL, NOAA JSON in a row):
// a TLS handshake costs this chip 2-8 s. A request to another host, an error, or a body
// left unread (the sink stopped early) closes it.
struct KeptConn {
  esp_http_client_handle_t h = nullptr;
  char host[64] = "";
};
inline KeptConn kept;
inline void kept_drop() {
  if (kept.h) {
    esp_http_client_close(kept.h);
    esp_http_client_cleanup(kept.h);
  }
  kept.h = nullptr;
  kept.host[0] = 0;
}
inline void url_host(const char *url, char *out, size_t n) {
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  const size_t k = std::min(strcspn(p, "/"), n - 1);
  memcpy(out, p, k);
  out[k] = 0;
}
inline HttpResult http_stream(const char *url, const char *what, const std::function<void(const char *, int)> &sink,
                              const bool *stop = nullptr) {  // stop: set by the sink once it has enough
  HttpResult res;
  char host[64];
  url_host(url, host, sizeof(host));
  http_last_modified[0] = 0;
  const int64_t t0 = net_us();
  int64_t sink_us = 0, t_hdr = 0;
  bool reused = false;
  ESP_LOGD(TAG, "%s: GET %s", what, url);  // NET-13: every request's URL, at DEBUG
  for (int attempt = 0; attempt < 3; attempt++) {
    reused = kept.h != nullptr && strcmp(kept.host, host) == 0;
    if (kept.h && !reused)
      kept_drop();
    if (kept.h == nullptr) {
      esp_http_client_config_t c = {};
      c.url = url;
      c.crt_bundle_attach = esp_crt_bundle_attach;
      c.timeout_ms = 15000;
      c.buffer_size = 8192;  // NET-11: fewer, larger reads
      c.buffer_size_tx = 1024;
      c.keep_alive_enable = true;
      c.event_handler = http_event;
      c.user_agent = "SkyTracker/4.5 (ESPHome display)";  // Wikimedia asks for a descriptive agent
      kept.h = esp_http_client_init(&c);
      if (kept.h == nullptr) {
        ESP_LOGW(TAG, "%s: client init failed (%s)", what, url);
        return res;
      }
      snprintf(kept.host, sizeof(kept.host), "%s", host);
    } else {
      esp_http_client_set_url(kept.h, url);
    }
    esp_err_t err = esp_http_client_open(kept.h, 0);
    int64_t clen = err == ESP_OK ? esp_http_client_fetch_headers(kept.h) : -1;
    res.status = err == ESP_OK ? esp_http_client_get_status_code(kept.h) : 0;
    if (err != ESP_OK || res.status < 100) {  // 4.5.33: a closed kept connection reads as status -1
      if (err == ESP_OK)
        err = ESP_FAIL;
      kept_drop();
      if (reused)
        continue;  // the server had closed the kept connection: a new one at once
      if (attempt < 2) {  // NET-11: a lost DNS answer or a refused connect: again after a second
        ESP_LOGW(TAG, "%s: connect failed (%s); retrying (%s)", what, esp_err_to_name(err), url);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      ESP_LOGW(TAG, "%s: connect failed (%s) (%s)", what, esp_err_to_name(err), url);
      return res;
    }
    t_hdr = net_us();
    if (dl_track) {  // UI-59/60: download progress for the picture screen
      img_total = clen > 0 ? (int32_t) clen : -1;
      img_bytes = 0;
    }
    if (res.status != 200) {
      if (res.status != 404)
        ESP_LOGW(TAG, "%s: HTTP %d (%s)", what, res.status, url);
      // 4.5.31: never reuse after an error reply: flushing its body left the next reply
      // misread on the kept connection (the Earth's 404 steps gave "not a readable JPEG")
      kept_drop();
      break;
    }
    static char chunk[8192];
    bool failed = false;
    for (;;) {
      int n = esp_http_client_read(kept.h, chunk, sizeof(chunk));
      if (n < 0) {
        failed = true;
        break;
      }
      if (n == 0)
        break;
      res.bytes += n;
      if (dl_track)
        img_bytes = (int32_t) res.bytes;
      const int64_t s0 = net_us();
      sink(chunk, n);
      sink_us += net_us() - s0;
      if (stop && *stop)
        break;
    }
    if (stop && *stop && !failed) {
      res.ok = true;  // read what was wanted; the rest is not needed (nor the connection)
      kept_drop();
      break;
    }
    if (failed || !esp_http_client_is_complete_data_received(kept.h)) {
      ESP_LOGW(TAG, "%s: read failed after %ld bytes (%s)", what, res.bytes, url);
      kept_drop();
      break;
    }
    res.ok = true;
    break;
  }
  res.reused = reused;
  if (res.bytes >= 50000) {  // NET-11: how fast, and how much of it was our own processing
    const int64_t now = net_us(), us = now - t0, body = t_hdr ? now - t_hdr : us;
    ESP_LOGI(TAG, "%s: %ld KB in %.1f s (%s %.1f s, body %.0f KB/s; %.1f s of it decoding)", what, res.bytes / 1024,
             us / 1e6, reused ? "reused connection," : "connect", (us - body) / 1e6,
             body > 0 ? res.bytes / 1.024 / (body / 1e3) : 0.0, sink_us / 1e6);
  }
  return res;
}

// Whole body into buf (small JSON replies). Returns length, or -1.
inline int http_get(const char *url, char *buf, size_t cap, const char *what) {
  size_t len = 0;
  bool overflow = false;
  HttpResult r = http_stream(url, what, [&](const char *d, int n) {
    if (len + n >= cap) {
      overflow = true;
      return;
    }
    memcpy(buf + len, d, n);
    len += n;
  });
  if (!r.ok)
    return -1;
  if (overflow) {
    ESP_LOGW(TAG, "%s: reply larger than %u bytes; dropped", what, (unsigned) cap);
    return -1;
  }
  buf[len] = 0;
  return (int) len;
}

// Splits a byte stream into lines for `on_line` (CR/LF stripped; long lines dropped).
struct LineSplitter {
  std::function<void(char *)> on_line;
  char line[512];
  size_t n = 0;
  bool too_long = false;
  void feed(const char *d, int len) {
    for (int i = 0; i < len; i++) {
      const char ch = d[i];
      if (ch == '\n') {
        if (!too_long) {
          while (n > 0 && line[n - 1] == '\r')
            n--;
          line[n] = 0;
          on_line(line);
        }
        n = 0;
        too_long = false;
      } else if (n + 1 < sizeof(line)) {
        line[n++] = ch;
      } else {
        too_long = true;
      }
    }
  }
  void finish() {
    if (n > 0 && !too_long) {
      line[n] = 0;
      on_line(line);
    }
    n = 0;
  }
};

// ================================================================== element sets (OMM CSV)
// Columns: OBJECT_NAME,OBJECT_ID,EPOCH,MEAN_MOTION,ECCENTRICITY,INCLINATION,
// RA_OF_ASC_NODE,ARG_OF_PERICENTER,MEAN_ANOMALY,EPHEMERIS_TYPE,CLASSIFICATION_TYPE,
// NORAD_CAT_ID,ELEMENT_SET_NO,REV_AT_EPOCH,BSTAR,MEAN_MOTION_DOT,MEAN_MOTION_DDOT
struct Omm {
  char name[26];
  char intl[12];
  double epoch;  // UTC seconds
  double n_revday, ecc, incl, raan, argp, ma, bstar, ndot, nddot;  // degrees for angles
  int32_t id;
};

inline int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned) (y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t) doe - 719468;
}

// "2026-09-24T01:49:53.443200" -> UTC seconds
inline bool parse_epoch(const char *s, double &out) {
  int y, mo, d, h, mi;
  double sec;
  if (sscanf(s, "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) != 6)
    return false;
  out = (double) days_from_civil(y, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0 + sec;
  return true;
}

// One CSV row. The name may contain commas, so the 16 fixed fields are taken from
// the right. Returns false for the header and anything malformed.
inline bool parse_omm(char *line, Omm &o) {
  char *f[17];
  int nf = 0;
  char *end = line + strlen(line);
  char *p = end;
  while (nf < 16) {
    while (p > line && p[-1] != ',')
      p--;
    if (p == line)
      return false;
    f[16 - nf] = p;
    p[-1] = 0;
    p--;
    nf++;
  }
  f[0] = line;
  if (strcmp(f[0], "OBJECT_NAME") == 0)
    return false;
  strncpy(o.name, f[0], sizeof(o.name) - 1);
  o.name[sizeof(o.name) - 1] = 0;
  strncpy(o.intl, f[1], sizeof(o.intl) - 1);
  o.intl[sizeof(o.intl) - 1] = 0;
  if (!parse_epoch(f[2], o.epoch))
    return false;
  o.n_revday = atof(f[3]);
  o.ecc = atof(f[4]);
  o.incl = atof(f[5]);
  o.raan = atof(f[6]);
  o.argp = atof(f[7]);
  o.ma = atof(f[8]);
  o.id = atol(f[11]);
  o.bstar = atof(f[14]);
  o.ndot = atof(f[15]);
  o.nddot = atof(f[16]);
  return o.id > 0 && o.n_revday > 0.5 && o.ecc >= 0 && o.ecc < 1;
}

constexpr double JD_UNIX = 2440587.5;  // JD of 1970-01-01T00:00Z

// Bounded, always terminated copy (strncpy trips -Wstringop-truncation).
inline void copy_cstr(char *dst, size_t n, const char *src) {
  const size_t k = strnlen(src, n - 1);
  memcpy(dst, src, k);
  dst[k] = 0;
}
// UI-78: text from the feeds (launch, pad, event and comet names) made drawable: the device's
// fonts hold printable ASCII plus a few symbols, so accented letters become their base letter,
// dashes and curly quotes plain ones, a no-break space a space, and anything else "?".
// In place (never longer).
inline void fold_text(char *s) {
  static const char LAT[] =  // U+00C0..U+017F, by Unicode decomposition (tools: NFKD)
      "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuytyAaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi"
      "IiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  const uint8_t *r = (const uint8_t *) s;
  char *w = s;
  while (*r) {
    uint32_t cp = *r;
    int n = 1;
    if (cp >= 0xF0 && r[1] && r[2] && r[3])
      cp = (cp & 7) << 18 | (r[1] & 63) << 12 | (r[2] & 63) << 6 | (r[3] & 63), n = 4;
    else if (cp >= 0xE0 && r[1] && r[2])
      cp = (cp & 15) << 12 | (r[1] & 63) << 6 | (r[2] & 63), n = 3;
    else if (cp >= 0xC0 && r[1])
      cp = (cp & 31) << 6 | (r[1] & 63), n = 2;
    r += n;
    if (cp < 0x80) {
      *w++ = cp == '^' || cp == '`' || cp == '{' || cp == '}' || cp == '\\' ? '-' : (char) cp;
    } else if (cp == 0xB0 || cp == 0xB1 || cp == 0xB7) {  // degree, plus-minus, middle dot: in the fonts
      memcpy(w, r - n, n);
      w += n;
    } else if (cp >= 0xC0 && cp < 0x180) {
      *w++ = LAT[cp - 0xC0];
    } else if ((cp >= 0x2010 && cp <= 0x2015) || cp == 0x2212) {
      *w++ = '-';
    } else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201B || cp == 0x2032) {
      *w++ = '\'';
    } else if (cp == 0x201C || cp == 0x201D || cp == 0x2033) {
      *w++ = '"';
    } else if (cp == 0xA0 || cp == 0x2009 || cp == 0x202F) {
      *w++ = ' ';
    } else if (cp == 0x2026 && w + 3 <= (char *) r) {
      memcpy(w, "...", 3);
      w += 3;
    } else {
      *w++ = '?';
    }
  }
  *w = 0;
}
// UI-62: Last-Modified capture (declared above http_stream)
inline esp_err_t http_event(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
      strcasecmp(evt->header_key, "Last-Modified") == 0)
    copy_cstr(http_last_modified, sizeof(http_last_modified), evt->header_value);
  return ESP_OK;
}
inline bool http_date(const char *s, double &t) {  // "Mon, 28 Sep 2026 20:55:12 GMT"
  static const char *const MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char m[4] = "";
  int d, y, h, mi, se;
  if (sscanf(s, "%*[^,], %d %3s %d %d:%d:%d", &d, m, &y, &h, &mi, &se) != 6)
    return false;
  const char *f = strstr(MON, m);
  if (!f || m[0] == 0)
    return false;
  t = (double) days_from_civil(y, (unsigned) ((f - MON) / 3 + 1), (unsigned) d) * 86400.0 + h * 3600 + mi * 60 + se;
  return true;
}

inline bool make_sgp(const Omm &o, SgpSat &s) {
  const double deg = M_PI / 180.0, xpdotp = 1440.0 / (2.0 * M_PI);
  s.id = o.id;
  copy_cstr(s.name, sizeof(s.name), o.name);
  copy_cstr(s.intl, sizeof(s.intl), o.intl);
  s.epoch = o.epoch;
  s.period_min = (float) (1440.0 / o.n_revday);
  s.incl_deg = (float) o.incl;
  // UI-28: LEO under 128 min; HEO when strongly elliptical; GEO near one orbit a day
  // and nearly circular; everything else MEO.
  if (s.period_min < 128.0f)
    s.cls = C_CLS_LEO;
  else if (o.ecc > 0.25)
    s.cls = C_CLS_HEO;
  else if (o.n_revday > 0.9 && o.n_revday < 1.1 && o.ecc < 0.1)
    s.cls = C_CLS_GEO;
  else
    s.cls = C_CLS_MEO;
  // UI-29: CelesTrak names spent stages "... R/B" and fragments "... DEB"
  s.debris = strstr(o.name, " R/B") != nullptr || strstr(o.name, " DEB") != nullptr || strncmp(o.name, "DEB", 3) == 0;
  s.tag = 0;
  if (s.cls == C_CLS_MEO || s.cls == C_CLS_HEO) {
    if (strncmp(o.name, "GPS", 3) == 0 || strstr(o.name, "NAVSTAR"))
      s.tag = 'G';
    else if (strstr(o.name, "GALILEO") || strncmp(o.name, "GSAT0", 5) == 0)
      s.tag = 'E';
    else if (strncmp(o.name, "COSMOS", 6) == 0 || strstr(o.name, "GLONASS"))
      s.tag = 'R';
    else if (strstr(o.name, "BEIDOU"))
      s.tag = 'C';
  }
  char satn[9];
  snprintf(satn, sizeof(satn), "%ld", (long) (o.id % 100000));
  const double epoch_1950 = o.epoch / 86400.0 + JD_UNIX - 2433281.5;
  return sgp4::sgp4init(sgp4::wgs72, 'i', satn, epoch_1950, o.bstar, o.ndot / (xpdotp * 1440.0),
                        o.nddot / (xpdotp * 1440.0 * 1440.0), o.ecc, o.argp * deg, o.incl * deg, o.ma * deg,
                        o.n_revday / xpdotp, o.raan * deg, s.rec) &&
         s.rec.error == 0;
}

// Starlink elements: SGP4's own initialisation gives the un-Kozai'd mean motion and
// the secular rates (J2, J4, first drag term); only those are kept and propagated.
// Short-period terms are left out: a few km, a fraction of a degree on the map.
inline bool make_sl(const Omm &o, SlElem &s) {
  SgpSat tmp;
  if (!make_sgp(o, tmp))
    return false;
  const sgp4::elsetrec &r = tmp.rec;
  s.id = o.id;
  const char *dash = strrchr(o.name, '-');
  s.num = dash ? atol(dash + 1) : 0;
  s.epoch = o.epoch;
  s.a_km = (float) (r.a * r.radiusearthkm);
  s.e = (float) r.ecco;
  s.incl = (float) r.inclo;
  s.m0 = r.mo;
  s.mdot = r.mdot / 60.0;
  s.raan0 = r.nodeo;
  s.raan_dot = r.nodedot / 60.0;
  s.argp0 = r.argpo;
  s.argp_dot = r.argpdot / 60.0;
  s.ndd = (float) (r.no_unkozai * r.t2cof / 3600.0);  // drag: SGP4's t2cof term, per s^2
  int ly = 0, ln = 0;  // UI-42: "2026-123A" -> 26123, so each launch's satellites share a key
  s.launch = sscanf(o.intl, "%d-%d", &ly, &ln) == 2 ? (uint16_t) ((ly % 100) * 1000 + ln % 1000) : 0;
  return true;
}

// ================================================================== propagation
// Long loops on core 0 give the idle task (and its watchdog) a tick now and then.
inline void breathe(uint32_t i, uint32_t every) {
  if (i % every == every - 1)
    vTaskDelay(1);
}

inline double gmst_rad(double unix_s) { return sgp4::gstime_SGP4(unix_s / 86400.0 + JD_UNIX); }

inline void teme_to_ecef(const double r[3], double g, double out[3]) {
  const double c = cos(g), s = sin(g);
  out[0] = c * r[0] + s * r[1];
  out[1] = -s * r[0] + c * r[1];
  out[2] = r[2];
}

inline bool sgp_ecef(SgpSat &s, double t, double out[3], double *speed = nullptr) {
  double r[3], v[3];
  if (!sgp4::sgp4(s.rec, (t - s.epoch) / 60.0, r, v) || s.rec.error != 0)
    return false;
  teme_to_ecef(r, gmst_rad(t), out);
  if (speed)
    *speed = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  return true;
}

inline void sl_ecef(const SlElem &s, double t, double out[3]) {
  const double dt = t - s.epoch;
  const double m = s.m0 + s.mdot * dt + s.ndd * dt * dt;
  const double raan = s.raan0 + s.raan_dot * dt, argp = s.argp0 + s.argp_dot * dt;
  double E = m + s.e * sin(m);
  E -= (E - s.e * sin(E) - m) / (1 - s.e * cos(E));
  const double r = s.a_km * (1 - s.e * cos(E));
  const double nu = atan2(sqrt(1 - s.e * s.e) * sin(E), cos(E) - s.e);
  const double u = argp + nu, cu = cos(u), su = sin(u), co = cos(raan), so = sin(raan);
  const double ci = cos(s.incl), si = sin(s.incl);
  const double x[3] = {r * (co * cu - so * su * ci), r * (so * cu + co * su * ci), r * su * si};
  teme_to_ecef(x, gmst_rad(t), out);
}

// ECEF km -> geodetic degrees and km (WGS84, Bowring iteration).
inline void geodetic(const double x[3], double &lat, double &lon, double &h) {
  constexpr double A = 6378.137, E2 = 6.69437999014e-3;
  const double p = sqrt(x[0] * x[0] + x[1] * x[1]);
  lon = atan2(x[1], x[0]);
  double la = atan2(x[2], p * (1 - E2)), n = A;
  for (int i = 0; i < 4; i++) {
    const double sl = sin(la);
    n = A / sqrt(1 - E2 * sl * sl);
    h = p / cos(la) - n;
    la = atan2(x[2], p * (1 - E2 * n / (n + h)));
  }
  lat = la * 180.0 / M_PI;
  lon *= 180.0 / M_PI;
}

inline void unit_from_latlon(double lat_deg, double lon_deg, float out[3]) {
  const double la = lat_deg * M_PI / 180.0, lo = lon_deg * M_PI / 180.0;
  out[0] = (float) (cos(la) * cos(lo));
  out[1] = (float) (cos(la) * sin(lo));
  out[2] = (float) sin(la);
}

// A fix and its motion from two positions 10 s apart (MOTION-1: great-circle arc of
// the sub-satellite point, plus a height rate).
inline void make_fix(const double x0[3], const double x1[3], double t, SatRec &r) {
  constexpr double DT = 10.0;
  double la0, lo0, h0, la1, lo1, h1;
  geodetic(x0, la0, lo0, h0);
  geodetic(x1, la1, lo1, h1);
  float p1[3];
  unit_from_latlon(la0, lo0, r.p);
  unit_from_latlon(la1, lo1, p1);
  r.alt_km = (float) h0;
  r.t_fix = t;
  float ax = r.p[1] * p1[2] - r.p[2] * p1[1];
  float ay = r.p[2] * p1[0] - r.p[0] * p1[2];
  float az = r.p[0] * p1[1] - r.p[1] * p1[0];
  const float s = sqrtf(ax * ax + ay * ay + az * az);
  const float c = r.p[0] * p1[0] + r.p[1] * p1[1] + r.p[2] * p1[2];
  if (s > 1e-9f) {
    r.axis[0] = ax / s;
    r.axis[1] = ay / s;
    r.axis[2] = az / s;
    r.rate = (float) (atan2f(s, c) / DT);
    r.alt_rate = (float) ((h1 - h0) / DT);
    r.has_vel = true;
  }
}

inline geo::AzEl azel_of(const geo::Observer &o, const double x[3]) {
  return geo::azel(o, {(float) x[0], (float) x[1], (float) x[2]});
}

// ================================================================== owner codes (UI-18)
// One SATCAT request (GROUP=visual, ~50 KB) at most once a day, then at most
// OWNER_LOOKUPS single catalogue numbers per element job for anything else. Cached
// for the boot; a number CelesTrak doesn't know is cached as "" and not asked again.
constexpr int OWNER_LOOKUPS = 3;
constexpr size_t OWNER_CACHE_MAX = 4000;
inline std::unordered_map<int32_t, std::array<char, 6>, std::hash<int32_t>, std::equal_to<int32_t>,
                          PsramAlloc<std::pair<const int32_t, std::array<char, 6>>>>
    owners;
// UI-24b: launch date from the same SATCAT records, days since 1957-01-01 (0 = unknown)
inline std::unordered_map<int32_t, uint16_t, std::hash<int32_t>, std::equal_to<int32_t>,
                          PsramAlloc<std::pair<const int32_t, uint16_t>>>
    launch_days;
constexpr int64_t SPACE_AGE_DAY = -4748;  // days_from_civil(1957, 1, 1)
inline uint16_t launch_from_text(const char *s) {  // "2024-01-03"
  int y, m, d;
  if (s == nullptr || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3 || y < 1957)
    return 0;
  return (uint16_t) (days_from_civil(y, m, d) - SPACE_AGE_DAY + 1);
}
inline double owner_bulk_next = 0;
inline double owner_backoff = 0;

// CelesTrak SATCAT owner code -> ISO 3166-1 alpha-3. Agencies and multinational
// owners (ESA, ISS, EUME, ...) have no country and keep CelesTrak's code.
inline void owner_to_iso3(const char *o, char out[6]) {
  static const char *const MAP[][2] = {
      {"US", "USA"},   {"PRC", "CHN"},  {"CIS", "RUS"},  {"JPN", "JPN"},  {"FR", "FRA"},   {"UK", "GBR"},
      {"IND", "IND"},  {"GER", "DEU"},  {"IT", "ITA"},   {"CA", "CAN"},   {"ISRA", "ISR"}, {"SKOR", "KOR"},
      {"NKOR", "PRK"}, {"BRAZ", "BRA"}, {"ARGN", "ARG"}, {"SPN", "ESP"},  {"TURK", "TUR"}, {"IRAN", "IRN"},
      {"UAE", "ARE"},  {"SAUD", "SAU"}, {"AUS", "AUS"},  {"NZ", "NZL"},   {"INDO", "IDN"}, {"TWN", "TWN"},
      {"THAI", "THA"}, {"MALA", "MYS"}, {"SING", "SGP"}, {"VTNM", "VNM"}, {"EGYP", "EGY"}, {"NIG", "NGA"},
      {"SAFR", "ZAF"}, {"MEX", "MEX"},  {"CHLE", "CHL"}, {"PER", "PER"},  {"KAZ", "KAZ"},  {"UKR", "UKR"},
      {"BEL", "BEL"},  {"NETH", "NLD"}, {"SWED", "SWE"}, {"NOR", "NOR"},  {"FIN", "FIN"},  {"DEN", "DNK"},
      {"POL", "POL"},  {"CZE", "CZE"},  {"AUT", "AUT"},  {"SWTZ", "CHE"}, {"LUXE", "LUX"}, {"POR", "PRT"},
      {"GREC", "GRC"}, {"PAKI", "PAK"}, {"BGD", "BGD"},  {"QAT", "QAT"},  {"ALG", "DZA"},  {"MA", "MAR"},
      {"TMMC", "TKM"}, {"AZER", "AZE"}, {"BELA", "BLR"}, {"HUN", "HUN"},  {"LTU", "LTU"},  {"EST", "EST"},
      {"LAOS", "LAO"}, {"PHL", "PHL"},  {"BOL", "BOL"},  {"ECU", "ECU"},  {"COL", "COL"},  {"URY", "URY"},
      {"VENZ", "VEN"}, {"NPL", "NPL"},  {"SRI", "LKA"},  {"KEN", "KEN"},  {"GHA", "GHA"},  {"ETH", "ETH"},
      {"RWA", "RWA"},  {"SEN", "SEN"},  {"MNG", "MNG"},  {"IRL", "IRL"},  {"ROM", "ROU"},  {"BGR", "BGR"},
  };
  for (const auto &m : MAP)
    if (strcmp(o, m[0]) == 0) {
      strncpy(out, m[1], 5);
      out[5] = 0;
      return;
    }
  strncpy(out, o, 5);
  out[5] = 0;
}

inline void owner_put(int32_t id, const char *owner) {
  if (owners.size() >= OWNER_CACHE_MAX)
    owners.clear();
  std::array<char, 6> v{};
  if (owner != nullptr && owner[0])
    owner_to_iso3(owner, v.data());
  owners[id] = v;
}

inline int owner_parse(const char *buf, int len) {
  JsonDocument filter;
  filter[0]["NORAD_CAT_ID"] = true;
  filter[0]["OWNER"] = true;
  filter[0]["LAUNCH_DATE"] = true;
  JsonDocument doc(&psram_alloc);
  if (deserializeJson(doc, buf, (size_t) len, DeserializationOption::Filter(filter)))
    return -1;
  int n = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    const int32_t id = o["NORAD_CAT_ID"] | 0;
    if (id == 0)
      continue;
    owner_put(id, o["OWNER"] | "");
    if (const uint16_t ld = launch_from_text(o["LAUNCH_DATE"] | ""))
      launch_days[id] = ld;
    n++;
  }
  return n;
}

inline void owners_update(double now) {
  bool miss = owner_bulk_next == 0 && !owners.empty();  // UI-24b: cached owners without launch dates
  for (const auto &s : sats)
    miss |= owners.find(s.id) == owners.end();
  if (!miss)
    return;
  if (now >= owner_bulk_next) {
    int len = http_get(ct_url("/satcat/records.php?GROUP=visual&FORMAT=JSON").c_str(), json_buf, JSON_BUF,
                       "satcat bulk");
    int n = len > 0 ? owner_parse(json_buf, len) : -1;
    owner_bulk_next = now + (n > 0 ? 86400 : 900);
    if (n > 0)
      ESP_LOGI(TAG, "satcat: %d owners cached", n);
  }
  if (now < owner_backoff)
    return;
  int budget = OWNER_LOOKUPS;
  for (const auto &s : sats) {
    if (budget <= 0)
      break;
    if (owners.find(s.id) != owners.end())
      continue;
    budget--;
    char url[96];
    snprintf(url, sizeof(url), "%s/satcat/records.php?CATNR=%ld&FORMAT=JSON", celestrak_base.c_str(), (long) s.id);
    int len = http_get(url, json_buf, JSON_BUF, "satcat");
    if (len < 0) {
      owner_backoff = now + 300;
      return;
    }
    if (owner_parse(json_buf, len) <= 0 || owners.find(s.id) == owners.end())
      owner_put(s.id, "");  // CelesTrak answers plain text for an unknown number
  }
}

inline uint16_t launched_of(int32_t id) {
  auto it = launch_days.find(id);
  return it != launch_days.end() ? it->second : 0;
}
inline void owner_of(int32_t id, char out[6]) {
  auto it = owners.find(id);
  if (it != owners.end())
    memcpy(out, it->second.data(), 6);
  else
    out[0] = 0;
}

// ================================================================== downloads (DATA-1)
inline void hold_save(double until);  // DATA-12, below with the cache
inline void set_error(const HttpResult &r, const char *what, double now) {
  if (r.status == 0)
    snprintf(status.error, sizeof(status.error), "Can't reach CelesTrak (%s)", what);
  else if (r.status == 403)
    snprintf(status.error, sizeof(status.error), "CelesTrak refused the %s download (HTTP 403)", what);
  else if (r.status != 200)
    snprintf(status.error, sizeof(status.error), "CelesTrak %s download failed (HTTP %d)", what, r.status);
  else
    snprintf(status.error, sizeof(status.error), "CelesTrak %s download was cut short", what);
  status.next_try = now + (r.status == 403 ? RETRY_403_S : RETRY_S);
  if (r.status == 403)
    hold_save(status.next_try);  // DATA-12: a reboot must not retry inside the 2 h window
}

// Download one GP group into `rows`. Returns false (with status.error set) on failure;
// the caller keeps its previous element sets.
inline bool download(const char *url, const char *what, const std::function<void(const Omm &)> &row, double now) {
  int n = 0, bad = 0;
  LineSplitter ls;
  ls.on_line = [&](char *line) {
    if (line[0] == 0)
      return;
    Omm o;
    if (parse_omm(line, o)) {
      row(o);
      n++;
    } else if (strncmp(line, "OBJECT_NAME", 11) != 0) {
      bad++;
    }
  };
  HttpResult r = http_stream(url, what, [&](const char *d, int len) { ls.feed(d, len); });
  ls.finish();
  if (!r.ok) {
    set_error(r, what, now);
    return false;
  }
  if (n == 0) {
    snprintf(status.error, sizeof(status.error), "CelesTrak sent no %s data", what);
    status.next_try = now + RETRY_S;
    return false;
  }
  ESP_LOGI(TAG, "%s: %d element sets (%ld bytes, %d unreadable rows)", what, n, r.bytes, bad);
  return true;
}

inline void update_oldest() {
  double oldest = 0;
  auto take = [&](double e) {
    if (oldest == 0 || e < oldest)
      oldest = e;
  };
  for (const auto &s : sats)
    take(s.epoch);
  if (have_iss)
    take(iss.epoch);
  for (const auto &s : starlink)
    take(s.epoch);
  status.oldest_epoch = oldest;
  double dt = 0;
  auto grp = [&](bool used, double t) {
    if (used && (dt == 0 || t < dt))
      dt = t;
  };
  grp(have_iss, iss_loaded);
  grp(!sats.empty(), sats_loaded);
  grp(!starlink.empty(), starlink_loaded);
  grp(!meo_sats.empty(), meo_loaded);
  grp(!geo_sats.empty(), geo_loaded);
  status.data_time = dt;
}

inline void publish_status() {
  status.n_sats = (int) (sats.size() + meo_sats.size() + geo_sats.size());
  status.n_starlink = (int) starlink.size();
  status.have_iss = have_iss;
  xSemaphoreTake(mutex, portMAX_DELAY);
  pending.status = status;
  pending.fresh |= L_STATUS;
  xSemaphoreGive(mutex);
}

inline void do_pass();


// ================================================================== flash cache (DATA-10)
// Downloaded element sets are kept in the "skydata" flash partition so a reboot or an
// update doesn't download them again (CelesTrak refuses repeats within 2 h). Two
// regions, each a 4 KB header sector followed by the payload; the header is written
// last, so a write cut short by a reset leaves an invalid header, never bad data.
//   A: ISS + visual satellites + owner codes     B: Starlink
// Without the partition (older partition table) the cache is simply off.
constexpr uint32_t CACHE_MAGIC = 0x534B5943;  // "SKYC"
constexpr uint32_t CACHE_VER = 2;  // 2: SgpSat gained class/debris/tag
constexpr uint32_t CACHE_A_OFF = 0x000000, CACHE_A_MAX = 0x0C0000;
constexpr uint32_t CACHE_B_OFF = 0x100000, CACHE_B_MAX = 0x200000;
constexpr uint32_t CACHE_C_OFF = 0x300000, CACHE_C_MAX = 0x040000;  // GNSS (MEO)
constexpr uint32_t CACHE_D_OFF = 0x340000, CACHE_D_MAX = 0x0C0000;  // geosynchronous
constexpr uint32_t CACHE_HDR = 0x1000;
constexpr uint32_t CACHE_HOLD_OFF = 0x0F0000;  // DATA-12: one sector in the gap after region A
constexpr uint32_t HOLD_MAGIC = 0x534B5948;    // "SKYH"
struct HoldRec {
  uint32_t magic;
  uint32_t crc;
  double until;  // UTC: no CelesTrak download before this
};
struct CacheHdr {
  uint32_t magic, ver, rec_size, n, n2, flags;  // flags bit0: have_iss
  double loaded, loaded2;                       // A: sats, ISS   B: Starlink
  uint32_t group_hash, payload_len, crc, pad;
};
struct OwnerRec {
  int32_t id;
  char cc[6];
  uint16_t launched;  // UI-24b (was padding: older caches read as 0 = unknown)
};
inline const esp_partition_t *cache_part = nullptr;
inline size_t cache_owner_count = 0;  // owners saved last time (A is rewritten when it grows)

inline uint32_t fnv(const std::string &s) {
  uint32_t h = 2166136261u;
  for (char c : s)
    h = (h ^ (uint8_t) c) * 16777619u;
  return h;
}

inline bool cache_write(uint32_t off, uint32_t max, CacheHdr h,
                        const std::function<bool(uint32_t &pos, uint32_t &crc)> &payload) {
  if (cache_part == nullptr)
    return false;
  const uint32_t len = CACHE_HDR + h.payload_len;
  if (len > max) {
    ESP_LOGW(TAG, "cache: %u bytes won't fit (%u)", (unsigned) len, (unsigned) max);
    return false;
  }
  const uint32_t erase = (len + 0xFFF) & ~0xFFFu;
  if (esp_partition_erase_range(cache_part, off, erase) != ESP_OK)
    return false;
  uint32_t pos = off + CACHE_HDR, crc = 0;
  if (!payload(pos, crc))
    return false;
  h.crc = crc;
  return esp_partition_write(cache_part, off, &h, sizeof(h)) == ESP_OK;
}
inline bool cache_put(uint32_t &pos, uint32_t &crc, const void *d, size_t n) {
  if (n == 0)
    return true;
  crc = esp_rom_crc32_le(crc, (const uint8_t *) d, n);
  const bool ok = esp_partition_write(cache_part, pos, d, n) == ESP_OK;
  pos += n;
  return ok;
}
inline bool cache_get(uint32_t &pos, uint32_t &crc, void *d, size_t n) {
  if (n == 0)
    return true;
  if (esp_partition_read(cache_part, pos, d, n) != ESP_OK)
    return false;
  crc = esp_rom_crc32_le(crc, (const uint8_t *) d, n);
  pos += n;
  return true;
}

inline void cache_save_a() {
  if (cache_part == nullptr || (!have_iss && sats.empty()))
    return;
  pvector<OwnerRec> own;
  own.reserve(owners.size());
  for (const auto &kv : owners) {
    OwnerRec r{};
    r.id = kv.first;
    memcpy(r.cc, kv.second.data(), 6);
    r.launched = launched_of(kv.first);
    own.push_back(r);
  }
  CacheHdr h{};
  h.magic = CACHE_MAGIC;
  h.ver = CACHE_VER;
  h.rec_size = sizeof(SgpSat);
  h.n = sats.size();
  h.n2 = own.size();
  h.flags = have_iss ? 1 : 0;
  h.loaded = sats_loaded;
  h.loaded2 = iss_loaded;
  h.group_hash = fnv(jc.sat_group);
  h.payload_len = sizeof(SgpSat) * (1 + h.n) + sizeof(OwnerRec) * h.n2;
  const bool ok = cache_write(CACHE_A_OFF, CACHE_A_MAX, h, [&](uint32_t &pos, uint32_t &crc) {
    return cache_put(pos, crc, &iss, sizeof(SgpSat)) && cache_put(pos, crc, sats.data(), sizeof(SgpSat) * h.n) &&
           cache_put(pos, crc, own.data(), sizeof(OwnerRec) * h.n2);
  });
  cache_owner_count = own.size();
  ESP_LOGI(TAG, "cache: saved ISS%s, %u satellites, %u owners%s", have_iss ? "" : " (none)", (unsigned) h.n,
           (unsigned) h.n2, ok ? "" : " - FAILED");
}

inline void cache_save_b() {
  if (cache_part == nullptr || starlink.empty())
    return;
  CacheHdr h{};
  h.magic = CACHE_MAGIC;
  h.ver = CACHE_VER;
  h.rec_size = sizeof(SlElem);
  h.n = starlink.size();
  h.loaded = starlink_loaded;
  h.payload_len = sizeof(SlElem) * h.n;
  const bool ok = cache_write(CACHE_B_OFF, CACHE_B_MAX, h, [&](uint32_t &pos, uint32_t &crc) {
    return cache_put(pos, crc, starlink.data(), h.payload_len);
  });
  ESP_LOGI(TAG, "cache: saved %u Starlink%s", (unsigned) h.n, ok ? "" : " - FAILED");
}

// C/D: a plain list of element sets.
inline void cache_save_list(uint32_t off, uint32_t max, const pvector<SgpSat> &v, double loaded, const char *what) {
  if (cache_part == nullptr || v.empty())
    return;
  CacheHdr h{};
  h.magic = CACHE_MAGIC;
  h.ver = CACHE_VER;
  h.rec_size = sizeof(SgpSat);
  h.n = v.size();
  h.loaded = loaded;
  h.payload_len = sizeof(SgpSat) * h.n;
  const bool ok = cache_write(off, max, h, [&](uint32_t &pos, uint32_t &crc) {
    return cache_put(pos, crc, v.data(), h.payload_len);
  });
  ESP_LOGI(TAG, "cache: saved %u %s%s", (unsigned) h.n, what, ok ? "" : " - FAILED");
}

inline bool cache_header(uint32_t off, uint32_t rec, CacheHdr &h);
inline bool cache_load_list(uint32_t off, uint32_t max, pvector<SgpSat> &v, double &loaded, const char *what) {
  CacheHdr h;
  if (!cache_header(off, sizeof(SgpSat), h) || h.payload_len > max || h.payload_len != sizeof(SgpSat) * h.n)
    return false;
  if (!room_for(v, h.n, what))
    return false;
  v.resize(h.n);  // FAIL-12: straight into the reserved list
  uint32_t pos = off + CACHE_HDR, crc = 0;
  if (!cache_get(pos, crc, v.data(), h.payload_len) || crc != h.crc) {
    v.clear();
    ESP_LOGW(TAG, "cache: %s damaged; ignored", what);
    return false;
  }
  loaded = h.loaded;
  ESP_LOGI(TAG, "cache: loaded %u %s", (unsigned) v.size(), what);
  return true;
}

inline bool cache_header(uint32_t off, uint32_t rec, CacheHdr &h) {
  return esp_partition_read(cache_part, off, &h, sizeof(h)) == ESP_OK && h.magic == CACHE_MAGIC &&
         h.ver == CACHE_VER && h.rec_size == rec;
}

// At task start: find the partition and load whatever is valid.
// DATA-12: the 403 back-off survives a reboot. Written only on a 403, so rarely.
inline void hold_save(double until) {
  if (cache_part == nullptr)
    return;
  HoldRec h{HOLD_MAGIC, 0, until};
  h.crc = esp_rom_crc32_le(0, (const uint8_t *) &h.until, sizeof(h.until));
  if (esp_partition_erase_range(cache_part, CACHE_HOLD_OFF, 0x1000) == ESP_OK &&
      esp_partition_write(cache_part, CACHE_HOLD_OFF, &h, sizeof(h)) == ESP_OK)
    ESP_LOGI(TAG, "cache: CelesTrak hold saved");
}
inline double hold_load() {
  HoldRec h{};
  if (cache_part == nullptr || esp_partition_read(cache_part, CACHE_HOLD_OFF, &h, sizeof(h)) != ESP_OK ||
      h.magic != HOLD_MAGIC || h.crc != esp_rom_crc32_le(0, (const uint8_t *) &h.until, sizeof(h.until)))
    return 0;
  return h.until;
}

// FAIL-12b: what each list holds in the cache (0 if none), so it is reserved at its real size
inline uint32_t cache_count(uint32_t off, uint32_t rec) {
  CacheHdr h;
  return cache_part && cache_header(off, rec, h) ? h.n : 0;
}
inline void reserve_lists() {  // FAIL-12: at task start, before PSRAM is cut up; the layers that are on,
  // at what the cache holds plus a margin (a list switched on later grows through room_for)
  auto room = [](size_t have, size_t margin, size_t cap, size_t none) {
    return std::min(cap, have ? have + margin : none);
  };
  if (cfg.sats_on || cfg.debris_on)
    sats.reserve(room(cache_count(CACHE_A_OFF, sizeof(SgpSat)), 48, SAT_CAP, 200));
  if (cfg.meo_on)
    meo_sats.reserve(room(cache_count(CACHE_C_OFF, sizeof(SgpSat)), 24, MEO_CAP, 160));
  if (cfg.geo_on)
    geo_sats.reserve(room(cache_count(CACHE_D_OFF, sizeof(SgpSat)), 48, GEO_CAP, 600));
  if (cfg.starlink_on)
    starlink.reserve(room(cache_count(CACHE_B_OFF, sizeof(SlElem)), 800, STARLINK_CAP, 12000));
}
inline void cache_load() {
  cache_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x40, "skydata");
  reserve_lists();
  if (cache_part == nullptr) {
    ESP_LOGW(TAG, "cache: no skydata partition (flash the new partition table over USB); cache off");
    return;
  }
  // DATA-12: still inside a CelesTrak hold from before the reboot? (UTC, compared once the clock is set)
  // (TEST-1: a hold is celestrak.org's; a test server pointed at by celestrak_base has none)
  if (const double hold = celestrak_base == "https://celestrak.org" ? hold_load() : 0; hold > status.next_try) {
    status.next_try = hold;
    ESP_LOGI(TAG, "cache: CelesTrak hold until %.0f carried over the reboot", hold);
  }
  bool any = false;
  CacheHdr h;
  if (cache_header(CACHE_A_OFF, sizeof(SgpSat), h) && h.payload_len <= CACHE_A_MAX &&
      h.payload_len == sizeof(SgpSat) * (1 + h.n) + sizeof(OwnerRec) * h.n2) {
    const bool same_group = h.group_hash == fnv(cfg.sat_group);
    const bool want = same_group && (cfg.sats_on || cfg.debris_on) && h.n;
    pvector<SgpSat> unused;  // read for the CRC when the list isn't wanted
    pvector<SgpSat> &s = want ? sats : unused;  // FAIL-12: straight into the reserved list
    pvector<OwnerRec> own;
    SgpSat i;
    s.resize(h.n);
    own.resize(h.n2);
    uint32_t pos = CACHE_A_OFF + CACHE_HDR, crc = 0;
    if (cache_get(pos, crc, &i, sizeof(i)) && cache_get(pos, crc, s.data(), sizeof(SgpSat) * h.n) &&
        cache_get(pos, crc, own.data(), sizeof(OwnerRec) * h.n2) && crc == h.crc) {
      if (h.flags & 1) {
        iss = i;
        have_iss = true;
        iss_loaded = h.loaded2;
      }
      if (want)
        sats_loaded = h.loaded;
      bool dated = false;
      for (const auto &r : own) {
        std::array<char, 6> v{};
        memcpy(v.data(), r.cc, 6);
        owners[r.id] = v;
        if (r.launched) {
          launch_days[r.id] = r.launched;
          dated = true;
        }
      }
      cache_owner_count = own.size();
      // the bulk list came with these; a cache from before launch dates asks for it once
      owner_bulk_next = dated || own.empty() ? h.loaded + 86400 : 0;
      any = true;
      ESP_LOGI(TAG, "cache: loaded ISS, %u satellites%s, %u owners", (unsigned) sats.size(),
               same_group ? "" : " (group changed: skipped)", (unsigned) own.size());
    } else {
      sats.clear();
      ESP_LOGW(TAG, "cache: region A damaged; ignored");
    }
  }
  if (cfg.starlink_on && cache_header(CACHE_B_OFF, sizeof(SlElem), h) && h.payload_len <= CACHE_B_MAX &&
      h.payload_len == sizeof(SlElem) * h.n) {
    starlink.resize(h.n);  // FAIL-12: straight into the reserved list
    uint32_t pos = CACHE_B_OFF + CACHE_HDR, crc = 0;
    if (cache_get(pos, crc, starlink.data(), h.payload_len) && crc == h.crc) {
      starlink_loaded = h.loaded;
      any = true;
      ESP_LOGI(TAG, "cache: loaded %u Starlink", (unsigned) starlink.size());
    } else {
      starlink.clear();
      ESP_LOGW(TAG, "cache: region B damaged; ignored");
    }
  }
  if (cfg.meo_on)
    any |= cache_load_list(CACHE_C_OFF, CACHE_C_MAX, meo_sats, meo_loaded, "GNSS");
  if (cfg.geo_on)
    any |= cache_load_list(CACHE_D_OFF, CACHE_D_MAX, geo_sats, geo_loaded, "GEO");
  if (any) {
    status.loaded = std::max({iss_loaded, sats_loaded, starlink_loaded, meo_loaded, geo_loaded});
    update_oldest();
    publish_status();
  }
}

// DATA-10: a layer switched on after boot (its list was not loaded then) is filled from
// flash at once, before any download: turning MEO on shows the GNSS satellites within a
// second instead of after four CelesTrak downloads. Stale lists still refresh on DATA-2.
inline bool cache_load_sats_only() {
  CacheHdr h;
  if (!cache_header(CACHE_A_OFF, sizeof(SgpSat), h) || h.payload_len > CACHE_A_MAX || h.n == 0 ||
      h.payload_len != sizeof(SgpSat) * (1 + h.n) + sizeof(OwnerRec) * h.n2 || h.group_hash != fnv(jc.sat_group))
    return false;
  if (!room_for(sats, h.n, "satellites"))
    return false;
  SgpSat i;
  sats.resize(h.n);  // FAIL-12: straight into the reserved list
  uint32_t pos = CACHE_A_OFF + CACHE_HDR, crc = 0;
  pvector<OwnerRec> own;  // read for the CRC only; the owners were loaded at boot
  own.resize(h.n2);
  if (!cache_get(pos, crc, &i, sizeof(i)) || !cache_get(pos, crc, sats.data(), sizeof(SgpSat) * h.n) ||
      !cache_get(pos, crc, own.data(), sizeof(OwnerRec) * h.n2) || crc != h.crc) {
    sats.clear();
    return false;
  }
  sats_loaded = h.loaded;
  ESP_LOGI(TAG, "cache: loaded %u satellites on demand", (unsigned) sats.size());
  return true;
}
inline bool cache_load_starlink() {
  CacheHdr h;
  if (!cache_header(CACHE_B_OFF, sizeof(SlElem), h) || h.payload_len > CACHE_B_MAX ||
      h.payload_len != sizeof(SlElem) * h.n)
    return false;
  if (!room_for(starlink, h.n, "Starlink"))
    return false;
  starlink.resize(h.n);  // FAIL-12: straight into the reserved list
  uint32_t pos = CACHE_B_OFF + CACHE_HDR, crc = 0;
  if (!cache_get(pos, crc, starlink.data(), h.payload_len) || crc != h.crc) {
    starlink.clear();
    return false;
  }
  starlink_loaded = h.loaded;
  ESP_LOGI(TAG, "cache: loaded %u Starlink on demand", (unsigned) starlink.size());
  return true;
}
inline double geo_next = 0;
inline bool cache_fill_missing() {
  if (cache_part == nullptr)
    return false;
  static double tried[4] = {0, 0, 0, 0};  // a region with nothing in it is re-read at most once a minute
  const double now = clock_now();
  auto due = [&](int k) {
    if (now - tried[k] < 60 && tried[k] != 0)
      return false;
    tried[k] = now;
    return true;
  };
  bool any = false;
  if ((jc.sats_on || jc.debris_on) && sats.empty() && due(0))
    any |= cache_load_sats_only();
  if (jc.starlink_on && starlink.empty() && due(1))
    any |= cache_load_starlink();
  if (jc.meo_on && meo_sats.empty() && due(2))
    any |= cache_load_list(CACHE_C_OFF, CACHE_C_MAX, meo_sats, meo_loaded, "GNSS");
  if (jc.geo_on && geo_sats.empty() && due(3)) {
    any |= cache_load_list(CACHE_D_OFF, CACHE_D_MAX, geo_sats, geo_loaded, "GEO");
    geo_next = 0;
  }
  if (any) {
    status.loaded = std::max({iss_loaded, sats_loaded, starlink_loaded, meo_loaded, geo_loaded});
    update_oldest();
    publish_status();
  }
  return any;
}

constexpr double GEO_EVERY_S = 300;  // UI-28: GEO positions are recomputed this often
inline void geo_force() { geo_next = 0; }

// UI-37: planetary Kp (observed + 3-day forecast) from NOAA SWPC, ~6 KB of JSON:
//   [{"time_tag":"2026-09-19T00:00:00","kp":2.33,"observed":"observed","noaa_scale":null},...]
// The older array-of-arrays form ([["time_tag","kp","observed",...],["2026-...","2.33",...]])
// is read as well. Returns the number of points.
inline size_t parse_kp(const char *js, pvector<KpPt> &out) {
  out.clear();
  auto parse_time = [](const char *q, int64_t &t) {
    int y, mo, d, h, mi, se;
    if (sscanf(q, "%d-%d-%d%*c%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
      return false;
    t = days_from_civil(y, (unsigned) mo, (unsigned) d) * 86400 + h * 3600 + mi * 60 + se;
    return true;
  };
  const char *p = js;
  if (strstr(js, "\"time_tag\":")) {  // object form
    while ((p = strstr(p, "\"time_tag\":")) != nullptr) {
      p += 11;
      while (*p == ' ' || *p == '"')
        p++;
      KpPt k;
      const char *next = strstr(p, "\"time_tag\":");
      const char *kq = strstr(p, "\"kp\":");
      const char *oq = strstr(p, "\"observed\":");
      if (!parse_time(p, k.t) || kq == nullptr || (next && kq > next))
        continue;
      kq += 5;
      while (*kq == ' ' || *kq == '"')
        kq++;
      k.kp = strtof(kq, nullptr);
      k.predicted = oq && (!next || oq < next) && strncmp(oq + 11, "\"predicted", 10) == 0;
      if (k.kp >= 0 && k.kp <= 9.5f)
        out.push_back(k);
    }
  } else {  // array form: rows of strings, header row first
    while ((p = strstr(p, "[\"")) != nullptr) {
      p += 2;
      KpPt k;
      if (!parse_time(p, k.t))
        continue;
      const char *c1 = strstr(p, "\",\"");
      if (c1 == nullptr)
        break;
      k.kp = strtof(c1 + 3, nullptr);
      const char *c2 = strstr(c1 + 3, "\",\"");
      k.predicted = c2 && strncmp(c2 + 3, "predicted", 9) == 0;
      if (k.kp >= 0 && k.kp <= 9.5f)
        out.push_back(k);
    }
  }
  return out.size();
}

constexpr double KP_EVERY_S = 3 * 3600.0, KP_RETRY_S = 1800.0;
constexpr int KP_SEEN_MAX = 64;
inline KpPt kp_seen[KP_SEEN_MAX];  // task-side copy of the last forecast (UI-58)
inline int kp_seen_n = 0;
inline double kp_next = 0;
inline void do_kp(double now) {
  if (now < kp_next)
    return;
  kp_next = now + KP_RETRY_S;
  const int len = http_get("https://services.swpc.noaa.gov/products/noaa-planetary-k-index-forecast.json", json_buf,
                           JSON_BUF, "Kp");
  if (len <= 0)
    return;
  pvector<KpPt> k;
  if (parse_kp(json_buf, k) == 0) {
    ESP_LOGW(TAG, "Kp: no values in %d bytes", len);
    return;
  }
  kp_next = now + KP_EVERY_S;
  ESP_LOGI(TAG, "Kp: %u values", (unsigned) k.size());
  kp_seen_n = 0;  // UI-58: kept for the aurora download gate
  for (const auto &x : k)
    if (kp_seen_n < KP_SEEN_MAX)
      kp_seen[kp_seen_n++] = x;
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.kp, k);
  pending.fresh |= L_KP;
  xSemaphoreGive(mutex);
}


// ------------------------------------------------------------------ DATA-13 SatNOGS fallback
// CelesTrak often refuses downloads (HTTP 403, then a 2 h hold). The ISS and Tiangong
// elements then come from the SatNOGS DB (TLE, relayed from Space-Track).
inline double tle_num(const char *s, int a, int b) {  // columns a..b (1-based) as a number
  char t[24];
  const int n = std::min(b - a + 1, (int) sizeof(t) - 1);
  memcpy(t, s + a - 1, n);
  t[n] = 0;
  return atof(t);
}
inline double tle_exp(const char *s, int a) {  // " 11922-3" style: 0.11922e-3 (8 columns)
  char m[8], e[3];
  memcpy(m, s + a - 1, 6);
  m[6] = 0;
  memcpy(e, s + a + 5, 2);
  e[2] = 0;
  const double mant = atof(m) * 1e-5;  // sign + 5 digits with an implied leading decimal point
  return mant * pow(10.0, atoi(e));
}
inline bool tle_to_omm(const char *l0, const char *l1, const char *l2, Omm &o) {
  if (strlen(l1) < 69 || strlen(l2) < 63 || l1[0] != '1' || l2[0] != '2')
    return false;
  memset(&o, 0, sizeof(o));
  const char *nm = (l0 && l0[0] == '0' && l0[1] == ' ') ? l0 + 2 : (l0 ? l0 : "");
  copy_cstr(o.name, sizeof(o.name), nm);
  o.id = (int32_t) tle_num(l1, 3, 7);
  // international designator "98067A  " -> "1998-067A"
  char yy[3] = {l1[9], l1[10], 0}, rest[7];
  memcpy(rest, l1 + 11, 6);
  rest[6] = 0;
  for (int i = 5; i >= 0 && rest[i] == ' '; i--)
    rest[i] = 0;
  const int y2 = atoi(yy);
  snprintf(o.intl, sizeof(o.intl), "%04u-%.3s%.3s", (unsigned) (y2 < 57 ? 2000 + y2 : 1900 + y2) % 10000u, rest, rest + 3);
  const int ey = (int) tle_num(l1, 19, 20);
  const double eday = tle_num(l1, 21, 32);
  o.epoch = (double) days_from_civil(ey < 57 ? 2000 + ey : 1900 + ey, 1, 1) * 86400.0 + (eday - 1.0) * 86400.0;
  o.ndot = tle_num(l1, 34, 43);
  o.nddot = tle_exp(l1, 45);
  o.bstar = tle_exp(l1, 54);
  o.incl = tle_num(l2, 9, 16);
  o.raan = tle_num(l2, 18, 25);
  char ec[10] = "0.";
  memcpy(ec + 2, l2 + 26, 7);
  ec[9] = 0;
  o.ecc = atof(ec);
  o.argp = tle_num(l2, 35, 42);
  o.ma = tle_num(l2, 44, 51);
  o.n_revday = tle_num(l2, 53, 63);
  return o.id > 0 && o.n_revday > 0.5 && o.ecc >= 0 && o.ecc < 1;
}
inline bool satnogs_elements(int32_t id, const char *what, SgpSat &out) {
  char url[96];
  snprintf(url, sizeof(url), "https://db.satnogs.org/api/tle/?norad_cat_id=%ld", (long) id);
  const int len = http_get(url, json_buf, JSON_BUF, what);
  if (len <= 0)
    return false;
  JsonDocument doc(&psram_alloc);
  if (deserializeJson(doc, json_buf, (size_t) len))
    return false;
  JsonObject t = doc[0];
  Omm o;
  if (t.isNull() || !tle_to_omm(t["tle0"] | "", t["tle1"] | "", t["tle2"] | "", o))
    return false;
  return make_sgp(o, out);
}
inline double fallback_next = 0;
inline bool iss_from_fallback = false;
// Refresh the stations from SatNOGS while CelesTrak is refusing (at most hourly).
inline bool stations_fallback(double now) {
  if (now < fallback_next)
    return false;
  const bool iss_due = !have_iss || now - iss_loaded >= ELEM_REFRESH_S;
  const bool css_due = !have_css || now - css_loaded >= ELEM_REFRESH_S;
  if (!iss_due && !css_due)
    return false;
  fallback_next = now + 3600;
  bool any = false;
  SgpSat s;
  if (iss_due && satnogs_elements(25544, "ISS (SatNOGS)", s) && (!have_iss || s.epoch > iss.epoch)) {
    copy_cstr(s.name, sizeof(s.name), "ISS");
    iss = s;
    have_iss = true;
    iss_loaded = now;
    iss_from_fallback = true;
    any = true;
    ESP_LOGI(TAG, "ISS elements from SatNOGS (CelesTrak unavailable)");
  }
  if (css_due && satnogs_elements(CSS_ID, "Tiangong (SatNOGS)", s) && (!have_css || s.epoch > css.epoch)) {
    copy_cstr(s.name, sizeof(s.name), "Tiangong");
    css = s;
    have_css = true;
    css_loaded = now;
    any = true;
  }
  return any;
}

// ------------------------------------------------------------------ UI-55 solar wind
// NOAA SWPC real-time solar wind, newest first; only the first samples from the active
// spacecraft are read (the files hold a day), then the download is cut short.
struct FlatObjScanner {  // splits a stream of flat JSON objects ({...},{...}) into objects
  char buf[1400];
  int n = -1;  // -1: outside an object
  std::function<bool(const char *)> on_obj;  // returns true when enough has been read
  bool done = false;
  void feed(const char *d, int len) {
    for (int i = 0; i < len && !done; i++) {
      const char c = d[i];
      if (n < 0) {
        if (c == '{')
          n = 0;
        continue;
      }
      if (c == '}') {
        buf[n] = 0;
        n = -1;
        done = on_obj(buf);
        continue;
      }
      if (n < (int) sizeof(buf) - 1)
        buf[n++] = c;
    }
  }
};
inline bool json_num(const char *obj, const char *key, float &v) {  // "key": 1.23 (not null)
  const char *p = strstr(obj, key);
  if (!p)
    return false;
  p = strchr(p + strlen(key), ':');
  if (!p)
    return false;
  while (*++p == ' ') {
  }
  if (*p == 'n')
    return false;  // null
  v = (float) atof(p);
  return true;
}
constexpr double WIND_EVERY_S = 600, WIND_SAMPLES = 15;
// UI-55: Bz south (negative) with a fast wind couples the solar wind to the magnetosphere
constexpr float WIND_BZ_SOUTH = -5.0f, WIND_BZ_STRONG = -10.0f, WIND_FAST = 450.0f;
inline bool wind_warns(const SolarWind &w, double t) {
  if (w.t <= 0 || t - w.t >= 1800 || std::isnan(w.bz))
    return false;
  const float v = std::isnan(w.speed) ? 400.0f : w.speed;
  return w.bz <= WIND_BZ_STRONG || (w.bz <= WIND_BZ_SOUTH && v >= WIND_FAST);
}
inline SolarWind wind_seen;  // task-side copy (UI-58 gate)
inline double wind_next = 0;
inline void do_wind(double now) {
  if (now < wind_next)
    return;
  wind_next = now + WIND_EVERY_S;
  SolarWind w;
  auto grab = [&](const char *url, const char *what, const char *k1, float &v1, const char *k2, float &v2) {
    float s1 = 0, s2 = 0;
    int n1 = 0, n2 = 0;
    FlatObjScanner sc;
    sc.on_obj = [&](const char *o) {
      if (!strstr(o, "\"active\": true") && !strstr(o, "\"active\":true"))
        return false;
      float a;
      if (json_num(o, k1, a)) {
        s1 += a;
        n1++;
        if (w.t == 0) {
          const char *tt = strstr(o, "\"time_tag\"");
          double ts;
          if (tt && (tt = strchr(tt + 10, '"')) && parse_epoch(tt + 1, ts))
            w.t = ts;
        }
      }
      if (json_num(o, k2, a)) {
        s2 += a;
        n2++;
      }
      return n1 >= WIND_SAMPLES;
    };
    const HttpResult r = http_stream(url, what, [&](const char *d, int len) { sc.feed(d, len); }, &sc.done);
    if (r.ok && n1)
      v1 = s1 / n1;
    if (r.ok && n2)
      v2 = s2 / n2;
  };
  grab("https://services.swpc.noaa.gov/json/rtsw/rtsw_mag_1m.json", "solar wind (mag)", "\"bz_gsm\"", w.bz, "\"bt\"", w.bt);
  const double t_mag = w.t;
  grab("https://services.swpc.noaa.gov/json/rtsw/rtsw_wind_1m.json", "solar wind (plasma)", "\"proton_speed\"", w.speed,
       "\"proton_density\"", w.density);
  if (t_mag > 0)
    w.t = t_mag;
  if (std::isnan(w.bz) && std::isnan(w.speed))
    return;
  ESP_LOGI(TAG, "solar wind: Bz %.1f nT, %.0f km/s", w.bz, w.speed);
  wind_seen = w;
  xSemaphoreTake(mutex, portMAX_DELAY);
  pending.wind = w;
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}

// ------------------------------------------------------------------ UI-54 launches
constexpr double LAUNCH_EVERY_S = 3 * 3600.0, LAUNCH_RETRY_S = 1800.0;
constexpr size_t LAUNCH_BUF = 400 * 1024;
inline double launch_next = 0;
// UI-54a: the last list is kept in flash, one sector in the gap after region A, so a reboot
// shows the launches at once and does not spend one of Launch Library's 15 free calls an
// hour (a night of reboots got "HTTP 429" and an empty launch list).
constexpr uint32_t CACHE_LAUNCH_OFF = 0x0F1000;
constexpr uint32_t LAUNCH_MAGIC = 0x534B594C;  // "SKYL"
constexpr int LAUNCH_KEEP = 8;
struct LaunchCache {
  uint32_t magic, n, rec_size, crc;
  double saved;  // UTC
  LaunchRec rec[LAUNCH_KEEP];
};
inline uint32_t launch_crc(const LaunchCache &c) {
  return esp_rom_crc32_le(0, (const uint8_t *) &c.saved, sizeof(c.saved) + sizeof(c.rec));
}
inline void launch_cache_save(const pvector<LaunchRec> &v, double now) {
  if (cache_part == nullptr)
    return;
  auto *c = (LaunchCache *) heap_caps_malloc(sizeof(LaunchCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (c == nullptr)
    return;
  memset((void *) c, 0, sizeof(*c));
  c->magic = LAUNCH_MAGIC;
  c->rec_size = sizeof(LaunchRec);
  c->n = (uint32_t) std::min<size_t>(v.size(), LAUNCH_KEEP);
  c->saved = now;
  for (uint32_t i = 0; i < c->n; i++)
    c->rec[i] = v[i];
  c->crc = launch_crc(*c);
  if (esp_partition_erase_range(cache_part, CACHE_LAUNCH_OFF, 0x1000) != ESP_OK ||
      esp_partition_write(cache_part, CACHE_LAUNCH_OFF, c, sizeof(*c)) != ESP_OK)
    ESP_LOGW(TAG, "cache: launches not saved");
  heap_caps_free(c);
}
// at task start: the saved list, and no download until it is LAUNCH_EVERY_S old
inline void launch_cache_load() {
  if (cache_part == nullptr)
    return;
  auto *c = (LaunchCache *) heap_caps_malloc(sizeof(LaunchCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (c == nullptr)
    return;
  memset((void *) c, 0, sizeof(*c));
  if (esp_partition_read(cache_part, CACHE_LAUNCH_OFF, c, sizeof(*c)) == ESP_OK && c->magic == LAUNCH_MAGIC &&
      c->rec_size == sizeof(LaunchRec) && c->n <= (uint32_t) LAUNCH_KEEP && c->crc == launch_crc(*c)) {
    pvector<LaunchRec> out(c->rec, c->rec + c->n);
    for (LaunchRec &l : out) {  // UI-78 (a list saved by an older version)
      fold_text(l.name);
      fold_text(l.rocket);
      fold_text(l.where);
    }
    launch_next = c->saved + LAUNCH_EVERY_S;
    ESP_LOGI(TAG, "cache: loaded %u launches", (unsigned) out.size());
    xSemaphoreTake(mutex, portMAX_DELAY);
    std::swap(pending.launches, out);
    pending.fresh |= L_EXTRA;
    xSemaphoreGive(mutex);
  }
  heap_caps_free(c);
}
inline void launches_publish(pvector<LaunchRec> &out, double now, const char *src) {
  for (LaunchRec &l : out) {  // UI-78
    fold_text(l.name);
    fold_text(l.rocket);
    fold_text(l.where);
  }
  std::sort(out.begin(), out.end(), [](const LaunchRec &a, const LaunchRec &b) { return a.net < b.net; });
  launch_next = now + LAUNCH_EVERY_S;
  // UI-76: in the last 90 min before a launch (and just after it) every 15 min, the last 20 min
  // every 10, so the countdown follows holds and scrubs (Launch Library allows 15 calls an hour)
  for (const LaunchRec &l : out) {  // (not when it was refused and the backup filled in)
    if (strcmp(src, "Launch Library") != 0)
      break;
    const double dt = l.net - now;
    if (dt > -20 * 60.0 && dt < 90 * 60.0)
      launch_next = std::min(launch_next, now + (dt > 0 && dt < 20 * 60.0 ? 600.0 : 900.0));
  }
  ESP_LOGI(TAG, "launches: %u upcoming (%s)", (unsigned) out.size(), src);
  launch_cache_save(out, now);
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.launches, out);
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}
// UI-54b: pads RocketLaunch.Live names without coordinates (its free feed has none), for the
// "launch near you" alert and the bearing. Matched on the start of the location name.
struct PadPos {
  const char *name;
  float lat, lon;
};
constexpr PadPos RLL_PADS[] = {
    {"Cape Canaveral", 28.49f, -80.58f}, {"Kennedy", 28.57f, -80.65f},     {"Vandenberg", 34.73f, -120.57f},
    {"Starbase", 25.99f, -97.16f},       {"Wallops", 37.84f, -75.48f},     {"Pacific Spaceport", 57.43f, -152.34f},
    {"Kodiak", 57.43f, -152.34f},        {"Rocket Lab Launch Complex", -39.26f, 177.86f},
    {"Mahia", -39.26f, 177.86f},         {"Baikonur", 45.96f, 63.31f},     {"Plesetsk", 62.93f, 40.58f},
    {"Vostochny", 51.88f, 128.33f},      {"Jiuquan", 40.96f, 100.29f},     {"Xichang", 28.25f, 102.03f},
    {"Taiyuan", 38.85f, 111.61f},        {"Wenchang", 19.61f, 110.95f},    {"Satish Dhawan", 13.72f, 80.23f},
    {"Sriharikota", 13.72f, 80.23f},     {"Tanegashima", 30.40f, 130.97f}, {"Uchinoura", 31.25f, 131.08f},
    {"Kourou", 5.24f, -52.77f},          {"Guiana", 5.24f, -52.77f},       {"Naro", 34.43f, 127.54f},
    {"Andøya", 69.29f, 16.02f},          {"SaxaVord", 60.82f, -0.77f},
};
inline void rll_pad(const char *loc, float &lat, float &lon) {
  for (const auto &p : RLL_PADS)
    if (strncmp(loc, p.name, strlen(p.name)) == 0) {
      lat = p.lat;
      lon = p.lon;
      return;
    }
}
// "2026-10-05T08:17Z" (no seconds) as well as the ISO form parse_epoch takes
inline bool parse_epoch_min(const char *s, double &out) {
  if (parse_epoch(s, out))
    return true;
  int y, mo, d, h, mi;
  if (sscanf(s, "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5)
    return false;
  out = (double) days_from_civil(y, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0;
  return true;
}
// UI-54b backup: RocketLaunch.Live's free feed (next 5, no key). Name and status are mapped
// to Launch Library's form ("Falcon 9 | NROL-97", "Go"/"TBD"/"Success"/"Failure").
inline void launches_rll(char *buf, double now) {
  const int len = http_get("https://fdo.rocketlaunch.live/json/launches/next/5", buf, LAUNCH_BUF, "launches (backup)");
  if (len <= 0)
    return;
  JsonDocument filter;
  JsonObject f = filter["result"][0].to<JsonObject>();
  f["name"] = true;
  f["t0"] = true;
  f["win_open"] = true;
  f["sort_date"] = true;
  f["result"] = true;
  f["vehicle"]["name"] = true;
  f["pad"]["location"]["name"] = true;
  f["pad"]["location"]["state"] = true;
  f["pad"]["location"]["country"] = true;
  JsonDocument doc(&psram_alloc);
  if (const DeserializationError de = deserializeJson(doc, buf, (size_t) len, DeserializationOption::Filter(filter))) {
    ESP_LOGW(TAG, "launches (backup): unreadable reply (%d bytes, %s)", len, de.c_str());
    return;
  }
  pvector<LaunchRec> out;
  for (JsonObject r : doc["result"].as<JsonArray>()) {
    LaunchRec l;
    const char *veh = r["vehicle"]["name"] | "";
    snprintf(l.name, sizeof(l.name), "%s%s%s", veh, *veh ? " | " : "", r["name"] | "");
    copy_cstr(l.rocket, sizeof(l.rocket), veh);
    const char *loc = r["pad"]["location"]["name"] | "";
    const char *st = r["pad"]["location"]["state"] | "";
    snprintf(l.where, sizeof(l.where), "%s%s%s", loc, *st ? ", " : "", st);
    bool exact = parse_epoch_min(r["t0"] | "", l.net);
    if (!exact && !parse_epoch_min(r["win_open"] | "", l.net)) {
      JsonVariant sd = r["sort_date"];  // a rough date only ("NET October"), as text or a number
      l.net = sd.is<const char *>() ? atof(sd.as<const char *>()) : sd.as<double>();
      if (l.net <= 0)
        continue;
    }
    exact = exact || !(r["win_open"].isNull());
    const int res = r["result"] | -1;
    copy_cstr(l.status, sizeof(l.status), res == 1   ? "Success"
                                          : res == 0 ? "Failure"
                                          : res >= 2 ? "Partial F"
                                          : exact    ? "Go"
                                                     : "TBD");
    rll_pad(loc, l.lat, l.lon);
    copy_cstr(l.cc, sizeof(l.cc), rll_cc(r["pad"]["location"]["country"] | ""));  // UI-54d
    out.push_back(l);
  }
  if (!out.empty())
    launches_publish(out, now, "RocketLaunch.Live");
}
inline void do_launches(double now) {
  if (now < launch_next)
    return;
  launch_next = now + LAUNCH_RETRY_S;
  static char *buf = nullptr;
  if (buf == nullptr)
    buf = (char *) heap_caps_malloc(LAUNCH_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr)
    return;
  const int len = http_get("https://ll.thespacedevs.com/2.3.0/launches/upcoming/?limit=8&mode=normal", buf, LAUNCH_BUF,
                           "launches");
  if (len <= 0) {  // UI-54b: refused (HTTP 429: free tier used up) or unreachable
    launches_rll(buf, now);
    return;
  }
  JsonDocument filter;
  JsonObject f = filter["results"][0].to<JsonObject>();
  f["name"] = true;
  f["net"] = true;
  f["status"]["abbrev"] = true;
  f["pad"]["latitude"] = true;
  f["pad"]["longitude"] = true;
  f["pad"]["country"]["alpha_3_code"] = true;  // UI-54d
  f["pad"]["location"]["name"] = true;
  f["rocket"]["configuration"]["name"] = true;
  f["webcast_live"] = true;  // UI-76
  JsonDocument doc(&psram_alloc);
  // the 2.3.0 reply nests 11 deep (ArduinoJson stops at 10 unless told)
  if (const DeserializationError de = deserializeJson(doc, buf, (size_t) len, DeserializationOption::Filter(filter),
                                                     DeserializationOption::NestingLimit(24))) {
    ESP_LOGW(TAG, "launches: unreadable reply (%d bytes, %s)", len, de.c_str());
    return;
  }
  auto num = [](JsonVariant v) -> float {
    if (v.is<const char *>())
      return (float) atof(v.as<const char *>());
    return v.is<float>() || v.is<double>() || v.is<int>() ? v.as<float>() : NAN;
  };
  pvector<LaunchRec> out;
  for (JsonObject r : doc["results"].as<JsonArray>()) {
    LaunchRec l;
    copy_cstr(l.name, sizeof(l.name), r["name"] | "");
    copy_cstr(l.rocket, sizeof(l.rocket), r["rocket"]["configuration"]["name"] | "");
    copy_cstr(l.where, sizeof(l.where), r["pad"]["location"]["name"] | "");
    copy_cstr(l.status, sizeof(l.status), r["status"]["abbrev"] | "");
    copy_cstr(l.cc, sizeof(l.cc), launch_cc(r["pad"]["country"]["alpha_3_code"] | ""));  // UI-54d
    if (!parse_epoch(r["net"] | "", l.net))
      continue;
    l.lat = num(r["pad"]["latitude"]);
    l.lon = num(r["pad"]["longitude"]);
    l.webcast = r["webcast_live"] | false;
    out.push_back(l);
  }
  launches_publish(out, now, "Launch Library");
}

// ------------------------------------------------------------------ UI-54c space events
// Launch Library 2's upcoming events (dockings, undockings, spacecraft releases, EVAs,
// landings...), every 6 h, kept in flash like the launches so a reboot spends no call.
constexpr double EVENT_EVERY_S = 6 * 3600.0, EVENT_RETRY_S = 3600.0;
constexpr int EVENT_KEEP = 10;
constexpr uint32_t CACHE_EVENT_OFF = 0x0F2000;
constexpr uint32_t EVENT_MAGIC = 0x534B5945;  // "SKYE"
inline double event_next = 0;
struct EventCache {
  uint32_t magic, n, rec_size, crc;
  double saved;
  EventRec rec[EVENT_KEEP];
};
inline uint32_t event_crc(const EventCache &c) {
  return esp_rom_crc32_le(0, (const uint8_t *) &c.saved, sizeof(c.saved) + sizeof(c.rec));
}
inline void events_publish(pvector<EventRec> &out) {
  for (EventRec &e : out) {  // UI-78
    fold_text(e.name);
    fold_text(e.type);
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.events, out);
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}
inline void event_cache_save(const pvector<EventRec> &v, double now) {
  if (cache_part == nullptr)
    return;
  auto *c = (EventCache *) heap_caps_malloc(sizeof(EventCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (c == nullptr)
    return;
  memset((void *) c, 0, sizeof(*c));
  c->magic = EVENT_MAGIC;
  c->rec_size = sizeof(EventRec);
  c->n = (uint32_t) std::min<size_t>(v.size(), EVENT_KEEP);
  c->saved = now;
  for (uint32_t i = 0; i < c->n; i++)
    c->rec[i] = v[i];
  c->crc = event_crc(*c);
  if (esp_partition_erase_range(cache_part, CACHE_EVENT_OFF, 0x1000) != ESP_OK ||
      esp_partition_write(cache_part, CACHE_EVENT_OFF, c, sizeof(*c)) != ESP_OK)
    ESP_LOGW(TAG, "cache: events not saved");
  heap_caps_free(c);
}
inline void event_cache_load() {
  if (cache_part == nullptr)
    return;
  auto *c = (EventCache *) heap_caps_malloc(sizeof(EventCache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (c == nullptr)
    return;
  memset((void *) c, 0, sizeof(*c));
  if (esp_partition_read(cache_part, CACHE_EVENT_OFF, c, sizeof(*c)) == ESP_OK && c->magic == EVENT_MAGIC &&
      c->rec_size == sizeof(EventRec) && c->n <= (uint32_t) EVENT_KEEP && c->crc == event_crc(*c)) {
    pvector<EventRec> out(c->rec, c->rec + c->n);
    event_next = c->saved + EVENT_EVERY_S;
    ESP_LOGI(TAG, "cache: loaded %u events", (unsigned) out.size());
    events_publish(out);
  }
  heap_caps_free(c);
}
inline void do_events(double now) {
  if (now < event_next)
    return;
  event_next = now + EVENT_RETRY_S;
  static char *buf = nullptr;
  if (buf == nullptr)
    buf = (char *) heap_caps_malloc(LAUNCH_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr)
    return;
  const int len = http_get("https://ll.thespacedevs.com/2.3.0/events/upcoming/?limit=10&mode=list", buf, LAUNCH_BUF,
                           "events");
  if (len <= 0)
    return;
  JsonDocument filter;
  JsonObject f = filter["results"][0].to<JsonObject>();
  f["name"] = true;
  f["date"] = true;
  f["type"]["name"] = true;
  f["date_precision"]["abbrev"] = true;
  f["location"] = true;  // a plain string in list mode: "International Space Station"
  JsonDocument doc(&psram_alloc);
  if (const DeserializationError de = deserializeJson(doc, buf, (size_t) len, DeserializationOption::Filter(filter),
                                                     DeserializationOption::NestingLimit(24))) {
    ESP_LOGW(TAG, "events: unreadable reply (%d bytes, %s)", len, de.c_str());
    return;
  }
  pvector<EventRec> out;
  for (JsonObject r : doc["results"].as<JsonArray>()) {
    EventRec e;
    copy_cstr(e.name, sizeof(e.name), r["name"] | "");
    copy_cstr(e.type, sizeof(e.type), r["type"]["name"] | "");
    e.iss = strstr(r["location"] | "", "International Space Station") != nullptr;
    copy_cstr(e.cc, sizeof(e.cc), event_cc(e.name));  // UI-54d
    if (!parse_epoch_min(r["date"] | "", e.t))
      continue;
    const char *pr = r["date_precision"]["abbrev"] | "";
    e.exact = !strcmp(pr, "SEC") || !strcmp(pr, "MIN") || !strcmp(pr, "HR") || !*pr;
    out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](const EventRec &a, const EventRec &b) { return a.t < b.t; });
  event_next = now + EVENT_EVERY_S;
  ESP_LOGI(TAG, "events: %u upcoming", (unsigned) out.size());
  event_cache_save(out, now);
  events_publish(out);
}

// ------------------------------------------------------------------ UI-63 comets
// JPL SBDB query API: comets with a perihelion within 4 AU between 300 days ago and 500
// days ahead that have total-magnitude parameters (~40 rows, ~4 KB). Once a day; the loop
// works out which are bright enough to show.
constexpr double COMET_EVERY_S = 86400.0, COMET_RETRY_S = 3600.0;
constexpr size_t COMET_BUF = 48 * 1024, COMET_MAX = 80;
inline double comet_next = 0;
inline void do_comets(double now) {
  if (now < comet_next || now < 1.7e9)
    return;
  comet_next = now + COMET_RETRY_S;
  static char *buf = nullptr;
  if (buf == nullptr)
    buf = (char *) heap_caps_malloc(COMET_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr)
    return;
  const double jd = now / 86400.0 + 2440587.5;
  char url[400];  // sb-cdata={"AND":["q|LT|4","M1|DF","tp|RG|<jd-300>|<jd+500>"]}, percent-encoded
  snprintf(url, sizeof(url),
           "https://ssd-api.jpl.nasa.gov/sbdb_query.api?fields=full_name,e,q,tp,om,w,i,M1,K1&sb-kind=c"
           "&sb-cdata=%%7B%%22AND%%22%%3A%%5B%%22q%%7CLT%%7C4%%22%%2C%%22M1%%7CDF%%22%%2C%%22tp%%7CRG%%7C%.0f%%7C%.0f%%22%%5D%%7D",
           jd - 300, jd + 500);
  const int len = http_get(url, buf, COMET_BUF, "comets");
  if (len <= 0)
    return;
  JsonDocument doc(&psram_alloc);
  if (const DeserializationError de = deserializeJson(doc, buf, (size_t) len, DeserializationOption::NestingLimit(24))) {
    ESP_LOGW(TAG, "comets: unreadable reply (%d bytes, %s)", len, de.c_str());
    return;
  }
  auto num = [](JsonVariant v) -> double {
    if (v.is<const char *>())
      return atof(v.as<const char *>());
    return v.is<double>() || v.is<int>() ? v.as<double>() : NAN;
  };
  pvector<comets::El> out;
  for (JsonArray r : doc["data"].as<JsonArray>()) {
    if (r.size() < 9 || out.size() >= COMET_MAX)
      continue;
    comets::El c{};
    const char *nm = r[0] | "";
    while (*nm == ' ')
      nm++;
    copy_cstr(c.name, sizeof(c.name), nm);
    c.e = num(r[1]);
    c.q = num(r[2]);
    c.tp = num(r[3]);
    c.om = num(r[4]);
    c.w = num(r[5]);
    c.i = num(r[6]);
    c.M1 = (float) num(r[7]);
    c.K1 = (float) num(r[8]);
    if (!(c.q > 0) || !(c.e >= 0) || !std::isfinite(c.tp) || !std::isfinite(c.M1) || !std::isfinite(c.K1))
      continue;
    fold_text(c.name);  // UI-78
    comets::short_tag(c.name, c.tag, sizeof(c.tag));
    out.push_back(c);
  }
  comet_next = now + COMET_EVERY_S;
  ESP_LOGI(TAG, "comets: %u with magnitude laws", (unsigned) out.size());
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.comet_list, out);
  pending.comets_new = true;
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}

// ------------------------------------------------------------------ UI-73 space weather
// NOAA SWPC's alerts (products/alerts.json, ~40 KB, the last month, newest first), every 30
// min: geomagnetic storm watches (WATA: the day with the highest level), storm alerts and
// warnings (ALTK/WARK, Kp 5 and up), flare summaries (SUMX: M5 and up) and proton events
// (ALTPX). The newest of each kind that is still current is kept.
constexpr double SWX_EVERY_S = 1800, SWX_RETRY_S = 600;
inline double swx_next = 0;
inline bool swpc_time(const char *s, double &t) {  // "2026 Oct 08 1548 UTC"
  static const char *const MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int y, d, hm;
  char m[4] = "";
  if (s == nullptr || sscanf(s, " %d %3s %d %d", &y, m, &d, &hm) != 4)
    return false;
  const char *f = strstr(MON, m);
  if (!f || m[0] == 0)
    return false;
  t = (double) days_from_civil(y, (unsigned) ((f - MON) / 3 + 1), (unsigned) d) * 86400.0 + (hm / 100) * 3600.0 +
      (hm % 100) * 60.0;
  return true;
}
inline const char *after(const char *s, const char *key) {
  const char *p = strstr(s, key);
  return p ? p + strlen(key) : nullptr;
}
// one notice -> r (false: not a kind kept)
inline bool swx_parse(const char *m, double issued, SwxRec &r) {
  const char *code = after(m, "Message Code: ");
  if (code == nullptr)
    return false;
  r = SwxRec();
  r.issued = r.t = issued;
  if (!strncmp(code, "WATA", 4)) {
    const char *g = after(m, "Category G");
    if (g == nullptr)
      return false;
    r.kind = SWX_WATCH;
    r.level = (uint8_t) atoi(g);
    // "Oct 07:  None (Below G1)   Oct 08:  None (Below G1)   Oct 09:  G2 (Moderate)"
    static const char *const MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int iy, im, id;
    {
      const int64_t dn = (int64_t) floor(issued / 86400.0);  // civil from days (issue date)
      int64_t z = dn + 719468;
      const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const unsigned doe = (unsigned) (z - era * 146097);
      const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
      id = (int) (doy - (153 * mp + 2) / 5 + 1);
      im = (int) (mp < 10 ? mp + 3 : mp - 9);
      iy = (int) (yoe + era * 400 + (im <= 2));
    }
    (void) id;
    int best = 0;
    double best_day = 0;
    const char *p = after(m, "Predicted by Day:");
    for (int k = 0; p && k < 4; k++) {
      char mon[4] = "";
      int d = 0, n = 0;
      while (*p == ' ' || *p == '\r' || *p == '\n')
        p++;
      if (sscanf(p, "%3s %d:%n", mon, &d, &n) != 2)
        break;
      const char *f = strstr(MON, mon);
      if (!f || mon[0] == 0)
        break;
      p += n;
      while (*p == ' ')
        p++;
      const int lvl = *p == 'G' ? atoi(p + 1) : 0;
      const int mo = (int) ((f - MON) / 3 + 1);
      const int y = mo < im - 6 ? iy + 1 : iy;  // a watch issued in December for January
      const double day = (double) days_from_civil(y, (unsigned) mo, (unsigned) d) * 86400.0;
      if (lvl > best) {
        best = lvl;
        best_day = day;
      }
      const char *nx = strstr(p, "   ");  // the next day, after the gap
      p = nx ? nx : nullptr;
    }
    if (best > 0) {
      r.level = (uint8_t) best;
      r.t = best_day;
      r.until = best_day + 86400.0;
    } else {
      r.until = issued + 3 * 86400.0;
    }
    return true;
  }
  if (!strncmp(code, "ALTK", 4) || !strncmp(code, "WARK", 4)) {
    const int k = atoi(code + 4);
    if (k < 5)
      return false;  // below storm level (the aurora alerts cover those, UI-38)
    r.level = (uint8_t) k;
    if (code[0] == 'A') {
      r.kind = SWX_STORM;
      r.until = issued + 4 * 3600.0;
    } else {
      r.kind = SWX_WARN;
      double u = 0;
      const char *v = after(m, "Valid To:");
      if (v == nullptr)
        v = after(m, "Now Valid Until:");
      r.until = swpc_time(v, u) ? u : issued + 6 * 3600.0;
    }
    return true;
  }
  if (!strncmp(code, "SUMX", 4)) {
    r.kind = SWX_FLARE;
    const char *c = after(m, "Xray Class:");
    if (c) {
      while (*c == ' ')
        c++;
      int n = 0;
      while (n < 7 && c[n] && c[n] != ' ' && c[n] != '\r' && c[n] != '\n') {
        r.cls[n] = c[n];
        n++;
      }
      r.cls[n] = 0;
    }
    double mx;
    if (swpc_time(after(m, "Maximum Time:"), mx))
      r.t = mx;
    const char *sc = after(m, "Scale: R");
    r.level = sc ? (uint8_t) atoi(sc) : 0;
    r.until = r.t + 12 * 3600.0;
    return r.cls[0] != 0;
  }
  if (!strncmp(code, "ALTPX", 5)) {
    r.kind = SWX_PROTON;
    r.level = (uint8_t) atoi(code + 5);  // 10, 100, 1000 pfu: S1, S2, S3
    r.until = issued + 24 * 3600.0;
    return r.level > 0;
  }
  return false;
}
inline void do_swx(double now) {
  if (now < swx_next || now < 1.7e9 || json_buf == nullptr)
    return;
  swx_next = now + SWX_RETRY_S;
  const int len = http_get("https://services.swpc.noaa.gov/products/alerts.json", json_buf, JSON_BUF, "space weather");
  if (len <= 0)
    return;
  JsonDocument filter;
  filter[0]["issue_datetime"] = true;
  filter[0]["message"] = true;
  JsonDocument doc(&psram_alloc);
  if (const DeserializationError de = deserializeJson(doc, json_buf, (size_t) len, DeserializationOption::Filter(filter))) {
    ESP_LOGW(TAG, "space weather: unreadable reply (%d bytes, %s)", len, de.c_str());
    return;
  }
  SwxRec best[SWX_KINDS];
  bool have[SWX_KINDS] = {};
  for (JsonObject o : doc.as<JsonArray>()) {
    char iso[32];
    copy_cstr(iso, sizeof(iso), o["issue_datetime"] | "");
    if (char *sp = strchr(iso, ' '))
      *sp = 'T';
    double issued;
    if (!parse_epoch(iso, issued))
      continue;
    SwxRec r;
    if (!swx_parse(o["message"] | "", issued, r) || r.until <= now)
      continue;
    if (!have[r.kind] || r.issued > best[r.kind].issued) {
      best[r.kind] = r;
      have[r.kind] = true;
    }
  }
  pvector<SwxRec> out;
  for (int k = 0; k < SWX_KINDS; k++)
    if (have[k])
      out.push_back(best[k]);
  swx_next = now + SWX_EVERY_S;
  ESP_LOGI(TAG, "space weather: %u current", (unsigned) out.size());
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.swx, out);
  pending.swx_new = true;
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}

// ------------------------------------------------------------------ UI-58 OVATION aurora nowcast
// NOAA SWPC's OVATION model on a 1-degree grid, 30-90 min ahead:
//   {"Observation Time": "...", "Forecast Time": "...", ..., "coordinates": [[lon, lat, pct], ...]}
// ~0.9 MB, ordered by longitude then latitude, so the read stops once past the observer.
// Downloaded only after dusk, and only often when aurora is in play (UI-58a):
//   daylight (Sun above -6°): never; the gate is looked at again every 15 min
//   dark and quiet: every 75 min
//   dark and active (Kp near the level that reaches this sky, a solar wind warning, or
//   the last nowcast at 5% overhead / 10% in view or more): every 15 min, and at once
//   when a quiet spell turns active (a wind warning arrives 30-60 min before the aurora)
constexpr double OVATION_ACTIVE_S = 900, OVATION_QUIET_S = 4500, OVATION_RETRY_S = 600;
constexpr float OVATION_DARK_EL = -6.0f;
constexpr float OVATION_HERE_PCT = 5, OVATION_VIEW_PCT = 10;
inline float kp_gate = 3.0f;  // Kp from which the aurora reaches this sky; set by the loop
inline AuroraChance ov_seen;   // task-side copy of the last nowcast
inline bool ovation_active_last = false;
inline bool aurora_active(double now) {
  if (wind_warns(wind_seen, now))
    return true;
  for (int i = 0; i < kp_seen_n; i++)  // the interval under way and the next one
    if (kp_seen[i].t + 3 * 3600 > now && kp_seen[i].t < now + 3 * 3600 && kp_seen[i].kp >= kp_gate)
      return true;
  return ov_seen.obs > 0 && now - ov_seen.obs < 3 * 3600 &&
         (ov_seen.here >= OVATION_HERE_PCT || ov_seen.view >= OVATION_VIEW_PCT);
}
constexpr float AURORA_VIEW_KM = 800;  // aurora (~110 km up) is seen this far away, low in the sky
inline double ovation_next = 0;
struct OvationScan {
  // observer
  double lat = 0, lon = 0;  // lon 0..360
  float dlon_max = 20;
  bool wrap = false;
  // results
  double obs = 0, fc = 0;
  float q[4] = {NAN, NAN, NAN, NAN};  // the four cells around the observer
  float view = -1, view_az = NAN, view_km = 1e9;
  bool done = false;
  long cells = 0;
  // parser
  char hdr[400];
  int hn = 0;
  bool in_coords = false, in_tri = false;
  double v[3];
  int k = 0;
  char tok[16];
  int tn = 0;
  void setup(double la, double lo) {
    lat = la;
    lon = fmod(lo + 360.0, 360.0);
    const double c = cos(la * M_PI / 180.0);
    dlon_max = (float) std::min(180.0, AURORA_VIEW_KM / 111.2 / std::max(0.05, c) + 1.0);
    wrap = lon - dlon_max < 0 || lon + dlon_max + 1 >= 360;
  }
  void header_times() {
    hdr[hn] = 0;
    auto grab = [&](const char *key, double &out) {
      const char *p = strstr(hdr, key);
      if (p && (p = strchr(p + strlen(key), '"')))
        parse_epoch(p + 1, out);
    };
    grab("\"Observation Time\":", obs);
    grab("\"Forecast Time\":", fc);
  }
  void cell(double clon, double clat, float pct) {
    cells++;
    const int lo0 = (int) floor(lon), la0 = (int) floor(lat);
    const int ilon = (int) lround(clon), ilat = (int) lround(clat);
    if (ilat == la0 || ilat == la0 + 1) {
      const int dx = ilon == lo0 ? 0 : ilon == (lo0 + 1) % 360 ? 1 : -1;
      if (dx >= 0)
        q[(ilat - la0) * 2 + dx] = pct;
    }
    if (fabs(clat - lat) <= AURORA_VIEW_KM / 111.2 + 1) {
      const double r = M_PI / 180.0, p1 = lat * r, p2 = clat * r, dl = (clon - lon) * r;
      const double a = sin((p2 - p1) / 2) * sin((p2 - p1) / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
      const double km = 2 * 6371.0 * asin(std::min(1.0, sqrt(a)));
      if (km <= AURORA_VIEW_KM && (pct > view || (pct == view && km < view_km))) {  // ties: the nearest
        view = pct;
        view_km = (float) km;
        view_az = (float) fmod(atan2(sin(dl) * cos(p2), cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl)) / r + 360.0, 360.0);
      }
    }
    if (!wrap && clon > lon + dlon_max + 1)
      done = true;  // past the observer's longitudes
  }
  void feed(const char *d, int len) {
    for (int i = 0; i < len && !done; i++) {
      const char c = d[i];
      if (!in_coords) {
        if (hn < (int) sizeof(hdr) - 1)
          hdr[hn++] = c;
        if (hn >= 13 && memcmp(hdr + hn - 13, "\"coordinates\"", 13) == 0) {
          header_times();
          in_coords = true;
        }
        continue;
      }
      if (c == '[') {
        in_tri = true;
        k = tn = 0;
      } else if (c == ',' || c == ']') {
        if (in_tri && tn && k < 3) {
          tok[tn] = 0;
          v[k++] = atof(tok);
        }
        tn = 0;
        if (c == ']') {
          if (in_tri && k == 3)
            cell(v[0], v[1], (float) v[2]);
          in_tri = false;
        }
      } else if (in_tri && tn < (int) sizeof(tok) - 1 && c != ' ' && c != '\n' && c != '\r') {
        tok[tn++] = c;
      }
    }
  }
  float here() const {  // bilinear between the four cells (the nearest known one if some are missing)
    const double fx = lon - floor(lon), fy = lat - floor(lat);
    if (!std::isnan(q[0]) && !std::isnan(q[1]) && !std::isnan(q[2]) && !std::isnan(q[3]))
      return (float) ((q[0] * (1 - fx) + q[1] * fx) * (1 - fy) + (q[2] * (1 - fx) + q[3] * fx) * fy);
    const int best = (fy >= 0.5 ? 2 : 0) + (fx >= 0.5 ? 1 : 0);
    if (!std::isnan(q[best]))
      return q[best];
    for (float x : q)
      if (!std::isnan(x))
        return x;
    return NAN;
  }
};
inline void do_ovation(double now) {
  if (astro::compute(now, jc.lat, jc.lon, jc.alt_m).sun.el > OVATION_DARK_EL) {
    ovation_active_last = false;  // daylight: nothing to see, nothing fetched
    return;
  }
  const bool active = aurora_active(now);
  if (now < ovation_next && !(active && !ovation_active_last))
    return;  // not due, and no quiet-to-active change
  ovation_next = now + OVATION_RETRY_S;
  ovation_active_last = active;
  static OvationScan sc;  // 0.5 KB: kept off the task stack
  sc = OvationScan();
  sc.setup(jc.lat, jc.lon);
  const HttpResult r = http_stream("https://services.swpc.noaa.gov/json/ovation_aurora_latest.json", "aurora (OVATION)",
                                   [&](const char *d, int len) { sc.feed(d, len); }, &sc.done);
  const float here = sc.here();
  if (!r.ok || std::isnan(here)) {
    if (r.ok)
      ESP_LOGW(TAG, "OVATION: observer not in the grid (%ld cells)", sc.cells);
    return;
  }
  AuroraChance a;
  a.obs = sc.obs;
  a.fc = sc.fc;
  a.here = here;
  a.view = std::max(here, sc.view);
  a.view_az = sc.view > here ? sc.view_az : NAN;
  ov_seen = a;
  ovation_active_last = aurora_active(now);
  ovation_next = now + (ovation_active_last ? OVATION_ACTIVE_S : OVATION_QUIET_S);
  ESP_LOGI(TAG, "aurora chance %.0f%% here, %.0f%% in view (%ld cells, %ld bytes); %s, next in %.0f min", a.here, a.view,
           sc.cells, r.bytes, ovation_active_last ? "active" : "quiet", (ovation_next - now) / 60);
  xSemaphoreTake(mutex, portMAX_DELAY);
  pending.ovation = a;
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}

// ------------------------------------------------------------------ UI-59/60 live pictures
// On request (the Sun or Moon card, or its picture, is open). Only the task writes
// img_work; the loop copies it in drain() while holding the mutex.
// UI-59e: ONE work buffer for every picture (the task decodes one at a time). img_work_owner
// says whose pixels it holds until the loop has copied them: -1 free, else a kind. Guarded by
// the mutex.
inline uint16_t *img_work_buf = nullptr;
inline int img_work_owner = -1;
// wait (a loop pass or two) until the loop has copied the last picture out, then take the buffer
inline uint16_t *img_work_take() {
  for (int i = 0; i < 150; i++) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool busy = img_work_owner >= 0;
    xSemaphoreGive(mutex);
    if (!busy) {
      if (img_work_buf == nullptr)
        img_work_buf = (uint16_t *) heap_caps_malloc(IMG_PX * IMG_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      return img_work_buf;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return nullptr;
}
inline double img_have[IMG_N] = {-1, -1, -1, -1, -1, -1};  // frame already decoded (a repeat is skipped)
constexpr size_t IMG_JPG_MAX = 768 * 1024;  // the Moon (~110 KB) and the Earth (~450 KB) JPEGs
// the box filter's sums (777 KB): one buffer for the Earth, the region and the planets (the
// net task decodes one picture at a time), taken once and kept, so it cannot be squeezed out
inline uint16_t *fit_acc() {
  static uint16_t *acc = nullptr;
  if (acc == nullptr)
    acc = (uint16_t *) heap_caps_malloc(IMG_PX * IMG_PX * 3 * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return acc;
}
inline uint8_t *img_jpg() {
  static uint8_t *jpg = nullptr;
  if (jpg == nullptr)
    jpg = (uint8_t *) heap_caps_malloc(IMG_JPG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return jpg;
}
inline bool sun_frame_time(const char *url, double &t) {  // "..._s20260928T190000Z_..."
  const char *p = strstr(url, "_s20");
  int y, mo, d, h, mi, se;
  if (!p || sscanf(p + 2, "%4d%2d%2dT%2d%2d%2dZ", &y, &mo, &d, &h, &mi, &se) != 6)
    return false;
  t = (double) days_from_civil(y, mo, d) * 86400.0 + h * 3600 + mi * 60 + se;
  return true;
}
inline const char *http_err(const HttpResult &r, const char *who, char *e, size_t n) {
  if (r.status && r.status != 200)
    snprintf(e, n, "%s answered HTTP %d", who, r.status);
  else
    snprintf(e, n, r.status ? "download cut short" : "could not reach %s", who);
  return e;
}
// UI-59f: before a live picture streams in: the one showing (or the page colour) under it
inline void img_prog_fill(uint16_t *work, int kind) {
  if (const uint16_t *old = img_shown[kind])
    memcpy(work, old, IMG_PX * IMG_PX * 2);
  else
    for (int i = 0; i < IMG_PX * IMG_PX; i++)
      work[i] = (uint16_t) (((PAGE_BG >> 19) & 31) << 11 | ((PAGE_BG >> 10) & 63) << 5 | ((PAGE_BG >> 3) & 31));
  img_prog_rows = 0;
}
// UI-59f: a JPEG (the Moon, the Earth, the region) decoded while it downloads: a short-lived
// "jpgdec" task reads the buffer as it fills (waiting at its end), and each finished row gets
// its round edge at once, so the rows shown are the final picture. The host tests decode once
// the download is in (start() is not called there), through the same row code.
struct JpgStream {
  const uint8_t *jpg = nullptr;
  uint16_t *work = nullptr, *acc = nullptr;
  bool half = false;     // the Moon (2:1), else the box filter
  float keep = 1.0f;     // decode_fit's keep
  float r0 = 0, r1 = 0;  // round_mask radii (0: none)
  bool show = false;     // report rows to the viewer (img_prog_rows)
  volatile size_t have = 0;
  volatile bool eof = false, done = false;
  bool started = false;
  const char *err = nullptr;
  int w = 0, h = 0, masked = 0;
  static void rows_cb(int upto, void *ctx) {
    JpgStream *j = (JpgStream *) ctx;
    for (; j->masked < upto; j->masked++)
      if (j->r1 > 0)
        round_mask_row(j->work + (size_t) j->masked * IMG_PX, j->masked, IMG_PX, j->r0, j->r1);
    if (j->show)
      img_prog_rows = upto;
#ifndef SAT_HOST_TEST
    if (j->started)
      vTaskDelay(1);  // PERF-15a: a breath per row of blocks for the PSRAM bus
#endif
  }
  void run(size_t len, bool streaming) {
    skyjpg::Stream st;
    if (streaming) {
      st.have = &have;
      st.eof = &eof;
    }
    st.rows = rows_cb;
    st.ctx = this;
    masked = 0;
    err = half ? skyjpg::decode_half(jpg, len, work, IMG_PX, IMG_PX, &w, &h, &st)
               : skyjpg::decode_fit(jpg, len, work, IMG_PX, IMG_PX, acc, &w, &h, keep, 0, false, &st);
    done = true;
  }
#ifndef SAT_HOST_TEST
  static void task(void *a) {
    ((JpgStream *) a)->run(IMG_JPG_MAX, true);
    vTaskDelete(nullptr);  // (nothing of the job is touched after done)
  }
  // the first bytes are in and look like a JPEG: decode alongside the rest of the download
  void start() {
    started = xTaskCreatePinnedToCore(task, "jpgdec", 6144, this, 1, nullptr, 0) == pdPASS;
  }
#else
  void start() {}
#endif
  // the download is over: the decoder finishes (or, started, stops at the end of what came)
  const char *finish(size_t n) {
    eof = true;
    if (!started) {
      run(n, false);
      return err;
    }
    while (!done)
      vTaskDelay(pdMS_TO_TICKS(5));
    return err;
  }
  void abandon() {  // a failed download: stop the decoder without using its result
    eof = true;
    while (started && !done)
      vTaskDelay(pdMS_TO_TICKS(5));
  }
  void reset() {
    jpg = nullptr;
    work = acc = nullptr;
    half = false;
    keep = 1.0f;
    r0 = r1 = 0;
    show = false;
    have = 0;
    eof = done = false;
    started = false;
    err = nullptr;
    w = h = masked = 0;
  }
  void feed(size_t n) {
    have = n;
    if (!started && !eof && n >= 4 && jpg[0] == 0xFF && jpg[1] == 0xD8)
      start();
  }
};
// UI-59: the newest frame of NOAA's SUVI 304 A animation, box-filtered to IMG_PX (SOHO EIT
// from 4.5.31 to 4.5.36; SUVI again from 4.5.37, picked over SOHO and SDO)
inline const char *sun_image(ImageInfo &info, uint16_t *work, char *e, size_t en) {
  // 1. the frame list (~40 KB): the last "url" is the newest exposure
  static char url[240];
  constexpr int KEEP = 16;  // newest frames remembered (4 min apart: an hour back)
  static char paths[KEEP][120];
  int np = 0;  // paths written (ring: newest at (np-1) % KEEP)
  char last[200] = "";
  img_stage = STG_LIST;
  {
    char key[5] = {0, 0, 0, 0, 0};
    int st = 0, n = 0;  // 0: looking for "url", 1: to the opening quote, 2: in the value
    char val[200];
    http_stream("https://services.swpc.noaa.gov/products/animations/suvi-primary-304.json", "Sun image list",
                [&](const char *d, int len) {
                  for (int i = 0; i < len; i++) {
                    const char c = d[i];
                    if (st == 0) {
                      memmove(key, key + 1, 4);
                      key[4] = c;
                      if (memcmp(key, "\"url\"", 5) == 0)
                        st = 1;
                    } else if (st == 1) {
                      if (c == '"')
                        st = 2, n = 0;
                    } else if (c == '"') {
                      val[n] = 0;
                      if (val[0] == '/') {
                        copy_cstr(last, sizeof(last), val);
                        copy_cstr(paths[np % KEEP], sizeof(paths[0]), val);
                        np++;
                      }
                      st = 0;
                    } else if (n < (int) sizeof(val) - 1) {
                      val[n++] = c;
                    }
                  }
                });
  }
  // 2. the newest frame that shows the Sun. Around the equinoxes each GOES satellite
  // passes through Earth's shadow once a night (up to ~70 min); SUVI's frames then come
  // out nearly black (~30 KB instead of ~1.1 MB). Those are skipped by their size (read
  // from the headers, before the body) and, if the size is not given, by their darkness.
  constexpr int32_t SUVI_MIN_BYTES = 150000;  // good ~1.1 MB, shadowed ~30 KB
  static skypng::Decoder dec;
  const int tries = np > 0 ? std::min(np, KEEP) : 1;
  bool dark_seen = false;
  for (int t = 0; t < tries; t++) {
    double obs = 0;
    const char *path = np > 0 ? paths[(np - 1 - t) % KEEP] : "";
    if (np > 0 && sun_frame_time(path, obs)) {
      snprintf(url, sizeof(url), "https://services.swpc.noaa.gov%s", path);
      const char *g = strstr(path, "_g");
      if (g && g[2] >= '0' && g[2] <= '9')
        snprintf(info.src, sizeof(info.src), "GOES-%d", atoi(g + 2));
    } else {  // no list: the "latest" copy (time unknown)
      copy_cstr(url, sizeof(url), "https://services.swpc.noaa.gov/images/animations/suvi/primary/304/latest.png");
      obs = 0;
    }
    info.obs = obs;
    info.shadow = dark_seen;
    if (obs > 0 && obs == img_have[IMG_SUN])
      return nullptr;  // nothing newer (that shows the Sun) than what is showing
    if (!dec.begin(work, IMG_PX, IMG_PX, SUN_CROP))
      return dec.error;
    // UI-59f: each row gets its round edge as it is finished, so what shows during the download
    // is the final picture; below the rows done, the one showing (or the page colour)
    img_prog_fill(work, IMG_SUN);
    dec.on_row = [](uint16_t *row, int y, void *) {
      round_mask_row(row, y, IMG_PX, 158, 178);  // UI-62b: the glow fades out, prominences kept
      img_prog_rows = y + 1;
    };
    img_stage = STG_DOWNLOAD;
    dl_track = true;
    bool small = false, stop = false;
    const HttpResult r = http_stream(
        url, "Sun image",
        [&](const char *d, int len) {
          if (img_total > 0 && img_total < SUVI_MIN_BYTES) {  // a dark frame: don't read it
            small = stop = true;
            return;
          }
          if (!dec.error)
            dec.feed((const uint8_t *) d, len);
        },
        &stop);
    dl_track = false;
    dec.release();
    if (small) {
      ESP_LOGI(TAG, "Sun image: frame %d back is dark (%ld bytes on offer): trying the one before", t,
               (long) img_total);
      dark_seen = true;
      continue;
    }
    if (!r.ok)
      return http_err(r, "NOAA", e, en);
    if (!dec.done)
      return dec.error ? dec.error : "incomplete image";
    // no size given: judge by the disc (fewer than a quarter of its pixels lit = dark)
    int lit = 0, all = 0;
    for (int y = IMG_PX / 4; y < IMG_PX * 3 / 4; y += 4)
      for (int x = IMG_PX / 4; x < IMG_PX * 3 / 4; x += 4) {
        const uint16_t p = work[y * IMG_PX + x];
        all++;
        lit += ((p >> 11) << 3) + ((p & 31) << 3) > 80;
      }
    if (lit * 4 < all) {
      ESP_LOGI(TAG, "Sun image: frame %d back is dark (%d of %d lit): trying the one before", t, lit, all);
      dark_seen = true;
      continue;
    }
    ESP_LOGI(TAG, "Sun image %s (%ld bytes, %ux%u)%s", info.src, r.bytes, (unsigned) dec.W, (unsigned) dec.H,
             dark_seen ? " (newer frames dark: the satellite is in Earth's shadow)" : "");
    img_have[IMG_SUN] = obs;  // (the round edge went on row by row: UI-59f)
    info.shadow = t > 0;
    info.fresh = true;
    return nullptr;
  }
  return "the satellite is in Earth's shadow";
}
// UI-60: NASA SVS Dial-A-Moon for this hour: the Moon as seen from Earth's centre (phase,
// libration, size and tilt), rendered from Lunar Reconnaissance Orbiter data. A small JSON
// names the frame (and a south-up copy for the southern hemisphere); the 730 px JPEG
// (~110 KB) is decoded by the ROM's TJpgDec and averaged 2x2 to IMG_PX.
inline const char *moon_image(ImageInfo &info, uint16_t *work, double now, char *e, size_t en) {
  img_stage = STG_LIST;
  const int64_t hr = (int64_t) floor(now / 3600.0) * 3600;
  int64_t z = hr / 86400 + 719468;  // civil_from_days (H. Hinnant)
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned) (z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  const int y = (int) (yoe + era * 400 + (m <= 2));
  char api[80];
  snprintf(api, sizeof(api), "https://svs.gsfc.nasa.gov/api/dialamoon/%04d-%02u-%02uT%02d:00", y % 10000, m, d,
           (int) ((hr % 86400) / 3600));
  const int len = http_get(api, json_buf, JSON_BUF, "Moon image info");
  if (len <= 0)
    return "could not reach NASA";
  JsonDocument filter;
  filter["image"]["url"] = true;
  filter["su_image"]["url"] = true;
  filter["phase"] = true;
  filter["age"] = true;
  filter["distance"] = true;
  JsonDocument doc(&psram_alloc);
  if (deserializeJson(doc, json_buf, (size_t) len, DeserializationOption::Filter(filter)))
    return "unreadable answer from NASA";
  info.south_up = jc.lat < 0;
  static char url[200];
  copy_cstr(url, sizeof(url), (info.south_up ? doc["su_image"]["url"] : doc["image"]["url"]) | "");
  if (strncmp(url, "https://", 8) != 0)
    return "NASA sent no picture";
  info.obs = (double) hr;
  info.phase = doc["phase"] | NAN;
  info.age = doc["age"] | NAN;
  info.dist_km = doc["distance"] | NAN;
  copy_cstr(info.src, sizeof(info.src), "NASA SVS");
  const double key = (double) hr + (info.south_up ? 0.5 : 0);
  if (key == img_have[IMG_MOON])
    return nullptr;  // this hour's frame is already showing
  uint8_t *jpg = img_jpg();
  if (jpg == nullptr)
    return "out of memory";
  size_t n = 0;
  bool big = false;
  JpgStream js;  // UI-59f: decoded as it arrives
  js.jpg = jpg;
  js.work = work;
  js.half = true;
  js.r0 = 166, js.r1 = 180;  // UI-62b
  js.show = true;
  img_prog_fill(work, IMG_MOON);
  img_stage = STG_DOWNLOAD;
  dl_track = true;
  const HttpResult r = http_stream(url, "Moon image", [&](const char *dd, int k) {
    if (n + k > IMG_JPG_MAX) {
      big = true;
      return;
    }
    memcpy(jpg + n, dd, k);
    n += k;
    js.feed(n);
  });
  dl_track = false;
  if (!r.ok || big) {
    js.abandon();
    return !r.ok ? http_err(r, "NASA", e, en) : "picture larger than expected";
  }
  img_stage = STG_DECODE;
  if (const char *de = js.finish(n))
    return de;
  ESP_LOGI(TAG, "Moon image (%u bytes, %dx%d, %.0f%% lit)%s", (unsigned) n, js.w, js.h, info.phase,
           js.started ? ", decoded as it came" : "");
  img_have[IMG_MOON] = key;
  info.fresh = true;
  return nullptr;
}
// UI-62: the Earth from NOAA's GOES West (GOES-18, 137.0 W) or East (GOES-19, 75.2 W),
// whichever is nearer the observer's longitude: the GeoColor full disk (true colour by day,
// clouds and city lights at night), 678 px JPEG every 10 min, box-filtered to IMG_PX.
// NOAA STAR names each frame by its scan start, every 10 minutes:
//   .../GOES18/ABI/SECTOR/ak/GEOCOLOR/20262720430_GOES18-ABI-ak-GEOCOLOR-500x500.jpg  (YYYYDDDHHMM)
// The newest is found by stepping back from the current 10 minutes until one exists (it
// appears ~10-15 min after the scan).
inline void star_stamp(double t, char *buf, size_t n) {
  const time_t tt = (time_t) t;
  struct tm g;
  gmtime_r(&tt, &g);
  snprintf(buf, n, "%04d%03d%02d%02d", (g.tm_year + 1900) % 10000, (g.tm_yday + 1) % 1000, g.tm_hour % 100, g.tm_min % 100);
}
inline const char *earth_image(ImageInfo &info, uint16_t *work, char *e, size_t en, bool regional = false) {
  auto gap = [](double a, double b) { return fabs(fmod(a - b + 540.0, 360.0) - 180.0); };
  const Region *rg = regional ? region_for(jc.lat, jc.lon) : nullptr;
  if (regional && !rg)
    return "no regional view for this location";
  const bool west = rg ? strcmp(rg->sat, "GOES18") == 0 : gap(jc.lon, -137.0) <= gap(jc.lon, -75.2);
  const int kind = regional ? IMG_REGION : IMG_EARTH;
  const char *sat = rg ? rg->sat : west ? "GOES18" : "GOES19";
  copy_cstr(info.src, sizeof(info.src), west ? "GOES-18" : "GOES-19");
  uint8_t *jpg = img_jpg();
  uint16_t *acc = fit_acc();
  if (jpg == nullptr || acc == nullptr)
    return "out of memory";
  const double start = floor(clock_now() / 600.0) * 600.0;
  const int steps = 6;  // up to 60 minutes back
  size_t n = 0;
  HttpResult r;
  double obs = 0;
  bool fresh_retry = false;
  JpgStream js;
  for (int i = 0; i < steps; i++) {
    const double t = start - 600.0 * i;
    const double key = t + (west ? 0.5 : 0);
    if (key == img_have[kind]) {
      info.obs = t;
      return nullptr;  // the newest there is is showing already
    }
    char stamp[32], url[176];
    star_stamp(t, stamp, sizeof(stamp));
    if (rg)
      snprintf(url, sizeof(url), "https://cdn.star.nesdis.noaa.gov/%s/ABI/SECTOR/%s/GEOCOLOR/%s_%s-ABI-%s-GEOCOLOR-%dx%d.jpg",
               sat, rg->code, stamp, sat, rg->code, rg->px, rg->px);
    else
      snprintf(url, sizeof(url), "https://cdn.star.nesdis.noaa.gov/%s/ABI/FD/GEOCOLOR/%s_%s-ABI-FD-GEOCOLOR-678x678.jpg",
               sat, stamp, sat);
    n = 0;
    bool big = false;
    js.reset();  // UI-59f: decoded as it arrives
    js.jpg = jpg;
    js.work = work;
    js.acc = acc;
    js.keep = rg ? 0.965f : 1.0f;  // the regional frames: NOAA's label strip (the bottom ~3%) cropped off
    if (!rg)
      js.r0 = 176, js.r1 = 180;  // UI-62b: a crisp limb, the corner label gone
    js.show = true;
    img_prog_fill(work, kind);
    img_stage = STG_DOWNLOAD;
    dl_track = true;
    r = http_stream(url, regional ? "Region image" : "Earth image", [&](const char *d, int k) {
      if (n + k > IMG_JPG_MAX) {
        big = true;
        return;
      }
      memcpy(jpg + n, d, k);
      n += k;
      js.feed(n);  // (a 404 page is not a JPEG: the decoder never starts on one)
    });
    dl_track = false;
    if (!r.ok || big)
      js.abandon();
    if (r.status == 404)
      continue;  // not there (yet): ten minutes earlier
    if (!r.ok)
      return http_err(r, "NOAA", e, en);
    if (big)
      return "picture larger than expected";
    if (n < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) {  // not a JPEG: log it; once more on a new connection
      ESP_LOGW(TAG, "%s: not a JPEG (%u bytes: %02x %02x %02x %02x)%s", regional ? "Region image" : "Earth image",
               (unsigned) n, n > 0 ? jpg[0] : 0, n > 1 ? jpg[1] : 0, n > 2 ? jpg[2] : 0, n > 3 ? jpg[3] : 0,
               r.reused ? ", retrying fresh" : "");
      kept_drop();
      if (r.reused && !fresh_retry) {
        fresh_retry = true;
        i--;
        continue;
      }
      return "NOAA sent something other than a JPEG";
    }
    obs = t;
    break;
  }
  if (obs == 0)
    return "no recent picture at NOAA";
  info.obs = obs;
  img_stage = STG_DECODE;
  if (const char *de = js.finish(n))
    return de;
  ESP_LOGI(TAG, "%s image %s (%u bytes, %dx%d)%s", rg ? rg->name : "Earth", info.src, (unsigned) n, js.w, js.h,
           js.started ? ", decoded as it came" : "");
  img_have[kind] = obs + (west ? 0.5 : 0);
  info.fresh = true;
  return nullptr;
}
// UI-61a / 61d: the planet's photo, built into the firmware (sky_photos.h: the Wikimedia
// Commons JPEGs, ~150 KB): nothing to download, nothing to keep. Letterboxed when not
// square (Saturn), Jupiter smaller to leave room for its moon strip; phase and the strip
// are added by the loop each minute.
inline const char *planet_image(ImageInfo &info, uint16_t *work, char *e, size_t en) {
  const int p = planet_req;
  if (p < 0 || p > 4)
    return "no planet";
  const PlanetPhoto &ph = planet_photo(p);
  info.planet = p;
  copy_cstr(info.src, sizeof(info.src), ph.credit);
  if (img_have[IMG_PLANET] == p)
    return nullptr;  // that photo is showing already
  const photos::Photo jp = photos::get(p);
  if (jp.data == nullptr || jp.len == 0)
    return "no photo in this firmware";
  uint16_t *acc = fit_acc();
  if (acc == nullptr)
    return "out of memory";
  img_stage = STG_DECODE;
  const uint16_t bg = (uint16_t) ((7 >> 3) << 11 | (11 >> 2) << 5 | (24 >> 3));
  for (int k = 0; k < IMG_PX * IMG_PX; k++)
    work[k] = bg;
  // size in the square: Jupiter 260 px at the top (its moon strip goes below), else full width
  const int tw = p == 3 ? 260 : IMG_PX, th = (int) ((int64_t) tw * ph.h / ph.w);
  const int ox = (IMG_PX - tw) / 2, oy = p == 3 ? 6 : (IMG_PX - th) / 2;
  int w = 0, h = 0;  // decoded straight into its place in the square
  if (const char *de =
          skyjpg::decode_fit(jp.data, jp.len, work + (size_t) oy * IMG_PX + ox, tw, th, acc, &w, &h, 1.0f, IMG_PX))
    return de;
  ESP_LOGI(TAG, "planet %d photo (built in, %u bytes, %dx%d)", p, (unsigned) jp.len, w, h);
  img_have[IMG_PLANET] = p;
  info.fresh = true;
  return nullptr;
}
// UI-75: the University of Alaska Fairbanks Geophysical Institute's all-sky camera at Poker
// Flat (allsky.gi.alaska.edu): its live feed (an event stream) names the current picture,
// "images/...jpg" (a "not dark yet" card by day, saying when it runs); the fisheye circle fills
// the top square of the 514x600 frame, decoded as it arrives (UI-59f) with a round edge.
constexpr const char *ALLSKY_BASE = "https://allsky.gi.alaska.edu/";
inline const char *allsky_image(ImageInfo &info, uint16_t *work, char *e, size_t en) {
  img_stage = STG_LIST;
  char path[120] = "";
  bool stop = false;
  {
    LineSplitter ls;  // data: "1": "images/PKR/....jpg",  (view 1, the site's default)
    ls.on_line = [&](char *line) {
      const char *q = strstr(line, "\"1\":");
      if (q && path[0] == 0) {
        q = strchr(q + 4, '"');
        const char *z = q ? strchr(q + 1, '"') : nullptr;
        if (q && z && z - q - 1 < (int) sizeof(path)) {
          memcpy(path, q + 1, z - q - 1);
          path[z - q - 1] = 0;
        }
      }
      if (strstr(line, "data: }"))
        stop = true;  // one event is all we need (the stream would stay open)
    };
    char url[96];
    snprintf(url, sizeof(url), "%ssrc/checkLive.php?cam=poker-flat", ALLSKY_BASE);
    http_stream(url, "all-sky list", [&](const char *d, int len) { ls.feed(d, len); }, &stop);
    ls.finish();
  }
  if (path[0] == 0 || strstr(path, "..") || !strstr(path, ".jpg"))
    return "no picture named by the camera";
  uint32_t hsh = 2166136261u;  // FNV-1a of the name: the same picture is not fetched again
  for (const char *c = path; *c; c++)
    hsh = (hsh ^ (uint8_t) *c) * 16777619u;
  const double key = (double) hsh;
  copy_cstr(info.src, sizeof(info.src), strstr(path, "notdark") ? "day" : "UAF GI");
  if (key == img_have[IMG_ALLSKY])
    return nullptr;
  uint8_t *jpg = img_jpg();
  uint16_t *acc = fit_acc();
  if (jpg == nullptr || acc == nullptr)
    return "out of memory";
  size_t n = 0;
  bool big = false;
  JpgStream js;
  js.jpg = jpg;
  js.work = work;
  js.acc = acc;
  js.keep = -1.0f;            // the top square
  js.r0 = 176, js.r1 = 180;   // the fisheye's rim
  js.show = true;
  img_prog_fill(work, IMG_ALLSKY);
  img_stage = STG_DOWNLOAD;
  dl_track = true;
  http_last_modified[0] = 0;
  char url[200];
  snprintf(url, sizeof(url), "%s%s", ALLSKY_BASE, path);
  const HttpResult r = http_stream(url, "all-sky image", [&](const char *d, int k) {
    if (n + k > IMG_JPG_MAX) {
      big = true;
      return;
    }
    memcpy(jpg + n, d, k);
    n += k;
    js.feed(n);
  });
  dl_track = false;
  if (!r.ok || big) {
    js.abandon();
    return !r.ok ? http_err(r, "the camera", e, en) : "picture larger than expected";
  }
  img_stage = STG_DECODE;
  if (const char *de = js.finish(n))
    return de;
  double lm = 0;
  info.obs = http_date(http_last_modified, lm) ? lm : clock_now();
  ESP_LOGI(TAG, "all-sky image %s (%u bytes, %dx%d)", path, (unsigned) n, js.w, js.h);
  img_have[IMG_ALLSKY] = key;
  info.fresh = true;
  return nullptr;
}
inline void do_image(uint8_t kind) {
  ImageInfo info;
  xSemaphoreTake(mutex, portMAX_DELAY);
  const bool busy = pending.img[kind].fresh;  // the loop has not copied the last one yet
  xSemaphoreGive(mutex);
  if (busy)
    return;
  img_stage_kind = kind;
  img_bytes = 0;
  img_total = -1;
  uint16_t *work = img_work_take();
  char e[48];
  const char *err = work == nullptr ? (img_work_buf ? "busy" : "out of memory")
                    : kind == IMG_SUN         ? sun_image(info, work, e, sizeof(e))
                    : kind == IMG_MOON        ? moon_image(info, work, clock_now(), e, sizeof(e))
                    : kind == IMG_PLANET      ? planet_image(info, work, e, sizeof(e))
                    : kind == IMG_ALLSKY      ? allsky_image(info, work, e, sizeof(e))
                                              : earth_image(info, work, e, sizeof(e), kind == IMG_REGION);
  img_stage = STG_IDLE;
  img_prog_rows = -1;
  if (err && !strcmp(err, "out of memory"))
    ESP_LOGW(TAG, "picture %u: out of memory (PSRAM free %u KB)", (unsigned) kind,
             (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));  // no heap walk (FAIL-9)
  if (err)
    copy_cstr(info.err, sizeof(info.err), err);
  info.ok = err == nullptr;
  xSemaphoreTake(mutex, portMAX_DELAY);
  ImageInfo &p = pending.img[kind];
  info.seq = p.seq + 1;
  if (info.fresh && info.ok)
    img_work_owner = kind;  // UI-59e: these pixels, until the loop copies them
  if (!info.fresh)
    info.fresh = p.fresh;  // an earlier picture still waiting to be copied
  if (!info.ok && p.obs > 0) {  // keep saying which picture is showing
    const uint32_t sq = info.seq;
    char er[48];
    copy_cstr(er, sizeof(er), info.err);
    info = p;
    info.seq = sq;
    info.ok = false;
    info.fresh = false;
    copy_cstr(info.err, sizeof(info.err), er);
  }
  p = info;
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}

inline void do_pass();
// DATA-13: new station elements from SatNOGS: say so, save them and recompute passes
inline void finish_fallback() {
  const size_t k = strlen(status.error);
  if (k && !strstr(status.error, "SatNOGS") && k + 20 < sizeof(status.error))
    snprintf(status.error + k, sizeof(status.error) - k, "; ISS via SatNOGS");
  update_oldest();
  publish_status();
  cache_save_a();
  do_pass();
}

// JOB_ELEMENTS: refresh whatever is due. Called every few minutes; cheap when nothing is.
inline void do_decay(double now);
inline void do_elements() {
  const double now = clock_now();
  do_kp(now);  // UI-37: NOAA, not CelesTrak: its own schedule, never held by a CelesTrak 403
  do_wind(now);      // UI-55: NOAA too
  do_launches(now);  // UI-54: Launch Library 2
  do_events(now);    // UI-54c: Launch Library 2 events
  do_comets(now);    // UI-63: JPL small bodies
  do_ovation(now);   // UI-58: NOAA OVATION
  do_swx(now);       // UI-73: NOAA SWPC space weather
  // DATA-10: a layer just switched on is drawn from flash first; a stale list is refreshed
  // on the next element pass (ELEM_PERIOD) rather than holding the display for downloads
  if (cache_fill_missing())
    return;
  if (now < status.next_try) {
    if (stations_fallback(now))  // DATA-13: CelesTrak on hold: the stations from SatNOGS
      finish_fallback();
    return;
  }
  bool any = false, failed = false, iss_new = false, save_a = false, save_b = false;
  auto due = [&](double loaded) { return loaded == 0 || now - loaded >= ELEM_REFRESH_S; };
  do_decay(now);  // UI-74: CelesTrak's decaying objects (its own 6 h schedule)
  if (now < status.next_try)
    return;  // (it was refused: CelesTrak's hold, DATA-12)

  if (!failed && (!have_iss || due(iss_loaded))) {
    SgpSat s;
    bool got = false;
    if (download(ct_url("/NORAD/elements/gp.php?CATNR=25544&FORMAT=csv").c_str(), "ISS",
                 [&](const Omm &o) { got = make_sgp(o, s); }, now) &&
        got) {
      strncpy(s.name, "ISS", sizeof(s.name) - 1);
      iss = s;
      have_iss = true;
      iss_loaded = now;
      iss_from_fallback = false;
      any = iss_new = save_a = true;
    } else {
      failed = true;
    }
  }
  // UI-52: Tiangong (not cached: fetched again after a reboot, one small request). A
  // failure here is not held against the other lists: it is tried again in an hour.
  if (!failed && (!have_css || due(css_loaded)) && now >= css_retry_at) {
    SgpSat s;
    bool got = false;
    char err0[sizeof(status.error)];
    memcpy(err0, status.error, sizeof(err0));
    const double next0 = status.next_try;
    if (download(ct_url("/NORAD/elements/gp.php?CATNR=48274&FORMAT=csv").c_str(), "Tiangong",
                 [&](const Omm &o) { got = make_sgp(o, s); }, now) &&
        got) {
      strncpy(s.name, "Tiangong", sizeof(s.name) - 1);
      css = s;
      have_css = true;
      css_loaded = now;
      any = true;
    } else if (strstr(status.error, "403") || strstr(status.error, "reach")) {
      failed = true;  // CelesTrak or the network is refusing everything: back off as usual
    } else {
      memcpy(status.error, err0, sizeof(err0));  // just this object: say nothing, try in an hour
      status.next_try = next0;
      css_retry_at = now + 3600;
    }
  }
  // UI-35: the visual list carries the payloads (LEO/MEO) and the debris, so it is kept
  // while either layer is on
  if (!failed && (jc.sats_on || jc.debris_on) && (sats.empty() || due(sats_loaded))) {
    char url[128];
    snprintf(url, sizeof(url), "%s/NORAD/elements/gp.php?GROUP=%s&FORMAT=csv", celestrak_base.c_str(), jc.sat_group.c_str());
    // FAIL-12: refilled in place (capacity kept); a failed download restores it from the cache
    {
      sats.clear();
      if (download(url, "satellite", [&](const Omm &o) {
            if (o.id == 25544 || o.id == CSS_ID || o.id == CSS_WENTIAN || o.id == CSS_MENGTIAN ||
                now - o.epoch > MAX_ELEM_AGE_S || !one_more(sats, SAT_CAP, 200, "satellites"))
              return;
            sats.emplace_back();
            if (!make_sgp(o, sats.back()))
              sats.pop_back();  // decayed or unusable
          }, now)) {
        sats_loaded = now;
        any = save_a = true;
      } else {
        sats.clear();
        cache_load_sats_only();
        failed = true;
      }
    }
  }
  if (!failed && jc.starlink_on && jc.starlink_radius > 0 && (starlink.empty() || due(starlink_loaded))) {
    // FAIL-12: refilled in place; it grows (one_more) only while PSRAM has the block, else the rest is
    // dropped, never reallocated
    {
      starlink.clear();
      size_t dropped = 0;
      if (download(ct_url("/NORAD/elements/gp.php?GROUP=starlink&FORMAT=csv").c_str(), "Starlink",
                   [&](const Omm &o) {
                     if (now - o.epoch > MAX_ELEM_AGE_S)
                       return;
                     if (!one_more(starlink, STARLINK_CAP, 12000, "Starlink")) {
                       dropped++;
                       return;
                     }
                     breathe((uint32_t) starlink.size(), 200);
                     starlink.emplace_back();
                     if (!make_sl(o, starlink.back()))
                       starlink.pop_back();
                   }, now)) {
        if (dropped)
          ESP_LOGW(TAG, "Starlink: %u more than the list holds (%u): dropped", (unsigned) dropped,
                   (unsigned) starlink.capacity());
        starlink_loaded = now;
        any = save_b = true;
      } else {
        starlink.clear();
        cache_load_starlink();
        failed = true;
      }
    }
  }
  // UI-28: GNSS (MEO) and geosynchronous lists, only while their class is shown
  // FAIL-12: refilled in place (capacity kept); a failed download restores it from the cache
  bool mem_skip = false;  // the last group_list had no room and was skipped (not an error)
  auto group_list = [&](const char *const *groups, int ng, const char *what, size_t cap, pvector<SgpSat> &dst,
                        double &loaded, uint32_t c_off, uint32_t c_max) -> bool {
    mem_skip = false;
    pvector<SgpSat> &staging = dst;
    staging.clear();
    std::unordered_map<int32_t, bool> ids;
    for (int g = 0; g < ng; g++) {
      char url[128];
      snprintf(url, sizeof(url), "%s/NORAD/elements/gp.php?GROUP=%s&FORMAT=csv", celestrak_base.c_str(), groups[g]);
      if (!download(url, what, [&](const Omm &o) {
            if (ids.count(o.id) || now - o.epoch > MAX_ELEM_AGE_S || !one_more(staging, cap, 64, what))
              return;
            breathe((uint32_t) staging.size(), 32);
            staging.emplace_back();
            if (make_sgp(o, staging.back()))
              ids[o.id] = true;
            else
              staging.pop_back();
          }, now)) {
        dst.clear();
        cache_load_list(c_off, c_max, dst, loaded, what);
        return false;
      }
    }
    loaded = now;
    return true;
  };
  bool save_c = false, save_d = false;
  if (!failed && jc.meo_on && (meo_sats.empty() || due(meo_loaded))) {
    static const char *const G[] = {"gps-ops", "galileo", "glo-ops", "beidou"};
    if (group_list(G, 4, "GNSS", MEO_CAP, meo_sats, meo_loaded, CACHE_C_OFF, CACHE_C_MAX)) {
      if (!mem_skip)
        any = save_c = true;
    } else {
      failed = true;
    }
  }
  if (!failed && jc.geo_on && (geo_sats.empty() || due(geo_loaded))) {
    static const char *const G[] = {"geo"};
    if (group_list(G, 1, "GEO", GEO_CAP, geo_sats, geo_loaded, CACHE_D_OFF, CACHE_D_MAX)) {
      if (!mem_skip) {
        any = save_d = true;
        geo_force();
      }
    } else {
      failed = true;
    }
  }
  if (!failed && (jc.sats_on || jc.debris_on))
    owners_update(now);
  if (any && !failed) {
    status.loaded = now;
    status.error[0] = 0;
    status.next_try = 0;
  }
  if (failed && stations_fallback(now)) {  // DATA-13: CelesTrak refused: the stations from SatNOGS
    finish_fallback();
    iss_new = false;  // done above
  } else if (any || failed) {
    update_oldest();
    publish_status();
  }
  // DATA-10: keep what was downloaded (and any new owner codes) for the next boot
  if (save_a || owners.size() > cache_owner_count)
    cache_save_a();
  if (save_b)
    cache_save_b();
  if (save_c)
    cache_save_list(CACHE_C_OFF, CACHE_C_MAX, meo_sats, meo_loaded, "GNSS");
  if (save_d)
    cache_save_list(CACHE_D_OFF, CACHE_D_MAX, geo_sats, geo_loaded, "GEO");
  if (iss_new)
    do_pass();
}

// ================================================================== propagation jobs
inline geo::Observer observer() {
  geo::Observer o;
  o.set((float) jc.lat, (float) jc.lon, (float) jc.alt_m);
  return o;
}

// ------------------------------------------------------------------ UI-74 re-entries
// CelesTrak's decaying objects (SPECIAL=DECAYING, ~100 rows, ~17 KB CSV), every 6 h. The
// time left is estimated from how fast the orbit is shrinking: the mean motion grows at
// 2 x MEAN_MOTION_DOT rev/day^2 towards ~16.55 rev/day (~120 km, where a stage breaks up).
// That rate itself grows near the end, so the estimate runs late: +/-30 %, at least an hour.
// For the ones due within two days, the highest pass over the observer before then.
constexpr double DECAY_EVERY_S = 6 * 3600.0, DECAY_RETRY_S = 3600.0, DECAY_N_END = 16.55;
constexpr int DECAY_KEEP = 12;
inline double decay_next = 0;
inline double decay_estimate(const Omm &o) {  // UTC seconds, 0: not decaying
  if (!(o.ndot > 0))
    return 0;
  return o.epoch + std::max(0.0, DECAY_N_END - o.n_revday) / (2.0 * o.ndot) * 86400.0;
}
inline void do_decay(double now) {
  if (now < decay_next || now < 1.7e9)
    return;
  decay_next = now + DECAY_RETRY_S;
  pvector<DecayRec> out;
  pvector<Omm> els;
  char err0[sizeof(status.error)];
  memcpy(err0, status.error, sizeof(err0));
  const double next0 = status.next_try;
  const bool ok = download(ct_url("/NORAD/elements/gp.php?SPECIAL=DECAYING&FORMAT=csv").c_str(), "re-entries",
                           [&](const Omm &o) {
                             const double est = decay_estimate(o);
                             if (est > now - 6 * 3600.0 && est < now + 48 * 3600.0)
                               els.push_back(o);
                           },
                           now);
  if (!ok) {
    if (!strstr(status.error, "403")) {  // only CelesTrak refusing everything holds the rest back
      memcpy(status.error, err0, sizeof(err0));
      status.next_try = next0;
    }
    return;
  }
  std::sort(els.begin(), els.end(), [](const Omm &a, const Omm &b) { return decay_estimate(a) < decay_estimate(b); });
  const geo::Observer obs = observer();
  for (const Omm &o : els) {
    if ((int) out.size() >= DECAY_KEEP)
      break;
    DecayRec d;
    copy_cstr(d.name, sizeof(d.name), o.name);
    copy_cstr(d.intl, sizeof(d.intl), o.intl);
    d.id = o.id;
    d.est = decay_estimate(o);
    d.unc_h = (float) std::max(1.0, 0.3 * fabs(d.est - now) / 3600.0);
    d.rb = strstr(o.name, " R/B") != nullptr;
    d.deb = strstr(o.name, " DEB") != nullptr || strncmp(o.name, "DEB", 3) == 0;
    SgpSat sat;
    if (make_sgp(o, sat)) {  // its passes until it comes down: the highest one
      const double end = std::min(d.est + d.unc_h * 3600.0, now + 48 * 3600.0);
      int k = 0;
      for (double t = now; t < end; t += 60, k++) {
        double x[3];
        if (!sgp_ecef(sat, t, x))
          break;  // (SGP4 gives up as the orbit decays)
        const geo::AzEl ae = azel_of(obs, x);
        if (ae.el > 0 && ae.el > d.over_el) {
          d.over_el = ae.el;
          d.over_t = t;
          copy_cstr(d.over_dir, sizeof(d.over_dir), compass(ae.az));
        }
        if ((k & 255) == 255)
          vTaskDelay(1);
      }
    }
    out.push_back(d);
  }
  decay_next = now + DECAY_EVERY_S;
  ESP_LOGI(TAG, "re-entries: %u within two days (of %u decaying soon)", (unsigned) out.size(), (unsigned) els.size());
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.decays, out);
  pending.decays_new = true;
  pending.fresh |= L_EXTRA;
  xSemaphoreGive(mutex);
}

inline bool sgp_rec(SgpSat &s, double t, SatRec &r, const geo::Observer &o, float *el = nullptr) {
  double x0[3], x1[3], v;
  if (!sgp_ecef(s, t, x0, &v) || !sgp_ecef(s, t + 10.0, x1))
    return false;
  if (el)
    *el = azel_of(o, x0).el;
  r.id = s.id;
  memcpy(r.name, s.name, sizeof(r.name));
  memcpy(r.intl, s.intl, sizeof(r.intl));
  r.speed_kms = (float) v;
  r.period_min = s.period_min;
  r.incl_deg = s.incl_deg;
  r.cls = s.cls;
  r.debris = s.debris;
  r.tag = s.tag;
  make_fix(x0, x1, t, r);
  return true;
}

// JOB_ABOVE: every satellite above (or just below) the horizon, by orbit class
// (UI-28): LEO and MEO/HEO become markers; GEO is recomputed every 5 minutes (it
// barely moves) and published separately, drawn as dots on the sky (L_GEO).
inline void do_above() {
  const double t = clock_now();
  const geo::Observer o = observer();
  // FAIL-12: persistent, at full size: swapped with pending (and the UI's live copy), so all
  // three buffers settle at this capacity and the 2 s job stops allocating in PSRAM
  static pvector<SatRec> staging;  // FAIL-12b: grows to the most it has needed, then stays
  staging.clear();
  int total = 0;
  auto wanted = [&](uint8_t cls) {
    return cls == C_CLS_LEO ? jc.sats_on : cls == C_CLS_GEO ? false : jc.meo_on;  // GEO handled below
  };
  auto take = [&](SgpSat &s) {
    if (s.debris && !jc.debris_on)  // UI-35
      return;
    double x[3] = {0, 0, 0};
    if (!sgp_ecef(s, t, x))
      return;
    const float el = azel_of(o, x).el;
    if (el < RISING_EL)
      return;
    SatRec r;
    if (!sgp_rec(s, t, r, o))
      return;
    r.kind = K_SAT;
    owner_of(s.id, r.cc);
    r.launched = launched_of(s.id);
    push_room(staging, r, SAT_CAP + MEO_CAP, "satellite positions");
    total += el >= 0;
  };
  std::unordered_map<int32_t, bool> seen;
  uint32_t n = 0;
  for (auto &s : sats) {
    breathe(n++, 64);
    seen[s.id] = true;
    // UI-35: debris is its own layer, independent of the LEO/MEO switches (GEO debris
    // goes with the GEO list below)
    if (s.cls != C_CLS_GEO && (s.debris ? jc.debris_on : wanted(s.cls)))
      take(s);
  }
  if (jc.meo_on)
    for (auto &s : meo_sats) {
      breathe(n++, 64);
      if (!seen.count(s.id) && s.cls != C_CLS_GEO && s.cls != C_CLS_LEO)
        take(s);
    }
  // GEO: its own list plus any geosynchronous object in the visual list
  static pvector<SatRec> geo;  // FAIL-12: persistent, as staging
  geo.clear();
  bool geo_done = false;
  int geo_total = 0;
  if (jc.geo_on && t >= geo_next) {
    geo_next = t + GEO_EVERY_S;
    geo_done = true;

    auto take_geo = [&](SgpSat &s) {
      if (s.debris && !jc.debris_on)  // UI-35
        return;
      SatRec r;
      float el = -90;
      if (!sgp_rec(s, t, r, o, &el) || el < 0)
        return;
      r.kind = K_GEO;
      owner_of(s.id, r.cc);
    r.launched = launched_of(s.id);
      push_room(geo, r, GEO_CAP, "GEO positions");
      geo_total++;
    };
    std::unordered_map<int32_t, bool> gseen;
    for (auto &s : sats)
      if (s.cls == C_CLS_GEO) {
        gseen[s.id] = true;
        take_geo(s);
      }
    for (auto &s : geo_sats) {
      breathe(n++, 32);
      if (!gseen.count(s.id) && s.cls == C_CLS_GEO)
        take_geo(s);
    }
  } else if (!jc.geo_on && geo_next != 0) {
    geo_next = 0;
    geo_done = true;  // switched off: publish an empty list
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.sats, staging);
  pending.sat_total = total;
  pending.fresh |= L_SAT;
  if (geo_done) {
    std::swap(pending.geo, geo);
    pending.geo_total = geo_total;
    pending.fresh |= L_GEO;
  }
  xSemaphoreGive(mutex);
}

// JOB_STARLINK: the whole catalogue, every scan (DATA-3: the loop draws only the cone).
inline void do_starlink() {
  if (starlink.empty())
    return;
  const double t = clock_now();
  const geo::Observer o = observer();
  struct Cand {
    float el;
    uint32_t i;
  };
  static std::vector<Cand, PsramAlloc<Cand>> cand;  // FAIL-12: persistent, grows to the most seen once
  cand.clear();

  for (uint32_t i = 0; i < starlink.size(); i++) {
    breathe(i, 1000);
    double x[3] = {0, 0, 0};
    sl_ecef(starlink[i], t, x);
    const float el = azel_of(o, x).el;
    if (el >= RISING_EL)
      push_room(cand, Cand{el, i}, 8000, "Starlink candidates");
  }
  if (cand.size() > STARLINK_PUB_CAP) {
    std::partial_sort(cand.begin(), cand.begin() + STARLINK_PUB_CAP, cand.end(),
                      [](const Cand &a, const Cand &b) { return a.el > b.el; });
    cand.resize(STARLINK_PUB_CAP);
  }
  static pvector<SatRec> staging;  // FAIL-12: persistent at STARLINK_PUB_CAP (see do_above)
  staging.clear();

  int total = 0;
  for (const auto &c : cand) {
    const SlElem &s = starlink[c.i];
    double x0[3], x1[3];
    sl_ecef(s, t, x0);
    sl_ecef(s, t + 10.0, x1);
    SatRec r;
    r.id = s.id;
    r.kind = K_STARLINK;
    r.launch = s.launch;
    if (s.num > 0)
      snprintf(r.name, sizeof(r.name), "STARLINK-%ld", (long) s.num);
    else
      snprintf(r.name, sizeof(r.name), "STARLINK %ld", (long) s.id);
    strncpy(r.cc, "USA", sizeof(r.cc) - 1);
    const double n = s.mdot;  // rad/s, close enough for the details card
    r.period_min = (float) (2 * M_PI / n / 60.0);
    r.incl_deg = s.incl * 180.0f / (float) M_PI;
    r.speed_kms = sqrtf(398600.4418f / s.a_km);
    make_fix(x0, x1, t, r);
    if (!push_room(staging, r, STARLINK_PUB_CAP, "Starlink positions"))
      break;
    total += c.el >= 0;
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.starlink, staging);
  pending.starlink_total = total;
  pending.fresh |= L_STARLINK;
  xSemaphoreGive(mutex);
}

inline void do_iss() {
  const double t = clock_now();
  const geo::Observer o = observer();
  SatRec r, c;
  const bool ok_i = have_iss && sgp_rec(iss, t, r, o);
  const bool ok_c = have_css && sgp_rec(css, t, c, o);  // UI-52: Tiangong rides along
  if (!ok_i && !ok_c)
    return;
  if (ok_i) {
    r.kind = K_ISS;
    strncpy(r.cc, "ISS", sizeof(r.cc) - 1);
  }
  if (ok_c) {
    c.kind = K_ISS;
    strncpy(c.cc, "CN", sizeof(c.cc) - 1);
  }
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (ok_i) {
    pending.iss = r;
    pending.iss_ok = true;
  }
  if (ok_c) {
    pending.css = c;
    pending.css_ok = true;
  }
  pending.fresh |= L_ISS;
  xSemaphoreGive(mutex);
}

// JOB_PASS (DATA-6): the next ISS passes over the next 4 days, horizon to horizon,
// kept when they climb above 10°. A 60 s search then bisection to 1 s.
constexpr double PASS_SPAN_S = 4 * 86400.0, PASS_STEP_S = 60.0;
constexpr float PASS_MIN_MAX_EL = 10.0f;
constexpr size_t PASS_KEEP = 3;

inline void find_passes(SgpSat &sat, pvector<Pass> &staging) {
  const geo::Observer o = observer();
  auto el_at = [&](double t, geo::AzEl *out = nullptr) {
    double x[3] = {0, 0, 0};
    if (!sgp_ecef(sat, t, x))
      return -90.0f;
    const geo::AzEl a = azel_of(o, x);
    if (out)
      *out = a;
    return a.el;
  };
  auto cross = [&](double a, double b, bool rising) {  // el crosses 0 between a and b
    for (int i = 0; i < 12; i++) {
      const double m = (a + b) / 2;
      if ((el_at(m) >= 0) == rising)
        b = m;
      else
        a = m;
    }
    return (a + b) / 2;
  };
  const double t0 = clock_now();
  staging.clear();
  double prev_t = t0;
  float prev_el = el_at(t0);
  double rise = prev_el >= 0 ? t0 : 0;
  uint32_t step = 0;
  for (double t = t0 + PASS_STEP_S; t < t0 + PASS_SPAN_S && staging.size() < PASS_KEEP; t += PASS_STEP_S) {
    breathe(step++, 500);
    const float el = el_at(t);
    if (prev_el < 0 && el >= 0)
      rise = cross(prev_t, t, true);
    if (prev_el >= 0 && el < 0 && rise > 0) {
      const double set = cross(prev_t, t, false);
      // culmination: golden-section search on elevation
      double a = rise, b = set;
      for (int i = 0; i < 30; i++) {
        const double m1 = b - (b - a) * 0.618, m2 = a + (b - a) * 0.618;
        if (el_at(m1) < el_at(m2))
          a = m1;
        else
          b = m2;
      }
      Pass p;
      p.start = (int64_t) llround(rise);
      p.end = (int64_t) llround(set);
      p.max = (int64_t) llround((a + b) / 2);
      geo::AzEl ar, am, as;
      el_at(rise, &ar);
      p.max_el = el_at((double) p.max, &am);
      el_at(set, &as);
      strncpy(p.start_dir, compass(ar.az), 3);
      strncpy(p.max_dir, compass(am.az), 3);
      strncpy(p.end_dir, compass(as.az), 3);
      // path and visibility: sunlit ISS while the observer's sky is dark
      p.npath = PASS_PTS;
      for (int k = 0; k < PASS_PTS; k++) {
        const double tk = rise + (set - rise) * k / (PASS_PTS - 1);
        double x[3] = {0, 0, 0};
        if (!sgp_ecef(sat, tk, x))
          continue;
        const geo::AzEl ak = azel_of(o, x);
        p.path[k] = {ak.az, std::max(0.0f, ak.el)};
        const astro::SunMoon sm = astro::compute(tk, jc.lat, jc.lon, jc.alt_m);
        if (sm.sun.el < -6.0f && astro::sunlit({(float) x[0], (float) x[1], (float) x[2]}, sm.sun_ecef_dir) &&
            ak.el >= 10.0f)
          p.visible = true;
      }
      if (p.max_el >= PASS_MIN_MAX_EL)
        staging.push_back(p);
      rise = 0;
    }
    prev_t = t;
    prev_el = el;
  }
}

inline void do_pass() {
  if (!have_iss && !have_css)
    return;
  pvector<Pass> a, b;
  if (have_iss)
    find_passes(iss, a);
  if (have_css)
    find_passes(css, b);  // UI-52
  xSemaphoreTake(mutex, portMAX_DELAY);
  std::swap(pending.passes, a);
  std::swap(pending.css_passes, b);
  pending.fresh |= L_PASS;
  xSemaphoreGive(mutex);
  ESP_LOGD(TAG, "passes: %u ISS, %u Tiangong", (unsigned) pending.passes.size(), (unsigned) pending.css_passes.size());
}

inline void run_job(uint8_t job) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  jc = cfg;  // UI-16: the observer can change at runtime
  xSemaphoreGive(mutex);
  switch (job) {
    case JOB_ELEMENTS:
      if (link_up)  // NET-8: the only job that uses the network
        do_elements();
      break;
    case JOB_ABOVE:
      do_above();
      break;
    case JOB_STARLINK:
      if (jc.starlink_radius > 0)
        do_starlink();
      break;
    case JOB_ISS:
      do_iss();
      break;
    case JOB_PASS:
      do_pass();
      break;
    case JOB_SUNIMG:  // UI-59
    case JOB_MOONIMG:  // UI-60
    case JOB_EARTHIMG:  // UI-62
    case JOB_REGIONIMG:  // UI-62a
    case JOB_PLANETIMG:  // UI-61a
    case JOB_ALLSKYIMG:  // UI-75
      if (link_up || job == JOB_PLANETIMG)  // UI-61d: the planets are built in
        do_image(job == JOB_SUNIMG     ? IMG_SUN
                 : job == JOB_MOONIMG  ? IMG_MOON
                 : job == JOB_EARTHIMG ? IMG_EARTH
                 : job == JOB_REGIONIMG ? IMG_REGION
                 : job == JOB_ALLSKYIMG ? IMG_ALLSKY
                                        : IMG_PLANET);
      break;
    default:
      if (extra_job)
        extra_job(job);
      break;
  }
}

// ARCH-4: one queued copy per job type at most. The task is single-threaded, so a
// long element download used to let the 1-2 s position jobs pile up (queue full, then
// a burst of stale repeats). A request for a job already waiting is merged into it.
inline std::atomic<uint32_t> queued{0};

inline void task_main(void *) {
  // the two big picture buffers (768 KB JPEG, 777 KB sums) are taken first, before the orbital
  // lists, while PSRAM is still in one piece, and kept (4.6.22-4.6.24 took them after reserving
  // the lists at their caps and every picture failed)
  if (img_jpg() == nullptr || fit_acc() == nullptr)
    ESP_LOGW(TAG, "pictures: PSRAM buffers not available");
  cache_load();         // DATA-10
  launch_cache_load();  // UI-54a
  event_cache_load();   // UI-54c
  ESP_LOGI(TAG, "PSRAM free %u KB", (unsigned) (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));  // no heap walk (FAIL-9)
  for (;;) {
    uint8_t job;
    if (xQueueReceive(queue, &job, portMAX_DELAY) != pdTRUE)
      continue;
    queued.fetch_and(~(1u << job));  // a request arriving from now on queues a new run
    if (!paused)  // BUILD-6: drop work during an upload
      run_job(job);
  }
}

}  // namespace net

// ------------------------------------------------------------------ loop-side API

inline const Config &config() { return net::cfg; }

// Called once from on_boot. Returns false if memory or the task could not be had.
inline bool net_init(const Config &c) {
  net::cfg = c;
  net::queue = xQueueCreate(12, sizeof(uint8_t));
  net::mutex = xSemaphoreCreateMutex();
  net::json_buf = (char *) heap_caps_malloc(net::JSON_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!net::queue || !net::mutex || !net::json_buf) {
    ESP_LOGE(net::TAG, "init: out of memory");
    return false;
  }
  // Core 0 beside Wi-Fi; the ESPHome loop and LVGL run on core 1. Priority 1: the
  // Starlink scan is several hundred ms of maths and must not hold up Wi-Fi.
  if (xTaskCreatePinnedToCore(net::task_main, "sat_net", net::STACK, nullptr, 1, nullptr, 0) != pdPASS) {
    ESP_LOGE(net::TAG, "init: task create failed");
    return false;
  }
  ESP_LOGI(net::TAG, "data task started (group %s, starlink r=%d)", c.sat_group.c_str(), c.starlink_radius);
  return true;
}

// ARCH-4: never blocks. False when the queue is full (the caller retries next tick).
inline bool enqueue(Job j) {
  if (net::queue == nullptr)
    return false;
  const uint32_t bit = 1u << j;
  if (net::queued.fetch_or(bit) & bit)
    return true;  // ARCH-4: already waiting; this request is served by that run
  uint8_t v = j;
  if (xQueueSend(net::queue, &v, 0) == pdTRUE)
    return true;
  net::queued.fetch_and(~bit);
  return false;
}

// UI-16: new observer location for later jobs.
inline void net_set_observer(double lat, double lon) {
  if (net::mutex != nullptr)
    xSemaphoreTake(net::mutex, portMAX_DELAY);
  net::cfg.lat = lat;
  net::cfg.lon = lon;
  net::geo_force();  // UI-28: GEO dots move with the observer
  if (net::mutex != nullptr)
    xSemaphoreGive(net::mutex);
}

// UI-22: which groups the element job downloads.
inline void net_set_layers(bool sats_on, bool starlink_on, bool meo_on, bool geo_on) {
  if (net::mutex != nullptr)
    xSemaphoreTake(net::mutex, portMAX_DELAY);
  net::cfg.sats_on = sats_on;
  net::cfg.starlink_on = starlink_on;
  if (geo_on && !net::cfg.geo_on)
    net::geo_force();
  net::cfg.meo_on = meo_on;
  net::cfg.geo_on = geo_on;
  if (net::mutex != nullptr)
    xSemaphoreGive(net::mutex);
}

inline void net_set_debris(bool on) {  // UI-35
  if (net::mutex != nullptr)
    xSemaphoreTake(net::mutex, portMAX_DELAY);
  net::cfg.debris_on = on;
  net::geo_force();
  if (net::mutex != nullptr)
    xSemaphoreGive(net::mutex);
}

inline void net_set_starlink_radius(int r) {  // UI-39
  if (net::mutex != nullptr)
    xSemaphoreTake(net::mutex, portMAX_DELAY);
  net::cfg.starlink_radius = r;
  if (net::mutex != nullptr)
    xSemaphoreGive(net::mutex);
}

// BUILD-6
inline void net_pause(bool on) {
  net::paused = on;
  if (on && net::queue)
    xQueueReset(net::queue);
}

// Loop-owned copies (ARCH-9: sensors read these).
struct Live {
  pvector<SatRec> sats, starlink, geo;
  int sat_total = 0, starlink_total = 0, geo_total = 0;
  SatRec iss;
  bool iss_ok = false;
  pvector<Pass> passes;
  SatRec css;  // UI-52
  bool css_ok = false;
  pvector<Pass> css_passes;
  net::SolarWind wind;               // UI-55
  net::AuroraChance ovation;         // UI-58
  net::ImageInfo img[net::IMG_N];                 // UI-59/60
  uint16_t *img_px[net::IMG_N] = {};  // IMG_PX x IMG_PX RGB565 shown by the pictures
  bool img_new[net::IMG_N] = {};      // new pixels since the picture last looked
  pvector<net::LaunchRec> launches;  // UI-54
  pvector<net::EventRec> events;     // UI-54c
  pvector<comets::El> comet_list;        // UI-63
  double comets_at = 0;              // when that list arrived (0: never)
  pvector<net::SwxRec> swx;          // UI-73: current space weather notices
  pvector<net::DecayRec> decays;     // UI-74: re-entries within two days
  double decays_at = 0;
  DataStatus status;
  pvector<net::KpPt> kp;  // UI-37
};
inline Live live;

// ARCH-5: never blocks; returns the fresh-layer mask (0 if nothing new or mutex busy).
inline uint8_t drain() {
  if (net::mutex == nullptr || xSemaphoreTake(net::mutex, 0) != pdTRUE)
    return 0;
  net::Pending &p = net::pending;
  const uint8_t fresh = p.fresh;
  if (fresh & L_SAT) {
    std::swap(live.sats, p.sats);
    live.sat_total = p.sat_total;
  }
  if (fresh & L_STARLINK) {
    std::swap(live.starlink, p.starlink);
    live.starlink_total = p.starlink_total;
  }
  if (fresh & L_GEO) {
    std::swap(live.geo, p.geo);
    live.geo_total = p.geo_total;
  }
  if (fresh & L_ISS) {
    live.iss = p.iss;
    live.iss_ok = p.iss_ok;
    live.css = p.css;
    live.css_ok = p.css_ok;
  }
  if (fresh & L_PASS) {
    std::swap(live.passes, p.passes);
    std::swap(live.css_passes, p.css_passes);
  }
  if (fresh & L_EXTRA) {  // UI-54/55/58/59
    live.wind = p.wind;
    live.ovation = p.ovation;
    for (int k = 0; k < net::IMG_N; k++) {  // UI-59/60: pixels copied while the task is held off
      const bool px_new = p.img[k].fresh && net::img_work_owner == k && net::img_work_buf;
      live.img[k] = p.img[k];
      if (px_new) {
        if (live.img_px[k] == nullptr)
          live.img_px[k] = (uint16_t *) heap_caps_malloc(net::IMG_PX * net::IMG_PX * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (live.img_px[k]) {
          memcpy(live.img_px[k], net::img_work_buf, net::IMG_PX * net::IMG_PX * 2);
          live.img_new[k] = true;
          net::img_shown[k] = live.img_px[k];  // UI-59f: under the next one as it arrives
        }
        net::img_work_owner = -1;
      }
      p.img[k].fresh = false;
    }
    if (!p.launches.empty()) {
      std::swap(live.launches, p.launches);
      p.launches.clear();
    }
    if (!p.events.empty()) {  // UI-54c
      std::swap(live.events, p.events);
      p.events.clear();
    }
    if (p.swx_new) {  // UI-73
      std::swap(live.swx, p.swx);
      p.swx.clear();
      p.swx_new = false;
    }
    if (p.decays_new) {  // UI-74
      std::swap(live.decays, p.decays);
      p.decays.clear();
      p.decays_new = false;
      live.decays_at = clock_now();
    }
    if (p.comets_new) {  // UI-63
      std::swap(live.comet_list, p.comet_list);
      p.comet_list.clear();
      p.comets_new = false;
      live.comets_at = clock_now();
    }
  }
  if (fresh & L_STATUS)
    live.status = p.status;
  if (fresh & L_KP)
    std::swap(live.kp, p.kp);
  p.fresh = 0;
  xSemaphoreGive(net::mutex);
  return fresh;
}

}  // namespace sat
