// Host test for the BLE HID host and the input HAL it feeds.
//
// Everything here runs with no radio and no ESP-IDF. Two things are under
// attack, and they are the two places a byte that arrived from somewhere else
// can walk out of bounds:
//
//   1. HID reports. They come over the air from a peripheral nobody audited,
//      at any length, with any byte values. The decoder is fed every length
//      from 0 to 255, every rollover-error shape, duplicate usages, reports
//      that end mid-slot and 64 KB of random noise, all inside canary frames
//      so an overrun fails a check instead of passing quietly.
//
//   2. The bond record. It comes back out of a flash page that may have been
//      written during a brownout. Every single-byte corruption of a valid
//      record must be rejected, and a rejected record must leave the caller's
//      struct untouched rather than half filled.
//
// The third section is the one that matters to the user: a synthesised
// super+return report is pushed through eos_input and eos_keys and must open a
// window, and super+2 must switch workspace. That is the whole chain from the
// wire to the window manager, running on a laptop.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "eos_ble.h"
#include "eos_input.h"
#include "eos_wm.h"
#include "eos_keys.h"

static int checks = 0, fails = 0;

#define CK(cond, msg) do { \
    checks++; \
    if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } \
} while (0)

#define CKI(got, want, msg) do { \
    long g_ = (long)(got), w_ = (long)(want); \
    checks++; \
    if (g_ != w_) { fails++; printf("    FAIL: %s (got %ld, want %ld)\n", msg, g_, w_); } \
} while (0)

#define CKS(got, want, msg) do { \
    checks++; \
    if (strcmp((got), (want)) != 0) { \
        fails++; printf("    FAIL: %s\n      got  [%s]\n      want [%s]\n", msg, got, want); \
    } \
} while (0)

// A deterministic generator, so a failure is reproducible.
static uint32_t rng_s = 0x13572468u;
static uint32_t rnd(void)
{
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}

// ------------------------------------------------------------- event sink

typedef struct {
    eos_event_t ev[256];
    int n;
} sink_t;

static void drain(sink_t *s)
{
    eos_event_t e;
    s->n = 0;
    while (eos_input_poll(&e)) {
        if (s->n < (int)(sizeof s->ev / sizeof s->ev[0])) s->ev[s->n++] = e;
    }
}

static int count_of(const sink_t *s, uint8_t type)
{
    int i, n = 0;
    for (i = 0; i < s->n; i++) if (s->ev[i].type == type) n++;
    return n;
}

static const eos_event_t *find_ev(const sink_t *s, uint8_t type, uint8_t key)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (s->ev[i].type == type && s->ev[i].key == key) return &s->ev[i];
    return NULL;
}

static void fresh(void)
{
    eos_input_cfg_t c = eos_input_defaults();
    eos_input_init(&c);
}

// A boot keyboard report: modifiers, reserved, six usages.
static void report(uint8_t *r, uint8_t mods, int a, int b, int c, int d, int e, int f)
{
    r[0] = mods; r[1] = 0;
    r[2] = (uint8_t)a; r[3] = (uint8_t)b; r[4] = (uint8_t)c;
    r[5] = (uint8_t)d; r[6] = (uint8_t)e; r[7] = (uint8_t)f;
}

// ===================================================== addresses and names

static void test_addr(void)
{
    static const uint8_t a[6] = { 0xAA, 0xBB, 0xCC, 0x0D, 0xEE, 0x0F };
    uint8_t back[6];
    char buf[EOS_BLE_ADDR_STR];
    int n;

    printf("  addresses\n");

    n = eos_ble_addr_str(buf, sizeof buf, a);
    CKI(n, 17, "an address formats to 17 characters");
    CKS(buf, "AA:BB:CC:0D:EE:0F", "leading zeroes are kept, hex is upper case");

    CK(eos_ble_addr_str(buf, 17, a) == (int)EOS_ERR_TOOBIG,
       "a buffer one byte short is refused, not truncated");
    CK(eos_ble_addr_str(NULL, 32, a) == (int)EOS_ERR_ARG, "NULL out is an argument error");

    CK(eos_ble_addr_parse("AA:BB:CC:0D:EE:0F", back), "the canonical form parses");
    CK(memcmp(back, a, 6) == 0, "and round trips byte for byte");
    CK(eos_ble_addr_parse("aa:bb:cc:0d:ee:0f", back), "lower case parses");
    CK(memcmp(back, a, 6) == 0, "lower case round trips too");
    CK(eos_ble_addr_parse("AA-BB-CC-0D-EE-0F", back), "dashes are accepted");

    CK(!eos_ble_addr_parse("", back), "the empty string is not an address");
    CK(!eos_ble_addr_parse("A", back), "one character is not an address");
    CK(!eos_ble_addr_parse("AA:BB:CC:DD:EE", back), "five bytes is not an address");
    CK(!eos_ble_addr_parse("AA:BB:CC:DD:EE:FF:", back), "trailing garbage is rejected");
    CK(!eos_ble_addr_parse("AA:BB:CC:DD:EE:FG", back), "a non-hex digit is rejected");
    CK(!eos_ble_addr_parse("AA;BB:CC:DD:EE:FF", back), "a wrong separator is rejected");
    CK(!eos_ble_addr_parse("AABBCCDDEEFF", back), "no separators is rejected");
    CK(!eos_ble_addr_parse(NULL, back), "NULL is rejected");

    // Every truncation of a valid address must be refused without reading past
    // the terminator it was given.
    {
        const char *full = "AA:BB:CC:0D:EE:0F";
        int i;
        for (i = 0; i < 17; i++) {
            char *p = malloc((size_t)i + 1);
            memcpy(p, full, (size_t)i);
            p[i] = '\0';
            CK(!eos_ble_addr_parse(p, back), "a truncated address is refused");
            free(p);
        }
    }
}

