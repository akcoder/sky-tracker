#pragma once
// Optional add-on sensors (HW-8, HW-9):
//   - GPS (ATGM336H or any NMEA module, 9600 baud) on a UART: when it has a fix it
//     supplies the observer latitude/longitude, and the manual fields are locked.
//   - Magnetometer on its own I2C bus (GPIO1/2): QMC5883L (0x0D), QMC5883P (0x2C) or
//     HMC5883L (0x1E), auto-detected. When present it supplies the map heading, and
//     the manual heading is locked.
// Everything here runs on the main loop (the I2C bus is shared with the GT911 touch
// controller, which is also polled from the main loop) if it is ever moved there.
#include <cmath>
#include <cstdint>
#include <cstring>
#include "esphome/components/i2c/i2c_bus.h"
#include "driver/gpio.h"
#include "esp_attr.h"

namespace sat {
namespace hw {

static const char *const HW_TAG = "sky_hw";

// ------------------------------------------------------------------ magnetometer
enum Chip : uint8_t { CHIP_NONE, CHIP_QMC5883L, CHIP_QMC5883P, CHIP_HMC5883L };

inline const char *chip_name(Chip c) {
  switch (c) {
    case CHIP_QMC5883L: return "QMC5883L";
    case CHIP_QMC5883P: return "QMC5883P";
    case CHIP_HMC5883L: return "HMC5883L";
    default: return "none";
  }
}

struct Calibration {  // hard-iron centre and soft-iron scale for X and Y (raw counts)
  float cx = 0, cy = 0, sx = 1, sy = 1;
  bool valid = false;
};

struct Compass {
  esphome::i2c::I2CBus *bus = nullptr;
  Chip chip = CHIP_NONE;
  uint8_t addr = 0;
  int fails = 0;
  uint32_t next_probe_ms = 0;
  Calibration cal;
  // calibration run (HW-9): min/max of X and Y while the user turns the display
  bool calibrating = false;
  uint32_t cal_end_ms = 0;
  float mnx = 0, mxx = 0, mny = 0, mxy = 0;
  int cal_samples = 0;
  // HW-9b guided run: 10° sectors seen, total turn, and the turning rate
  uint64_t cal_bins = 0;
  float cal_last_ang = NAN, cal_turn = 0, cal_rate = 0;
  uint32_t cal_start_ms = 0, cal_last_ms = 0;
  static constexpr int CAL_HIST = 320;  // 64 s at 5 Hz
  float hx[CAL_HIST], hy[CAL_HIST];
  int hn = 0;
  double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sxz = 0, syz = 0, sz = 0;  // circle-fit sums (about the first sample)
  // smoothed heading as a unit vector (averages correctly across 0/360)
  float vc = 0, vs = 0;
  bool have = false;
  float raw_x = 0, raw_y = 0, raw_z = 0;
};
inline Compass compass;

inline bool reg_write(uint8_t a, uint8_t reg, uint8_t val) {
  uint8_t b[2] = {reg, val};
  return compass.bus->write(a, b, 2) == esphome::i2c::ERROR_OK;
}
inline bool reg_read(uint8_t a, uint8_t reg, uint8_t *out, size_t n) {
  return compass.bus->write_readv(a, &reg, 1, out, n) == esphome::i2c::ERROR_OK;
}

inline bool init_chip() {
  const uint8_t a = compass.addr;
  switch (compass.chip) {
    case CHIP_QMC5883L:
      // SET/RESET period, then OSR 512, range 2 G, 50 Hz, continuous (0b00_00_01_01)
      return reg_write(a, 0x0B, 0x01) && reg_write(a, 0x09, 0x05);
    case CHIP_QMC5883P:
      // datasheet start-up: axis signs, SET/RESET on + 8 G, normal mode 200 Hz
      return reg_write(a, 0x29, 0x06) && reg_write(a, 0x0B, 0x08) && reg_write(a, 0x0A, 0xCD);
    case CHIP_HMC5883L:
      // 8-sample average, 15 Hz; gain 1.3 G; continuous
      return reg_write(a, 0x00, 0x70) && reg_write(a, 0x01, 0x20) && reg_write(a, 0x02, 0x00);
    default:
      return false;
  }
}

// Look for a magnetometer; true when one is found and started.
inline bool probe() {
  if (compass.bus == nullptr)
    return false;
  uint8_t b[3] = {0, 0, 0};
  compass.chip = CHIP_NONE;
  if (reg_read(0x0D, 0x0D, b, 1) && b[0] == 0xFF) {
    compass.chip = CHIP_QMC5883L;
    compass.addr = 0x0D;
  } else if (reg_read(0x2C, 0x00, b, 1) && b[0] == 0x80) {
    compass.chip = CHIP_QMC5883P;
    compass.addr = 0x2C;
  } else if (reg_read(0x1E, 0x0A, b, 3) && b[0] == 'H' && b[1] == '4' && b[2] == '3') {
    compass.chip = CHIP_HMC5883L;
    compass.addr = 0x1E;
  }
  if (compass.chip == CHIP_NONE)
    return false;
  if (!init_chip()) {
    compass.chip = CHIP_NONE;
    return false;
  }
  compass.fails = 0;
  compass.have = false;
  ESP_LOGI(HW_TAG, "compass: %s at 0x%02X", chip_name(compass.chip), compass.addr);
  return true;
}

// One raw sample: 1 = new sample in x/y/z, 0 = none ready yet, -1 = bus error.
inline int read_raw(float &x, float &y, float &z) {
  uint8_t d[6];
  const uint8_t a = compass.addr;
  switch (compass.chip) {
    case CHIP_QMC5883L: {
      uint8_t st = 0;
      if (!reg_read(a, 0x06, &st, 1))
        return -1;
      if (!(st & 0x01))
        return 0;
      if (!reg_read(a, 0x00, d, 6))
        return -1;
      x = (int16_t) (d[0] | d[1] << 8);
      y = (int16_t) (d[2] | d[3] << 8);
      z = (int16_t) (d[4] | d[5] << 8);
      return 1;
    }
    case CHIP_QMC5883P: {
      uint8_t st = 0;
      if (!reg_read(a, 0x09, &st, 1))
        return -1;
      if (!(st & 0x01))
        return 0;
      if (!reg_read(a, 0x01, d, 6))
        return -1;
      x = (int16_t) (d[0] | d[1] << 8);
      y = (int16_t) (d[2] | d[3] << 8);
      z = (int16_t) (d[4] | d[5] << 8);
      return 1;
    }
    case CHIP_HMC5883L:
      if (!reg_read(a, 0x03, d, 6))
        return -1;
      x = (int16_t) (d[0] << 8 | d[1]);  // register order X, Z, Y
      z = (int16_t) (d[2] << 8 | d[3]);
      y = (int16_t) (d[4] << 8 | d[5]);
      return 1;
    default:
      return -1;
  }
}

inline void set_bus(esphome::i2c::I2CBus *bus) { compass.bus = bus; }
inline void set_calibration(float cx, float cy, float sx, float sy, bool valid) {
  compass.cal = {cx, cy, (sx > 0.05f && sx < 20) ? sx : 1.0f, (sy > 0.05f && sy < 20) ? sy : 1.0f, valid};
}
inline bool compass_present() { return compass.chip != CHIP_NONE; }
inline const char *compass_chip() { return chip_name(compass.chip); }
inline bool compass_calibrated() { return compass.cal.valid; }
inline bool compass_calibrating() { return compass.calibrating; }

// HW-9: start a calibration run of `secs` seconds.
inline void calibrate_start(uint32_t now_ms, uint32_t secs) {
  if (!compass_present())
    return;
  compass.calibrating = true;
  compass.cal_end_ms = now_ms + secs * 1000;
  compass.cal_samples = 0;
  compass.cal_bins = 0;
  compass.cal_last_ang = NAN;
  compass.cal_turn = compass.cal_rate = 0;
  compass.cal_start_ms = compass.cal_last_ms = now_ms;
  compass.hn = 0;
  compass.sx = compass.sy = compass.sxx = compass.syy = compass.sxy = compass.sxz = compass.syz = compass.sz = 0;
  ESP_LOGI(HW_TAG, "compass calibration: turn the display through a full circle");
}

// Result of a finished calibration run; valid=false if the run saw too little rotation.
inline Calibration calibrate_finish() {
  Calibration c;
  compass.calibrating = false;
  const float rx = compass.mxx - compass.mnx, ry = compass.mxy - compass.mny;
  if (compass.cal_samples < 20 || rx <= 0 || ry <= 0 || rx / ry > 3 || ry / rx > 3) {
    ESP_LOGW(HW_TAG, "compass calibration failed (%d samples, ranges %.0f/%.0f)", compass.cal_samples, rx, ry);
    return c;  // invalid
  }
  const float avg = (rx + ry) / 2;
  c.cx = (compass.mxx + compass.mnx) / 2;
  c.cy = (compass.mxy + compass.mny) / 2;
  c.sx = avg / rx;
  c.sy = avg / ry;
  c.valid = true;
  compass.cal = c;
  compass.have = false;
  ESP_LOGI(HW_TAG, "compass calibrated: centre %.0f,%.0f scale %.3f,%.3f", c.cx, c.cy, c.sx, c.sy);
  return c;
}

// HW-9 / HW-9b: one fresh sample during a calibration run
inline void cal_feed(float x, float y, uint32_t now_ms) {
  if (compass.cal_samples == 0) {
    compass.mnx = compass.mxx = x;
    compass.mny = compass.mxy = y;
  }
  compass.mnx = std::min(compass.mnx, x);
  compass.mxx = std::max(compass.mxx, x);
  compass.mny = std::min(compass.mny, y);
  compass.mxy = std::max(compass.mxy, y);
  compass.cal_samples++;
  // HW-9b: where the field points, measured about a circle fitted to the samples so
  // far (algebraic least squares; right after a few seconds of turning, unlike the
  // min/max midpoint, which needs half a turn), then the sectors seen, the total
  // turn and the turning rate, all recomputed from the samples
  if (compass.hn < Compass::CAL_HIST) {
    compass.hx[compass.hn] = x;
    compass.hy[compass.hn] = y;
    compass.hn++;
    const double u = x - compass.hx[0], v = y - compass.hy[0], z = u * u + v * v;
    compass.sx += u;
    compass.sy += v;
    compass.sxx += u * u;
    compass.syy += v * v;
    compass.sxy += u * v;
    compass.sxz += u * z;
    compass.syz += v * z;
    compass.sz += z;
  }
  const double n = compass.hn;
  // normal equations for u^2+v^2 + D u + E v + F = 0
  const double a11 = compass.sxx, a12 = compass.sxy, a13 = compass.sx, a22 = compass.syy, a23 = compass.sy,
               a33 = n, b1 = -compass.sxz, b2 = -compass.syz, b3 = -compass.sz;
  const double det = a11 * (a22 * a33 - a23 * a23) - a12 * (a12 * a33 - a23 * a13) + a13 * (a12 * a23 - a22 * a13);
  const float span = std::max(compass.mxx - compass.mnx, compass.mxy - compass.mny);
  if (n >= 6 && fabs(det) > 1e-9 && span > 20) {
    const double D = (b1 * (a22 * a33 - a23 * a23) - a12 * (b2 * a33 - a23 * b3) + a13 * (b2 * a23 - a22 * b3)) / det;
    const double E = (a11 * (b2 * a33 - a23 * b3) - b1 * (a12 * a33 - a23 * a13) + a13 * (a12 * b3 - b2 * a13)) / det;
    const float ccx = compass.hx[0] - (float) (D / 2), ccy = compass.hy[0] - (float) (E / 2);
    uint64_t bins = 0;
    float turn = 0, prev = NAN;
    for (int i = 0; i < compass.hn; i++) {
      const float ang = atan2f(compass.hy[i] - ccy, compass.hx[i] - ccx) * 57.29578f;
      bins |= 1ull << ((int) ((ang + 180.0f) / 10.0f) % 36);
      if (!std::isnan(prev)) {
        float d = ang - prev;
        d -= 360.0f * floorf((d + 180.0f) / 360.0f);
        turn += d;
      }
      prev = ang;
    }
    const float dt = (now_ms - compass.cal_last_ms) / 1000.0f;
    if (!std::isnan(compass.cal_last_ang) && dt > 0.01f) {
      float d = prev - compass.cal_last_ang;
      d -= 360.0f * floorf((d + 180.0f) / 360.0f);
      compass.cal_rate += 0.3f * (fabsf(d) / dt - compass.cal_rate);
    }
    compass.cal_bins = bins;
    compass.cal_turn = turn;
    compass.cal_last_ang = prev;
    compass.cal_last_ms = now_ms;
  }
}

// Poll at ~5 Hz from the main loop. Handles detection, hot-unplug and smoothing.
// Returns true while a calibration run has just ended (caller saves the result).
inline bool compass_poll(uint32_t now_ms) {
  if (compass.bus == nullptr)
    return false;
  if (compass.chip == CHIP_NONE) {
    if ((int32_t) (now_ms - compass.next_probe_ms) >= 0) {
      compass.next_probe_ms = now_ms + 30000;  // look again every 30 s
      probe();
    }
    return false;
  }
  float x = compass.raw_x, y = compass.raw_y, z = compass.raw_z;
  const int r = read_raw(x, y, z);
  if (r < 0) {
    if (++compass.fails >= 10) {
      ESP_LOGW(HW_TAG, "compass lost");
      compass.chip = CHIP_NONE;
      compass.calibrating = false;
      compass.have = false;
      compass.next_probe_ms = now_ms + 30000;
    }
    return false;
  }
  compass.fails = 0;
  const bool fresh = r > 0;
  compass.raw_x = x;
  compass.raw_y = y;
  compass.raw_z = z;
  if (compass.calibrating) {
    if (fresh)
      cal_feed(x, y, now_ms);
    // done: every sector seen after at least a full turn, or out of time
    if ((__builtin_popcountll(compass.cal_bins) >= 35 && fabsf(compass.cal_turn) >= 360.0f &&
         compass.cal_samples >= 40) ||
        (int32_t) (now_ms - compass.cal_end_ms) >= 0)
      return true;
  }
  if (!fresh && compass.have)
    return false;
  const float cx = (x - compass.cal.cx) * compass.cal.sx, cy = (y - compass.cal.cy) * compass.cal.sy;
  const float n = sqrtf(cx * cx + cy * cy);
  if (n < 1e-3f)
    return false;
  if (!compass.have) {
    compass.vc = cx / n;
    compass.vs = cy / n;
    compass.have = true;
  } else {
    compass.vc += 0.25f * (cx / n - compass.vc);
    compass.vs += 0.25f * (cy / n - compass.vs);
  }
  return false;
}

// Magnetic heading of the sensor's X arrow, degrees clockwise from magnetic north,
// sensor level with its chip facing up. NAN without a compass.
inline float magnetic_heading(bool reverse) {
  if (!compass.have || compass.chip == CHIP_NONE)
    return NAN;
  float h = atan2f(compass.vs, compass.vc) * 57.29578f;
  if (reverse)
    h = -h;
  return h;
}

inline float wrap360(float d) {
  d = fmodf(d, 360.0f);
  return d < 0 ? d + 360.0f : d;
}
inline float angle_diff(float a, float b) {  // a-b in -180..180
  float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
  return d;
}

// ------------------------------------------------------------------ GPS (HW-8)
enum GpsState : uint8_t { GPS_ABSENT, GPS_SEARCHING, GPS_FIX };
struct Gps {
  uint32_t last_chars = 0, last_rx_ms = 0;
  uint32_t last_fix_ms = 0;
  bool ever_fix = false;
  GpsState state = GPS_ABSENT;
  int sats = 0;
  double lat = NAN, lon = NAN, alt = NAN;
  bool applied = false;               // observer has been set from this lock
  double applied_lat = NAN, applied_lon = NAN;  // what the GPS last wrote to the observer
};
inline Gps gps;

// Feed from the YAML interval with the TinyGPS++ counters. `valid` = a location
// less than 5 s old with at least 4 satellites and HDOP under 5.
inline void gps_update(uint32_t now_ms, uint32_t chars, bool valid, double lat, double lon, double alt, int sats) {
  if (chars != gps.last_chars) {
    gps.last_chars = chars;
    gps.last_rx_ms = now_ms;
  }
  gps.sats = sats;
  const bool talking = gps.last_rx_ms != 0 && now_ms - gps.last_rx_ms < 10000;
  if (valid && talking) {
    gps.lat = lat;
    gps.lon = lon;
    gps.alt = alt;
    gps.last_fix_ms = now_ms;
    if (!gps.ever_fix)
      ESP_LOGI(HW_TAG, "GPS fix: %.5f, %.5f (%d satellites)", lat, lon, sats);
    gps.ever_fix = true;
  }
  const GpsState was = gps.state;
  gps.state = !talking ? GPS_ABSENT : (valid ? GPS_FIX : GPS_SEARCHING);
  if (!talking) {
    gps.ever_fix = false;  // unplugged: manual entry again
    gps.applied = false;
  }
  if (was != gps.state)
    ESP_LOGI(HW_TAG, "GPS: %s", gps.state == GPS_ABSENT ? "not detected" : gps.state == GPS_FIX ? "fix" : "searching");
}

// The location is locked to GPS once the module has had a fix and is still talking.
inline bool gps_locked() { return gps.ever_fix && gps.state != GPS_ABSENT; }

// New observer position from the GPS (3 decimals), or false to leave it. The first fix after a
// lock moves the observer on any change; after that only moves of
// 0.0015° (~150 m) count, so fix jitter never rewrites the saved location.
inline bool gps_new_position(double cur_lat, double cur_lon, double &lat, double &lon) {
  if (!gps_locked() || gps.state != GPS_FIX)
    return false;
  // 3 decimals (~100 m). First fix: any change; after that two steps, so a fix
  // wobbling across a rounding boundary never rewrites the saved location.
  const double th = gps.applied ? 1.5e-3 : 5e-4;
  lat = round(gps.lat * 1000.0) / 1000.0;
  lon = round(gps.lon * 1000.0) / 1000.0;
  gps.applied = true;
  if (fabs(lat - cur_lat) < th && fabs(lon - cur_lon) < th) {
    gps.applied_lat = cur_lat;
    gps.applied_lon = cur_lon;
    return false;
  }
  gps.applied_lat = lat;
  gps.applied_lon = lon;
  return true;
}

// HA/web edits of a locked value are rejected: these say whether a new value is the
// device's own.
inline bool loc_edit_allowed(double lat, double lon) {
  return !gps_locked() || !gps.applied || (fabs(lat - gps.applied_lat) < 1e-6 && fabs(lon - gps.applied_lon) < 1e-6);
}

// ------------------------------------------------------------------ heading output
inline float head_pub = NAN;  // last heading published from the compass (true, degrees)
inline bool heading_locked() { return compass_present() && compass.have; }
inline bool head_edit_allowed(float x) { return !heading_locked() || std::isnan(head_pub) || fabsf(angle_diff(x, head_pub)) < 0.5f; }


// ------------------------------------------------------------------ GPS diagnostics (HW-8)
// Tells "nothing on the wire" apart from "signal at the wrong speed" and "good NMEA":
// edges on the RX pin are counted with a GPIO interrupt for 1 s in every 5 (the UART
// keeps the pin; an interrupt only watches it), and the idle level is sampled. If the
// line is busy but no sentence passes its checksum, the baud rate is stepped through
// the usual GPS rates.
struct GpsDiag {
  int pin = -1;
  volatile uint32_t edges = 0;
  bool isr_ok = false, counting = false;
  uint32_t win_start = 0, next_win = 0;
  int32_t edges_per_s = -1;  // -1 = not measured yet
  int level = -1;
  uint32_t baud = 9600;
  uint32_t last_chars = 0, last_passed = 0, last_failed = 0;
  uint32_t bytes_per_s = 0, good_total = 0, bad_total = 0;
  uint32_t busy_no_nmea_s = 0;
  uint32_t baud_idx = 0;
};
inline GpsDiag gps_diag;

static void IRAM_ATTR gps_edge_isr(void *) { gps_diag.edges = gps_diag.edges + 1; }

inline void gps_diag_init(int pin) {
  gps_diag.pin = pin;
  esp_err_t e = gpio_install_isr_service(0);
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {  // INVALID_STATE: already installed
    ESP_LOGW(HW_TAG, "GPS diag: no GPIO ISR service (%d)", e);
    return;
  }
  gpio_set_intr_type((gpio_num_t) pin, GPIO_INTR_ANYEDGE);
  if (gpio_isr_handler_add((gpio_num_t) pin, gps_edge_isr, nullptr) != ESP_OK) {
    ESP_LOGW(HW_TAG, "GPS diag: cannot watch GPIO%d", pin);
    return;
  }
  gpio_intr_disable((gpio_num_t) pin);
  gps_diag.isr_ok = true;
}

// Call once a second with TinyGPS++'s counters. Returns a new baud rate to switch the
// UART to, or 0 to keep the current one.
inline uint32_t gps_diag_tick(uint32_t now_ms, uint32_t chars, uint32_t passed, uint32_t failed) {
  GpsDiag &d = gps_diag;
  if (d.pin >= 0)
    d.level = gpio_get_level((gpio_num_t) d.pin);
  if (d.isr_ok) {
    if (d.counting && now_ms - d.win_start >= 1000) {
      gpio_intr_disable((gpio_num_t) d.pin);
      d.counting = false;
      d.edges_per_s = (int32_t) ((uint64_t) d.edges * 1000 / (now_ms - d.win_start));
      d.next_win = now_ms + 4000;
    } else if (!d.counting && d.good_total == 0 && (int32_t) (now_ms - d.next_win) >= 0) {
      // (stops once good NMEA arrives: the diagnostic is no longer needed)
      d.edges = 0;
      d.win_start = now_ms;
      d.counting = true;
      gpio_intr_enable((gpio_num_t) d.pin);
    }
  }
  d.bytes_per_s = chars - d.last_chars;
  const uint32_t good = passed - d.last_passed, bad = failed - d.last_failed;
  d.good_total += good;
  d.bad_total += bad;
  d.last_chars = chars;
  d.last_passed = passed;
  d.last_failed = failed;
  // Busy line (bytes or edges) but not one good sentence: try the next speed after 5 s.
  const bool busy = d.bytes_per_s > 0 || d.edges_per_s > 20;
  if (busy && good == 0 && d.good_total == 0)
    d.busy_no_nmea_s++;
  else
    d.busy_no_nmea_s = 0;
  if (d.busy_no_nmea_s >= 5) {
    static const uint32_t rates[] = {9600, 115200, 38400, 57600, 4800, 19200};
    d.baud_idx = (d.baud_idx + 1) % (sizeof(rates) / sizeof(rates[0]));
    d.baud = rates[d.baud_idx];
    d.busy_no_nmea_s = 0;
    ESP_LOGW(HW_TAG, "GPS: line busy but no valid NMEA; trying %u baud", (unsigned) d.baud);
    return d.baud;
  }
  return 0;
}

// One line for Home Assistant / the log.
inline void gps_diag_text(char *b, size_t n) {
  const GpsDiag &d = gps_diag;
  char e[16];
  if (d.edges_per_s < 0)
    snprintf(e, sizeof(e), "?");
  else
    snprintf(e, sizeof(e), "%d", (int) d.edges_per_s);
  snprintf(b, n, "GPIO%d %s, %s edges/s, %u B/s @%u, NMEA ok %u bad %u", d.pin,
           d.level < 0 ? "?" : (d.level ? "high" : "LOW"), e, (unsigned) d.bytes_per_s, (unsigned) d.baud,
           (unsigned) d.good_total, (unsigned) d.bad_total);
}



// ------------------------------------------------------------------ NMEA reader (HW-8)
// Our own small parser instead of ESPHome's gps component, so the raw sentences can be
// shown on the debug page. Reads GGA (fix, satellites used, HDOP, altitude), RMC
// (validity, speed, course, date) and GSV (satellites in view), any talker (GP/GN/BD).
struct Nmea {
  char buf[96];
  int len = 0;
  uint32_t chars = 0, passed = 0, failed = 0;
  static constexpr int KEEP = 6;
  char recent[KEEP][72] = {};  // last sentences, newest at [head-1]
  int head = 0;
  // latest decoded values
  bool rmc_valid = false;
  int fix_q = 0, sats_used = -1, sats_view = -1;
  float hdop = NAN, alt = NAN, speed_kmh = NAN, course = NAN;
  double lat = NAN, lon = NAN;
  char utc[12] = "--:--:--", date[12] = "--";
  uint32_t fix_ms = 0;  // when GGA last reported a position fix
};
inline Nmea nmea;

inline int nmea_fields(char *s, char **f, int max) {
  int n = 0;
  f[n++] = s;
  for (char *p = s; *p && n < max; p++)
    if (*p == ',') {
      *p = 0;
      f[n++] = p + 1;
    }
  return n;
}
inline double nmea_coord(const char *v, const char *hemi) {
  if (!*v || !*hemi)
    return NAN;
  const double x = atof(v);
  const double deg = floor(x / 100.0);
  double d = deg + (x - deg * 100.0) / 60.0;
  if (*hemi == 'S' || *hemi == 'W')
    d = -d;
  return d;
}
inline uint8_t hexv(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

inline void nmea_sentence(uint32_t now_ms) {
  char *s = nmea.buf;
  s[nmea.len] = 0;
  char *star = strrchr(s, '*');
  if (s[0] != '$' || star == nullptr || star[1] == 0 || star[2] == 0) {
    nmea.failed++;
    return;
  }
  uint8_t sum = 0;
  for (char *p = s + 1; p < star; p++)
    sum ^= (uint8_t) *p;
  if (sum != (uint8_t) (hexv(star[1]) << 4 | hexv(star[2]))) {
    nmea.failed++;
    return;
  }
  nmea.passed++;
  {  // bounded copy (s lives in nmea.buf; snprintf here trips -Wrestrict/-Wformat-truncation)
    const size_t n = strnlen(s, sizeof(nmea.recent[0]) - 1);
    memmove(nmea.recent[nmea.head], s, n);
    nmea.recent[nmea.head][n] = 0;
  }
  nmea.head = (nmea.head + 1) % Nmea::KEEP;
  *star = 0;
  char *f[24];
  const int n = nmea_fields(s + 1, f, 24);
  if (strlen(f[0]) < 5)
    return;
  const char *type = f[0] + strlen(f[0]) - 3;
  if (!strcmp(type, "GGA") && n >= 10) {
    if (f[1][0])
      snprintf(nmea.utc, sizeof(nmea.utc), "%.2s:%.2s:%.2s", f[1], f[1] + 2, f[1] + 4);
    nmea.fix_q = atoi(f[6]);
    nmea.sats_used = f[7][0] ? atoi(f[7]) : -1;
    nmea.hdop = f[8][0] ? atof(f[8]) : NAN;
    nmea.alt = f[9][0] ? atof(f[9]) : NAN;
    if (nmea.fix_q > 0) {
      nmea.lat = nmea_coord(f[2], f[3]);
      nmea.lon = nmea_coord(f[4], f[5]);
      if (!std::isnan(nmea.lat) && !std::isnan(nmea.lon))
        nmea.fix_ms = now_ms;
    }
  } else if (!strcmp(type, "RMC") && n >= 10) {
    nmea.rmc_valid = f[2][0] == 'A';
    nmea.speed_kmh = f[7][0] ? atof(f[7]) * 1.852f : NAN;
    nmea.course = f[8][0] ? atof(f[8]) : NAN;
    if (strlen(f[9]) >= 6)
      snprintf(nmea.date, sizeof(nmea.date), "20%.2s-%.2s-%.2s", f[9] + 4, f[9] + 2, f[9]);
  } else if (!strcmp(type, "GSV") && n >= 4) {
    // several talkers (GP, BD, GL) each report their own count: keep the GN total if
    // present, else the largest single count seen in this second
    const int v = atoi(f[3]);
    if (!strncmp(f[0], "GN", 2) || v > nmea.sats_view)
      nmea.sats_view = v;
  }
}

// Feed received bytes. Call often (the UART buffer holds about 1 s of data).
inline void nmea_feed(uint8_t c, uint32_t now_ms) {
  nmea.chars++;
  if (c == '$')
    nmea.len = 0;
  if (c == '\r' || c == '\n') {
    if (nmea.len > 6)
      nmea_sentence(now_ms);
    nmea.len = 0;
    return;
  }
  if (nmea.len < (int) sizeof(nmea.buf) - 1)
    nmea.buf[nmea.len++] = (char) c;
}

// A usable position: GGA fix less than 5 s old, >= 4 satellites, HDOP < 5.
inline bool nmea_fix_ok(uint32_t now_ms) {
  return nmea.fix_ms != 0 && now_ms - nmea.fix_ms < 5000 && nmea.sats_used >= 4 &&
         (std::isnan(nmea.hdop) || nmea.hdop < 5.0f);
}

// ------------------------------------------------------------------ debug page (UI-27)
inline void debug_text(char *b, size_t n, uint32_t now_ms, int raw = Nmea::KEEP) {  // raw: NMEA lines shown
  const Nmea &m = nmea;
  const Compass &c = compass;
  int k = 0;
  auto add = [&](const char *fmt, auto... a) {
    if (k < (int) n - 1)
      k += snprintf(b + k, n - k, fmt, a...);
  };
  add("GPS  GPIO%d @%u baud  %u B/s\n", gps_diag.pin, (unsigned) gps_diag.baud, (unsigned) gps_diag.bytes_per_s);
  add("  %s  fix q%d  sats %d/%d  HDOP %.1f\n",
      gps.state == GPS_FIX ? "FIX" : gps.state == GPS_SEARCHING ? "searching" : "absent", m.fix_q,
      std::max(0, m.sats_used), std::max(0, m.sats_view), m.hdop);
  if (m.fix_ms)
    add("  %.6f, %.6f  alt %.1f m  (%lu s ago)\n", m.lat, m.lon, m.alt, (unsigned long) ((now_ms - m.fix_ms) / 1000));
  else
    add("  no position yet\n");
  add("  UTC %s %s  RMC %s  %.1f km/h  %.0f\xC2\xB0\n", m.date, m.utc, m.rmc_valid ? "A" : "V", m.speed_kmh, m.course);
  add("  NMEA ok %u  bad %u\n", (unsigned) m.passed, (unsigned) m.failed);
  for (int i = std::max(0, Nmea::KEEP - raw); i < Nmea::KEEP; i++) {  // the newest `raw`
    const char *r = m.recent[(m.head + i) % Nmea::KEEP];
    if (*r)
      add("  %.62s\n", r);
  }
  add("\nCOMPASS  %s", compass_present() ? chip_name(c.chip) : "not connected");
  if (compass_present()) {
    add(" @0x%02X  %s\n", c.addr, c.cal.valid ? "calibrated" : "NOT calibrated");
    add("  raw X %6.0f  Y %6.0f  Z %6.0f\n", c.raw_x, c.raw_y, c.raw_z);
    const float cx = (c.raw_x - c.cal.cx) * c.cal.sx, cy = (c.raw_y - c.cal.cy) * c.cal.sy;
    add("  cal X %6.0f  Y %6.0f   |B| %.0f\n", cx, cy, sqrtf(cx * cx + cy * cy));
    add("  centre %.0f,%.0f  scale %.3f,%.3f\n", c.cal.cx, c.cal.cy, c.cal.sx, c.cal.sy);
    const float mh = magnetic_heading(false);
    add("  magnetic %.1f deg  (published %.0f)\n", std::isnan(mh) ? NAN : wrap360(mh), head_pub);
    if (c.calibrating)
      add("  CALIBRATING: %d samples\n", c.cal_samples);
  } else {
    add("\n");
  }
}


// ------------------------------------------------------------------ HW-9b calibration screen
// A full-screen guide over everything (LVGL top layer): a ring of 36 sectors that
// light up as the display is turned through them, the percentage in the middle, and
// a pace hint: "Turn faster" / "Good pace" / "Slow down" (the chip is read at 5 Hz, so
// a sector needs about half a second). Ends by itself when every sector is seen after
// a full turn (or after 60 s); the caller reports the result with cal_screen_result().
constexpr float CAL_SLOW_DPS = 8.0f, CAL_FAST_DPS = 50.0f;
constexpr uint32_t CAL_MAX_S = 60;
struct CalScreen {
  lv_obj_t *root = nullptr, *ring = nullptr, *pct = nullptr, *pace = nullptr, *hint = nullptr, *btn = nullptr,
           *btn_lbl = nullptr;
  bool done = false;
  uint64_t drawn_bins = 0;
  float drawn_ang = NAN;
};
inline CalScreen cs;

inline void cal_ring_draw_cb(lv_event_t *e) {
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(cs.ring, &a);
  const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2, r = (a.x2 - a.x1) / 2 - 8;
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.center.x = cx;
  d.center.y = cy;
  d.radius = r;
  d.width = 16;
  for (int i = 0; i < 36; i++) {
    const bool on = (compass.cal_bins >> i) & 1;
    d.color = lv_color_hex(on ? 0x4FD18B : 0x26314F);
    // bin i covers field angles -180 + 10 i .. +10; drawn clockwise from the top
    d.start_angle = (lv_value_precise_t) fmodf(270.0f + i * 10.0f + 1.0f, 360.0f);
    d.end_angle = (lv_value_precise_t) fmodf(270.0f + i * 10.0f + 9.0f, 360.0f);
    lv_draw_arc(layer, &d);
  }
  if (!std::isnan(compass.cal_last_ang) && compass.calibrating) {  // where it points now
    const float t = (270.0f + (compass.cal_last_ang + 180.0f)) * 0.0174533f;
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_hex(0xFFFFFF);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = LV_RADIUS_CIRCLE;
    const int px = cx + (int) lroundf(r * cosf(t)), py = cy + (int) lroundf(r * sinf(t));
    const lv_area_t da = {px - 7, py - 7, px + 7, py + 7};
    lv_draw_rect(layer, &rd, &da);
  }
}
inline void cal_screen_close() {
  if (cs.root) {
    lv_obj_delete(cs.root);
    cs = CalScreen();
  }
}
inline void cal_btn_cb(lv_event_t *) {
  if (compass.calibrating)
    compass.calibrating = false;  // cancelled: the saved calibration stays
  cal_screen_close();
}
inline lv_obj_t *cal_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int y, const char *t) {
  lv_obj_t *l = lv_label_create(p);
  if (f)
    lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, 460);
  lv_label_set_text(l, t);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}
inline void cal_screen_open(uint32_t now_ms) {
  if (!compass_present() || cs.root)
    return;
  calibrate_start(now_ms, CAL_MAX_S);
  lv_obj_t *root = lv_obj_create(lv_layer_top());
  cs.root = root;
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_set_style_bg_color(root, lv_color_hex(0x070B18), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);  // nothing underneath takes taps
  const auto &w = ui.w;
  cal_label(root, w.title_font, 0xFF8A1F, 14, "Compass calibration");
  cs.hint = cal_label(root, w.card_font ? w.card_font : w.label_font, 0xC9D3F2, 44,
                      "Keep the display flat and turn it\nslowly all the way round");
  cs.ring = lv_obj_create(root);
  lv_obj_remove_style_all(cs.ring);
  lv_obj_set_size(cs.ring, 260, 260);
  lv_obj_align(cs.ring, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_add_event_cb(cs.ring, cal_ring_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  cs.pct = lv_label_create(root);
  if (w.card_title_font)
    lv_obj_set_style_text_font(cs.pct, w.card_title_font, 0);
  lv_obj_set_style_text_color(cs.pct, lv_color_hex(0xEEF2FF), 0);
  lv_label_set_text(cs.pct, "0%");
  lv_obj_align_to(cs.pct, cs.ring, LV_ALIGN_CENTER, 0, 0);
  cs.pace = cal_label(root, w.title_font, 0xC9D3F2, 372, "Start turning");
  cs.btn = lv_button_create(root);
  lv_obj_set_size(cs.btn, 140, 44);
  lv_obj_align(cs.btn, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_style_bg_color(cs.btn, lv_color_hex(0x1A2547), 0);
  lv_obj_set_style_shadow_width(cs.btn, 0, 0);
  cs.btn_lbl = lv_label_create(cs.btn);
  if (w.title_font)
    lv_obj_set_style_text_font(cs.btn_lbl, w.title_font, 0);
  lv_obj_set_style_text_color(cs.btn_lbl, lv_color_hex(0xEEF2FF), 0);
  lv_label_set_text(cs.btn_lbl, "Cancel");
  lv_obj_center(cs.btn_lbl);
  lv_obj_add_event_cb(cs.btn, cal_btn_cb, LV_EVENT_CLICKED, nullptr);
}
// every compass poll (5 Hz) while the screen is up
inline void cal_screen_update(uint32_t now_ms) {
  if (cs.root == nullptr || cs.done)
    return;
  if (!compass.calibrating) {  // lost the chip mid-run
    cs.done = true;
    lv_label_set_text(cs.pace, "Compass disconnected");
    lv_label_set_text(cs.btn_lbl, "Close");
    return;
  }
  const int seen = __builtin_popcountll(compass.cal_bins);
  char b[32];
  snprintf(b, sizeof(b), "%d%%", std::min(100, seen * 100 / 35));
  if (strcmp(lv_label_get_text(cs.pct), b) != 0)
    lv_label_set_text(cs.pct, b);
  const float el = (now_ms - compass.cal_start_ms) / 1000.0f;
  const char *pace;
  uint32_t col;
  if (compass.cal_rate > CAL_FAST_DPS) {
    pace = "Slow down";
    col = 0xFF9E4A;
  } else if (el < 3.0f && seen < 4) {
    pace = "Start turning";
    col = 0xC9D3F2;
  } else if (compass.cal_rate < CAL_SLOW_DPS) {
    pace = "Turn a little faster";
    col = 0xFFD54A;
  } else {
    pace = "Good pace";
    col = 0x4FD18B;
  }
  if (strcmp(lv_label_get_text(cs.pace), pace) != 0) {
    lv_label_set_text(cs.pace, pace);
    lv_obj_set_style_text_color(cs.pace, lv_color_hex(col), 0);
  }
  if (compass.cal_bins != cs.drawn_bins || compass.cal_last_ang != cs.drawn_ang) {
    cs.drawn_bins = compass.cal_bins;
    cs.drawn_ang = compass.cal_last_ang;
    lv_obj_invalidate(cs.ring);
  }
}
// the run ended (by itself or on time): say how it went
inline void cal_screen_result(bool ok) {
  if (cs.root == nullptr)
    return;
  cs.done = true;
  lv_obj_invalidate(cs.ring);
  lv_label_set_text(cs.pace, ok ? "Calibrated" : "Not enough turning - try again");
  lv_obj_set_style_text_color(cs.pace, lv_color_hex(ok ? 0x4FD18B : 0xFF9E4A), 0);
  if (ok)
    lv_label_set_text(cs.pct, "100%");
  lv_label_set_text(cs.btn_lbl, "Done");
}

}  // namespace hw
}  // namespace sat
