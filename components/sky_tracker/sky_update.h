#pragma once
// UI-68 internet updates. The firmware is built by GitHub Actions on every release and
// published with a manifest on GitHub Pages; ESPHome's http_request update entity reads the
// manifest (every hour, and when Settings > Upgrade Check is tapped). This shows the result:
// "Checking for updates", then "Up to date" / "Update available" (Not now, Update), then the
// download's progress; ESPHome's http_request OTA installs it and restarts. An update found by
// the hourly check does not open anything: an update icon appears beside the gear on the map
// page, and tapping it shows the prompt. The icon stays until the update is installed.
// Compiled in sky_extra.cpp (SKY_IMPL), as sky_about.h.
#include <cstdint>
namespace esphome {
namespace update {
class UpdateEntity;
}
}  // namespace esphome

#ifndef SKY_IMPL
namespace sat {
namespace upd {
void init(esphome::update::UpdateEntity *e, const lv_font_t *title, const lv_font_t *body, const lv_font_t *small);
void check_now();  // Settings > Upgrade Check
void check_quiet(const char *why);  // NET-13a: hourly, at boot, the web/HA button (no prompt)
void set_source(const char *url);   // the manifest URL, for the log
void set_button(lv_obj_t *b);  // the map page's update icon, beside the gear
void offer_now();              // that icon tapped
}  // namespace upd
}  // namespace sat
#else
#include <cstdio>
#include <cstdlib>
#include <string>
#ifndef SAT_HOST_TEST
#include "esphome/components/update/update_entity.h"
#endif

