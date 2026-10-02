#pragma once
// NET-10: the web page's browser-tab icon.
//
// ESPHome's bundled web UI (web_server version 3, local: true) ships an empty icon
// (<link rel=icon href=data:image />) and has no option to change it; js_include is not
// loaded by the bundled page. So "/" is answered here first: ESPHome's own gzipped page
// (INDEX_GZ, the same bytes web_server would send) is inflated once into PSRAM with the
// ROM's miniz, the icon link is swapped for an inline SVG sun, and the result is served
// (the app's firstUpdated() overwrites the first link[rel~='icon'] with its house
// icon, so that selector is renamed to one that matches nothing, same length)
// uncompressed (~78 KB, LAN only). The page itself is untouched, so an ESPHome update
// brings its new UI along. Anything that goes wrong (no PSRAM, unexpected page) leaves
// ESPHome's handler to serve its page as before. The captive portal keeps "/" while the
// fallback AP is up.

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_WEBSERVER) && defined(USE_WEBSERVER_LOCAL) && \
    defined(USE_WEBSERVER_GZIP) && USE_WEBSERVER_VERSION == 3

#include <cstring>
#include <span>
#include <string>
#include "esp_heap_caps.h"
#include "miniz.h"
#include "esphome/core/log.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/web_server/server_index_v3.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif

