#pragma once
// UI-66 firmware update from a microSD card in the TF slot.
//
// The slot is wired for SPI (CS GPIO42, MOSI GPIO47, CLK GPIO48, MISO GPIO41), but GPIO42
// carries the GPS (HW-8). So the card is driven in SD 1-bit mode instead: CLK 48, CMD 47
// (the MOSI line), DAT0 41 (the MISO line). DAT3 (the CS line) is never driven by the
// chip: the GPS keeps it, and a card only looks at DAT3 when told to enter SPI mode (high
// at power-up = SD mode, which the GPS's idle-high TX and the card's own pull-up give).
// 47/48 also reach the panel's 3-wire init interface. Its CS (39) floats once ESPHome has
// run the init, so mount() drives it high before the first probe (see hold_panel_cs).
//
// The net task probes every 5 s (JOB_SD). A card is mounted, its root searched for
// sky_tracker_firmware_<anything>.bin (the newest by version order when there are several),
// the image header checked (ESP32-S3, this project, not the build already running), and the
// card unmounted again. The loop then asks "Firmware update" Update / Not now. Not now (or
// leaving it) is remembered for that file until the card is taken out. Update runs JOB_SDFLASH:
// the file streamed into the other OTA slot (esp_ota_*: the image is checked on the way),
// that slot set to boot, and the device restarted.
#ifndef SKY_IMPL  // compiled in sky_extra.cpp (see sky_about.h)
namespace sat {
namespace sdfw {
void init(const char *version, const lv_font_t *title, const lv_font_t *body, const lv_font_t *small);
void tick(uint32_t uptime_ms);
}  // namespace sdfw
}  // namespace sat
#else
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <strings.h>
#include <cstring>
#include <string>
#include <atomic>
#include <dirent.h>
#include <sys/stat.h>
#ifndef SAT_HOST_TEST
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#endif

