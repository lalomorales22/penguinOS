// eos_app_camera — a viewfinder for a penguinOS camera node.
//
// The camera node is a screenless board with an OV3660 and 8MB of PSRAM. This
// board has a screen and about 30KB of free heap. That asymmetry decides
// everything here.
//
// WHAT THIS APP DOES NOT DO: hold a frame. A 240x320 RGB565 image is 153,600
// bytes and the largest block on this board is under 30,000. There is no
// arrangement of this app that buffers a picture, so it never tries.
//
// Instead it draws ONE HORIZONTAL STRIP PER FRAME. Each draw call fetches 40
// rows - 19,200 bytes - blits them, and advances. Eight calls later the picture
// is complete and it starts again at the top. A viewfinder that fills in from
// the top over about a second, which is what a board this size can honestly do.
//
// The strip size is measured, not guessed. Fetching costs a fixed ~65ms per
// request regardless of size, so small strips are dominated by connection
// setup: 16 rows takes 1.55s for a full frame, 40 rows takes 0.75s, and 80 rows
// would be faster still but its 38,400 bytes do not fit. 40 is the largest that
// does.
//
// The node holds one captured frame and serves strips out of it, so all eight
// strips of a picture come from the same moment. Without that the image would
// tear across every strip boundary - eight photographs of a moving world rather
// than one photograph.

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#endif

#include "eos_app_registry.h"
#include "eos_display.h"
#include "eos_settings.h"

// How many rows are FETCHED per draw call. Measured: a request costs a fixed
// ~65ms regardless of size, so 16-row strips spend 1.55s per picture on
// connection setup alone while 40-row strips take 0.75s.
// SCREEN rows per request. Raised from 40 once the picture went to half
// resolution: a strip that used to be 18,560 bytes is now 4,640, so four times
// as many rows fit in the same transfer. That matters more than the bytes -
// each request costs a FIXED ~65ms of connection setup regardless of size, so
// halving the request count halves the dominant cost.
//
//   40 screen rows, full size   8 requests, ~134 KB, about 2.4s a picture
//  160 screen rows, half size   2 requests,  ~33 KB, well under a second
#define CAM_STRIP_ROWS 160
#define CAM_MAX_W      240

// How many rows are HELD AT ONCE, which is a different question and much
// smaller. The board never buffers the strip it asked for: it reads the socket
// in chunks and blits each chunk the moment it is complete. Eight rows is
// 3,840 bytes.
//
// The first attempt held a whole 40-row strip - 19,200 bytes - and overflowed
// this board's DRAM by 14,864. That failure was useful: it forced the
// realisation that a strip does not need to be held either. The picture arrives
// as a stream and leaves as a stream, and the only memory required is the
// window between the two.
// SOURCE rows held at once. Each becomes two rows on the glass - see
// CAM_SCALE - so eight rows of screen come out of four rows of wire.
#define CAM_CHUNK_ROWS 4

// The picture is fetched at HALF the tile's size and doubled on the way to the
// panel. That is a quarter of the bytes for a barely visible loss on a 2.4 inch
// screen, and it is the difference between a viewfinder and a wipe:
//
//   full size   232x289 = 134,096 bytes, 8 requests, about 2.4s per picture
//   half size   116x144 =  33,408 bytes, 4 requests, well under a second
//
// At 2.4s the top of the frame is already being redrawn before the bottom has
// arrived, so what you watch is a bar sweeping down rather than an image.
#define CAM_SCALE 2
#ifdef ESP_PLATFORM
// Two small buffers, both device-only: off-target there is no socket to fill
// them and an unused array is a -Werror away from breaking three host suites,
// which is exactly how that was found the first time.
//
// s_chunk holds SOURCE rows straight off the wire - half width, so half of
// CAM_MAX_W. s_out holds the doubled result on its way to the panel. Together
// about 4.6 KB, against 134 KB for the frame neither of them ever holds.
static uint16_t s_chunk[(CAM_MAX_W / CAM_SCALE) * CAM_CHUNK_ROWS];
static uint16_t s_out[CAM_MAX_W * CAM_CHUNK_ROWS * CAM_SCALE];
#endif

static bool     s_visible;       // is the window on the glass this pass
static uint32_t s_next_ms;       // when the next strip may be fetched
static bool     s_dirty;         // ask the shell for a redraw

// How often a strip may be fetched, and the honest cost of this design.
//
// The fetch happens INSIDE the draw call, because that is the only place an app
// is allowed to put pixels on the glass. Each strip is about 90ms of blocking
// network, so the shell loses that time whenever a camera window is on screen -
// which is why the desktop feels heavy while it is open.
//
// 300ms is a compromise, not a fix: the shell keeps roughly two thirds of its
// time and a full picture lands in about two and a half seconds. Lower it for a
// livelier viewfinder and a heavier desktop; raise it for the reverse.
//
// THE REAL FIX is to move the fetch off the draw path entirely - a background
// task that owns a strip buffer and blits under the display lock - which needs
// the shell to hand out that lock and is a larger change than this app.
#define CAM_PERIOD_MS 120

