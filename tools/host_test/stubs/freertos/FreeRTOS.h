#pragma once
#include <cstdint>
#include <mutex>
#include <deque>
typedef int BaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(x) (x)
#endif
