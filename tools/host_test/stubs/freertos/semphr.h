#pragma once
#include "FreeRTOS.h"
struct HostMutex { std::mutex m; };
typedef HostMutex *SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new HostMutex; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t t) { if (t == 0) return h->m.try_lock() ? pdTRUE : pdFALSE; h->m.lock(); return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h) { h->m.unlock(); return pdTRUE; }