static int      s_y;             // which strip comes next
static uint32_t s_ok, s_fail;    // strips fetched, strips that failed
static char     s_note[64];      // what to say when there is nothing to show

#ifdef ESP_PLATFORM
// Fetch one strip and blit it AS IT ARRIVES, never holding more than
// CAM_CHUNK_ROWS rows. Returns the number of rows drawn.
//
// The shell calls this from its draw path, so the timeouts are short: a camera
// that has gone away must cost a stutter, not a hung desktop.
// sw is the width ARRIVING (half), dw the width DRAWN (full), rows the number
// of screen rows this strip should fill.
static int cam_stream(const char *host, const char *path,
                      int sw, int dw, int rows, int16_t dx, int16_t dy,
                      const eos_app_ctx_t *c)
{
    (void)c;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) { freeaddrinfo(res); return -2; }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res); close(fd); return -3;
    }
    freeaddrinfo(res);

    char req[192];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s\r\n"
                     "User-Agent: penguinos/1\r\nConnection: close\r\n\r\n",
                     path, host);
    if (send(fd, req, (size_t)n, 0) != n) { close(fd); return -4; }

    const size_t row_bytes   = (size_t)sw * 2u;   /* a SOURCE row */
    const size_t chunk_bytes = row_bytes * CAM_CHUNK_ROWS;
    uint8_t *acc = (uint8_t *)s_chunk;

    size_t have = 0;          // bytes in the chunk buffer
    int    drawn = 0;         // rows blitted so far
    int    hdr_done = 0;
    uint8_t rb[512];
    int match = 0;            // how much of CRLFCRLF has been seen

    while (drawn < rows) {
        int r = recv(fd, rb, sizeof rb, 0);
        if (r <= 0) break;
        int off = 0;
        if (!hdr_done) {
            // Scan for the blank line, carrying state ACROSS reads - a header
            // that straddles two recv() calls is normal, and a scanner that
            // restarts each time silently drops the body.
            for (int i = 0; i < r; i++) {
                char ch = (char)rb[i];
                if ((match == 0 && ch == '\r') || (match == 2 && ch == '\r')) match++;
                else if ((match == 1 && ch == '\n') || (match == 3 && ch == '\n')) match++;
                else match = (ch == '\r') ? 1 : 0;
                if (match == 4) { hdr_done = 1; off = i + 1; break; }
            }
            if (!hdr_done) continue;
        }

        size_t take = (size_t)(r - off);
        const uint8_t *src = rb + off;
        while (take && drawn < rows) {
            size_t room = chunk_bytes - have;
            size_t n2   = take < room ? take : room;
            memcpy(acc + have, src, n2);
            have += n2; src += n2; take -= n2;

            if (have == chunk_bytes) {
                // Expand in place on the way out: each source row becomes
                // CAM_SCALE rows, each source pixel CAM_SCALE pixels. Done here
                // rather than asked of the camera because the wire is the
                // expensive part - doubling on this side is a memcpy the CPU
                // barely notices, while sending the big version costs four
                // times the radio time and the shell's whole frame budget.
                int srows = CAM_CHUNK_ROWS;
                if (drawn + srows * CAM_SCALE > rows)
                    srows = (rows - drawn) / CAM_SCALE;
                if (srows > 0) {
                    for (int sy = srows - 1; sy >= 0; sy--) {
                        const uint16_t *sp = s_chunk + (size_t)sy * (size_t)sw;
                        for (int k = CAM_SCALE - 1; k >= 0; k--) {
                            uint16_t *dp = s_out + (size_t)(sy * CAM_SCALE + k) * (size_t)dw;
                            for (int x = 0; x < sw; x++) {
                                uint16_t v = sp[x];
                                for (int j = 0; j < CAM_SCALE; j++)
                                    dp[x * CAM_SCALE + j] = v;
                            }
                        }
                    }
                    eos_bitmap_t b = {
                        .pixels = s_out, .w = (int16_t)dw,
                        .h = (int16_t)(srows * CAM_SCALE),
                        .stride = (int16_t)(dw * 2), .fmt = EOS_PIXFMT_RGB565,
                        .key = EOS_COLOR_NONE, .tint = EOS_COLOR_NONE,
                        .bg = EOS_COLOR_NONE,
                    };
                    eos_display_blit(dx, (int16_t)(dy + drawn), &b);
                    drawn += srows * CAM_SCALE;
                }
                have = 0;
            }
        }
    }

    // A partial tail: whole SOURCE rows only, so a half-received row is dropped
    // rather than drawn as noise.
    if (have >= row_bytes && drawn < rows) {
        int srows = (int)(have / row_bytes);
        if (drawn + srows * CAM_SCALE > rows) srows = (rows - drawn) / CAM_SCALE;
        if (srows > 0) {
            for (int sy = srows - 1; sy >= 0; sy--) {
                const uint16_t *sp = s_chunk + (size_t)sy * (size_t)sw;
                for (int k = CAM_SCALE - 1; k >= 0; k--) {
                    uint16_t *dp = s_out + (size_t)(sy * CAM_SCALE + k) * (size_t)dw;
                    for (int x = 0; x < sw; x++) {
                        uint16_t v = sp[x];
                        for (int j = 0; j < CAM_SCALE; j++) dp[x * CAM_SCALE + j] = v;
                    }
                }
            }
            eos_bitmap_t b = {
                .pixels = s_out, .w = (int16_t)dw,
                .h = (int16_t)(srows * CAM_SCALE),
                .stride = (int16_t)(dw * 2), .fmt = EOS_PIXFMT_RGB565,
                .key = EOS_COLOR_NONE, .tint = EOS_COLOR_NONE, .bg = EOS_COLOR_NONE,
            };
            eos_display_blit(dx, (int16_t)(dy + drawn), &b);
            drawn += srows * CAM_SCALE;
        }
    }

    close(fd);
    return drawn;
}

