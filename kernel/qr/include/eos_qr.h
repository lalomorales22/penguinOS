// eos_qr — a QR encoder scoped to exactly one job: putting the SoftAP join
// string on the panel during SETUP.
//
// docs/provisioning.md makes the screen the out-of-band channel. The board
// brings up a WPA2 SoftAP with a password nobody has ever seen, prints that
// password on the LCD, and prints a QR beside it so a phone camera joins by
// pointing rather than by typing. The string is short and fixed in shape:
//
//     WIFI:S:esp-os-f048;T:WPA;P:<12 chars>;;      about 42 characters
//
// So this encoder is byte mode only, error-correction level L only, versions 1
// through 4 only — 21x21 up to 33x33 modules, 78 bytes at the top end. That is
// three quarters of the standard left unimplemented on purpose. Anything longer
// than the version-4 capacity is REFUSED with EOS_QR_ERR_TOO_LONG; it is never
// truncated, because a truncated QR still scans and hands the phone a wrong
// password, which is a far worse failure than an error return.
//
// The non-obvious constraint: the module buffer uses a FIXED five-byte row
// stride at every version, not the (size+7)/8 the version would justify. A
// version-1 symbol therefore wastes two bytes on each of its 21 rows. That is
// deliberate. Five bytes is the version-4 stride, so one compile-time buffer
// size serves every version, and the packing is byte-for-byte EOS_PIXFMT_MONO1
// with stride = EOS_QR_STRIDE — the panel blits it with no repacking and no
// second buffer, on a board that has no PSRAM to spare for one.
//
// No allocation anywhere. The caller owns the eos_qr_t; everything the encoder
// needs, including the Reed-Solomon scratch and the function-module map, lives
// inside it.

#ifndef EOS_QR_H
#define EOS_QR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ------------------------------------------------------------------- limits

#define EOS_QR_MIN_VERSION    1
#define EOS_QR_MAX_VERSION    4
#define EOS_QR_MIN_SIZE       21                  // modules per side, version 1
#define EOS_QR_MAX_SIZE       33                  // modules per side, version 4
#define EOS_QR_STRIDE         5                   // bytes per module row, ALWAYS
#define EOS_QR_BUF_BYTES      (EOS_QR_STRIDE * EOS_QR_MAX_SIZE)   // 165
#define EOS_QR_MAX_CODEWORDS  100                 // version 4: 80 data + 20 ECC
#define EOS_QR_MAX_ECC        20                  // ECC codewords, version 4 at L
#define EOS_QR_MAX_BYTES      78                  // byte-mode payload, version 4 at L
#define EOS_QR_MASKS          8
#define EOS_QR_QUIET          4                   // minimum quiet zone, in modules

// The size in modules of a version. 21, 25, 29, 33.
#define EOS_QR_VERSION_SIZE(v)  (17 + 4 * (v))

// ------------------------------------------------------------------- errors

typedef enum {
    EOS_QR_OK = 0,
    EOS_QR_ERR_NULL,      // NULL eos_qr_t or NULL data with a non-zero length
    EOS_QR_ERR_EMPTY,     // zero-length payload; there is nothing to encode
    EOS_QR_ERR_TOO_LONG,  // more than EOS_QR_MAX_BYTES, or than the forced version holds
    EOS_QR_ERR_VERSION,   // requested version outside 1..4
} eos_qr_err_t;

const char *eos_qr_strerror(eos_qr_err_t e);

// -------------------------------------------------------------------- state
//
// One symbol, everything it took to build it, and the scratch. ~300 bytes.
// Declare it static or on a task stack; nothing here is allocated.

typedef struct {
    uint8_t  modules[EOS_QR_BUF_BYTES];   // the symbol. Set bit = DARK.
    uint8_t  reserved[EOS_QR_BUF_BYTES];  // set bit = function module, never masked
    uint8_t  codewords[EOS_QR_MAX_CODEWORDS];  // data then ECC, after interleaving
    uint32_t penalty[EOS_QR_MASKS];       // score of every mask, in mask order
    uint8_t  version;                     // 1..4
    uint8_t  size;                        // modules per side
    uint8_t  mask;                        // the winning mask, 0..7
    uint8_t  data_len;                    // payload bytes encoded
    uint8_t  data_cw;                     // data codewords for this version
    uint8_t  ecc_cw;                      // ECC codewords for this version
    uint8_t  total_cw;                    // data_cw + ecc_cw
    uint8_t  blocks;                      // ECC blocks; 1 for every supported version
} eos_qr_t;

