#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
extern int host_tasks_created;
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t, void *, unsigned, TaskHandle_t *, int) { host_tasks_created++; return pdPASS; }
inline void vTaskDelay(unsigned) {}