static void test_names(void)
{
    char out[EOS_BLE_NAME_MAX];
    int n;

    printf("  name sanitiser\n");

    n = eos_ble_name_sanitize(out, sizeof out, "K809", 4);
    CKI(n, 4, "a plain name copies whole");
    CKS(out, "K809", "and is unchanged");

    n = eos_ble_name_sanitize(out, sizeof out, "K8\x01\x1f\x7f\xff""9", 7);
    CKI(n, 7, "control and high bytes are replaced, not dropped");
    CKS(out, "K8????9", "each one becomes a question mark");

    n = eos_ble_name_sanitize(out, sizeof out, "K809\0junk", 9);
    CKI(n, 4, "a NUL ends the name; padding is not part of it");
    CKS(out, "K809", "and the padding does not appear");

    {
        char src[80];
        memset(src, 'x', sizeof src);
        n = eos_ble_name_sanitize(out, sizeof out, src, (int)sizeof src);
        CKI(n, EOS_BLE_NAME_MAX - 1, "an over-long name truncates to the field");
        CKI((int)strlen(out), EOS_BLE_NAME_MAX - 1, "and stays NUL terminated");
    }

    out[0] = 'z';
    CKI(eos_ble_name_sanitize(out, sizeof out, NULL, 5), 0, "NULL source gives an empty name");
    CKS(out, "", "and terminates the buffer anyway");
    CKI(eos_ble_name_sanitize(out, sizeof out, "abc", 0), 0, "zero length gives an empty name");
    CKI(eos_ble_name_sanitize(out, sizeof out, "abc", -3), 0, "a negative length is not a length");
    CKI(eos_ble_name_sanitize(NULL, 8, "abc", 3), 0, "NULL destination writes nothing");

    {
        char one[1];
        one[0] = 'q';
        CKI(eos_ble_name_sanitize(one, 1, "abc", 3), 0, "a one-byte buffer holds only the terminator");
        CKI(one[0], 0, "and gets it");
    }

    // Exercised at every source length against a canary frame.
    {
        struct { char guard0[8]; char buf[EOS_BLE_NAME_MAX]; char guard1[8]; } f;
        int len;
        for (len = 0; len <= 64; len++) {
            char src[64];
            int i;
            for (i = 0; i < 64; i++) src[i] = (char)(rnd() & 0xFF);
            memset(f.guard0, 0x5A, sizeof f.guard0);
            memset(f.guard1, 0xA5, sizeof f.guard1);
            eos_ble_name_sanitize(f.buf, EOS_BLE_NAME_MAX, src, len);
            CK(f.guard0[0] == 0x5A && f.guard0[7] == 0x5A &&
               (unsigned char)f.guard1[0] == 0xA5 && (unsigned char)f.guard1[7] == 0xA5,
               "the sanitiser stays inside its buffer");
            CK((int)strlen(f.buf) < EOS_BLE_NAME_MAX, "and always terminates");
        }
    }
}

// ================================================================ scan list

static eos_ble_dev_t mkdev(uint8_t last, int8_t rssi, uint8_t flags, const char *name)
{
    eos_ble_dev_t d;
    memset(&d, 0, sizeof d);
    d.addr[0] = 0xC0; d.addr[1] = 0xFF; d.addr[2] = 0xEE;
    d.addr[3] = 0x00; d.addr[4] = 0x00; d.addr[5] = last;
    d.addr_type = EOS_BLE_ADDR_RANDOM;
    d.rssi  = rssi;
    d.flags = flags;
    if (name) eos_ble_name_sanitize(d.name, EOS_BLE_NAME_MAX, name, (int)strlen(name));
    return d;
}

static void test_devlist(void)
{
    eos_ble_dev_t tbl[EOS_BLE_SCAN_MAX];
    eos_ble_dev_t d;
    int n = 0, i, idx;

    printf("  scan table\n");
    memset(tbl, 0, sizeof tbl);

    d = mkdev(1, -50, EOS_BLE_F_HID, NULL);
    idx = eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    CKI(idx, 0, "the first advertisement lands at zero");
    CKI(n, 1, "and the table grows");

    // The scan response for the same device: a name, no service list.
    d = mkdev(1, -48, EOS_BLE_F_NAMED, "K809");
    idx = eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    CKI(idx, 0, "a second report from the same address merges");
    CKI(n, 1, "and does not add a row");
    CKS(tbl[0].name, "K809", "the name from the scan response is kept");
    CK((tbl[0].flags & EOS_BLE_F_HID) != 0, "and the HID flag from the first report survives");
    CKI(tbl[0].rssi, -48, "the newer signal wins");

    // A different address type at the same address is a different device.
    d = mkdev(1, -60, EOS_BLE_F_HID, "other");
    d.addr_type = EOS_BLE_ADDR_PUBLIC;
    idx = eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    CKI(idx, 1, "public and random addresses do not merge");

    n = 0;
    memset(tbl, 0, sizeof tbl);
    for (i = 0; i < EOS_BLE_SCAN_MAX; i++) {
        d = mkdev((uint8_t)(0x10 + i), (int8_t)(-40 - i), 0, "noise");
        eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    }
    CKI(n, EOS_BLE_SCAN_MAX, "the table fills");

    d = mkdev(0x99, -90, EOS_BLE_F_HID | EOS_BLE_F_KEYBOARD, "K809");
    idx = eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    CK(idx >= 0, "a weak keyboard still displaces the strongest beacon");
    CK((tbl[idx].flags & EOS_BLE_F_KEYBOARD) != 0, "and it is the keyboard that is kept");

    // Once the table is all HID, a beacon shouting from the next desk cannot
    // push a keyboard out of it however strong its signal is.
    n = 0;
    memset(tbl, 0, sizeof tbl);
    for (i = 0; i < EOS_BLE_SCAN_MAX; i++) {
        d = mkdev((uint8_t)(0x40 + i), (int8_t)(-80 - i), EOS_BLE_F_HID, "kbd");
        eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    }
    d = mkdev(0x9A, -20, 0, "loud noise");
    CKI(eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d), -1,
        "a loud non-HID beacon cannot displace a weak keyboard");
    for (i = 0; i < EOS_BLE_SCAN_MAX; i++)
        CK((tbl[i].flags & EOS_BLE_F_HID) != 0, "every row is still a HID device");

    // A stronger keyboard does get in, and it is the weakest that leaves.
    d = mkdev(0x9B, -30, EOS_BLE_F_HID, "closer kbd");
    idx = eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    CK(idx >= 0, "a stronger HID device does displace a weaker one");
    CKI(tbl[idx].rssi, -30, "and it is the new one that is kept");

    // Lookup by address alone, which is all a web form can send: the address
    // TYPE is not representable as text and has to come back out of the table.
    n = 0;
    memset(tbl, 0, sizeof tbl);
    d = mkdev(0x11, -55, EOS_BLE_F_HID, "K809");
    d.addr_type = EOS_BLE_ADDR_RANDOM;
    eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    d = mkdev(0x22, -65, EOS_BLE_F_HID, "other");
    d.addr_type = EOS_BLE_ADDR_PUBLIC;
    eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, &d);
    {
        eos_ble_dev_t want = mkdev(0x22, 0, 0, NULL);
        eos_ble_dev_t miss = mkdev(0x99, 0, 0, NULL);
        int at = eos_ble_devlist_find(tbl, n, want.addr);
        CKI(at, 1, "an address in the table is found");
        CKI(tbl[at].addr_type, EOS_BLE_ADDR_PUBLIC, "and carries the type back with it");
        CKI(eos_ble_devlist_find(tbl, n, miss.addr), -1, "an address not in it is not");
        CKI(eos_ble_devlist_find(tbl, 0, want.addr), -1, "an empty table finds nothing");
        CKI(eos_ble_devlist_find(NULL, n, want.addr), -1, "NULL table finds nothing");
        CKI(eos_ble_devlist_find(tbl, n, NULL), -1, "NULL address finds nothing");
    }

    CKI(eos_ble_devlist_add(NULL, &n, EOS_BLE_SCAN_MAX, &d), -1, "NULL table is refused");
    CKI(eos_ble_devlist_add(tbl, NULL, EOS_BLE_SCAN_MAX, &d), -1, "NULL count is refused");
    CKI(eos_ble_devlist_add(tbl, &n, 0, &d), -1, "a zero-length table is refused");
    CKI(eos_ble_devlist_add(tbl, &n, EOS_BLE_SCAN_MAX, NULL), -1, "NULL device is refused");
    { int bad = EOS_BLE_SCAN_MAX + 3;
      CKI(eos_ble_devlist_add(tbl, &bad, EOS_BLE_SCAN_MAX, &d), -1,
          "a count past the table is refused rather than trusted"); }
}