namespace sat {
namespace upd {

inline const char *source = "";  // the manifest URL (NET-13a)

enum Mode : uint8_t { M_NONE = 0, M_CHECKING, M_LATEST, M_AVAILABLE, M_INSTALLING, M_FAILED };

#ifdef SAT_HOST_TEST
// host stand-in for ESPHome's entity: the tests set state/info and count calls
struct HostUpdate {
  int state = 0;  // 0 unknown, 1 no update, 2 available, 3 installing
  std::string latest, current;
  bool has_progress = false;
  float progress = 0;
  int checks = 0, performs = 0;
};
inline HostUpdate host;
inline int st() { return host.state; }
inline std::string latest() { return host.latest; }
inline std::string current() { return host.current; }
inline bool has_prog() { return host.has_progress; }
inline float prog() { return host.progress; }
inline void do_check() { host.checks++; }
inline void do_perform() { host.performs++; }
inline uint32_t now_ms() { return (uint32_t) (uint64_t) (sat_host_now * 1000.0); }  // wraps as on the device (a plain cast saturates on ARM64)
#else
inline esphome::update::UpdateEntity *ent = nullptr;
inline int st() { return ent ? (int) ent->state : 0; }
inline std::string latest() { return ent ? ent->update_info.latest_version : std::string(); }
inline std::string current() { return ent ? ent->update_info.current_version : std::string(); }
inline bool has_prog() { return ent && ent->update_info.has_progress; }
inline float prog() { return ent ? ent->update_info.progress : 0.0f; }
inline void do_check() {
  ESP_LOGD("upd", "firmware: GET %s", source);  // NET-13a: the update check's request
  if (ent)
    ent->check();
}
inline void do_perform() {
  if (ent)
    ent->perform();
}
inline uint32_t now_ms() { return esphome::millis(); }
#endif

inline const lv_font_t *f_title = nullptr, *f_body = nullptr, *f_small = nullptr;
struct Ui {
  lv_obj_t *root = nullptr, *title = nullptr, *l1 = nullptr, *l2 = nullptr, *bar = nullptr;
  lv_obj_t *btn[2] = {nullptr, nullptr}, *btn_lbl[2] = {nullptr, nullptr};
  Mode mode = M_NONE;
  uint32_t since = 0;      // when CHECKING / INSTALLING began
  bool asked = false;      // the check was started from Settings (show "Up to date" too)
};
inline Ui ui_;
inline std::string declined;  // the version Not now was given to (until restart)
inline lv_obj_t *icon = nullptr;  // the map page's update icon (YAML upd_btn)

inline void close() {
  if (ui_.root)
    lv_obj_delete(ui_.root);
  const bool asked = ui_.asked;
  ui_ = Ui();
  (void) asked;
}
inline lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int y) {
  lv_obj_t *l = lv_label_create(p);
  if (f)
    lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(l, 380);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}
inline void btn_cb(lv_event_t *e);
inline void open() {
  if (ui_.root)
    return;
  lv_obj_t *dim = lv_obj_create(lv_layer_top());  // as the microSD prompt (UI-66)
  lv_obj_remove_style_all(dim);
  lv_obj_set_size(dim, 480, 480);
  lv_obj_set_style_bg_color(dim, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
  lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
  ui_.root = dim;
  lv_obj_t *c = lv_obj_create(dim);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, 400, 260);
  lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x0E1836), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_border_color(c, lv_color_hex(0x2A3A66), 0);
  lv_obj_set_style_border_width(c, 2, 0);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  ui_.title = label(c, f_title, 0xFFFFFF, 16);
  ui_.l1 = label(c, f_body, 0xDCE4F8, 76);
  ui_.l2 = label(c, f_body, 0x7E8BB3, 104);
  ui_.bar = lv_bar_create(c);
  lv_obj_set_size(ui_.bar, 340, 16);
  lv_obj_align(ui_.bar, LV_ALIGN_TOP_MID, 0, 180);
  lv_bar_set_range(ui_.bar, 0, 100);
  lv_obj_set_style_bg_color(ui_.bar, lv_color_hex(0x1A2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(ui_.bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(ui_.bar, lv_color_hex(0x2D5BD0), LV_PART_INDICATOR);
  lv_obj_add_flag(ui_.bar, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 2; i++) {
    lv_obj_t *b = lv_button_create(c);
    lv_obj_set_size(b, 170, 44);
    lv_obj_set_style_bg_color(b, lv_color_hex(i == 0 ? 0x1A2547 : 0x2D5BD0), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *l = lv_label_create(b);
    if (f_body)
      lv_obj_set_style_text_font(l, f_body, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    ui_.btn[i] = b;
    ui_.btn_lbl[i] = l;
  }
}
inline void buttons(const char *a, const char *b) {  // nullptr: hidden; b nullptr: a alone, centred
  for (int i = 0; i < 2; i++) {
    const char *t = i == 0 ? a : b;
    if (t == nullptr) {
      lv_obj_add_flag(ui_.btn[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(ui_.btn[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ui_.btn_lbl[i], t);
    lv_obj_align(ui_.btn[i], LV_ALIGN_TOP_LEFT, i == 0 ? 24 : 206, 196);
  }
  if (a && !b)
    lv_obj_align(ui_.btn[0], LV_ALIGN_TOP_MID, 0, 196);
}
inline void show(Mode m) {
  open();
  ui_.mode = m;
  char b[96];
  lv_obj_add_flag(ui_.bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_text_color(ui_.l2, lv_color_hex(0x7E8BB3), 0);
  const std::string cur = current(), lat = latest();
  switch (m) {
    case M_CHECKING:
      lv_label_set_text(ui_.title, "Checking for updates");
      lv_label_set_text(ui_.l1, "Asking GitHub for the latest release");
      snprintf(b, sizeof(b), "Installed: %s", cur.c_str());
      lv_label_set_text(ui_.l2, b);
      buttons("Close", nullptr);
      break;
    case M_LATEST:
      lv_label_set_text(ui_.title, "Up to date");
      snprintf(b, sizeof(b), "%s is the latest version", cur.c_str());
      lv_label_set_text(ui_.l1, b);
      lv_label_set_text(ui_.l2, "Checks again every hour");
      buttons("Close", nullptr);
      break;
    case M_AVAILABLE:
      lv_label_set_text(ui_.title, "Update available");
      snprintf(b, sizeof(b), "New version: %s", lat.c_str());
      lv_label_set_text(ui_.l1, b);
      snprintf(b, sizeof(b), "Installed: %s", cur.c_str());
      lv_label_set_text(ui_.l2, b);
      buttons("Not now", "Update");
      break;
    case M_INSTALLING:
      lv_label_set_text(ui_.title, "Updating firmware");
      lv_label_set_text(ui_.l1, "Downloading");
      lv_label_set_text(ui_.l2, "Keep the power on");
      lv_obj_set_style_text_color(ui_.l2, lv_color_hex(0xFFB547), 0);
      lv_obj_remove_flag(ui_.bar, LV_OBJ_FLAG_HIDDEN);
      lv_bar_set_value(ui_.bar, 0, LV_ANIM_OFF);
      buttons(nullptr, nullptr);
      break;
    case M_FAILED:
      lv_label_set_text(ui_.title, "Couldn't check");
      lv_label_set_text(ui_.l1, "No answer from the update server");
      lv_label_set_text(ui_.l2, "Check the Wi-Fi and try again later");
      buttons("Close", nullptr);
      break;
    default:
      break;
  }
}
inline void btn_cb(lv_event_t *e) {
  const int i = (int) (intptr_t) lv_event_get_user_data(e);
  if (ui_.mode == M_AVAILABLE && i == 1) {
    ui_.since = now_ms();
    show(M_INSTALLING);
    do_perform();
    return;
  }
  if (ui_.mode == M_AVAILABLE)
    declined = latest();
  close();
}
// "4.6.10" > "4.6.9": numbers compared as numbers
inline int vcmp(const char *a, const char *b) {
  while (*a && *b) {
    if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
      unsigned long x = strtoul(a, (char **) &a, 10), y = strtoul(b, (char **) &b, 10);
      if (x != y)
        return x < y ? -1 : 1;
    } else {
      if (*a != *b)
        return (unsigned char) *a < (unsigned char) *b ? -1 : 1;
      a++, b++;
    }
  }
  return *a ? 1 : *b ? -1 : 0;
}
// ESPHome calls any difference "update available", an older release included (4.6.5 offered
// the GitHub 4.6.3 and installed it). Only a newer version counts here.
inline int state_newer_only() {
  const int s = st();
  if (s == 2 && vcmp(latest().c_str(), current().c_str()) <= 0)
    return 1;
  return s;
}
// NET-13a: log each check's answer once (what the manifest says, against what is installed)
inline void log_result(int s) {
  static int last_s = -1;
  static std::string last_v;
  const std::string v = latest();
  if (s == last_s && v == last_v)
    return;
  last_s = s;
  last_v = v;
  if ((s == 1 || s == 2) && !v.empty())
    ESP_LOGI("upd", "firmware: manifest has %s, installed %s (%s)", v.c_str(), current().c_str(),
             s == 2 ? "newer, offered" : "nothing newer");
}
// every 500 ms (lv_timer): follow the entity
inline void tick() {
  const int s = state_newer_only();
  log_result(s);
  switch (ui_.mode) {
    case M_CHECKING:
      if (s == 2)
        show(M_AVAILABLE);
      else if (s == 1 && now_ms() - ui_.since > 1500)
        show(M_LATEST);
      else if (now_ms() - ui_.since > 25000)
        show(s == 1 ? M_LATEST : M_FAILED);
      break;
    case M_INSTALLING:
      if (has_prog()) {
        const int pc = (int) (prog() + 0.5f);
        char b[32];
        snprintf(b, sizeof(b), "Downloading  %d%%", pc);
        lv_label_set_text(ui_.l1, b);
        lv_bar_set_value(ui_.bar, pc, LV_ANIM_OFF);
      }
      if (s == 2 && now_ms() - ui_.since > 120000)  // back to "available": the install failed
        show(M_AVAILABLE);
      break;
    default:
      break;
  }
  // the icon: a newer version is waiting and nothing is installing
  if (icon) {
    const bool want = s == 2 && ui_.mode != M_INSTALLING;
    if (want == lv_obj_has_flag(icon, LV_OBJ_FLAG_HIDDEN)) {
      if (want)
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }
  }
}
void set_button(lv_obj_t *b) {
  icon = b;
  if (icon)
    lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
}
void offer_now() {
  if (state_newer_only() != 2)
    return;
  close();
  show(M_AVAILABLE);
}
void check_quiet(const char *why) {
  ESP_LOGD("upd", "firmware check (%s)", why);
  do_check();
}
void set_source(const char *url) { source = url; }
void check_now() {
  close();
  ui_.asked = true;
  ui_.since = now_ms();
  show(M_CHECKING);
  do_check();
}
void init(esphome::update::UpdateEntity *e, const lv_font_t *title, const lv_font_t *body, const lv_font_t *small) {
#ifndef SAT_HOST_TEST
  ent = e;
#else
  (void) e;
#endif
  f_title = title;
  f_body = body;
  f_small = small;
  lv_timer_create([](lv_timer_t *) { tick(); }, 500, nullptr);
}

}  // namespace upd
}  // namespace sat
#endif  // SKY_IMPL
