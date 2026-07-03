#pragma once
// Off-target FreeRTOS shim — base types only.
#include <cstdint>

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;

#ifndef pdTRUE
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xFFFFFFFF
#endif

// Queue handle type only — the UDS RX queue is declared in defs.h; the queue
// API itself lives in firmware TUs excluded from the native build.
typedef void *QueueHandle_t;
