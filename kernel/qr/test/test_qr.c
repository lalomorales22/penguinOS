// Host test for eos_qr. Runs on the Mac, no hardware needed.
//
//   cc -std=c99 -Wall -Wextra -pedantic -O1 -Ikernel/qr/include \
//      kernel/qr/eos_qr.c kernel/qr/test/test_qr.c -o /tmp/tq && /tmp/tq
//   cc -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
//      -Ikernel/qr/include kernel/qr/eos_qr.c kernel/qr/test/test_qr.c \
//      -o /tmp/tq && /tmp/tq
//
// The reference matrices below are not this encoder's own output written down.
// Each one was produced by an independent implementation (the python-qrcode
// package) with the version and mask forced to what eos_qr chose, and the two
// agreed module for module before being pasted in. A separate sweep compared
// 916 payloads — every version, every boundary length, arbitrary binary bytes,
// automatic and forced version — against that implementation and against segno,
// at all eight masks: zero structural differences. So these four matrices stand
// in for a much larger agreement than four symbols' worth.
//
// What that does NOT cover is which mask wins, because the three encoders
// consulted all score penalty rule 3 differently and therefore disagree with
// each other about the mask perhaps half the time. Mask choice is a print
// quality heuristic and every mask decodes, so this file pins the rule eos_qr
// documents instead: all eight masks are scored, the lowest score wins, ties go
// to the lowest mask number. The eight scores are asserted literally.
//
// Format information is checked against ISO/IEC 18004 Table C.1 by reading the
// modules back out of the finished symbol, both copies, which is an assertion
// against the standard rather than against any implementation.
//
// The last thing this file does is print a real WIFI: string as a QR big enough
// to scan off the terminal, with a quiet zone. That is the check that matters:
// a QR that is subtly wrong still scans on the phone of whoever wrote it.

#include <stdio.h>
#include <string.h>
#include "eos_qr.h"

static int checks = 0, fails = 0;
#define CK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("    FAIL: %s\n", msg); } } while (0)

static eos_qr_t qr;
static eos_qr_t qr2;

// The string the SETUP screen actually shows. 41 bytes, which lands on version
// 3 and leaves twelve of the fifty-three payload bytes spare.
#define WIFI_STR "WIFI:S:esp-os-f048;T:WPA;P:k9mQ2xR7vT4b;;"

// ------------------------------------------------------------- reference: v1
// "ESP-OS", 6 bytes, version 1, mask 0.

static const char *REF_V1[] = {
    "111111100010101111111",
    "100000100000101000001",
    "101110101010001011101",
    "101110100000101011101",
    "101110100101101011101",
    "100000100111001000001",
    "111111101010101111111",
    "000000001010000000000",
    "111011111010111000100",
    "001101001111010101000",
    "101101111001011101011",
    "110001010111110110000",
    "010111100101011101111",
    "000000001110001101010",
    "111111101010100010111",
    "100000101010001011011",
    "101110101010101010011",
    "101110100111010000110",
    "101110101101011111101",
    "100000101111110010010",
    "111111101001011100111",
};

// ------------------------------------------------------------- reference: v2
// "esp-os-f048 setup mode", 22 bytes, version 2, mask 0. Carries the one
// alignment pattern, centred on (18,18).

static const char *REF_V2[] = {
    "1111111000100011101111111",
    "1000001001000101001000001",
    "1011101010010010101011101",
    "1011101001001011101011101",
    "1011101001001111001011101",
    "1000001000111111101000001",
    "1111111010101010101111111",
    "0000000010010111100000000",
    "1110111110001100011000100",
    "0011100111011000101101001",
    "0010101010111100111010111",
    "1001000011101111110101001",
    "0001111100110100001100001",
    "0100000001110100101100101",
    "1000111010100000000001111",
    "0110010001010101110000000",
    "1011001000101100111110011",
    "0000000010111101100010111",
    "1111111011011001101011011",
    "1000001010101110100010011",
    "1011101010110010111111011",
    "1011101000010000110111000",
    "1011101011100100100111001",
    "1000001011010000110001010",
    "1111111010001001101100011",
};

// ------------------------------------------------------------- reference: v3
// WIFI_STR, 41 bytes, version 3, mask 4. This is the symbol the panel shows.

static const char *REF_WIFI[] = {
    "11111110100001001001001111111",
    "10000010100111000011001000001",
    "10111010110010000101101011101",
    "10111010111011010110101011101",
    "10111010011011011011001011101",
    "10000010111001001011101000001",
    "11111110101010101010101111111",
    "00000000001100000111000000000",
    "11001110001010001110000101111",
    "11100001100001110100111111000",
    "01011011111001011000011011001",
    "01101001010010001110100010010",
    "11001011000100101001000000010",
    "00100000011010001001001111000",
    "11111011000111011100100000101",
    "11100100111100111101001011001",
    "00100010001010111110010000101",
    "11001000000000011100101011100",
    "00011110011001011000001101101",
    "00101101110010101100010100000",
    "11100110100101101100111110010",
    "00000000111010110110100011010",
    "11111110011111110001101010101",
    "10000010111101011110100011001",
    "10111010111011001111111111001",
    "10111010001001101101100100101",
    "10111010001000010101000001011",
    "10000010100010110111111011011",
    "11111110110101110101100010010",
};

