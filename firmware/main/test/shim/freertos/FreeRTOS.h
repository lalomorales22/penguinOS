// Just enough FreeRTOS vocabulary for eos_brain_bridge.c to compile on the
// host. Nothing here schedules anything; the test drives the task by hand.
#ifndef EOS_SHIM_FREERTOS_H
#define EOS_SHIM_FREERTOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;

#define pdPASS          1
#define pdTRUE          1
#define pdFALSE         0
#define portMAX_DELAY   0xFFFFFFFFu
#define pdMS_TO_TICKS(x) ((TickType_t)(x))

#endif
