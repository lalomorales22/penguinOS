#ifndef EOS_SHIM_FREERTOS_SEMPHR_H
#define EOS_SHIM_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

// Single-threaded on the host, so the mutex is a token. What it still buys is
// the ORDER of lock/unlock around the code under test, which is where the bug
// this suite guards lives.
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
void              vSemaphoreDelete(SemaphoreHandle_t s);

#endif