// ------------------------------------------------------------- reference: v4
// The same join string with a setup URL appended. 65 bytes, version 4, mask 4.

#define V4_STR "WIFI:S:esp-os-f048;T:WPA;P:k9mQ2xR7vT4b;;http://192.168.4.1/setup"

static const char *REF_V4[] = {
    "111111101110101100000101001111111",
    "100000101110000010011100101000001",
    "101110101011001111001100001011101",
    "101110101101011100101010101011101",
    "101110100000010100000111101011101",
    "100000101001101000111000101000001",
    "111111101010101010101010101111111",
    "000000000100101001000101000000000",
    "110011100001000101101110000101111",
    "011001001110111101000100000000000",
    "111000101001101110001101100101100",
    "001111000011001101010100000010010",
    "100110110010100011100011011011110",
    "000101001000010101101011100100111",
    "110110100010001110101111110010110",
    "000010010100100001110101000001001",
    "110011110001000101100100111011001",
    "010000010110110101101010000000011",
    "010110101011101110100011001001110",
    "111100011111001111110111000110010",
    "100110110010100111111110011110010",
    "100111001110010100000110000101111",
    "000001110010000111000111101000010",
    "000010001100100001101110111000011",
    "110011110101011011110010111111001",
    "000000001110101101001011100010101",
    "111111100011111101000011101010110",
    "100000101101010111101110100010010",
    "101110101110110011110100111111011",
    "101110100010011100010010001010111",
    "101110100010000110000001110011010",
    "100000101000101001011100110011000",
    "111111101001011101110110111110001",
};

// ------------------------------------------------------------------ helpers

static void compare(const char *label, const char **ref, int n)
{
    int x, y, bad = 0;
    char msg[96];

    checks++;
    if (eos_qr_size(&qr) != n) {
        fails++;
        printf("    FAIL: %s size %d, expected %d\n", label, eos_qr_size(&qr), n);
        return;
    }
    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            bool want = ref[y][x] == '1';
            checks++;
            if (eos_qr_module(&qr, x, y) != want) { fails++; bad++; }
        }
    }
    if (bad) {
        snprintf(msg, sizeof msg, "%s: %d of %d modules differ from the reference",
                 label, bad, n * n);
        printf("    FAIL: %s\n", msg);
    }
}

// ------------------------------------------------------------------- table

static void test_table(void)
{
    printf("  capacity table\n");
    CK(eos_qr_capacity(0) == 0, "capacity(0) is 0");
    CK(eos_qr_capacity(1) == 17, "capacity(1) is 17");
    CK(eos_qr_capacity(2) == 32, "capacity(2) is 32");
    CK(eos_qr_capacity(3) == 53, "capacity(3) is 53");
    CK(eos_qr_capacity(4) == 78, "capacity(4) is 78");
    CK(eos_qr_capacity(5) == 0, "capacity(5) is 0, version 5 is out of scope");
    CK(eos_qr_capacity(-1) == 0, "capacity(-1) is 0");
    CK(EOS_QR_MAX_BYTES == 78, "EOS_QR_MAX_BYTES agrees with the table");

    CK(EOS_QR_VERSION_SIZE(1) == 21, "version 1 is 21 modules");
    CK(EOS_QR_VERSION_SIZE(2) == 25, "version 2 is 25 modules");
    CK(EOS_QR_VERSION_SIZE(3) == 29, "version 3 is 29 modules");
    CK(EOS_QR_VERSION_SIZE(4) == 33, "version 4 is 33 modules");
    CK(EOS_QR_MAX_SIZE == 33, "EOS_QR_MAX_SIZE agrees");
    CK(EOS_QR_STRIDE * 8 >= EOS_QR_MAX_SIZE, "the fixed stride holds a version 4 row");
    CK(EOS_QR_BUF_BYTES == 165, "module buffer is 165 bytes");

    CK(eos_qr_version_for(0) == 1, "0 bytes still names version 1");
    CK(eos_qr_version_for(17) == 1, "17 bytes is version 1");
    CK(eos_qr_version_for(18) == 2, "18 bytes needs version 2");
    CK(eos_qr_version_for(32) == 2, "32 bytes is version 2");
    CK(eos_qr_version_for(33) == 3, "33 bytes needs version 3");
    CK(eos_qr_version_for(53) == 3, "53 bytes is version 3");
    CK(eos_qr_version_for(54) == 4, "54 bytes needs version 4");
    CK(eos_qr_version_for(78) == 4, "78 bytes is version 4");
    CK(eos_qr_version_for(79) == 0, "79 bytes fits nowhere");
    CK(eos_qr_version_for(1000) == 0, "1000 bytes fits nowhere");

    CK(eos_qr_version_for(strlen(WIFI_STR)) == 3, "the join string is a version 3");
    CK(strlen(WIFI_STR) == 41, "the join string is 41 bytes");
}

