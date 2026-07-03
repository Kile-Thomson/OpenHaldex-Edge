#pragma once
// Off-target FreeRTOS task shim. TaskHandle_t and a no-op xTaskCreate so the
// core's startHaldexLearn() compiles and links on the host.
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

// No-op task creation: returns pdPASS without spawning anything.
static inline BaseType_t xTaskCreate(TaskFunction_t /*fn*/,
                                     const char * /*name*/,
                                     uint32_t /*stackDepth*/,
                                     void * /*params*/,
                                     UBaseType_t /*priority*/,
                                     TaskHandle_t * /*handle*/) {
  return pdPASS;
}

static inline void vTaskDelay(TickType_t) {}
static inline void vTaskDelete(TaskHandle_t) {}
