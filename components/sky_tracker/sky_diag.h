#pragma once
// FAIL-10: crash capture. ESPHome's ESP32 crash handler keeps the last panic (reason,
// PC, backtrace) in no-init RAM, logs it once at boot and to the first API client, then
// clears it, so it is gone before anyone reads the log (the 4.3.9 pool crash). At boot
// (on_boot 600, before the API) this copies those log lines, keeps them in one sector
// of the skydata partition (0x0F1000, after the DATA-12 hold sector) and shows them on
// the debug page and as the "Last Crash" text sensor until the next crash replaces
// them. The reset reason of every boot is reported too.

#include "esphome/core/defines.h"
#include <cstdio>
#include <cstring>
#include <string>

#if defined(USE_ESP32) && !defined(SAT_HOST_TEST)
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esphome/core/log.h"
#include "esphome/components/logger/logger.h"
#ifdef USE_ESP32_CRASH_HANDLER
#include "esphome/components/esp32/crash_handler.h"
#endif

namespace skydiag {

static const char *const TAG = "sky_diag";
constexpr uint32_t DIAG_OFF = 0x0F1000, DIAG_MAGIC = 0x43524153;  // "CRAS"
constexpr size_t TEXT_MAX = 1400;
struct Rec {
  uint32_t magic, len, crc;
  char fw[16];
  char text[TEXT_MAX];
};
inline Rec rec{};
inline bool have = false;       // a crash record exists (this boot or earlier)
inline bool fresh = false;      // captured at this boot
inline char reset_reason[24] = "";
inline std::string capture;
inline bool capturing = false;

inline const char *reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}
inline const esp_partition_t *part() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x40, "skydata");
}
inline void log_cb(void *, uint8_t, const char *tag, const char *msg, size_t len) {
  if (!capturing || tag == nullptr || strcmp(tag, "esp32.crash") != 0)
    return;
  // strip the "[E][esp32.crash:123]: " prefix and colour codes if present
  std::string line(msg, len);
  size_t p = line.find("]: ");
  if (p != std::string::npos)
    line = line.substr(p + 3);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  if (line.size() > 4 && line.compare(line.size() - 4, 4, "\033[0m") == 0)
    line.resize(line.size() - 4);
  if (line.rfind("Use: addr2line", 0) == 0 || line.rfind("Other core: addr2line", 0) == 0)
    return;  // the PCs are already listed
  if (capture.size() + line.size() + 1 < TEXT_MAX - 1)
    capture += line + "\n";
}

// Call once at boot, before the API component starts (on_boot priority 600).
inline void install(const char *fw_version) {
  snprintf(reset_reason, sizeof(reset_reason), "%s", reason_name(esp_reset_reason()));
  const esp_partition_t *pt = part();
  if (pt && esp_partition_read(pt, DIAG_OFF, &rec, sizeof(rec)) == ESP_OK && rec.magic == DIAG_MAGIC &&
      rec.len < TEXT_MAX && rec.crc == esp_rom_crc32_le(0, (const uint8_t *) rec.text, rec.len)) {
    rec.text[rec.len] = 0;
    have = true;
  }
#ifdef USE_ESP32_CRASH_HANDLER
  if (esphome::esp32::crash_handler_has_data() && esphome::logger::global_logger != nullptr) {
    capturing = true;
    esphome::logger::global_logger->add_log_callback(nullptr, log_cb);
    esphome::esp32::crash_handler_log();  // replays the record through our callback
    capturing = false;
    if (!capture.empty()) {
      Rec r{};
      r.magic = DIAG_MAGIC;
      snprintf(r.fw, sizeof(r.fw), "%s", fw_version);
      r.len = (uint32_t) std::min(capture.size(), TEXT_MAX - 1);
      memcpy(r.text, capture.data(), r.len);
      r.crc = esp_rom_crc32_le(0, (const uint8_t *) r.text, r.len);
      if (pt && esp_partition_erase_range(pt, DIAG_OFF, 0x1000) == ESP_OK &&
          esp_partition_write(pt, DIAG_OFF, &r, sizeof(r)) == ESP_OK)
        ESP_LOGW(TAG, "crash record saved (%u bytes)", (unsigned) r.len);
      rec = r;
      have = fresh = true;
    }
  }
#endif
}
// "Last Crash" text sensor: the reason line and the fault PC
inline std::string summary() {
  if (!have)
    return "none recorded";
  std::string s;
  const char *t = rec.text;
  auto grab = [&](const char *key) {
    const char *p = strstr(t, key);
    if (p == nullptr)
      return;
    p += strlen(key);
    const char *e = strchr(p, '\n');
    std::string v(p, e ? (size_t) (e - p) : strlen(p));
    while (!v.empty() && v.front() == ' ')
      v.erase(0, 1);
    if (!s.empty())
      s += " / ";
    s += v;
  };
  grab("Reason:");
  grab("PC:");
  if (s.empty())
    s = "crash (see debug page)";
  if (s.size() > 200)
    s.resize(200);
  return s + (fresh ? " (this boot)" : "");
}
// Debug page lines: reset reason, then the record (first lines)
inline void debug_text(char *buf, size_t n) {
  int k = snprintf(buf, n, "RESET  %s\n", reset_reason);
  if (!have || k < 0 || (size_t) k >= n)
    return;
  k += snprintf(buf + k, n - k, "LAST CRASH%s:\n", fresh ? " (this boot)" : "");
  const char *p = rec.text;
  for (int lines = 0; *p && lines < 8 && (size_t) k < n - 1; lines++) {
    const char *e = strchr(p, '\n');
    const size_t len = e ? (size_t) (e - p) : strlen(p);
    k += snprintf(buf + k, n - k, " %.*s\n", (int) std::min<size_t>(len, 44), p);
    p = e ? e + 1 : p + len;
  }
}

}  // namespace skydiag

#else
namespace skydiag {
inline void install(const char *) {}
inline std::string summary() { return "none recorded"; }
inline void debug_text(char *buf, size_t n) {
  if (n)
    buf[0] = 0;
}
}  // namespace skydiag
#endif