// ------------------------------------------------------------------- errors
//
// Every one of these must return a named error and leave nothing half-built.
// Silent truncation is the failure mode this component exists to avoid: a
// truncated QR still scans, and hands the phone the wrong password.

static void test_errors(void)
{
    static uint8_t big[200];
    size_t i;

    printf("  refusals\n");
    for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)('a' + (i % 26));

    CK(eos_qr_encode(NULL, "x") == EOS_QR_ERR_NULL, "NULL symbol is refused");
    CK(eos_qr_encode(&qr, NULL) == EOS_QR_ERR_NULL, "NULL text is refused");
    CK(eos_qr_encode_bytes(&qr, NULL, 4) == EOS_QR_ERR_NULL, "NULL data with a length is refused");
    CK(eos_qr_encode(&qr, "") == EOS_QR_ERR_EMPTY, "empty string is refused");
    CK(eos_qr_encode_bytes(&qr, big, 0) == EOS_QR_ERR_EMPTY, "zero length is refused");

    CK(eos_qr_encode_bytes(&qr, big, 79) == EOS_QR_ERR_TOO_LONG, "79 bytes is refused");
    CK(eos_qr_encode_bytes(&qr, big, 200) == EOS_QR_ERR_TOO_LONG, "200 bytes is refused");
    CK(eos_qr_encode_bytes(&qr, big, 78) == EOS_QR_OK, "78 bytes is accepted");

    CK(eos_qr_encode_version(&qr, big, 4, 0) == EOS_QR_ERR_VERSION, "version 0 is refused");
    CK(eos_qr_encode_version(&qr, big, 4, 5) == EOS_QR_ERR_VERSION, "version 5 is refused");
    CK(eos_qr_encode_version(&qr, big, 4, -1) == EOS_QR_ERR_VERSION, "version -1 is refused");
    CK(eos_qr_encode_version(&qr, big, 18, 1) == EOS_QR_ERR_TOO_LONG,
       "18 bytes forced into version 1 is refused, not truncated");
    CK(eos_qr_encode_version(&qr, big, 33, 2) == EOS_QR_ERR_TOO_LONG,
       "33 bytes forced into version 2 is refused");
    CK(eos_qr_encode_version(&qr, big, 54, 3) == EOS_QR_ERR_TOO_LONG,
       "54 bytes forced into version 3 is refused");

    // A refused call must not have disturbed the previous symbol.
    CK(eos_qr_encode(&qr, "ESP-OS") == EOS_QR_OK, "a good encode after failures");
    CK(eos_qr_encode_bytes(&qr, big, 200) == EOS_QR_ERR_TOO_LONG, "refused again");
    CK(qr.version == 1 && qr.size == 21, "the refusal left the previous symbol alone");

    CK(strcmp(eos_qr_strerror(EOS_QR_OK), "ok") == 0, "strerror(OK)");
    CK(strlen(eos_qr_strerror(EOS_QR_ERR_TOO_LONG)) > 8, "strerror(TOO_LONG) says something");
    CK(strlen(eos_qr_strerror((eos_qr_err_t)99)) > 0, "strerror of a bogus code still returns text");

    CK(eos_qr_size(NULL) == 0, "size of NULL is 0");
    CK(eos_qr_module(NULL, 0, 0) == false, "module of NULL is light");
}

// ------------------------------------------------------ reference comparison

static void test_reference(void)
{
    printf("  reference matrices\n");

    CK(eos_qr_encode(&qr, "ESP-OS") == EOS_QR_OK, "encode ESP-OS");
    CK(qr.version == 1, "ESP-OS is a version 1");
    CK(qr.mask == 0, "ESP-OS picks mask 0");
    compare("v1 ESP-OS", REF_V1, 21);

    CK(eos_qr_encode(&qr, "esp-os-f048 setup mode") == EOS_QR_OK, "encode the v2 string");
    CK(qr.version == 2, "22 bytes is a version 2");
    CK(qr.mask == 0, "the v2 string picks mask 0");
    compare("v2 setup mode", REF_V2, 25);

    CK(eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK, "encode the join string");
    CK(qr.version == 3, "the join string is a version 3");
    CK(qr.mask == 4, "the join string picks mask 4");
    compare("v3 WIFI join string", REF_WIFI, 29);

    CK(eos_qr_encode(&qr, V4_STR) == EOS_QR_OK, "encode the v4 string");
    CK(qr.version == 4, "65 bytes is a version 4");
    CK(qr.mask == 4, "the v4 string picks mask 4");
    compare("v4 join string plus URL", REF_V4, 33);
}

