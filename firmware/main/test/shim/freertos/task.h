#ifndef EOS_SHIM_FREERTOS_TASK_H
#define EOS_SHIM_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

// xTaskCreate records the entry point without running it: the test calls the
// task function itself, so every pass happens where it can be observed.
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, TaskHandle_t *out);
void       xTaskNotifyGive(TaskHandle_t h);
uint32_t   ulTaskNotifyTake(BaseType_t clear, TickType_t wait);

#endif