// -------------------------------------------------------------------- table
//
// Byte-mode payload capacity at ECC level L. Returns 0 for a version outside
// 1..4, so a caller can loop without bounds-checking first.

int eos_qr_capacity(int version);

// The smallest version that holds len payload bytes, or 0 if none does. This
// is the function to ask before building a string you intend to encode.
int eos_qr_version_for(size_t len);

// ------------------------------------------------------------------- encode
//
// Each of these fills *qr completely on success and leaves it untouched on
// failure. They are pure: same input, same symbol, every time — the mask is
// chosen by the standard's penalty score, not by anything ambient.

eos_qr_err_t eos_qr_encode(eos_qr_t *qr, const char *text);
eos_qr_err_t eos_qr_encode_bytes(eos_qr_t *qr, const uint8_t *data, size_t len);

// Forces a version instead of picking the smallest that fits. The provisioning
// path does not want this; the test suite does, to exercise all four versions
// against reference matrices.
eos_qr_err_t eos_qr_encode_version(eos_qr_t *qr, const uint8_t *data,
                                   size_t len, int version);

// -------------------------------------------------------------------- reads

// True when the module is dark. Out-of-range coordinates read light, which is
// the quiet zone, so a renderer can walk a padded box without clamping.
bool eos_qr_module(const eos_qr_t *qr, int x, int y);

// Modules per side, 0 for a NULL or unencoded symbol.
int eos_qr_size(const eos_qr_t *qr);

// ------------------------------------------------------------------- render
//
// Scaled 1bpp render into a caller-provided buffer: every module becomes a
// scale x scale block, with `quiet` light modules of margin on all four sides.
// Set bit = dark, MSB first, one row per output pixel row. That is MONO1 with
// stride = EOS_QR_SCALED_STRIDE(...), which is what eos_display_blit wants.
//
// Size the buffer with the macros. They take the symbol size in MODULES:
//
//     uint8_t buf[EOS_QR_SCALED_BYTES(EOS_QR_MAX_SIZE, 5, EOS_QR_QUIET)];
//     int w, h;
//     eos_qr_render(&qr, 5, EOS_QR_QUIET, buf, sizeof buf, &w, &h);
//     eos_bitmap_t bm = { buf, w, h, EOS_QR_SCALED_STRIDE(qr.size, 5, EOS_QR_QUIET),
//                         EOS_PIXFMT_MONO1, EOS_COLOR_NONE, ink, paper };
//
// This header deliberately does NOT include eos_display.h. The encoder has no
// business knowing about panels; the four lines above are the whole bridge.
//
// At scale 5 with a 4-module quiet zone a version-4 symbol is 205x205 pixels
// and costs 5,330 bytes, which fits on a 240x240 panel with room for the AP
// name underneath. Scale 1 exists for tests and is not scannable.

#define EOS_QR_SCALED_PX(mod, scale, quiet)   (((mod) + 2 * (quiet)) * (scale))
#define EOS_QR_SCALED_STRIDE(mod, scale, quiet) \
    ((EOS_QR_SCALED_PX(mod, scale, quiet) + 7) / 8)
#define EOS_QR_SCALED_BYTES(mod, scale, quiet) \
    (EOS_QR_SCALED_STRIDE(mod, scale, quiet) * EOS_QR_SCALED_PX(mod, scale, quiet))

// Bytes eos_qr_render() will write for this symbol at this scale and quiet
// zone. 0 if any argument is out of range. Use it to check a buffer before the
// call; eos_qr_render() checks too and refuses rather than overrunning.
size_t eos_qr_render_bytes(const eos_qr_t *qr, int scale, int quiet);

// Fills out[0..render_bytes) and, when out_w/out_h are non-NULL, the pixel
// dimensions. False if the symbol is unencoded, scale < 1, quiet < 0, or the
// buffer is too small. Nothing is written on a false return.
bool eos_qr_render(const eos_qr_t *qr, int scale, int quiet,
                   uint8_t *out, size_t out_len, int *out_w, int *out_h);

#endif // EOS_QR_H
