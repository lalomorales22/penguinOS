// eos_ble_absent — the Bluetooth surface of a node that has no Bluetooth.
//
// eos_httpd.c serves /api/ble/scan, /pair, /status and /forget, so it
// references the BLE service whether or not an image has one. The screen boards
// link kernel/svc/eos_ble.c and get NimBLE. This node does not, deliberately:
// it has no keyboard and no mouse, and the NimBLE stack costs about 37 KB that
// its camera would rather spend on frame buffers.
//
// So the symbols exist and answer honestly. The endpoints report "no adapter"
// rather than 404ing, because the route DOES exist on this image - it is the
// hardware that is absent, and those are different answers to give someone
// looking at a scan that returns nothing.
//
// If a camera node ever needs a keyboard, delete this file and add
// kernel/svc/eos_ble.c to SRCS. Nothing else changes.

#include <stdio.h>
#include <string.h>

#include "eos_ble.h"

const char *eos_ble_pair_warning(void)
{
    // The screen boards return a warning about pairing while a scan is live.
    // NULL means "nothing to warn about", which is true when there is no radio.
    return NULL;
}

void eos_ble_status(eos_ble_status_t *out)
{
    if (out) memset(out, 0, sizeof *out);   // not present, not paired, nothing
}

eos_err_t eos_ble_scan_start(uint16_t ms)
{
    (void)ms;
    return EOS_ERR_NODEV;                   // there is no adapter to scan with
}

bool eos_ble_scanning(void) { return false; }

int eos_ble_scan_results(eos_ble_dev_t *out, int max)
{
    (void)out; (void)max;
    return 0;                               // nothing was ever scanned
}

uint32_t eos_ble_scan_age_ms(void) { return 0; }

eos_err_t eos_ble_pair_addr(const char *addr)
{
    (void)addr;
    return EOS_ERR_NODEV;
}

eos_err_t eos_ble_forget(void) { return EOS_ERR_NODEV; }

int eos_ble_addr_str(char *out, int max, const uint8_t addr[6])
{
    // Still useful and entirely local: it formats bytes, it does not touch a
    // radio. Kept faithful to the real one so a status page renders the same.
    if (!out || max <= 0) return 0;
    if (!addr) { out[0] = 0; return 0; }
    int n = snprintf(out, (size_t)max, "%02x:%02x:%02x:%02x:%02x:%02x",
                     addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return (n < 0) ? 0 : (n >= max ? max - 1 : n);
}