// ---------------------------------------------------------------- codewords
//
// The version-1 stream is short enough to derive by hand, so it is derived
// here in the comment and asserted below. The ECC half was cross-checked
// against an independent Reed-Solomon implementation over the same 19 data
// codewords.
//
//   mode      0100
//   count     00000101              (5 bytes)
//   'H'..'O'  01001000 01000101 01001100 01001100 01001111
//   terminator 0000                 (52 bits + 4 = 56, already byte aligned)
//   -> 40 54 84 54 C4 C4 F0, then 12 pad codewords EC 11 alternating.

static void test_codewords(void)
{
    static const uint8_t HELLO[] = {
        0x40, 0x54, 0x84, 0x54, 0xC4, 0xC4, 0xF0,
        0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11, 0xEC, 0x11,
        0x4D, 0x2A, 0xD3, 0xBB, 0x9F, 0x20, 0x84,
    };
    int i, bad;

    printf("  codewords and Reed-Solomon\n");
    CK(eos_qr_encode(&qr, "HELLO") == EOS_QR_OK, "encode HELLO");
    CK(qr.version == 1, "HELLO is a version 1");
    CK(qr.data_cw == 19, "version 1 at L holds 19 data codewords");
    CK(qr.ecc_cw == 7, "version 1 at L holds 7 ECC codewords");
    CK(qr.total_cw == 26, "version 1 at L is 26 codewords");
    CK(qr.blocks == 1, "version 1 at L is one ECC block");
    CK(qr.data_len == 5, "HELLO is 5 payload bytes");

    for (i = 0, bad = 0; i < 26; i++) {
        checks++;
        if (qr.codewords[i] != HELLO[i]) {
            fails++;
            if (bad++ < 4)
                printf("    FAIL: codeword %d is %02X, expected %02X\n",
                       i, qr.codewords[i], HELLO[i]);
        }
    }

    // The pad pattern must alternate and must start with 0xEC. A stream that
    // starts on 0x11 encodes and scans, and is still wrong.
    CK(qr.codewords[7] == 0xEC, "padding starts with 0xEC");
    CK(qr.codewords[8] == 0x11, "padding alternates to 0x11");
    CK(qr.codewords[18] == 0x11, "padding runs to the end of the data capacity");

    // A full-capacity payload. In byte mode the header is 12 bits, so a version
    // holds capacity = data_cw - 2 bytes and the slack is always exactly the
    // four bits the terminator wants: no pad codewords, and the terminator is
    // never the truncated kind. The encoder implements the truncating case
    // anyway, because the moment this table grows a mode or a version whose
    // arithmetic differs, silently overrunning the capacity is the bug.
    CK(eos_qr_encode(&qr, "01234567890123456") == EOS_QR_OK, "encode 17 bytes");
    CK(qr.version == 1, "17 bytes exactly fills version 1");
    CK(qr.data_len == 17, "17 payload bytes");
    CK(qr.codewords[0] == 0x41, "mode nibble plus the high nibble of count 17");
    CK(qr.codewords[1] == 0x13, "low nibble of count 17 plus the high nibble of '0'");
    CK(qr.codewords[17] == 0x53, "codeword 17 is the '5' and '6' nibbles");
    CK(qr.codewords[18] == 0x60, "the last data codeword is the low nibble of '6' plus the terminator");
    CK(qr.codewords[18] != 0xEC && qr.codewords[18] != 0x11, "a full payload takes no padding");

    // Sizes for the other three versions, straight off the table.
    CK(eos_qr_encode(&qr, "esp-os-f048 setup mode") == EOS_QR_OK, "encode v2");
    CK(qr.data_cw == 34 && qr.ecc_cw == 10 && qr.total_cw == 44, "version 2 at L is 34+10");
    CK(eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK, "encode v3");
    CK(qr.data_cw == 55 && qr.ecc_cw == 15 && qr.total_cw == 70, "version 3 at L is 55+15");
    CK(eos_qr_encode(&qr, V4_STR) == EOS_QR_OK, "encode v4");
    CK(qr.data_cw == 80 && qr.ecc_cw == 20 && qr.total_cw == 100, "version 4 at L is 80+20");
    CK(qr.blocks == 1, "version 4 at L is still one ECC block");
}

// -------------------------------------------------------------- format info
//
// ISO/IEC 18004 Table C.1, ECC level L, the eight masks. Read back out of the
// finished symbol, both copies, so this asserts against the standard and not
// against another encoder.

static const char *FORMAT_L[8] = {
    "111011111000100",   // mask 0
    "111001011110011",   // mask 1
    "111110110101010",   // mask 2
    "111100010011101",   // mask 3
    "110011000101111",   // mask 4
    "110001100011000",   // mask 5
    "110110001000001",   // mask 6
    "110100101110110",   // mask 7
};

