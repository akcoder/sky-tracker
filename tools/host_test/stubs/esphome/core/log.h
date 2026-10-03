#pragma once
#include <cstdio>
extern int host_warnings;
#define ESP_LOGE(tag, fmt, ...) (host_warnings++, printf("E [%s] " fmt "\n", tag, ##__VA_ARGS__))
#define ESP_LOGW(tag, fmt, ...) (host_warnings++, printf("W [%s] " fmt "\n", tag, ##__VA_ARGS__))
#define ESP_LOGI(tag, fmt, ...) printf("I [%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("D [%s] " fmt "\n", tag, ##__VA_ARGS__)
