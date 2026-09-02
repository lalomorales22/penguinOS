// Host stand-ins for the IDF logging macros, so eos_brain_bridge.c compiles
// off the board.
//
// The warning macro is not silent: the sweeper announces itself with
// ESP_LOGW, and whether the sweeper ran is exactly what the regression test
// needs to observe. eos_bridge_shim_sweeps() counts it, and the test drives
// the real code rather than inspecting the message text.
#ifndef EOS_SHIM_ESP_LOG_H
#define EOS_SHIM_ESP_LOG_H

#include <stdio.h>

void eos_bridge_shim_note_warning(const char *tag, const char *fmt);
int  eos_bridge_shim_sweeps(void);
int  eos_bridge_shim_verbose(void);

#define ESP_LOGI(tag, fmt, ...) \
    do { if (eos_bridge_shim_verbose()) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, fmt, ...) \
    do { eos_bridge_shim_note_warning(tag, fmt); \
         if (eos_bridge_shim_verbose()) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) \
    do { if (eos_bridge_shim_verbose()) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__); } while (0)

#endif
