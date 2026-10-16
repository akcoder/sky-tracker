#pragma once
// UI-68 internet updates. The card itself (checking / up to date / update available with the
// release notes / installing, the icon beside the gear, ESPHome's update entity) is
// tracker-common's tracker::upd, shared with f1-tracker; the manifest is published by GitHub
// Actions on GitHub Pages and read by ESPHome's http_request update entity (every hour, and
// when Settings > Upgrade Check is tapped). An update found by the hourly check does not open
// anything: an update icon appears beside the gear on the map page, and tapping it shows the
// prompt. This file is what is sky-tracker's own: NET-14, the check's and the install's TLS
// connection waiting until the data task has dropped its memory.
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
void set_button(lv_obj_t *b);  // the map page's update icon, beside the gear (before init)
void offer_now();              // that icon tapped
void idle_close();             // UI-72: after a minute untouched, the screen closes (not while installing)
// UI-68b the internet install's own callbacks (begin, progress %, end, error): the download
// blocks the main loop, so the screen is updated and redrawn from here
void ota_begin(const char *what = "Downloading");
void ota_progress(float pc);
void ota_end(bool ok);
}  // namespace upd
}  // namespace sat
#else
#include "esphome/components/tracker_common/tracker_update.h"
namespace sat {
namespace upd {
using namespace tracker::upd;  // (the host tests drive the card through these names)

#ifdef SAT_HOST_TEST
inline HostUpdate host;  // the update entity the tests set and read
#else
// NET-14: the check's and the install's TLS connection needs the memory the data task's kept connection and
// request hold: it is asked to drop them and stay off the network, and the request starts once it has (or
// after 6 s)
struct Deferred {
  void (*fn)();
  uint32_t t0, hold_ms;
};
inline Deferred *deferred = nullptr;  // the one request waiting (a second one would run its own blocking fetch beside it)
inline void when_quiet(void (*fn)(), uint32_t hold_ms) {
  if (deferred) {  // the request with the longer hold (the install's) wins; the other was a check, soon repeated
    if (hold_ms > deferred->hold_ms) {
      deferred->fn = fn;
      deferred->hold_ms = hold_ms;
      net_hold(hold_ms);
    }
    return;
  }
  net_hold(hold_ms);
  deferred = new Deferred{fn, esphome::millis(), hold_ms};
  lv_timer_create(
      [](lv_timer_t *t) {
        Deferred *d = deferred;
        if (net_quiet() || esphome::millis() - d->t0 > 6000) {
          void (*fn)() = d->fn;
          delete d;
          deferred = nullptr;  // (before the call: the fetch blocks the loop, and its own hooks may call back)
          lv_timer_delete(t);
          fn();
        }
      },
      250, nullptr);
}
// a pointer to the entity's protected state_ (named through a derived class, which is allowed)
struct Poke : esphome::update::UpdateEntity {
  static esphome::update::UpdateState esphome::update::UpdateEntity::*state() { return &Poke::state_; }
};
#endif

void init(esphome::update::UpdateEntity *e, const lv_font_t *title, const lv_font_t *body, const lv_font_t *small) {
#ifdef SAT_HOST_TEST
  (void) e;
  tracker::upd::init(&host, title, body, small, nullptr);
#else
  tracker::upd::run_hook = when_quiet;
  tracker::upd::init(e, title, body, small, nullptr);
  // UI-68: ESPHome's http_request entity calls ANY version difference "available", an older release too (a dev build
  // newer than the published one showed UPDATE AVAILABLE in the web page and Home Assistant, with nothing to install).
  // The display already counts only a newer version (newer_only); this puts the entity's own state right, within 5 s
  // of each check.
  lv_timer_create(
      [](lv_timer_t *) {
        auto *u = tracker::upd::ent;
        if (u == nullptr || u->state != esphome::update::UPDATE_STATE_AVAILABLE)
          return;
        if (tracker::upd::newer_only((int) u->state, tracker::upd::latest().c_str(), tracker::upd::current().c_str()) ==
            tracker::upd::E_AVAILABLE)
          return;
        u->*Poke::state() = esphome::update::UPDATE_STATE_NO_UPDATE;  // (state is a read-only view of a protected field)
        u->publish_state();
        ESP_LOGI("upd", "firmware: the manifest's %s is not newer than %s: no update", tracker::upd::latest().c_str(),
                 tracker::upd::current().c_str());
      },
      5000, nullptr);
#endif
}
void set_button(lv_obj_t *b) {
  set_icon(b);
  set_page_ok(true);  // the icon is a widget of the map page itself, shown wherever that is
  if (b)
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
}
void check_now() { tracker::upd::check_now(); }
void check_quiet(const char *why) { tracker::upd::check_quiet(why); }
void set_source(const char *url) { tracker::upd::set_source(url); }
void offer_now() { tracker::upd::offer_now(); }
void idle_close() { tracker::upd::idle_close(); }
void ota_begin(const char *what) { tracker::upd::ota_begin(what); }
void ota_progress(float pc) { tracker::upd::ota_progress(pc); }
void ota_end(bool ok) { tracker::upd::ota_end(ok); }

}  // namespace upd
}  // namespace sat
#endif  // SKY_IMPL