static void test_adv_filter(void)
{
    uint16_t hid[2]   = { 0x180F, EOS_BLE_UUID_HID };
    uint16_t plain[2] = { 0x180F, 0x180A };

    printf("  advertisement filter\n");
    CK(eos_ble_adv_is_hid(0, hid, 2), "service 0x1812 anywhere in the list is HID");
    CK(!eos_ble_adv_is_hid(0, plain, 2), "battery and device-info are not");
    CK(eos_ble_adv_is_hid(EOS_BLE_APPEARANCE_KBD, NULL, 0), "appearance 0x03C1 alone is enough");
    CK(eos_ble_adv_is_hid(EOS_BLE_APPEARANCE_HID, plain, 2), "so is the generic HID appearance");
    CK(!eos_ble_adv_is_hid(0, NULL, 0), "nothing at all is not HID");
    CK(!eos_ble_adv_is_hid(0, hid, 0), "a zero-length list is not read");
    CK(!eos_ble_adv_is_hid(0x0341, plain, 2), "an unrelated appearance is not HID");
}

// ================================================================= bond

static void test_bond(void)
{
    eos_ble_bond_t b, out;
    uint8_t rec[EOS_BLE_BOND_BYTES];
    int i, nl;

    printf("  bond record\n");

    memset(&b, 0, sizeof b);
    for (i = 0; i < 6; i++) b.addr[i] = (uint8_t)(0x11 * (i + 1));
    b.addr_type  = EOS_BLE_ADDR_RANDOM;
    b.appearance = EOS_BLE_APPEARANCE_KBD;
    strcpy(b.name, "K809");

    CKI(eos_ble_bond_encode(rec, sizeof rec, &b), EOS_BLE_BOND_BYTES,
        "a record encodes to exactly its fixed size");
    CK(eos_ble_bond_decode(&out, rec, EOS_BLE_BOND_BYTES), "and decodes again");
    CK(memcmp(out.addr, b.addr, 6) == 0, "the address survives");
    CKI(out.addr_type, b.addr_type, "the address type survives");
    CKI(out.appearance, b.appearance, "the appearance survives");
    CKS(out.name, "K809", "the name survives");

    CK(eos_ble_bond_encode(rec, EOS_BLE_BOND_BYTES - 1, &b) == (int)EOS_ERR_TOOBIG,
       "a short buffer gets no partial record");
    CK(eos_ble_bond_encode(NULL, 64, &b) == (int)EOS_ERR_ARG, "NULL out is an argument error");
    CK(eos_ble_bond_encode(rec, 64, NULL) == (int)EOS_ERR_ARG, "NULL bond is an argument error");

    // Every name length round trips, including the empty name and the longest
    // one the field can hold.
    for (nl = 0; nl < EOS_BLE_NAME_MAX; nl++) {
        memset(b.name, 0, sizeof b.name);
        for (i = 0; i < nl; i++) b.name[i] = (char)('a' + (i % 26));
        eos_ble_bond_encode(rec, sizeof rec, &b);
        CK(eos_ble_bond_decode(&out, rec, EOS_BLE_BOND_BYTES), "every name length encodes");
        CKS(out.name, b.name, "and comes back identical");
    }

    strcpy(b.name, "K809");
    eos_ble_bond_encode(rec, sizeof rec, &b);

    // Wrong length, in both directions and at every value nearby.
    for (i = 0; i <= EOS_BLE_BOND_BYTES + 8; i++) {
        if (i == EOS_BLE_BOND_BYTES) continue;
        CK(!eos_ble_bond_decode(&out, rec, i), "only the exact record length is accepted");
    }
    CK(!eos_ble_bond_decode(&out, rec, -1), "a negative length is not a length");
    CK(!eos_ble_bond_decode(&out, NULL, EOS_BLE_BOND_BYTES), "NULL buffer is refused");
    CK(!eos_ble_bond_decode(NULL, rec, EOS_BLE_BOND_BYTES), "NULL output is refused");

    // Every single-bit flip anywhere in the record must be caught. This is the
    // brownout case: a record that reads as valid but is not is a board that
    // chases an address that was never a keyboard.
    {
        int bit, caught = 0, total = 0;
        for (i = 0; i < EOS_BLE_BOND_BYTES; i++) {
            for (bit = 0; bit < 8; bit++) {
                uint8_t bad[EOS_BLE_BOND_BYTES];
                memcpy(bad, rec, sizeof bad);
                bad[i] ^= (uint8_t)(1u << bit);
                total++;
                if (!eos_ble_bond_decode(&out, bad, EOS_BLE_BOND_BYTES)) caught++;
            }
        }
        CKI(caught, total, "every single-bit corruption of a record is rejected");
    }

    // A record whose name length claims more than the field holds.
    {
        uint8_t bad[EOS_BLE_BOND_BYTES];
        uint8_t sum = 0xA5;
        memcpy(bad, rec, sizeof bad);
        bad[10] = EOS_BLE_NAME_MAX;          // one past the largest legal length
        for (i = 0; i < EOS_BLE_BOND_BYTES - 1; i++) sum ^= bad[i];
        bad[EOS_BLE_BOND_BYTES - 1] = sum;   // fix the checksum so only the rule catches it
        CK(!eos_ble_bond_decode(&out, bad, EOS_BLE_BOND_BYTES),
           "an over-long name length is rejected even with a valid checksum");
    }

    // And one with an address type nothing uses.
    {
        uint8_t bad[EOS_BLE_BOND_BYTES];
        uint8_t sum = 0xA5;
        memcpy(bad, rec, sizeof bad);
        bad[3] = 7;
        for (i = 0; i < EOS_BLE_BOND_BYTES - 1; i++) sum ^= bad[i];
        bad[EOS_BLE_BOND_BYTES - 1] = sum;
        CK(!eos_ble_bond_decode(&out, bad, EOS_BLE_BOND_BYTES),
           "an unknown address type is rejected");
    }

    // Pure noise, at the right length, must essentially never decode.
    {
        int accepted = 0;
        for (i = 0; i < 20000; i++) {
            uint8_t junk[EOS_BLE_BOND_BYTES];
            int j;
            for (j = 0; j < EOS_BLE_BOND_BYTES; j++) junk[j] = (uint8_t)(rnd() & 0xFF);
            if (eos_ble_bond_decode(&out, junk, EOS_BLE_BOND_BYTES)) accepted++;
        }
        CKI(accepted, 0, "20000 random 48-byte blobs are all rejected");
    }

    // A record inside a canary frame: decoding must not touch either side.
    {
        struct { uint8_t g0[16]; uint8_t rec[EOS_BLE_BOND_BYTES]; uint8_t g1[16]; } f;
        memset(f.g0, 0x5A, sizeof f.g0);
        memset(f.g1, 0xA5, sizeof f.g1);
        memcpy(f.rec, rec, sizeof f.rec);
        CK(eos_ble_bond_decode(&out, f.rec, EOS_BLE_BOND_BYTES), "a framed record decodes");
        for (i = 0; i < 16; i++) {
            CK(f.g0[i] == 0x5A, "the decoder does not read or write below the record");
            CK(f.g1[i] == 0xA5, "the decoder does not read or write above the record");
        }
    }
}

