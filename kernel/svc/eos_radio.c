// eos_radio — implementation. See eos_radio.h for why it is a flag and not a
// mutex, and why it does not live inside either radio stack.

#include "eos_radio.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define RLOCK()   portENTER_CRITICAL(&s_mux)
#define RUNLOCK() portEXIT_CRITICAL(&s_mux)
#else
#define RLOCK()   ((void)0)
#define RUNLOCK() ((void)0)
#endif

static bool        s_held;
static const char *s_by = "";

const char *eos_radio_user_name(eos_radio_user_t u)
{
    switch (u) {
    case EOS_RADIO_WIFI: return "wifi";
    case EOS_RADIO_BLE:  return "ble";
    case EOS_RADIO_USER_COUNT: break;
    }
    return "?";
}

bool eos_radio_lock(const char *owner, uint32_t wait_ms)
{
    uint32_t waited = 0;

    if (!owner) owner = "?";
    for (;;) {
        bool got = false;

        RLOCK();
        if (!s_held) {
            s_held = true;
            s_by   = owner;
            got = true;
        }
        RUNLOCK();
        if (got) return true;
        if (waited >= wait_ms) return false;
#ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
#else
        // Single-threaded on the host: nobody is going to release it while we
        // spin, so waiting would be an infinite loop rather than a delay.
        return false;
#endif
    }
}

void eos_radio_unlock(const char *owner)
{
    RLOCK();
    if (s_held && (!owner || strcmp(owner, s_by) == 0)) {
        s_held = false;
        s_by   = "";
    }
    RUNLOCK();
}

bool eos_radio_busy(void)
{
    bool b;

    RLOCK();
    b = s_held;
    RUNLOCK();
    return b;
}

const char *eos_radio_owner(void)
{
    const char *o;

    RLOCK();
    o = s_held ? s_by : "";
    RUNLOCK();
    return o;
}

bool eos_radio_acquire(eos_radio_user_t u, uint32_t wait_ms)
{
    return eos_radio_lock(eos_radio_user_name(u), wait_ms);
}

void eos_radio_release(eos_radio_user_t u)
{
    eos_radio_unlock(eos_radio_user_name(u));
}
