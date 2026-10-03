#pragma once
// host stub: a RAM-backed "skydata" partition for the cache test
#include <cstdint>
#include <cstring>
#include <vector>
typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#define ESP_FAIL -1
typedef enum { ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef int esp_partition_subtype_t;
typedef struct { uint32_t size; } esp_partition_t;
inline std::vector<uint8_t> host_flash;
inline esp_partition_t host_part{0x400000};
inline bool host_part_enabled = false;
inline const esp_partition_t *esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char *) {
  if (!host_part_enabled) return nullptr;
  if (host_flash.size() != host_part.size) host_flash.assign(host_part.size, 0xFF);
  return &host_part;
}
inline esp_err_t esp_partition_erase_range(const esp_partition_t *, size_t off, size_t n) {
  if (off % 4096 || n % 4096 || off + n > host_flash.size()) return ESP_FAIL;
  memset(host_flash.data() + off, 0xFF, n); return ESP_OK;
}
inline esp_err_t esp_partition_write(const esp_partition_t *, size_t off, const void *d, size_t n) {
  if (off + n > host_flash.size()) return ESP_FAIL;
  for (size_t i = 0; i < n; i++) host_flash[off + i] &= ((const uint8_t *) d)[i]; return ESP_OK;
}
inline esp_err_t esp_partition_read(const esp_partition_t *, size_t off, void *d, size_t n) {
  if (off + n > host_flash.size()) return ESP_FAIL;
  memcpy(d, host_flash.data() + off, n); return ESP_OK;
}