// ============================================================ radio lock

static void test_radio(void)
{
    printf("  radio lock\n");

    CK(!eos_radio_busy(), "the radio starts free");
    CKS(eos_radio_owner(), "", "and unowned");

    CKI(eos_ble_scan_age_ms(), EOS_BLE_SCAN_NEVER,
        "a board that has never scanned says so rather than reporting an age of zero");
    // The argument check is portable on purpose, so a malformed address is
    // refused identically on the board and here, before any radio work.
    CK(eos_ble_pair_addr("not an address") == EOS_ERR_ARG,
       "a pair request with a malformed address is refused");
    CK(eos_ble_pair_addr("") == EOS_ERR_ARG, "so is an empty one");
    CK(eos_ble_pair_addr(NULL) == EOS_ERR_ARG, "so is a missing one");
    CK(eos_ble_pair_addr("AA:BB:CC:DD:EE:FF") != EOS_ERR_ARG,
       "a well-formed address gets past the check and fails on the absent radio");

    CK(eos_radio_lock("ble-scan", 0), "the first taker gets it");
    CK(eos_radio_busy(), "and it reads busy");
    CKS(eos_radio_owner(), "ble-scan", "and names its owner");
    CK(!eos_radio_lock("wifi-scan", 0), "a WiFi scan cannot start on top of a BLE scan");
    CK(!eos_radio_lock("wifi-scan", 30), "and waiting for it does not help while it is held");

    eos_radio_unlock("wifi-scan");
    CK(eos_radio_busy(), "releasing someone else's lock does nothing");
    CKS(eos_radio_owner(), "ble-scan", "the real owner still holds it");

    eos_radio_unlock("ble-scan");
    CK(!eos_radio_busy(), "the owner can release it");
    CK(eos_radio_lock("wifi-scan", 0), "and the other stack gets it next");
    eos_radio_unlock(NULL);
    CK(!eos_radio_busy(), "a NULL owner is the unconditional release");
}

// ============================================================ HID decode

static void test_hid_basics(void)
{
    uint8_t r[8];
    sink_t s;
    const eos_event_t *e;

    printf("  HID report decoding\n");

    fresh();
    report(r, 0, EOS_KEY_A, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 100);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_DOWN), 1, "one key down for one key");
    e = find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_A);
    CK(e != NULL, "and it is the key that was pressed");
    CKI(e ? e->src : 0, EOS_SRC_KEYBOARD, "tagged as coming from the keyboard");
    CKI(e ? e->ms : 0, 100, "carrying the timestamp it was given");
    CKI(count_of(&s, EOS_EV_TEXT), 1, "a printable key also produces text");
    CKI(s.ev[1].ch, 'a', "which is the unshifted character");
    CK(eos_input_held(EOS_KEY_A), "and the key reads as held");

    // The same report again is not a second press.
    eos_input_hid_report(r, 8, 110);
    drain(&s);
    CKI(s.n, 0, "an unchanged report produces nothing");

    // Release.
    report(r, 0, 0, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 120);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_UP), 1, "dropping the key out of the report is a release");
    CK(find_ev(&s, EOS_EV_KEY_UP, EOS_KEY_A) != NULL, "of the right key");
    CK(!eos_input_held(EOS_KEY_A), "and it stops reading as held");
    CKI(count_of(&s, EOS_EV_TEXT), 0, "a release produces no text");
}

