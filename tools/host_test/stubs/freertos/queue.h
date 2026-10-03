#pragma once
#include "FreeRTOS.h"
#include <cstring>
#include <vector>
struct HostQueue { size_t item, cap; std::deque<std::vector<uint8_t>> q; };
typedef HostQueue *QueueHandle_t;
inline QueueHandle_t xQueueCreate(unsigned n, unsigned sz) { return new HostQueue{sz, n, {}}; }
inline BaseType_t xQueueSend(QueueHandle_t h, const void *v, TickType_t) {
  if (h->q.size() >= h->cap) return pdFALSE;
  std::vector<uint8_t> b(h->item); memcpy(b.data(), v, h->item); h->q.push_back(std::move(b)); return pdTRUE; }
inline BaseType_t xQueueReceive(QueueHandle_t h, void *v, TickType_t) {
  if (h->q.empty()) return pdFALSE; memcpy(v, h->q.front().data(), h->item); h->q.pop_front(); return pdTRUE; }
inline BaseType_t xQueueReset(QueueHandle_t h) { h->q.clear(); return pdTRUE; }
inline unsigned uxQueueMessagesWaiting(QueueHandle_t h) { return (unsigned) h->q.size(); }
