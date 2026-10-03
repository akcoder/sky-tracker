#pragma once
// Host stub: serves canned bodies registered by the test, keyed by URL substring.
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <map>
#include "esp_crt_bundle.h"
#define ESP_OK 0
#define ESP_FAIL -1
inline const char *esp_err_to_name(esp_err_t) { return "ERR"; }
struct HostHttp;
typedef HostHttp *esp_http_client_handle_t;
enum esp_http_client_event_id_t { HTTP_EVENT_ERROR = 0, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT, HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED };
struct esp_http_client_event_t { esp_http_client_event_id_t event_id; esp_http_client_handle_t client; void *data; int data_len; void *user_data; char *header_key; char *header_value; };
struct esp_http_client_config_t { const char *url; esp_err_t (*crt_bundle_attach)(void *); int timeout_ms; int buffer_size; int buffer_size_tx; bool keep_alive_enable; esp_err_t (*event_handler)(esp_http_client_event_t *); const char *user_agent; };
struct HostHttp { bool reused = false; std::string body; size_t pos = 0; int status = 200; bool chunked = false; std::string lm; esp_err_t (*ev)(esp_http_client_event_t *) = nullptr; };
extern std::map<std::string, std::string> host_http_bodies;
inline std::map<std::string, std::string> host_http_last_modified;  // URL substring -> Last-Modified
extern std::string host_last_url;
inline int host_http_inits = 0, host_http_opens = 0;
inline bool host_http_close_kept = false;  // the server closed the kept connection: the next reused request reads status -1  // connection reuse: opens without a new client
inline void host_http_resolve(HostHttp *h, const char *url) {
  host_last_url = url; h->body.clear(); h->pos = 0; h->status = 200; h->lm.clear();
  for (auto &kv : host_http_last_modified) if (host_last_url.find(kv.first) != std::string::npos) h->lm = kv.second;
  for (auto &kv : host_http_bodies) if (host_last_url.find(kv.first) != std::string::npos) {
    if (kv.second.rfind("HTTP", 0) == 0 && kv.second.size() == 7) { h->status = atoi(kv.second.c_str() + 4); return; }
    h->body = kv.second; return; }
  h->status = 404; }
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
  auto *h = new HostHttp; h->ev = c->event_handler; host_http_inits++; host_http_resolve(h, c->url); return h; }
inline esp_err_t esp_http_client_set_url(esp_http_client_handle_t h, const char *url) { host_http_resolve(h, url); h->reused = true; return ESP_OK; }
inline esp_err_t esp_http_client_open(esp_http_client_handle_t h, int) { h->pos = 0; host_http_opens++; return ESP_OK; }
inline esp_err_t esp_http_client_flush_response(esp_http_client_handle_t h, int *len) { if (len) *len = (int) (h->body.size() - h->pos); h->pos = h->body.size(); return ESP_OK; }
inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) {
  if (h->reused && host_http_close_kept) { host_http_close_kept = false; h->status = -1; return -1; }
  if (h->ev && !h->lm.empty()) { esp_http_client_event_t e{}; e.event_id = HTTP_EVENT_ON_HEADER; e.client = h;
    char k[] = "Last-Modified"; e.header_key = k; e.header_value = (char *) h->lm.c_str(); h->ev(&e); }
  return h->chunked ? -1 : (int64_t) h->body.size(); }
inline int esp_http_client_get_status_code(esp_http_client_handle_t h) { return h->status; }
inline int esp_http_client_read(esp_http_client_handle_t h, char *buf, int n) {
  int k = (int) std::min<size_t>((size_t) n, h->body.size() - h->pos); k = std::min(k, 1500);  // short reads
  memcpy(buf, h->body.data() + h->pos, k); h->pos += k; return k; }
inline bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { return h->pos == h->body.size(); }
inline esp_err_t esp_http_client_close(esp_http_client_handle_t) { return ESP_OK; }
inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { delete h; return ESP_OK; }