static void test_hid_modifiers(void)
{
    uint8_t r[8];
    sink_t s;
    const eos_event_t *e;

    printf("  modifiers\n");

    fresh();
    // Shift and A in the same report, which is how a keyboard sends a capital.
    report(r, EOS_MOD_LSHIFT, EOS_KEY_A, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    e = find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_A);
    CK(e != NULL, "the letter still arrives");
    CKI(e ? e->mods : 0, EOS_MOD_LSHIFT, "carrying the modifier from the same report");
    CK(find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_LSHIFT) != NULL,
       "and the modifier itself is a key down");
    {
        int i, shift_at = -1, a_at = -1;
        for (i = 0; i < s.n; i++) {
            if (s.ev[i].type == EOS_EV_KEY_DOWN && s.ev[i].key == EOS_KEY_LSHIFT) shift_at = i;
            if (s.ev[i].type == EOS_EV_KEY_DOWN && s.ev[i].key == EOS_KEY_A) a_at = i;
        }
        CK(shift_at >= 0 && a_at > shift_at, "the modifier is queued before the key");
    }
    {
        int i, found = 0;
        for (i = 0; i < s.n; i++) if (s.ev[i].type == EOS_EV_TEXT && s.ev[i].ch == 'A') found = 1;
        CK(found, "and the text is the shifted character");
    }
    CKI(eos_input_mods(), EOS_MOD_LSHIFT, "held modifier state matches the report");
    CK(eos_input_held(EOS_KEY_LSHIFT), "the modifier key reads as held");

    // Super is what every shell bind needs, and it is only ever in byte 0.
    fresh();
    report(r, EOS_MOD_LGUI, EOS_KEY_ENTER, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    e = find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_ENTER);
    CK(e != NULL, "super+return delivers the return");
    CK(e && (e->mods & EOS_MOD_SUPER), "with the super bit set");
    CKI(eos_keys_mods_from_hid(e ? e->mods : 0), EOS_MOD_SUPER,
        "which collapses to the shell's SUPER group");

    // Right-hand modifiers are the same group.
    fresh();
    report(r, EOS_MOD_RGUI | EOS_MOD_RSHIFT, EOS_KEY_2, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    e = find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_2);
    CK(e != NULL, "the right-hand chord arrives");
    CKI(eos_keys_mods_from_hid(e ? e->mods : 0), EOS_MOD_SUPER | EOS_MOD_SHIFT,
        "and collapses to super+shift");

    // Releasing a modifier while the key stays down.
    fresh();
    report(r, EOS_MOD_LCTRL, EOS_KEY_C, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    {
        int i, found = 0;
        for (i = 0; i < s.n; i++) if (s.ev[i].type == EOS_EV_TEXT && s.ev[i].ch == 3) found = 1;
        CK(found, "ctrl-c folds to control code 3");
    }
    report(r, 0, EOS_KEY_C, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 20);
    drain(&s);
    CK(find_ev(&s, EOS_EV_KEY_UP, EOS_KEY_LCTRL) != NULL, "the modifier releases on its own");
    CK(eos_input_held(EOS_KEY_C), "the letter is still held");
    CKI(eos_input_mods(), 0, "and no modifier is left latched");

    // All eight bits at once, then none.
    fresh();
    report(r, 0xFF, 0, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_DOWN), 8, "eight modifier bits are eight key downs");
    CKI(eos_input_mods(), 0xFF, "and all eight read as held");
    report(r, 0x00, 0, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 20);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_UP), 8, "and eight key ups when they let go");
    CK(!eos_input_any_held(), "leaving nothing held");
}

static void test_hid_rollover(void)
{
    uint8_t r[8];
    sink_t s;
    int i;

    printf("  rollover and n-key\n");

    fresh();
    report(r, 0, EOS_KEY_A, EOS_KEY_S, EOS_KEY_D, EOS_KEY_F, EOS_KEY_G, EOS_KEY_H);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_DOWN), 6, "six keys at once is six key downs");
    for (i = 0; i < 6; i++)
        CK(eos_input_held(r[2 + i]), "and every one of them reads as held");

    // Two of the six let go; the other four must not be disturbed.
    report(r, 0, EOS_KEY_A, EOS_KEY_D, EOS_KEY_F, EOS_KEY_H, 0, 0);
    eos_input_hid_report(r, 8, 20);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_UP), 2, "only the two that left are released");
    CKI(count_of(&s, EOS_EV_KEY_DOWN), 0, "and nothing is pressed again");
    CK(eos_input_held(EOS_KEY_A) && eos_input_held(EOS_KEY_H), "the survivors stay held");
    CK(!eos_input_held(EOS_KEY_S) && !eos_input_held(EOS_KEY_G), "the leavers do not");

    // The rollover flood. Every slot 0x01 means the keyboard cannot resolve
    // its matrix; it is not six keys and it is not a release either.
    fresh();
    report(r, 0, EOS_KEY_J, EOS_KEY_K, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_DOWN), 2, "two keys go down normally");

    report(r, 0, 1, 1, 1, 1, 1, 1);
    eos_input_hid_report(r, 8, 20);
    drain(&s);
    CKI(s.n, 0, "a report of six 0x01 rollover errors produces no events at all");
    CK(eos_input_held(EOS_KEY_J) && eos_input_held(EOS_KEY_K),
       "and leaves the keys that really were down alone");

    // 0x02 POSTFail and 0x03 ErrorUndefined are the same rule.
    report(r, 0, 2, 2, 2, 2, 2, 2);
    eos_input_hid_report(r, 8, 30);
    drain(&s);
    CKI(s.n, 0, "a POSTFail flood produces nothing");
    report(r, 0, 3, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 40);
    drain(&s);
    CKI(s.n, 0, "so does a single ErrorUndefined");
    CK(eos_input_held(EOS_KEY_J), "still nothing has been dropped");

    // Coming out of rollover releases what really went away.
    report(r, 0, EOS_KEY_J, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 50);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_UP), 1, "the first good report after rollover catches up");
    CK(find_ev(&s, EOS_EV_KEY_UP, EOS_KEY_K) != NULL, "releasing the key that had gone");

    // A modifier still applies during rollover: shift is byte 0, not a slot.
    report(r, EOS_MOD_LSHIFT, 1, 1, 1, 1, 1, 1);
    eos_input_hid_report(r, 8, 60);
    drain(&s);
    CK(find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_LSHIFT) != NULL,
       "modifiers are still read from a rollover report");

    // The same usage twice in one report is one key.
    fresh();
    report(r, 0, EOS_KEY_Z, EOS_KEY_Z, EOS_KEY_Z, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_DOWN), 1, "a usage repeated in one report is one key down");
}