#else
// The host suites link the app table, so this function must EXIST off-target
// even though there are no sockets here. Returning 0 makes the app draw its
// "no frame" state, which is the truth on a machine with no camera node.
//
// The alternative - excluding the app from the host build - would mean the
// registry test no longer checks the row that ships, and that test exists
// precisely to catch a table whose entries do not match its enum.
static int cam_stream(const char *host, const char *path,
                      int sw, int dw, int rows, int16_t dx, int16_t dy,
                      const eos_app_ctx_t *c)
{
    (void)host; (void)path; (void)sw; (void)dw; (void)rows;
    (void)dx; (void)dy; (void)c;
    return 0;
}
#endif

// Told once per pass whether the window is on the glass. A camera that kept
// fetching behind a tab would spend the board's radio time on pixels nobody is
// looking at - the same reason the files app only scans while visible.
void eos_app_camera_tick(bool visible, uint32_t now_ms)
{
    s_visible = visible;
    if (!visible) { s_y = 0; return; }   // start at the top when it comes back
    if ((int32_t)(now_ms - s_next_ms) >= 0) {
        s_next_ms = now_ms + CAM_PERIOD_MS;
        s_dirty = true;
    }
}

bool eos_app_camera_take_dirty(void)
{
    bool d = s_dirty;
    s_dirty = false;
    return d;
}

void eos_app_draw_camera(const eos_app_ctx_t *c, eos_rect_t r)
{
    const eos_settings_t *s = eos_app_settings();
    const char *host = (s && s->cam_host[0]) ? s->cam_host : NULL;

    // A tile can be ANY size - the window manager splits until it decides a
    // region is too small, and on a 240x320 panel that happens quickly. Text
    // placed at a fixed offset spills out of a short tile, which the shell's
    // own suite catches as drawing outside the rect. So every line is checked
    // against the height it actually has.
    const int16_t lh = (int16_t)(c->tiny ? c->tiny->h + 2 : 10);

    if (!host) {
        eos_display_fill(r, c->bg);
        if (r.h >= lh)
            eos_app_text((int16_t)(r.x + 2), (int16_t)(r.y + 2),
                         c->tiny, c->muted, "no camera", (int16_t)(r.w - 4));
        if (r.h >= lh * 2)
            eos_app_text((int16_t)(r.x + 2), (int16_t)(r.y + 2 + lh),
                         c->tiny, c->muted, "set cam.host", (int16_t)(r.w - 4));
        return;
    }

    int w = r.w > CAM_MAX_W ? CAM_MAX_W : r.w;
    int h = r.h;
    if (w <= 0 || h <= 0) return;

    // Everything below is in SCREEN rows; the wire carries half of each.
    int rows = CAM_STRIP_ROWS;
    if (s_y + rows > h) rows = h - s_y;
    rows = (rows / CAM_SCALE) * CAM_SCALE;      /* whole source rows only */
    if (rows <= 0) { s_y = 0; return; }

    const int sw = w / CAM_SCALE;               /* what we ask for */
    const int sh = h / CAM_SCALE;
    const int sy = s_y / CAM_SCALE;
    const int srows = rows / CAM_SCALE;
    if (sw <= 0 || sh <= 0) return;

    // y == 0 is what makes the node capture a NEW frame; every later strip is
    // sliced out of the one it is holding, so all of them agree.
    char path[128];
    snprintf(path, sizeof path,
             "/api/cam/frame?w=%d&h=%d&rotate=90&y=%d&rows=%d", sw, sh, sy, srows);

    int drawn = cam_stream(host, path, sw, w, rows, r.x, (int16_t)(r.y + s_y), c);

    if (drawn > 0) {
        s_ok++;
        s_y += drawn;
        if (s_y >= h - (CAM_SCALE - 1)) s_y = 0;   // wrap: next picture
        s_note[0] = 0;
    } else {
        s_fail++;
        s_y = 0;
        snprintf(s_note, sizeof s_note, "no frame");
    }

    if (s_note[0] && r.h >= lh)
        eos_app_text((int16_t)(r.x + 2), (int16_t)(r.y + 2),
                     c->tiny, c->warn, s_note, (int16_t)(r.w - 4));
}