// Bit k of the format string, counting from the least significant end, which
// is how the two copies are laid out around the finders.
static bool fmt_bit(int mask, int k)
{
    return FORMAT_L[mask][14 - k] == '1';
}

static void check_format(const char *label)
{
    const int n = eos_qr_size(&qr);
    const int m = qr.mask;
    int i;
    char msg[96];

    for (i = 0; i <= 5; i++) {
        snprintf(msg, sizeof msg, "%s copy 1 bit %d", label, i);
        CK(eos_qr_module(&qr, 8, i) == fmt_bit(m, i), msg);
    }
    CK(eos_qr_module(&qr, 8, 7) == fmt_bit(m, 6), "copy 1 bit 6");
    CK(eos_qr_module(&qr, 8, 8) == fmt_bit(m, 7), "copy 1 bit 7");
    CK(eos_qr_module(&qr, 7, 8) == fmt_bit(m, 8), "copy 1 bit 8");
    for (i = 9; i < 15; i++) {
        snprintf(msg, sizeof msg, "%s copy 1 bit %d", label, i);
        CK(eos_qr_module(&qr, 14 - i, 8) == fmt_bit(m, i), msg);
    }

    for (i = 0; i < 8; i++) {
        snprintf(msg, sizeof msg, "%s copy 2 bit %d", label, i);
        CK(eos_qr_module(&qr, n - 1 - i, 8) == fmt_bit(m, i), msg);
    }
    for (i = 8; i < 15; i++) {
        snprintf(msg, sizeof msg, "%s copy 2 bit %d", label, i);
        CK(eos_qr_module(&qr, 8, n - 15 + i) == fmt_bit(m, i), msg);
    }

    CK(eos_qr_module(&qr, 8, n - 8), "the always-dark module is dark");
}

static void test_format(void)
{
    printf("  format information, ISO/IEC 18004 Table C.1\n");
    CK(eos_qr_encode(&qr, "ESP-OS") == EOS_QR_OK, "v1 for format check");
    check_format("v1");
    CK(eos_qr_encode(&qr, "esp-os-f048 setup mode") == EOS_QR_OK, "v2 for format check");
    check_format("v2");
    CK(eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK, "v3 for format check");
    check_format("v3");
    CK(eos_qr_encode(&qr, V4_STR) == EOS_QR_OK, "v4 for format check");
    check_format("v4");
}

// --------------------------------------------------------- function patterns
//
// Finders, separators, timing, alignment and the dark module, checked
// positionally rather than by comparing against a stored matrix, so a wrong
// alignment centre is named rather than showing up as "471 modules differ".