static void test_hid_malformed(void)
{
    sink_t s;
    int len, iter;

    printf("  malformed and truncated reports\n");

    fresh();
    eos_input_hid_report(NULL, 8, 10);
    drain(&s);
    CKI(s.n, 0, "a NULL report is ignored");

    {
        uint8_t r[8];
        report(r, EOS_MOD_LALT, EOS_KEY_A, 0, 0, 0, 0, 0);
        eos_input_hid_report(r, 0, 10);
        drain(&s);
        CKI(s.n, 0, "a zero-length report is ignored");

        fresh();
        eos_input_hid_report(r, 1, 10);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_DOWN), 1, "a one-byte report is modifiers only");
        CK(find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_LALT) != NULL, "and they are applied");

        fresh();
        eos_input_hid_report(r, 2, 10);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_DOWN), 1, "a two-byte report has no key slots either");

        fresh();
        eos_input_hid_report(r, 3, 10);
        drain(&s);
        CK(find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_A) != NULL,
           "a three-byte report carries exactly one usage");
    }

    // A report cut short at every length, held in an exactly sized allocation
    // so that a read past the end is a real overrun an allocator can see.
    for (len = 0; len <= 32; len++) {
        uint8_t *buf = malloc((size_t)(len ? len : 1));
        int i;
        for (i = 0; i < len; i++) buf[i] = (uint8_t)(0x04 + i);
        fresh();
        eos_input_hid_report(buf, (uint8_t)len, 10);
        drain(&s);
        CK(s.n >= 0, "a report of every length from 0 to 32 is decoded without a crash");
        free(buf);
    }

    // Random noise at every length, inside a canary frame.
    for (iter = 0; iter < 4000; iter++) {
        struct { uint8_t g0[16]; uint8_t r[256]; uint8_t g1[16]; } f;
        int i, n = (int)(rnd() % 257u);
        memset(f.g0, 0x5A, sizeof f.g0);
        memset(f.g1, 0xA5, sizeof f.g1);
        for (i = 0; i < 256; i++) f.r[i] = (uint8_t)(rnd() & 0xFF);
        if (n > 255) n = 255;
        eos_input_hid_report(f.r, (uint8_t)n, (uint32_t)iter);
        eos_input_flush();
        for (i = 0; i < 16; i++) {
            if (f.g0[i] != 0x5A || f.g1[i] != 0xA5) {
                CK(0, "a random report wrote outside its buffer");
                break;
            }
        }
    }
    CK(1, "4000 random reports of random lengths never left their buffer");

    // A keyboard that goes away mid-chord must not leave a modifier latched.
    fresh();
    {
        uint8_t r[8];
        report(r, EOS_MOD_LGUI, EOS_KEY_LEFT, 0, 0, 0, 0, 0);
        eos_input_hid_report(r, 8, 10);
        drain(&s);
        CK(eos_input_held(EOS_KEY_LEFT), "the arrow is held");
        eos_input_inject_conn(EOS_SRC_KEYBOARD, false, 20);
        drain(&s);
        CKI(count_of(&s, EOS_EV_DISCONNECT), 1, "a disconnect is an event");
        CK(!eos_input_any_held(), "and it clears everything the keyboard was holding");
        CKI(eos_input_mods(), 0, "including the modifier");

        // The next keyboard's first report is a fresh diff, not six releases.
        report(r, 0, EOS_KEY_A, 0, 0, 0, 0, 0);
        eos_input_hid_report(r, 8, 30);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_UP), 0, "the next keyboard starts from nothing");
        CKI(count_of(&s, EOS_EV_KEY_DOWN), 1, "and its first key is a press");
    }
}

// ================================================================ queue

static void test_queue(void)
{
    eos_event_t e;
    sink_t s;
    int i;

    printf("  event queue\n");

    fresh();
    CK(!eos_input_poll(&e), "a fresh queue is empty");
    CK(!eos_input_peek(&e), "and peeking finds nothing");
    CKI(eos_input_dropped(), 0, "with nothing dropped");

    // Text events need no held slot, so this is a clean way to overrun the ring.
    for (i = 0; i < EOS_INPUT_QUEUE + 8; i++)
        eos_input_inject_text((uint16_t)('a' + i), EOS_SRC_WEB, (uint32_t)i);
    CK(eos_input_peek(&e), "a filled queue peeks");
    CKI(e.ch, 'a', "at the oldest event, not the newest");
    CKI(eos_input_dropped(), 8, "and every event that would not fit is counted");

    drain(&s);
    CKI(s.n, EOS_INPUT_QUEUE, "the ring holds exactly its stated depth");
    CKI(s.ev[0].ch, 'a', "delivering oldest first");
    CKI(s.ev[EOS_INPUT_QUEUE - 1].ch, 'a' + EOS_INPUT_QUEUE - 1,
        "and dropping the newest, never overwriting unread history");
    CK(!eos_input_poll(&e), "then reads empty again");

    // The held table is finite too, and it refuses rather than evicting: an
    // eviction would lose that key's release and latch it down forever.
    fresh();
    for (i = 0; i < 40; i++)
        eos_input_inject_key((uint8_t)(EOS_KEY_A + i), true, 0, EOS_SRC_TOUCH, 0);
    eos_input_flush();
    CK(eos_input_held(EOS_KEY_A), "the first key of an impossible chord is held");
    CK(!eos_input_held((uint8_t)(EOS_KEY_A + 39)), "the fortieth is refused, not swapped in");

    fresh();
    eos_input_inject_key(EOS_KEY_A, true, 0, EOS_SRC_WEB, 1);
    eos_input_flush();
    CK(!eos_input_poll(&e), "flush empties the queue");
    CK(eos_input_held(EOS_KEY_A), "but leaves held state alone");
    CK(!eos_input_push(NULL), "pushing NULL is refused");
}