namespace sat {
namespace sdfw {

static const char *const TAG = "sdfw";
constexpr const char *PREFIX = "sky_tracker_firmware_";

enum State : uint8_t { S_NONE = 0, S_CARD, S_OFFER, S_FLASHING, S_DONE, S_FAILED };

struct Offer {
  char file[96] = "";     // name on the card
  char ver[40] = "";      // the <anything> part, shown
  char built[24] = "";    // from the image: "Oct  2 2026"
  uint32_t size = 0;
  uint32_t key = 0;       // file name + size: what "Not now" remembers
};

inline std::atomic<uint8_t> state{S_NONE};
inline Offer offer;                       // written by the task before state = S_OFFER
inline std::atomic<uint32_t> done_bytes{0};
inline char err[80] = "";
inline uint32_t declined = 0;             // key of the file "Not now" was given to (0: none)
inline int misses = 0;                    // probes in a row without a card

inline uint32_t fnv(const char *s, uint32_t h = 2166136261u) {
  while (*s)
    h = (h ^ (uint8_t) *s++) * 16777619u;
  return h;
}

// "4.10.2" > "4.9.7": runs of digits compare as numbers, everything else as characters
inline int vcmp(const char *a, const char *b) {
  while (*a && *b) {
    if (isdigit((unsigned char) *a) && isdigit((unsigned char) *b)) {
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

// sky_tracker_firmware_<ver>.bin (case-insensitive) -> ver
inline bool match_name(const char *name, char *ver, size_t n) {
  const size_t pl = strlen(PREFIX), k = strlen(name);
  if (k <= pl + 4 || strncasecmp(name, PREFIX, pl) != 0 || strcasecmp(name + k - 4, ".bin") != 0)
    return false;
  snprintf(ver, n, "%.*s", (int) (k - pl - 4), name + pl);
  return true;
}

#ifdef SAT_HOST_TEST
inline std::string host_dir;        // the "card" (empty: no card)
inline std::string host_flashed;    // what JOB_SDFLASH wrote
inline bool host_same_build = false;
inline const char *MOUNT = "";
inline bool mount() { return !host_dir.empty(); }
inline void unmount() {}
inline std::string path_of(const char *f) { return host_dir + "/" + f; }
inline const char *mount_dir() { return host_dir.c_str(); }
#else
inline const char *MOUNT = "/sd";
inline sdmmc_card_t *card = nullptr;
// The panel's 3-wire init interface shares CLK 48 / MOSI 47 with the card. ESPHome lets go
// of its CS (39) after the init, so it floats and the panel takes the card's CMD0/ACMD41
// traffic as register writes (seen as the whole screen turning red). Hold CS high first.
inline void hold_panel_cs() {
  static bool done = false;
  if (done)
    return;
  done = true;
  gpio_set_level(GPIO_NUM_39, 1);
  gpio_set_direction(GPIO_NUM_39, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_39, 1);
}
inline bool mount() {
  hold_panel_cs();
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk = GPIO_NUM_48;
  slot.cmd = GPIO_NUM_47;
  slot.d0 = GPIO_NUM_41;
  slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;  // DAT3 is the GPS line: never driven
  slot.cd = slot.wp = GPIO_NUM_NC;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  esp_vfs_fat_sdmmc_mount_config_t mc = {};
  mc.format_if_mount_failed = false;
  mc.max_files = 2;
  mc.allocation_unit_size = 0;
  esp_log_level_set("sdmmc_common", ESP_LOG_NONE);  // "no card" every 5 s is not news
  esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
  return esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &card) == ESP_OK;
}
inline void unmount() {
  if (card)
    esp_vfs_fat_sdcard_unmount(MOUNT, card);
  card = nullptr;
}
inline std::string path_of(const char *f) { return std::string(MOUNT) + "/" + f; }
inline const char *mount_dir() { return MOUNT; }
#endif

// The image header: ESP32-S3, the same project, and not the build already running.
// Returns nullptr when it may be offered, else why not (logged, not shown).
inline const char *check_image(const char *path, char *built, size_t bn) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return "cannot open";
  uint8_t h[32 + 256];
  const size_t n = fread(h, 1, sizeof(h), f);
  fclose(f);
  if (n < sizeof(h) || h[0] != 0xE9)
    return "not a firmware image";
#ifdef SAT_HOST_TEST
  snprintf(built, bn, "%.16s", (const char *) h + 32 + 112);
  return host_same_build ? "already installed" : nullptr;
#else
  const auto *ih = (const esp_image_header_t *) h;
  if (ih->chip_id != ESP_CHIP_ID_ESP32S3)
    return "built for another chip";
  const auto *ad = (const esp_app_desc_t *) (h + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
  if (ad->magic_word != ESP_APP_DESC_MAGIC_WORD)
    return "no app description";
  const esp_app_desc_t *me = esp_app_get_description();
  if (strncmp(ad->project_name, me->project_name, sizeof(ad->project_name)) != 0)
    return "another project";
  if (memcmp(ad->app_elf_sha256, me->app_elf_sha256, sizeof(me->app_elf_sha256)) == 0)
    return "already installed";
  snprintf(built, bn, "%.11s", ad->date);
  return nullptr;
#endif
}

// JOB_SD (net task): is there a card, and on it an image to offer?
inline void probe() {
  const uint8_t st = state.load();
  if (st == S_FLASHING || st == S_DONE)
    return;
  if (!mount()) {
    if (st != S_NONE && ++misses >= 2) {  // two misses in a row: the card is out
      ESP_LOGI(TAG, "card removed");
      declined = 0;
      state = S_NONE;
    }
    return;
  }
  misses = 0;
  if (st == S_OFFER || st == S_FAILED) {  // still in: nothing new to look at
    unmount();
    return;
  }
  Offer best;
  if (DIR *d = opendir(mount_dir())) {
    while (dirent *e = readdir(d)) {
      char ver[40];
      if (!match_name(e->d_name, ver, sizeof(ver)))
        continue;
      if (best.file[0] && vcmp(ver, best.ver) <= 0)
        continue;
      struct stat sb;
      const std::string p = path_of(e->d_name);
      if (stat(p.c_str(), &sb) != 0)
        continue;
      Offer o;
      snprintf(o.file, sizeof(o.file), "%.95s", e->d_name);
      snprintf(o.ver, sizeof(o.ver), "%s", ver);
      o.size = (uint32_t) sb.st_size;
      if (const char *why = check_image(p.c_str(), o.built, sizeof(o.built))) {
        ESP_LOGI(TAG, "%s: %s", e->d_name, why);
        continue;
      }
      char k[16];
      snprintf(k, sizeof(k), "/%u", (unsigned) o.size);
      o.key = fnv(k, fnv(o.file));
      best = o;
    }
    closedir(d);
  }
  unmount();
  if (!best.file[0]) {
    if (st == S_NONE)
      ESP_LOGI(TAG, "card in; no firmware to offer");
    state = S_CARD;
    return;
  }
  if (best.key == declined) {
    state = S_CARD;
    return;
  }
  ESP_LOGI(TAG, "card offers %s (%u bytes, built %s)", best.file, (unsigned) best.size, best.built);
  offer = best;
  state = S_OFFER;
}

// JOB_SDFLASH (net task): the offered file into the other OTA slot, then boot from it
inline void flash() {
  done_bytes = 0;
  err[0] = 0;
  state = S_FLASHING;
  auto fail = [](const char *m) {
    snprintf(err, sizeof(err), "%s", m);
    ESP_LOGW(TAG, "update failed: %s", m);
    state = S_FAILED;
  };
  if (!mount())
    return fail("The card could not be read");
  const std::string p = path_of(offer.file);
  FILE *f = fopen(p.c_str(), "rb");
  if (!f) {
    unmount();
    return fail("The file is gone from the card");
  }
  constexpr size_t CH = 4096;
  static uint8_t *buf = nullptr;
  if (buf == nullptr)
    buf = (uint8_t *) malloc(CH);
  if (buf == nullptr) {
    fclose(f);
    unmount();
    return fail("Out of memory");
  }
#ifdef SAT_HOST_TEST
  host_flashed.clear();
  size_t n;
  while ((n = fread(buf, 1, CH, f)) > 0) {
    host_flashed.append((const char *) buf, n);
    done_bytes += n;
  }
  fclose(f);
  unmount();
  if (done_bytes != offer.size)
    return fail("The card stopped part way");
#else
  const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
  if (part == nullptr || offer.size > part->size) {
    fclose(f);
    unmount();
    return fail(part ? "The file is too big for this device" : "No update slot");
  }
  esp_ota_handle_t h = 0;
  if (esp_ota_begin(part, offer.size, &h) != ESP_OK) {  // erases only what the image needs
    fclose(f);
    unmount();
    return fail("Could not prepare the update slot");
  }
  size_t n;
  bool ok = true;
  while ((n = fread(buf, 1, CH, f)) > 0) {
    if (esp_ota_write(h, buf, n) != ESP_OK) {
      ok = false;
      break;
    }
    done_bytes += n;
    vTaskDelay(1);  // let the loop draw the progress
  }
  fclose(f);
  unmount();
  if (!ok || done_bytes != offer.size) {
    esp_ota_abort(h);
    return fail(ok ? "The card stopped part way" : "Writing the update failed");
  }
  if (esp_ota_end(h) != ESP_OK)  // checks the image (format, SHA-256)
    return fail("The file is damaged");
  if (esp_ota_set_boot_partition(part) != ESP_OK)
    return fail("Could not switch to the update");
#endif
  ESP_LOGI(TAG, "update %s written (%u bytes); restarting", offer.ver, (unsigned) done_bytes.load());
  state = S_DONE;
}


// ------------------------------------------------------------------ the prompt (loop side)
inline const char *fw_ver = "";  // the running version (${fw_version})
inline const lv_font_t *f_title = nullptr, *f_body = nullptr, *f_small = nullptr;
struct Ui {
  lv_obj_t *root = nullptr, *title = nullptr, *l1 = nullptr, *l2 = nullptr, *l3 = nullptr, *bar = nullptr;
  lv_obj_t *btn[2] = {nullptr, nullptr}, *btn_lbl[2] = {nullptr, nullptr};
  uint8_t mode = S_NONE;  // what it shows: S_OFFER, S_FLASHING, S_DONE, S_FAILED
  uint32_t done_at = 0;
};
inline Ui sui;
#ifdef SAT_HOST_TEST
inline int host_restarts = 0;
inline void restart() { host_restarts++; }
inline uint32_t ms() { return (uint32_t) (sat_host_now * 1000.0); }
#else
inline void restart() { esphome::App.safe_reboot(); }
inline uint32_t ms() { return esphome::millis(); }
#endif

inline void close_ui() {
  if (sui.root)
    lv_obj_delete(sui.root);
  sui = Ui();
}
inline lv_obj_t *ui_label(lv_obj_t *p, const lv_font_t *f, uint32_t col, int y) {
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
inline void open_ui() {
  close_ui();
  lv_obj_t *dim = lv_obj_create(lv_layer_top());  // the screen behind dimmed; taps go nowhere
  lv_obj_remove_style_all(dim);
  lv_obj_set_size(dim, 480, 480);
  lv_obj_set_style_bg_color(dim, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(dim, LV_OPA_70, 0);
  lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
  sui.root = dim;
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
  sui.title = ui_label(c, f_title, 0xFFFFFF, 16);
  sui.l1 = ui_label(c, f_body, 0xDCE4F8, 72);
  sui.l2 = ui_label(c, f_body, 0x7E8BB3, 98);
  sui.l3 = ui_label(c, f_small, 0x5A6687, 126);
  sui.bar = lv_bar_create(c);
  lv_obj_set_size(sui.bar, 340, 16);
  lv_obj_align(sui.bar, LV_ALIGN_TOP_MID, 0, 180);
  lv_bar_set_range(sui.bar, 0, 100);
  lv_obj_set_style_bg_color(sui.bar, lv_color_hex(0x1A2547), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sui.bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sui.bar, lv_color_hex(0x2D5BD0), LV_PART_INDICATOR);
  lv_obj_add_flag(sui.bar, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 2; i++) {
    lv_obj_t *b = lv_button_create(c);
    lv_obj_set_size(b, 170, 44);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, i == 0 ? 24 : 206, 196);
    lv_obj_set_style_bg_color(b, lv_color_hex(i == 0 ? 0x1A2547 : 0x2D5BD0), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_t *l = lv_label_create(b);
    if (f_body)
      lv_obj_set_style_text_font(l, f_body, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
    sui.btn[i] = b;
    sui.btn_lbl[i] = l;
  }
}
inline void show_buttons(const char *a, const char *b) {  // b == nullptr: one button, centred
  for (int i = 0; i < 2; i++) {
    const char *t = i == 0 ? a : b;
    if (t == nullptr) {
      lv_obj_add_flag(sui.btn[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(sui.btn[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(sui.btn_lbl[i], t);
  }
  if (b == nullptr)
    lv_obj_align(sui.btn[0], LV_ALIGN_TOP_MID, 0, 196);
}
inline void ui_update();
inline void btn_cb(lv_event_t *e) {
  const int i = (int) (intptr_t) lv_event_get_user_data(e);
  if (sui.mode == S_OFFER) {
    if (i == 1) {  // Update
      state = S_FLASHING;
      done_bytes = 0;
      if (!enqueue(JOB_SDFLASH)) {
        state = S_OFFER;
        return;
      }
    } else {  // Not now: not again for this file until the card comes out
      declined = offer.key;
      state = S_CARD;
      close_ui();
      return;
    }
  } else if (sui.mode == S_FAILED) {  // Close
    declined = offer.key;
    state = S_CARD;
    close_ui();
    return;
  }
  ui_update();
}
// every 500 ms (an lv_timer): open, update or close the prompt to match the task's state
inline void ui_update() {
  const uint8_t st = state.load();
  if (st == S_OFFER && offer.key == declined) {
    state = S_CARD;
    return;
  }
  const bool want = st == S_OFFER || st == S_FLASHING || st == S_DONE || st == S_FAILED;
  if (!want) {
    if (sui.root)
      close_ui();
    return;
  }
  if (!sui.root)
    open_ui();
  char b[96];
  if (st != sui.mode) {
    sui.mode = st;
    lv_obj_add_flag(sui.bar, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(sui.l3, "");
    if (st == S_OFFER) {
      lv_label_set_text(sui.title, "Firmware update");
      snprintf(b, sizeof(b), "On the card: %s", offer.ver);
      lv_label_set_text(sui.l1, b);
      snprintf(b, sizeof(b), "Installed: %s", fw_ver);
      lv_label_set_text(sui.l2, b);
      lv_label_set_text(sui.l3, offer.file);
      show_buttons("Not now", "Update");
    } else if (st == S_FLASHING) {
      lv_label_set_text(sui.title, "Updating firmware");
      lv_label_set_text(sui.l2, "Keep the card in and the power on");
      lv_obj_set_style_text_color(sui.l2, lv_color_hex(0xFFB547), 0);
      lv_obj_remove_flag(sui.bar, LV_OBJ_FLAG_HIDDEN);
      show_buttons(nullptr, nullptr);
      lv_obj_add_flag(sui.btn[0], LV_OBJ_FLAG_HIDDEN);
    } else if (st == S_DONE) {
      lv_label_set_text(sui.title, "Update installed");
      snprintf(b, sizeof(b), "Restarting with %s", offer.ver);
      lv_label_set_text(sui.l1, b);
      lv_label_set_text(sui.l2, "");
      lv_obj_add_flag(sui.btn[0], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(sui.btn[1], LV_OBJ_FLAG_HIDDEN);
      sui.done_at = ms();
    } else {  // S_FAILED
      lv_label_set_text(sui.title, "Update failed");
      lv_label_set_text(sui.l1, err);
      lv_obj_set_style_text_color(sui.l2, lv_color_hex(0x7E8BB3), 0);
      snprintf(b, sizeof(b), "Still running %s", fw_ver);
      lv_label_set_text(sui.l2, b);
      show_buttons("Close", nullptr);
    }
  }
  if (st == S_FLASHING) {
    const uint32_t d = done_bytes.load(), t = offer.size ? offer.size : 1;
    const int pc = (int) ((uint64_t) d * 100 / t);
    snprintf(b, sizeof(b), "%d%%  -  %.1f of %.1f MB", pc, d / 1048576.0, t / 1048576.0);
    lv_label_set_text(sui.l1, b);
    lv_bar_set_value(sui.bar, pc, LV_ANIM_OFF);
  } else if (st == S_DONE && ms() - sui.done_at > 1500) {
    restart();
  }
}
inline void job(uint8_t j) {
  if (j == JOB_SD)
    probe();
  else if (j == JOB_SDFLASH)
    flash();
}
// on_boot: fonts, the job hook and the prompt's timer
void init(const char *version, const lv_font_t *title, const lv_font_t *body, const lv_font_t *small) {
  fw_ver = version;
  f_title = title;
  f_body = body;
  f_small = small;
  extra_job = job;
  lv_timer_create([](lv_timer_t *) { ui_update(); }, 500, nullptr);
}
// every 5 s from the YAML, after boot has settled: look for a card
void tick(uint32_t uptime_ms) {
  const uint8_t st = state.load();
  if (uptime_ms > 20000 && st != S_FLASHING && st != S_DONE)
    enqueue(JOB_SD);
}
}  // namespace sdfw
}  // namespace sat
#endif  // SKY_IMPL
