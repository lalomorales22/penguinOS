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
#define CAM_STRIP_ROWS 40
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
#define CAM_CHUNK_ROWS 8
#ifdef ESP_PLATFORM
// Only on the device. Off-target there is no socket to fill it from, and a
// 3,840-byte buffer nothing reads is a -Werror=unused-variable away from
// breaking three host suites - which is exactly how this was found.
static uint16_t s_chunk[CAM_MAX_W * CAM_CHUNK_ROWS];
#endif

static int      s_y;             // which strip comes next
static uint32_t s_ok, s_fail;    // strips fetched, strips that failed
static char     s_note[64];      // what to say when there is nothing to show

#ifdef ESP_PLATFORM
// Fetch one strip and blit it AS IT ARRIVES, never holding more than
// CAM_CHUNK_ROWS rows. Returns the number of rows drawn.
//
// The shell calls this from its draw path, so the timeouts are short: a camera
// that has gone away must cost a stutter, not a hung desktop.
static int cam_stream(const char *host, const char *path,
                      int w, int rows, int16_t dx, int16_t dy,
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

    const size_t row_bytes   = (size_t)w * 2u;
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
                int cr = CAM_CHUNK_ROWS;
                if (drawn + cr > rows) cr = rows - drawn;
                eos_bitmap_t b = {
                    .pixels = s_chunk, .w = (int16_t)w, .h = (int16_t)cr,
                    .stride = (int16_t)row_bytes, .fmt = EOS_PIXFMT_RGB565,
                    .key = EOS_COLOR_NONE, .tint = EOS_COLOR_NONE, .bg = EOS_COLOR_NONE,
                };
                eos_display_blit(dx, (int16_t)(dy + drawn), &b);
                drawn += cr;
                have = 0;
            }
        }
    }

    // A partial tail: whole rows only, so a half-received row is dropped rather
    // than drawn as garbage.
    if (have >= row_bytes && drawn < rows) {
        int cr = (int)(have / row_bytes);
        if (drawn + cr > rows) cr = rows - drawn;
        eos_bitmap_t b = {
            .pixels = s_chunk, .w = (int16_t)w, .h = (int16_t)cr,
            .stride = (int16_t)row_bytes, .fmt = EOS_PIXFMT_RGB565,
            .key = EOS_COLOR_NONE, .tint = EOS_COLOR_NONE, .bg = EOS_COLOR_NONE,
        };
        eos_display_blit(dx, (int16_t)(dy + drawn), &b);
        drawn += cr;
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
                      int w, int rows, int16_t dx, int16_t dy,
                      const eos_app_ctx_t *c)
{
    (void)host; (void)path; (void)w; (void)rows; (void)dx; (void)dy; (void)c;
    return 0;
}
#endif

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

    int rows = CAM_STRIP_ROWS;
    if (s_y + rows > h) rows = h - s_y;
    if (rows <= 0) { s_y = 0; rows = CAM_STRIP_ROWS > h ? h : CAM_STRIP_ROWS; }

    // y == 0 is what makes the node capture a NEW frame; every other strip is
    // sliced out of the one it is holding.
    char path[128];
    snprintf(path, sizeof path,
             "/api/cam/frame?w=%d&h=%d&rotate=90&y=%d&rows=%d", w, h, s_y, rows);

    int drawn = cam_stream(host, path, w, rows, r.x, (int16_t)(r.y + s_y), c);

    if (drawn == rows) {
        s_ok++;
        s_y += rows;
        if (s_y >= h) s_y = 0;          // wrap: start the next picture
        s_note[0] = 0;
    } else {
        s_fail++;
        s_y = 0;                        // start clean rather than half a picture
        snprintf(s_note, sizeof s_note, "no frame (%d)", drawn);
    }

    if (s_note[0] && r.h >= lh)
        eos_app_text((int16_t)(r.x + 2), (int16_t)(r.y + 2),
                     c->tiny, c->warn, s_note, (int16_t)(r.w - 4));
}
