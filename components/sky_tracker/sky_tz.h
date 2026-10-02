#pragma once
// UI-70 time zone picker (Settings > Display, the "Time Zone" select in HA and the web page).
// Auto takes Home Assistant's zone (sent with each time sync, HA 2026.3+). Any other choice is
// applied over it: HA re-sends its zone every time it syncs the clock, so a timer puts the
// chosen one back within half a second. The zones are pre-parsed POSIX rules (ESPHome's
// ParsedTimezone), so no parsing happens on the device.
// Compiled in sky_extra.cpp (SKY_IMPL), as sky_about.h.
#include <cstdint>

#ifndef SKY_IMPL
namespace sat {
namespace tz {
void init();
void set(int index);  // the select's index: 0 = Auto (Home Assistant)
}  // namespace tz
}  // namespace sat
#else
#include <cstring>
#if !defined(SAT_HOST_TEST) && !defined(USE_TIME_TIMEZONE)
#define SKY_TZ_OFF  // a build without time zones: the picker does nothing
#endif
#if !defined(SAT_HOST_TEST) && !defined(SKY_TZ_OFF)
#include "esphome/components/time/posix_tz.h"
#endif

namespace sat {
namespace tz {

#ifdef SKY_TZ_OFF
void init() {}
void set(int) {}
#else
#ifdef SAT_HOST_TEST
struct DSTRule {
  int32_t time_seconds;
  uint16_t day;
  uint8_t type, month, week, day_of_week;
};
struct ParsedTimezone {
  int32_t std_offset_seconds, dst_offset_seconds;
  DSTRule dst_start, dst_end;
};
inline ParsedTimezone host_global{};
inline void set_global(const ParsedTimezone &z) { host_global = z; }
inline const ParsedTimezone &get_global() { return host_global; }
constexpr uint8_t MWD = 1, NONE = 0;
#else
using esphome::time::DSTRule;
using esphome::time::ParsedTimezone;
inline void set_global(const ParsedTimezone &z) { esphome::time::set_global_tz(z); }
inline const ParsedTimezone &get_global() { return esphome::time::get_global_tz(); }
constexpr esphome::time::DSTRuleType MWD = esphome::time::DSTRuleType::MONTH_WEEK_DAY;
constexpr esphome::time::DSTRuleType NONE = esphome::time::DSTRuleType::NONE;
#endif

// Mm.w.d/time; offsets are POSIX (seconds WEST of UTC)
constexpr DSTRule R(int month, int week, int dow, int hour) {
  return DSTRule{hour * 3600, 0, MWD, (uint8_t) month, (uint8_t) week, (uint8_t) dow};
}
constexpr DSTRule NO = DSTRule{0, 0, NONE, 0, 0, 0};
constexpr int H = 3600;

// The select's options, in this order (index 0 is Auto)
constexpr int N_ZONES = 20;
inline const ParsedTimezone ZONES[N_ZONES] = {
    {0, 0, NO, NO},                                                       // Auto (unused)
    {10 * H, 10 * H, NO, NO},                                             // Hawaii      HST10
    {9 * H, 8 * H, R(3, 2, 0, 2), R(11, 1, 0, 2)},                        // Alaska      AKST9AKDT
    {8 * H, 7 * H, R(3, 2, 0, 2), R(11, 1, 0, 2)},                        // Pacific     PST8PDT
    {7 * H, 6 * H, R(3, 2, 0, 2), R(11, 1, 0, 2)},                        // Mountain    MST7MDT
    {7 * H, 7 * H, NO, NO},                                               // Arizona     MST7
    {6 * H, 5 * H, R(3, 2, 0, 2), R(11, 1, 0, 2)},                        // Central     CST6CDT
    {5 * H, 4 * H, R(3, 2, 0, 2), R(11, 1, 0, 2)},                        // Eastern     EST5EDT
    {4 * H, 3 * H, R(3, 2, 0, 2), R(11, 1, 0, 2)},                        // Atlantic    AST4ADT
    {3 * H + 1800, 2 * H + 1800, R(3, 2, 0, 2), R(11, 1, 0, 2)},          // Newfoundland NST3:30NDT
    {0, 0, NO, NO},                                                       // UTC
    {0, -1 * H, R(3, 5, 0, 1), R(10, 5, 0, 2)},                           // London      GMT0BST
    {-1 * H, -2 * H, R(3, 5, 0, 2), R(10, 5, 0, 3)},                      // Central Europe CET-1CEST
    {-2 * H, -3 * H, R(3, 5, 0, 3), R(10, 5, 0, 4)},                      // Eastern Europe EET-2EEST
    {-3 * H, -3 * H, NO, NO},                                             // Moscow      MSK-3
    {-5 * H - 1800, -5 * H - 1800, NO, NO},                               // India       IST-5:30
    {-8 * H, -8 * H, NO, NO},                                             // China       CST-8
    {-9 * H, -9 * H, NO, NO},                                             // Japan       JST-9
    {-10 * H, -11 * H, R(10, 1, 0, 2), R(4, 1, 0, 3)},                    // Sydney      AEST-10AEDT
    {-12 * H, -13 * H, R(9, 5, 0, 2), R(4, 1, 0, 3)},                     // Auckland    NZST-12NZDT
};

inline bool same(const ParsedTimezone &a, const ParsedTimezone &b) { return memcmp(&a, &b, sizeof(a)) == 0; }

struct St {
  int index = 0;
  ParsedTimezone chosen{};
  ParsedTimezone ha{};  // the zone from Home Assistant (or the build) while a choice overrides it
  bool have_ha = false;
};
inline St st;

// every 500 ms: an HA time sync has put its zone back; re-apply the chosen one
inline void tick() {
  if (st.index == 0)
    return;
  const ParsedTimezone &cur = get_global();
  if (!same(cur, st.chosen)) {
    st.ha = cur;
    st.have_ha = true;
    set_global(st.chosen);
  }
}

void set(int index) {
  if (index < 0 || index >= N_ZONES)
    index = 0;
  if (index == 0) {
    if (st.index != 0 && st.have_ha)
      set_global(st.ha);  // back to HA's zone (HA's next sync sends it again anyway)
    st.index = 0;
    return;
  }
  if (st.index == 0) {  // keep the zone in force, for Auto later
    st.ha = get_global();
    st.have_ha = true;
  }
  st.index = index;
  st.chosen = ZONES[index];
  set_global(st.chosen);
}

void init() {
#ifndef SAT_HOST_TEST
  lv_timer_create([](lv_timer_t *) { tick(); }, 500, nullptr);
#endif
}

#endif  // SKY_TZ_OFF
}  // namespace tz
}  // namespace sat
#endif  // SKY_IMPL