namespace skyweb {

static const char *const TAG = "sky_web";

// A sun in the map's Sun colour (C_SUN 0xFFD54A): disc and eight rays.
static const char ICON_LINK[] =
    "<link rel=icon type=\"image/svg+xml\" href=\"data:image/svg+xml,"
    "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E"
    "%3Ccircle cx='16' cy='16' r='7' fill='%23FFD54A'/%3E"
    "%3Cpath d='M16 2.5v4.2M16 25.3v4.2M2.5 16h4.2M25.3 16h4.2M6.5 6.5l3 3M22.5 22.5l3 3M6.5 25.5l3-3M22.5 9.5l3-3' "
    "stroke='%23FFD54A' stroke-width='2.8' stroke-linecap='round'/%3E%3C/svg%3E\">";

// NET-10a: header logo, 40x40 (shown 52x40, centred)
static const char LOGO_SVG[] =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 40 40\"><circle cx=\"20\" cy=\"20\" r=\"18.2\" fill=\"#0B1220\" stroke=\"#3B4C70\" stroke-width=\"1.6\"/><circle cx=\"20\" cy=\"20\" r=\"11\" fill=\"none\" stroke=\"#22324F\" stroke-width=\"1\"/><path d=\"M4.5 27.5C11 16 24 9 35.5 11\" fill=\"none\" stroke=\"#18BCF2\" stroke-width=\"1.6\" stroke-linecap=\"round\" stroke-dasharray=\"0.1 3.2\"/><circle cx=\"11\" cy=\"11.5\" r=\"1\" fill=\"#FFFFFF\"/><circle cx=\"28\" cy=\"28\" r=\"0.9\" fill=\"#FFFFFF\"/><circle cx=\"16\" cy=\"30.5\" r=\"0.7\" fill=\"#C9D3E6\"/><circle cx=\"31.5\" cy=\"20\" r=\"0.7\" fill=\"#C9D3E6\"/><g transform=\"translate(21.5 15.5) rotate(-28)\"><rect x=\"-6.6\" y=\"-1.9\" width=\"4.6\" height=\"3.8\" rx=\"0.5\" fill=\"#18BCF2\"/><rect x=\"2\" y=\"-1.9\" width=\"4.6\" height=\"3.8\" rx=\"0.5\" fill=\"#18BCF2\"/><rect x=\"-2\" y=\"-0.4\" width=\"4\" height=\"0.8\" fill=\"#9FB3D1\"/><rect x=\"-1.7\" y=\"-2.2\" width=\"3.4\" height=\"4.4\" rx=\"0.8\" fill=\"#F2F4F9\"/></g><circle cx=\"29.5\" cy=\"30.5\" r=\"3.2\" fill=\"#FFD54A\"/></svg>";

inline char *page = nullptr;
inline size_t page_len = 0;

// Inflate ESPHome's page and put the icon in. Returns false (and serves nothing) on any doubt.
inline bool build() {
  const uint8_t *gz = esphome::web_server::INDEX_GZ;
  const size_t gz_len = sizeof(esphome::web_server::INDEX_GZ);
  // gzip: 10-byte header with no optional fields (FLG = 0), raw deflate, 8-byte trailer
  if (gz_len < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8 || gz[3] != 0)
    return false;
  const uint32_t raw_len = gz[gz_len - 4] | (gz[gz_len - 3] << 8) | (gz[gz_len - 2] << 16) | ((uint32_t) gz[gz_len - 1] << 24);
  if (raw_len == 0 || raw_len > 512 * 1024)
    return false;
  auto *raw = (uint8_t *) heap_caps_malloc(raw_len, MALLOC_CAP_SPIRAM);
  auto *inf = (tinfl_decompressor *) heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);  // ~11 KB: not on the stack
  bool ok = raw != nullptr && inf != nullptr;
  if (ok) {
    tinfl_init(inf);
    size_t in_len = gz_len - 18, out_len = raw_len;
    ok = tinfl_decompress(inf, gz + 10, &in_len, raw, raw, &out_len, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) ==
             TINFL_STATUS_DONE &&
         out_len == raw_len;
  }
  heap_caps_free(inf);
  if (!ok) {
    heap_caps_free(raw);
    ESP_LOGW(TAG, "could not unpack the web page; tab icon unchanged");
    return false;
  }
  const std::string_view html((const char *) raw, raw_len);
  static const char OLD[] = "<link rel=icon href=data:image />";
  size_t at = html.find(OLD), cut = sizeof(OLD) - 1;
  if (at == std::string_view::npos) {  // a newer page: add the icon right after <head>
    at = html.find("<head>");
    if (at == std::string_view::npos) {
      heap_caps_free(raw);
      ESP_LOGW(TAG, "web page not recognised; tab icon unchanged");
      return false;
    }
    at += 6;
    cut = 0;
  }
  // NET-10a: the header logo (the esp-logo element renders a fixed SVG string) becomes
  // the Sky Tracker badge: sky disc, orbit, satellite, Sun
  static const char LOGO_OLD[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?> <svg id=\"Layer_2\"";
  size_t lat = html.find(LOGO_OLD, at + cut), lcut = 0, ladd = 0;
  if (lat != std::string_view::npos) {
    const size_t lend = html.find("</svg>`", lat);
    if (lend != std::string_view::npos) {
      lcut = lend + 6 - lat;
      ladd = sizeof(LOGO_SVG) - 1;
    }
  }
  if (ladd == 0) {
    lat = raw_len;
    ESP_LOGW(TAG, "header logo not found; left as is");
  }
  const size_t add = sizeof(ICON_LINK) - 1, len = raw_len - cut + add - lcut + ladd;
  page = (char *) heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (page != nullptr) {
    char *o = page;
    auto put = [&](const void *src, size_t n) {
      memcpy(o, src, n);
      o += n;
    };
    put(raw, at);
    put(ICON_LINK, add);
    put(raw + at + cut, lat - at - cut);
    if (ladd) {
      put(LOGO_SVG, ladd);
      put(raw + lat + lcut, raw_len - lat - lcut);
    }
    page_len = len;
    // stop the app replacing the icon: link[rel~='icon'] -> link[rel~='none']
    static const char SEL[] = "link[rel~='icon']";
    int n = 0;
    const std::string_view all(page, len);
    for (size_t at2 = all.find(SEL); at2 != std::string_view::npos; at2 = all.find(SEL, at2 + 1), n++)
      memcpy(page + at2 + 11, "none", 4);
    if (n == 0)
      ESP_LOGW(TAG, "app icon selector not found; the app may replace the tab icon");
  }
  heap_caps_free(raw);
  return page != nullptr;
}

class IndexHandler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (page == nullptr || request->method() != HTTP_GET)
      return false;
#ifdef USE_CAPTIVE_PORTAL
    if (esphome::captive_portal::global_captive_portal != nullptr &&
        esphome::captive_portal::global_captive_portal->is_active())
      return false;  // NET-2: the Wi-Fi setup page owns "/" while the AP is up
#endif
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(buf) == "/";
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    request->send(request->beginResponse(200, "text/html", (const uint8_t *) page, page_len));
  }
};

// Call before web_server's setup (priority 249) so this handler is asked first.
inline void install() {
  auto *base = esphome::web_server_base::global_web_server_base;
  if (base == nullptr || page != nullptr || !build())
    return;
  base->add_handler(new IndexHandler());  // NOLINT: lives for the program
  ESP_LOGI(TAG, "web page tab icon: sun (%u bytes)", (unsigned) page_len);
}

}  // namespace skyweb

#else
namespace skyweb {
inline void install() {}
}  // namespace skyweb
#endif