static void check_finder(int ox, int oy, const char *label)
{
    int dx, dy;
    char msg[96];

    for (dy = 0; dy < 7; dy++)
        for (dx = 0; dx < 7; dx++) {
            bool want = (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                         (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
            snprintf(msg, sizeof msg, "%s finder (%d,%d)", label, dx, dy);
            CK(eos_qr_module(&qr, ox + dx, oy + dy) == want, msg);
        }
    // The separator: the light ring on the two inward sides.
    for (dy = -1; dy <= 7; dy++) {
        int x = ox + (ox == 0 ? 7 : -1);
        if (oy + dy < 0 || oy + dy >= eos_qr_size(&qr)) continue;
        snprintf(msg, sizeof msg, "%s separator column at row %d", label, oy + dy);
        CK(eos_qr_module(&qr, x, oy + dy) == false, msg);
    }
    for (dx = -1; dx <= 7; dx++) {
        int y = oy + (oy == 0 ? 7 : -1);
        if (ox + dx < 0 || ox + dx >= eos_qr_size(&qr)) continue;
        snprintf(msg, sizeof msg, "%s separator row at column %d", label, ox + dx);
        CK(eos_qr_module(&qr, ox + dx, y) == false, msg);
    }
}

static void check_patterns(const char *label, int align_centre)
{
    const int n = eos_qr_size(&qr);
    int i, dx, dy;
    char msg[96];

    check_finder(0, 0, "top-left");
    check_finder(n - 7, 0, "top-right");
    check_finder(0, n - 7, "bottom-left");

    for (i = 8; i < n - 8; i++) {
        snprintf(msg, sizeof msg, "%s horizontal timing at %d", label, i);
        CK(eos_qr_module(&qr, i, 6) == ((i % 2) == 0), msg);
        snprintf(msg, sizeof msg, "%s vertical timing at %d", label, i);
        CK(eos_qr_module(&qr, 6, i) == ((i % 2) == 0), msg);
    }

    if (align_centre) {
        for (dy = -2; dy <= 2; dy++)
            for (dx = -2; dx <= 2; dx++) {
                int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
                bool want = (ax > ay ? ax : ay) != 1;
                snprintf(msg, sizeof msg, "%s alignment (%d,%d)", label, dx, dy);
                CK(eos_qr_module(&qr, align_centre + dx, align_centre + dy) == want, msg);
            }
    }

    CK(eos_qr_module(&qr, 8, n - 8), "dark module at (8, size-8)");

    // Out of range in every direction reads light, so a renderer can walk a
    // padded box without clamping.
    CK(eos_qr_module(&qr, -1, 0) == false, "x = -1 is light");
    CK(eos_qr_module(&qr, 0, -1) == false, "y = -1 is light");
    CK(eos_qr_module(&qr, n, 0) == false, "x = size is light");
    CK(eos_qr_module(&qr, 0, n) == false, "y = size is light");
    CK(eos_qr_module(&qr, 10000, 10000) == false, "far out of range is light");
}

static void test_patterns(void)
{
    printf("  function patterns\n");
    CK(eos_qr_encode(&qr, "ESP-OS") == EOS_QR_OK, "v1 for pattern check");
    check_patterns("v1", 0);
    CK(eos_qr_encode(&qr, "esp-os-f048 setup mode") == EOS_QR_OK, "v2 for pattern check");
    check_patterns("v2", 18);
    CK(eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK, "v3 for pattern check");
    check_patterns("v3", 22);
    CK(eos_qr_encode(&qr, V4_STR) == EOS_QR_OK, "v4 for pattern check");
    check_patterns("v4", 26);
}

// -------------------------------------------------------------------- masks
//
// The documented rule: all eight masks are applied, formatted and scored, the
// lowest score wins, and a tie goes to the lowest mask number.

static void test_masks(void)
{
    // The eight scores for the join string. Asserted literally, because
    // "the penalty function returns something" is not a test.
    static const uint32_t WIFI_PEN[8] = {
        1335, 1416, 1309, 1362, 1292, 1435, 1405, 1381
    };
    int m, distinct, i, j;
    uint32_t lo;
    char msg[96];

    printf("  mask evaluation\n");
    CK(eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK, "encode the join string");

    for (m = 0; m < EOS_QR_MASKS; m++) {
        snprintf(msg, sizeof msg, "mask %d scores %u", m, WIFI_PEN[m]);
        CK(qr.penalty[m] == WIFI_PEN[m], msg);
    }

    // Every mask was actually evaluated: a mask that was skipped would leave a
    // zero behind, and no real symbol scores zero.
    for (m = 0; m < EOS_QR_MASKS; m++) {
        snprintf(msg, sizeof msg, "mask %d was evaluated", m);
        CK(qr.penalty[m] > 0, msg);
    }

    // The eight scores must not be all the same, which is what a mask function
    // that ignores its argument would produce.
    distinct = 0;
    for (i = 0; i < EOS_QR_MASKS; i++) {
        int seen = 0;
        for (j = 0; j < i; j++) if (qr.penalty[j] == qr.penalty[i]) seen = 1;
        if (!seen) distinct++;
    }
    CK(distinct >= 6, "the eight masks produce distinct scores");

    // The winner is the argmin, ties to the lowest index.
    lo = qr.penalty[0];
    for (m = 1; m < EOS_QR_MASKS; m++) if (qr.penalty[m] < lo) lo = qr.penalty[m];
    CK(qr.penalty[qr.mask] == lo, "the chosen mask holds the lowest score");
    for (m = 0; m < qr.mask; m++) {
        snprintf(msg, sizeof msg, "no earlier mask ties the winner (%d)", m);
        CK(qr.penalty[m] > lo, msg);
    }
    CK(qr.mask == 4, "mask 4 wins for the join string");
    CK(qr.mask < EOS_QR_MASKS, "the mask is in range");

    // Same for the other three sizes: whatever wins, it is the argmin.
    {
        const char *cases[3] = { "ESP-OS", "esp-os-f048 setup mode", V4_STR };
        int c;
        for (c = 0; c < 3; c++) {
            CK(eos_qr_encode(&qr, cases[c]) == EOS_QR_OK, "encode a mask case");
            lo = qr.penalty[0];
            for (m = 1; m < EOS_QR_MASKS; m++) if (qr.penalty[m] < lo) lo = qr.penalty[m];
            CK(qr.penalty[qr.mask] == lo, "argmin wins");
            for (m = 0; m < EOS_QR_MASKS; m++) {
                snprintf(msg, sizeof msg, "case %d mask %d evaluated", c, m);
                CK(qr.penalty[m] > 0, msg);
            }
        }
    }
}

// ------------------------------------------------------------- determinism
//
// The provisioning flow encodes the same string on every boot into SETUP. Two
// encodes of the same bytes must produce the same symbol, and the encoder must
// carry nothing between calls.

static void test_determinism(void)
{
    int i, bad = 0;

    printf("  determinism\n");
    CK(eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK, "first encode");
    CK(eos_qr_encode(&qr2, V4_STR) == EOS_QR_OK, "an unrelated encode in between");
    CK(eos_qr_encode(&qr2, WIFI_STR) == EOS_QR_OK, "second encode of the join string");

    for (i = 0; i < EOS_QR_BUF_BYTES; i++) {
        checks++;
        if (qr.modules[i] != qr2.modules[i]) { fails++; bad++; }
    }
    CK(bad == 0, "the two symbols are byte identical");
    CK(qr.mask == qr2.mask, "the same mask is chosen both times");
    CK(memcmp(qr.penalty, qr2.penalty, sizeof qr.penalty) == 0, "the same scores both times");
    CK(memcmp(qr.codewords, qr2.codewords, sizeof qr.codewords) == 0, "the same codewords");

    // A shorter payload after a longer one must not leave the tail of the
    // previous symbol behind.
    CK(eos_qr_encode(&qr2, "ESP-OS") == EOS_QR_OK, "a shorter payload after a longer one");
    CK(eos_qr_encode(&qr, "ESP-OS") == EOS_QR_OK, "the same, into a fresh symbol");
    CK(memcmp(qr.modules, qr2.modules, EOS_QR_BUF_BYTES) == 0, "no residue from the longer symbol");
}

// ----------------------------------------------------------- every length
//
// Encode every payload length from 1 to 78 and check the invariants that hold
// regardless of content: the version is the smallest that fits, the finders
// and the dark module are where they belong, and the mask is the argmin.

static void test_all_lengths(void)
{
    static uint8_t buf[EOS_QR_MAX_BYTES];
    size_t len;
    int i;

    printf("  every payload length\n");
    for (i = 0; i < EOS_QR_MAX_BYTES; i++) buf[i] = (uint8_t)(i * 7 + 33);

    for (len = 1; len <= EOS_QR_MAX_BYTES; len++) {
        int want = eos_qr_version_for(len);
        int n, m;
        uint32_t lo;
        char msg[96];

        snprintf(msg, sizeof msg, "encode %d bytes", (int)len);
        CK(eos_qr_encode_bytes(&qr, buf, len) == EOS_QR_OK, msg);
        n = eos_qr_size(&qr);
        snprintf(msg, sizeof msg, "%d bytes lands on version %d", (int)len, want);
        CK(qr.version == want, msg);
        CK(n == EOS_QR_VERSION_SIZE(want), "size matches the version");
        CK(qr.data_len == (uint8_t)len, "data_len records the payload");
        CK(eos_qr_module(&qr, 0, 0) && eos_qr_module(&qr, 6, 6), "top-left finder corners");
        CK(eos_qr_module(&qr, n - 1, 0) && eos_qr_module(&qr, 0, n - 1), "the other two finders");
        CK(eos_qr_module(&qr, 7, 7) == false, "the separator corner is light");
        CK(eos_qr_module(&qr, 8, n - 8), "the dark module is dark");
        lo = qr.penalty[0];
        for (m = 1; m < EOS_QR_MASKS; m++) if (qr.penalty[m] < lo) lo = qr.penalty[m];
        CK(qr.penalty[qr.mask] == lo, "the argmin mask won");
    }
}

// ------------------------------------------------------------------- render

static void test_render(void)
{
    static uint8_t buf[EOS_QR_SCALED_BYTES(EOS_QR_MAX_SIZE, 6, EOS_QR_QUIET)];
    int w = 0, h = 0, px, stride, x, y, bad;

    printf("  scaled render\n");
    CK(eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK, "encode for render");

    CK(eos_qr_render_bytes(NULL, 4, 4) == 0, "render_bytes of NULL is 0");
    CK(eos_qr_render_bytes(&qr, 0, 4) == 0, "scale 0 is refused");
    CK(eos_qr_render_bytes(&qr, -1, 4) == 0, "negative scale is refused");
    CK(eos_qr_render_bytes(&qr, 4, -1) == 0, "negative quiet zone is refused");

    // 29 modules + 8 quiet = 37, at scale 4 = 148 px, stride 19, 2812 bytes.
    CK(eos_qr_render_bytes(&qr, 4, EOS_QR_QUIET) == 19 * 148, "render_bytes arithmetic");
    CK(eos_qr_render_bytes(&qr, 4, EOS_QR_QUIET) ==
       (size_t)EOS_QR_SCALED_BYTES(29, 4, EOS_QR_QUIET), "the macro agrees with the function");
    CK(EOS_QR_SCALED_PX(29, 4, 4) == 148, "SCALED_PX");
    CK(EOS_QR_SCALED_STRIDE(29, 4, 4) == 19, "SCALED_STRIDE");

    CK(eos_qr_render(&qr, 4, EOS_QR_QUIET, buf, 10, &w, &h) == false,
       "a buffer that is too small is refused");
    CK(eos_qr_render(&qr, 4, EOS_QR_QUIET, NULL, sizeof buf, &w, &h) == false,
       "a NULL buffer is refused");
    CK(eos_qr_render(NULL, 4, EOS_QR_QUIET, buf, sizeof buf, &w, &h) == false,
       "a NULL symbol is refused");

    // Scale 1, no quiet zone: the render must reproduce the module map exactly.
    CK(eos_qr_render(&qr, 1, 0, buf, sizeof buf, &w, &h), "scale 1 render");
    CK(w == 29 && h == 29, "scale 1 is 29x29 pixels");
    stride = (29 + 7) / 8;
    for (y = 0, bad = 0; y < 29; y++)
        for (x = 0; x < 29; x++) {
            bool got = (buf[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
            checks++;
            if (got != eos_qr_module(&qr, x, y)) { fails++; bad++; }
        }
    CK(bad == 0, "scale 1 reproduces the module map");

    // Scale 3 with a quiet zone: every module becomes a solid 3x3 block, the
    // quiet zone is entirely light, and the border is where it should be.
    CK(eos_qr_render(&qr, 3, EOS_QR_QUIET, buf, sizeof buf, &w, &h), "scale 3 render");
    px = (29 + 8) * 3;
    CK(w == px && h == px, "scale 3 pixel size");
    stride = (px + 7) / 8;
    for (y = 0, bad = 0; y < px; y++)
        for (x = 0; x < px; x++) {
            int mx = x / 3 - EOS_QR_QUIET, my = y / 3 - EOS_QR_QUIET;
            bool want = eos_qr_module(&qr, mx, my);   // out of range reads light
            bool got = (buf[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1;
            checks++;
            if (got != want) { fails++; bad++; }
        }
    CK(bad == 0, "scale 3 blocks and quiet zone are exact");

    // The quiet zone specifically: the outer four modules on all four sides.
    for (y = 0, bad = 0; y < px; y++)
        for (x = 0; x < px; x++) {
            int q = EOS_QR_QUIET * 3;
            if (x >= q && x < px - q && y >= q && y < px - q) continue;
            checks++;
            if ((buf[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1) { fails++; bad++; }
        }
    CK(bad == 0, "the quiet zone is entirely light");

    // The largest thing the macro budgets for must fit.
    CK(eos_qr_encode(&qr, V4_STR) == EOS_QR_OK, "encode the largest symbol");
    CK(eos_qr_render_bytes(&qr, 6, EOS_QR_QUIET) <= sizeof buf,
       "a version 4 at scale 6 fits the macro-sized buffer");
    CK(eos_qr_render(&qr, 6, EOS_QR_QUIET, buf, sizeof buf, &w, &h), "version 4 at scale 6");
    CK(w == (33 + 8) * 6 && h == w, "246 pixels square");
}

// ------------------------------------------------------------------- output
//
// A QR that is subtly wrong scans on the phone of whoever wrote it and on
// nobody else's. So print it, large, with a real quiet zone, and let a human
// point a phone at the terminal. Dark modules are drawn as a black background
// and light modules as a white one, because a terminal with a dark theme would
// otherwise render the symbol inverted and half the scanners in the world
// refuse an inverted symbol.

#define BLK_DARK  "\033[40m  \033[0m"
#define BLK_LIGHT "\033[47m  \033[0m"

static void render_ansi(const char *what)
{
    const int n = eos_qr_size(&qr);
    const int q = EOS_QR_QUIET;
    int x, y;

    printf("\n  %s\n", what);
    printf("  version %d, %dx%d modules, mask %d, quiet zone %d\n\n",
           qr.version, n, n, qr.mask, q);

    for (y = -q; y < n + q; y++) {
        printf("  ");
        for (x = -q; x < n + q; x++)
            fputs(eos_qr_module(&qr, x, y) ? BLK_DARK : BLK_LIGHT, stdout);
        putchar('\n');
    }
    putchar('\n');
}

static void render_ascii(void)
{
    const int n = eos_qr_size(&qr);
    const int q = EOS_QR_QUIET;
    int x, y;

    printf("  the same symbol one character per module, for diffing:\n\n");
    for (y = -q; y < n + q; y++) {
        printf("    ");
        for (x = -q; x < n + q; x++)
            putchar(eos_qr_module(&qr, x, y) ? '#' : '.');
        putchar('\n');
    }
    putchar('\n');
}

int main(void)
{
    printf("test_qr\n");
    test_table();
    test_errors();
    test_reference();
    test_codewords();
    test_format();
    test_patterns();
    test_masks();
    test_determinism();
    test_all_lengths();
    test_render();

    if (eos_qr_encode(&qr, WIFI_STR) == EOS_QR_OK) {
        render_ansi("point a phone at this. It should offer to join \"esp-os-f048\".");
        printf("  payload: %s\n", WIFI_STR);
        render_ascii();
    }

    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
