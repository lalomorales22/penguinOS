// Host stand-in for esp_timer. The test owns the clock: it advances only when
// the code under test blocks or does work, so a "pass" of the bridge task
// costs real simulated milliseconds. That is the whole point - the bug being
// guarded against is an arithmetic one between two readings of this clock.
#ifndef EOS_SHIM_ESP_TIMER_H
#define EOS_SHIM_ESP_TIMER_H

#include <stdint.h>

int64_t esp_timer_get_time(void);

#endif