// ============================================================= injection

static void test_injection(void)
{
    sink_t s;
    eos_input_cfg_t cfg = eos_input_defaults();

    printf("  injection and hold expiry\n");

    fresh();
    eos_input_inject_key(EOS_KEY_2, true, EOS_MOD_LGUI, EOS_SRC_WEB, 1000);
    drain(&s);
    {
        const eos_event_t *e = find_ev(&s, EOS_EV_KEY_DOWN, EOS_KEY_2);
        CK(e != NULL, "the phone page can inject a key");
        CK(e && (e->mods & EOS_MOD_SUPER), "carrying the modifiers it named");
        CKI(e ? e->src : 0, EOS_SRC_WEB, "tagged as web");
    }

    // The page stops refreshing. The hold has to let go by itself.
    eos_input_tick(1000 + cfg.web_hold_ms - 1);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_UP), 0, "an injected hold survives up to its expiry");
    CK(eos_input_held(EOS_KEY_2), "and still reads as held");
    eos_input_tick(1000 + cfg.web_hold_ms);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_UP), 1, "and releases itself at it");
    CK(!eos_input_held(EOS_KEY_2), "leaving nothing latched");

    // A refresh before the expiry extends it without a second key down.
    fresh();
    eos_input_inject_key(EOS_KEY_LEFT, true, 0, EOS_SRC_WEB, 0);
    drain(&s);
    eos_input_inject_key(EOS_KEY_LEFT, true, 0, EOS_SRC_WEB, 300);
    drain(&s);
    CKI(count_of(&s, EOS_EV_KEY_DOWN), 0, "a refreshed hold is not a second press");
    eos_input_tick(300 + cfg.web_hold_ms - 1);
    CK(eos_input_held(EOS_KEY_LEFT), "and the expiry moved with it");

    // A keyboard hold does NOT expire: BLE delivers releases reliably.
    fresh();
    {
        uint8_t r[8];
        report(r, 0, EOS_KEY_LEFT, 0, 0, 0, 0, 0);
        eos_input_hid_report(r, 8, 0);
        drain(&s);
        eos_input_tick(60000);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_UP), 0, "a keyboard hold never expires on its own");
        CK(eos_input_held(EOS_KEY_LEFT), "it is still down a minute later");
    }

    printf("  auto repeat\n");
    fresh();
    {
        uint8_t r[8];
        report(r, 0, EOS_KEY_X, 0, 0, 0, 0, 0);
        eos_input_hid_report(r, 8, 0);
        drain(&s);
        eos_input_tick(cfg.repeat_delay_ms - 1);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_REPEAT), 0, "nothing repeats before the delay");
        eos_input_tick(cfg.repeat_delay_ms);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_REPEAT), 1, "the first repeat lands on the delay");
        CKI(count_of(&s, EOS_EV_TEXT), 1, "and produces text, so held keys type");
        eos_input_tick(cfg.repeat_delay_ms + cfg.repeat_rate_ms);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_REPEAT), 1, "then one per rate interval");
    }

    // Modifiers do not repeat, which is the whole reason repeat_mods exists.
    fresh();
    {
        uint8_t r[8];
        report(r, EOS_MOD_LSHIFT, 0, 0, 0, 0, 0, 0);
        eos_input_hid_report(r, 8, 0);
        drain(&s);
        eos_input_tick(10000);
        drain(&s);
        CKI(count_of(&s, EOS_EV_KEY_REPEAT), 0, "a held shift never repeats");
    }

    printf("  held timing and touch\n");
    fresh();
    {
        uint8_t r[8];
        report(r, 0, EOS_KEY_ESC, 0, 0, 0, 0, 0);
        eos_input_hid_report(r, 8, 5000);
        eos_input_flush();
        CKI(eos_input_held_ms(EOS_KEY_ESC, 5000), 1, "a key just pressed reads as held, not zero");
        CKI(eos_input_held_ms(EOS_KEY_ESC, 6200), 1200, "and counts up from the press");
        CKI(eos_input_held_ms(EOS_KEY_A, 6200), 0, "a key that is up reads zero");
        eos_input_clear_held();
        CKI(eos_input_held_ms(EOS_KEY_ESC, 6200), 0, "clearing held state empties it");
        drain(&s);
        CKI(s.n, 0, "and does so without inventing key ups");
    }

    fresh();
    eos_input_inject_touch(EOS_EV_TOUCH_DOWN, 120, 64, EOS_SRC_TOUCH, 7);
    eos_input_inject_touch(99, 1, 2, EOS_SRC_TOUCH, 8);
    drain(&s);
    CKI(s.n, 1, "a bogus touch type is refused and a real one is not");
    CKI(s.ev[0].x, 120, "the x coordinate survives");
    CKI(s.ev[0].y, 64, "and the y coordinate");

    fresh();
    eos_input_inject_text(0x00E9, EOS_SRC_WEB, 3);
    eos_input_inject_text(0, EOS_SRC_WEB, 4);
    drain(&s);
    CKI(s.n, 1, "an empty character is not an event");
    CKI(s.ev[0].ch, 0x00E9, "a Latin-1 character passes through unchanged");
}

// ================================================== the wire to the window

// This is the point of the whole component: a report off the air has to move
// the window manager. Nothing is faked between the two.
static void test_wire_to_wm(void)
{
    eos_wm_t wm;
    eos_wm_cfg_t cfg;
    eos_keymap_t km;
    eos_shell_state_t st;
    eos_rect_t screen;
    eos_event_t ev;
    uint8_t r[8];
    int windows_before, windows_after = 0, i;

    printf("  keyboard to window manager\n");

    memset(&cfg, 0, sizeof cfg);
    cfg.min_tile_w = 80;
    cfg.min_tile_h = 40;
    cfg.gap = 2;
    cfg.bar_h = 12;
    cfg.tab_h = 10;
    eos_wm_init(&wm, &cfg);
    eos_keys_defaults(&km);
    eos_shell_state_init(&st, 1);
    screen.x = 0; screen.y = 0; screen.w = 240; screen.h = 240;

    fresh();

    windows_before = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) if (wm.win[i].alive) windows_before++;

    // super+return, exactly as a K809 puts it on the wire.
    report(r, EOS_MOD_LGUI, EOS_KEY_ENTER, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 10);

    while (eos_input_poll(&ev)) {
        eos_key_result_t res;
        if (ev.type != EOS_EV_KEY_DOWN && ev.type != EOS_EV_KEY_REPEAT) continue;
        res = eos_keys_feed(&km, &wm, &st, screen,
                            eos_keys_mods_from_hid(ev.mods), ev.key);
        (void)res;
    }
    for (i = 0; i < EOS_MAX_WINDOWS; i++) if (wm.win[i].alive) windows_after++;
    CKI(windows_after, windows_before + 1, "super+return opens a window");

    // super+2 switches workspace. Release first, the way a keyboard does.
    report(r, EOS_MOD_LGUI, 0, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 20);
    report(r, EOS_MOD_LGUI, EOS_KEY_2, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 30);

    CKI(wm.ws, 0, "the board starts on workspace 1");
    while (eos_input_poll(&ev)) {
        if (ev.type != EOS_EV_KEY_DOWN && ev.type != EOS_EV_KEY_REPEAT) continue;
        eos_keys_feed(&km, &wm, &st, screen,
                      eos_keys_mods_from_hid(ev.mods), ev.key);
    }
    CKI(wm.ws, 1, "super+2 switches to workspace 2");

    // The same chord from the phone page has to do the same thing.
    report(r, 0, 0, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 40);
    eos_input_flush();
    eos_input_inject_key(EOS_KEY_1, true, EOS_MOD_LGUI, EOS_SRC_WEB, 50);
    while (eos_input_poll(&ev)) {
        if (ev.type != EOS_EV_KEY_DOWN && ev.type != EOS_EV_KEY_REPEAT) continue;
        eos_keys_feed(&km, &wm, &st, screen,
                      eos_keys_mods_from_hid(ev.mods), ev.key);
    }
    CKI(wm.ws, 0, "the same bind fires from the phone page");

    // A bare return with no super must not spawn anything.
    eos_input_flush();
    windows_before = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) if (wm.win[i].alive) windows_before++;
    report(r, 0, EOS_KEY_ENTER, 0, 0, 0, 0, 0);
    eos_input_hid_report(r, 8, 60);
    while (eos_input_poll(&ev)) {
        if (ev.type != EOS_EV_KEY_DOWN && ev.type != EOS_EV_KEY_REPEAT) continue;
        eos_keys_feed(&km, &wm, &st, screen,
                      eos_keys_mods_from_hid(ev.mods), ev.key);
    }
    windows_after = 0;
    for (i = 0; i < EOS_MAX_WINDOWS; i++) if (wm.win[i].alive) windows_after++;
    CKI(windows_after, windows_before, "return on its own opens nothing");
}

// ======================================================= usage/keycode map

static void test_keymap(void)
{
    printf("  usage to character map\n");

    // The keycode space IS the HID usage page. Nothing translates on the way
    // in, and the shell's table is written in the same numbers.
    CKI(EOS_KEY_A, 0x04, "usage 0x04 is A on the wire and in the shell");
    CKI(EOS_KEY_ENTER, 0x28, "return is 0x28");
    CKI(EOS_KEY_LGUI, 0xE3, "left GUI is 0xE3");
    CKI(eos_mod_bit(EOS_KEY_LGUI), EOS_MOD_LGUI, "and contributes the super bit");
    CKI(eos_mod_bit(EOS_KEY_RGUI), EOS_MOD_RGUI, "as does the right one");
    CKI(eos_mod_bit(EOS_KEY_A), 0, "a letter contributes no modifier bit");
    CK(eos_key_is_mod(EOS_KEY_RSHIFT), "the modifier range is recognised");
    CK(!eos_key_is_mod(EOS_KEY_Z), "and letters are not in it");

    CKI(eos_input_char(EOS_KEY_A, 0), 'a', "a is a");
    CKI(eos_input_char(EOS_KEY_A, EOS_MOD_RSHIFT), 'A', "right shift capitalises too");
    CKI(eos_input_char(EOS_KEY_1, EOS_MOD_LSHIFT), '!', "shift+1 is a bang");
    CKI(eos_input_char(EOS_KEY_ENTER, 0), '\n', "return is a newline");
    CKI(eos_input_char(EOS_KEY_DELETE, 0), 127, "delete is 127");
    CKI(eos_input_char(EOS_KEY_F5, 0), 0, "a function key has no character");
    CKI(eos_input_char(EOS_KEY_LGUI, 0), 0, "nor does a modifier");
    CKI(eos_input_char(EOS_KEY_A, EOS_MOD_LCTRL), 1, "ctrl-a is 1");
    CKI(eos_input_char(EOS_KEY_Z, EOS_MOD_RCTRL), 26, "ctrl-z is 26");
    CKI(eos_input_char(0xFF, 0), 0, "a usage past the table is not looked up");
    CKI(eos_input_char(EOS_KEY_BOOT, 0), 0, "the board's own button usages have no character");

    // Every usage, every modifier combination, must not read out of the table.
    {
        int k, m, nonzero = 0;
        for (k = 0; k < 256; k++)
            for (m = 0; m < 256; m++)
                if (eos_input_char((uint8_t)k, (uint8_t)m)) nonzero++;
        CK(nonzero > 0, "the whole 256x256 usage/modifier space is safe to look up");
    }
}

int main(void)
{
    printf("\neos_ble host test\n\n");
    printf("  sizeof(eos_ble_dev_t)    = %u bytes\n", (unsigned)sizeof(eos_ble_dev_t));
    printf("  sizeof(eos_ble_bond_t)   = %u bytes\n", (unsigned)sizeof(eos_ble_bond_t));
    printf("  sizeof(eos_ble_status_t) = %u bytes\n", (unsigned)sizeof(eos_ble_status_t));
    printf("  bond record on flash     = %u bytes\n\n", (unsigned)EOS_BLE_BOND_BYTES);

    test_addr();
    test_names();
    test_devlist();
    test_adv_filter();
    test_bond();
    test_radio();
    test_hid_basics();
    test_hid_modifiers();
    test_hid_rollover();
    test_hid_malformed();
    test_queue();
    test_injection();
    test_keymap();
    test_wire_to_wm();

    printf("\n=== %d checks, %d failed ===\n", checks, fails);
    return fails ? 1 : 0;
}
