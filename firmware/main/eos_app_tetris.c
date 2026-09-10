// eos_app_tetris — the first game window.
//
// Two rules shape every line of this file, and both come from the OS rather
// than from Tetris.
//
// DRAW IS CALLED ONCE PER BAND. On this board that is forty times a frame, at
// eight rows each, and the tile rect handed in is clipped to the band. So draw
// here is a pure function of the state below and touches nothing else: no
// gravity, no line clears, no clock. Everything that MOVES lives in tick().
// Getting that backwards is what made the camera app slow, and a game that
// stepped its gravity in draw would run forty times too fast on this panel and
// eight times too fast on the 2.4in one.
//
// HELD STATE IS WHAT GAMES POLL. kernel/hal/README.md says so in the section
// written off the arcade build: "a ship moves while left is down, not once per
// keypress". So the soft drop reads eos_input_held(EOS_KEY_DOWN) from tick at
// the frame rate, while the moves that should fire once per press - shift,
// rotate, hard drop - are handled as discrete key events. Both idioms are here
// on purpose, because using either one for both jobs is wrong in a way that
// only shows up under a finger that is already down.

#include "eos_app_registry.h"
#include "eos_display.h"
#include "eos_input.h"
#include "eos_storage.h"
#include "eos_theme.h"

#include <stdio.h>
#include <stdlib.h>   /* strtoul, for the high score */
#include <string.h>

#define COLS 10
#define ROWS 20

// Where the high score lives. The card first, because it survives a reflash -
// the internal filesystem does too, but the card survives the BOARD, and this
// number is the one thing here worth carrying to another one.
#define HISCORE_SD  "/sd/tetris.txt"
#define HISCORE_INT "/int/tetris.txt"

// Each tetromino is four rotations of a 4x4 grid packed into a uint16_t, one
// bit per cell, row-major from the top-left. A table beats rotation arithmetic
// here: it is 56 constants against a transpose-and-mirror that has to special
// case the I and the O anyway, and the wall kick below only has to test the
// result rather than reason about how it was produced.
static const uint16_t PIECE[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },   // I
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 },   // J
    { 0x2E00, 0x4460, 0x0E80, 0xC440 },   // L
    { 0x6600, 0x6600, 0x6600, 0x6600 },   // O
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 },   // S
    { 0x4E00, 0x4640, 0x0E40, 0x4C40 },   // T
    { 0xC600, 0x2640, 0x0C60, 0x4C80 },   // Z
};

// Classic colours, put through the theme's cube rather than a palette role.
// A role would make the pieces change meaning with the theme - "accent" is the
// colour of a button, not of an S-piece - and the cube is what lets a tier 0
// indexed panel say cyan at all.
static const eos_rgb_t PIECE_RGB[7] = {
    { 0, 240, 240 }, {  0,   0, 240 }, { 240, 160,   0 }, { 240, 240,   0 },
    { 0, 240,   0 }, { 160,  0, 240 }, { 240,   0,   0 },
};

static struct {
    uint8_t  cell[ROWS][COLS];    // 0 empty, else piece index + 1
    uint8_t  piece, rot, next;
    int8_t   px, py;              // top-left of the 4x4 grid, may be negative
    uint32_t score, hiscore;
    uint16_t lines;
    uint8_t  level;
    bool     over, started, dirty, hi_loaded;
    uint32_t last_fall_ms, last_soft_ms;
    uint32_t seed;
} T;

// ----------------------------------------------------------------- helpers

static uint8_t rnd7(void)
{
    // xorshift, seeded from the clock the first time a piece is asked for.
    // Not uniform enough for anything that matters and entirely good enough to
    // stop the same bag arriving twice in a row.
    T.seed ^= T.seed << 13; T.seed ^= T.seed >> 17; T.seed ^= T.seed << 5;
    return (uint8_t)(T.seed % 7u);
}

static bool occupied(uint16_t bits, int i) { return (bits >> (15 - i)) & 1u; }

// Would the piece at (nx, ny, nrot) overlap the walls, the floor or a settled
// cell? Cells above the top are allowed: a piece spawns partly off-screen and
// must be able to rotate there.
static bool collides(uint8_t piece, uint8_t rot, int nx, int ny)
{
    uint16_t bits = PIECE[piece][rot & 3];
    int i;
    for (i = 0; i < 16; i++) {
        int cx, cy;
        if (!occupied(bits, i)) continue;
        cx = nx + (i & 3);
        cy = ny + (i >> 2);
        if (cx < 0 || cx >= COLS || cy >= ROWS) return true;
        if (cy >= 0 && T.cell[cy][cx]) return true;
    }
    return false;
}

static void spawn(void)
{
    T.piece = T.next;
    T.next  = rnd7();
    T.rot   = 0;
    T.px    = (COLS - 4) / 2;
    T.py    = -1;
    // A spawn that already collides is the end. Checked here rather than in
    // tick so that a hard drop into a full board ends on the same frame.
    if (collides(T.piece, T.rot, T.px, T.py)) T.over = true;
}

static void hiscore_load(void);
static void hiscore_save(void);

static void reset(uint32_t now_ms)
{
    memset(T.cell, 0, sizeof T.cell);
    T.score = 0; T.lines = 0; T.level = 1;
    T.over = false; T.started = true;
    T.seed = now_ms ? now_ms : 0x1234567u;
    T.next = rnd7();
    spawn();
    T.last_fall_ms = now_ms;
    T.dirty = true;
}

// Lock the piece into the board, clear any full rows, score them.
static void lock_piece(void)
{
    uint16_t bits = PIECE[T.piece][T.rot & 3];
    int i, r, c, cleared = 0;

    for (i = 0; i < 16; i++) {
        int cx = T.px + (i & 3), cy = T.py + (i >> 2);
        if (!occupied(bits, i)) continue;
        if (cy >= 0 && cy < ROWS && cx >= 0 && cx < COLS)
            T.cell[cy][cx] = (uint8_t)(T.piece + 1);
    }

    for (r = ROWS - 1; r >= 0; r--) {
        bool full = true;
        for (c = 0; c < COLS; c++) if (!T.cell[r][c]) { full = false; break; }
        if (!full) continue;
        // Pull everything above down one row, then look at this row again -
        // the row that just arrived might also be full.
        for (i = r; i > 0; i--) memcpy(T.cell[i], T.cell[i - 1], COLS);
        memset(T.cell[0], 0, COLS);
        cleared++;
        r++;
    }

    if (cleared) {
        // The classic 40/100/300/1200 table, scaled by level. Four at once is
        // worth more than four singles by a wide margin, which is the whole
        // reason anyone builds a well.
        static const uint16_t PAY[5] = { 0, 40, 100, 300, 1200 };
        T.score += (uint32_t)PAY[cleared] * T.level;
        T.lines = (uint16_t)(T.lines + cleared);
        T.level = (uint8_t)(1 + T.lines / 10);
        if (T.level > 15) T.level = 15;
    }
    spawn();
}

static uint32_t fall_interval(void)
{
    // 700 ms at level 1 down to 100 ms, and never below: the loop's fast rate
    // is 100 ms, so a shorter interval would ask for frames that do not exist
    // and the piece would fall in visible jumps instead of faster.
    uint32_t ms = 700u - (uint32_t)(T.level - 1) * 42u;
    return ms < 100u ? 100u : ms;
}

// ------------------------------------------------------------ persistence

static void hiscore_load(void)
{
    char buf[24];
    eos_file_t *f;
    int n;

    T.hi_loaded = true;
    f = eos_storage_open(HISCORE_SD, EOS_O_READ);
    if (!f) f = eos_storage_open(HISCORE_INT, EOS_O_READ);
    if (!f) return;
    n = eos_storage_read(f, buf, (int)sizeof buf - 1);
    eos_storage_close(f);
    if (n <= 0) return;
    buf[n] = 0;
    T.hiscore = (uint32_t)strtoul(buf, NULL, 10);
}

static void hiscore_save(void)
{
    char buf[24];
    eos_file_t *f;
    int n = snprintf(buf, sizeof buf, "%lu\n", (unsigned long)T.hiscore);
    if (n <= 0) return;

    // The card if it is there, the internal filesystem if it is not. Failing
    // to record a high score is not worth a message on the glass: the game is
    // still playable and the player is mid-round.
    f = eos_storage_open(HISCORE_SD, EOS_O_WRITE | EOS_O_CREATE | EOS_O_TRUNC);
    if (!f) f = eos_storage_open(HISCORE_INT, EOS_O_WRITE | EOS_O_CREATE | EOS_O_TRUNC);
    if (!f) return;
    (void)eos_storage_write(f, buf, n);
    eos_storage_close(f);
}

// ------------------------------------------------------------------- input
//
// Discrete presses only. The soft drop is NOT here - see the note at the top.

bool eos_app_tetris_key(const eos_event_t *e)
{
    if (!e) return false;
    if (e->type != EOS_EV_KEY_DOWN && e->type != EOS_EV_KEY_REPEAT) return false;

    if (T.over || !T.started) {
        // Enter and space both restart, because a player who has just lost is
        // hitting whichever one they were already using.
        if (e->key == EOS_KEY_ENTER || e->key == EOS_KEY_SPACE) {
            reset(e->ms);
            return true;
        }
        return false;
    }

    switch (e->key) {
    case EOS_KEY_LEFT:
        if (!collides(T.piece, T.rot, T.px - 1, T.py)) { T.px--; T.dirty = true; }
        return true;
    case EOS_KEY_RIGHT:
        if (!collides(T.piece, T.rot, T.px + 1, T.py)) { T.px++; T.dirty = true; }
        return true;
    case EOS_KEY_UP: {
        // Rotate, and if the wall is in the way try shifting one then two
        // cells off it before giving up. Without the kick an I-piece against
        // the right wall simply refuses to turn, which reads as a dropped
        // keypress rather than as a rule.
        uint8_t nr = (uint8_t)((T.rot + 1) & 3);
        static const int KICK[5] = { 0, -1, 1, -2, 2 };
        int k;
        for (k = 0; k < 5; k++) {
            if (!collides(T.piece, nr, T.px + KICK[k], T.py)) {
                T.px = (int8_t)(T.px + KICK[k]);
                T.rot = nr;
                T.dirty = true;
                break;
            }
        }
        return true;
    }
    case EOS_KEY_SPACE: {
        // Hard drop: fall until it would collide, then lock on the same frame.
        int guard = ROWS + 4;
        while (guard-- > 0 && !collides(T.piece, T.rot, T.px, T.py + 1)) T.py++;
        lock_piece();
        T.last_fall_ms = e->ms;
        T.dirty = true;
        return true;
    }
    default:
        return false;
    }
}

// -------------------------------------------------------------------- tick

void eos_app_tetris_tick(bool visible, uint32_t now_ms)
{
    uint32_t iv;

    if (!T.hi_loaded) hiscore_load();

    // Nothing moves for a window nobody is looking at. A game that kept
    // falling behind a tab would be lost by the time it came back, and the
    // player did not put it down mid-piece to lose it.
    if (!visible || !T.started || T.over) return;

    // The soft drop, polled from held state at the frame rate. Rate-limited on
    // its own clock so that holding down does not outrun the display.
    if (eos_input_held(EOS_KEY_DOWN)) {
        if ((uint32_t)(now_ms - T.last_soft_ms) >= 45u) {
            T.last_soft_ms = now_ms;
            if (!collides(T.piece, T.rot, T.px, T.py + 1)) {
                T.py++;
                T.score++;               // one point a cell, as it should be
                T.last_fall_ms = now_ms; // a soft drop resets gravity
                T.dirty = true;
            }
        }
    }

    iv = fall_interval();
    if ((uint32_t)(now_ms - T.last_fall_ms) < iv) return;
    T.last_fall_ms = now_ms;

    if (!collides(T.piece, T.rot, T.px, T.py + 1)) T.py++;
    else                                            lock_piece();
    T.dirty = true;

    if (T.over && T.score > T.hiscore) { T.hiscore = T.score; hiscore_save(); }
}

bool eos_app_tetris_take_dirty(void) { bool d = T.dirty; T.dirty = false; return d; }
bool eos_app_tetris_active(void)     { return T.started && !T.over; }

// Right aligned against `right`. The column sits against the edge of the tile,
// so every line in it has to be measured rather than placed.
static void right_text(int16_t right, int16_t y, const eos_font_t *f,
                       eos_color_t col, const char *str)
{
    int16_t w = (int16_t)((int)strlen(str) * (int)f->cell_w);
    eos_app_text((int16_t)(right - w), y, f, col, str, w);
}

// -------------------------------------------------------------------- draw

void eos_app_draw_tetris(const eos_app_ctx_t *c, eos_rect_t r)
{
    int16_t line_h, cell, bw, bh, bx, by, side;
    int rr, cc, i;
    char buf[24];

    if (!c->ui || eos_rect_empty(r)) return;
    line_h = (int16_t)(c->ui->h + 1);

    // HEIGHT FIRST. The well is twenty cells tall and that is what decides how
    // big a cell can be; the width only ever shrinks it further. Sizing it the
    // other way - reserving five columns for the numbers before measuring
    // anything - is what made a narrow tile draw a thin well AND a column too
    // narrow to read: the panel was paid for whether or not it could be used.
    cell = (int16_t)(r.h / ROWS);
    if (cell > (int16_t)(r.w / COLS)) cell = (int16_t)(r.w / COLS);
    if (cell < 2 || r.h < 4 * line_h) {
        eos_app_text(r.x, r.y, c->ui, c->accent, "tetris", r.w);
        if (r.h >= 2 * line_h)
            eos_app_text(r.x, (int16_t)(r.y + line_h), c->ui, c->muted,
                         "needs room", r.w);
        return;
    }

    bw = (int16_t)(cell * COLS);
    bh = (int16_t)(cell * ROWS);

    // The column gets what the well does not want, and only when that is
    // enough to read: six columns of the small face, which is "score" and a
    // four-digit number. Below that there is no column at all and the well
    // takes the tile and centres in it.
    side = (int16_t)(r.w - bw - cell);
    if (side < 6 * (int16_t)c->ui->cell_w) side = 0;

    bx = side ? r.x : (int16_t)(r.x + (r.w - bw) / 2);
    by = (int16_t)(r.y + (r.h - bh) / 2);

    eos_display_fill(eos_rect(bx, by, bw, bh), c->surface);

    // The settled cells.
    for (rr = 0; rr < ROWS; rr++)
        for (cc = 0; cc < COLS; cc++) {
            uint8_t v = T.cell[rr][cc];
            if (!v) continue;
            eos_display_fill(eos_rect((int16_t)(bx + cc * cell),
                                      (int16_t)(by + rr * cell),
                                      (int16_t)(cell - 1), (int16_t)(cell - 1)),
                             eos_theme_cube_index(PIECE_RGB[v - 1]));
        }

    // The piece in play, drawn from the same table draw never edits.
    if (T.started && !T.over) {
        uint16_t bits = PIECE[T.piece][T.rot & 3];
        for (i = 0; i < 16; i++) {
            int cx = T.px + (i & 3), cy = T.py + (i >> 2);
            if (!occupied(bits, i) || cy < 0) continue;
            eos_display_fill(eos_rect((int16_t)(bx + cx * cell),
                                      (int16_t)(by + cy * cell),
                                      (int16_t)(cell - 1), (int16_t)(cell - 1)),
                             eos_theme_cube_index(PIECE_RGB[T.piece]));
        }
    }

    eos_display_border(eos_rect(bx, by, bw, bh), 1, c->bunf);

    // The column, RIGHT ALIGNED against the edge of the tile. Left aligned it
    // floated in the middle of whatever space was left over and read as part
    // of the well rather than as a margin beside it.
    if (!side) return;
    {
        int16_t y     = by;
        int16_t right = (int16_t)(r.x + r.w);

        snprintf(buf, sizeof buf, "%lu", (unsigned long)T.score);
        right_text(right, y, c->ui, c->muted, "score"); y = (int16_t)(y + line_h);
        right_text(right, y, c->ui, c->text,  buf);     y = (int16_t)(y + line_h + 2);

        snprintf(buf, sizeof buf, "%lu", (unsigned long)T.hiscore);
        right_text(right, y, c->ui, c->muted, "best");  y = (int16_t)(y + line_h);
        right_text(right, y, c->ui, c->text,  buf);     y = (int16_t)(y + line_h + 2);

        snprintf(buf, sizeof buf, "%u/%u", (unsigned)T.lines, (unsigned)T.level);
        right_text(right, y, c->ui, c->muted, "lines"); y = (int16_t)(y + line_h);
        right_text(right, y, c->ui, c->text,  buf);     y = (int16_t)(y + line_h + 2);

        // The next piece, hung off the same right edge so its box lines up
        // with the numbers above it rather than starting where they start.
        if (side >= 4 * cell && y + 4 * cell < r.y + r.h) {
            uint16_t bits = PIECE[T.next][0];
            int16_t  nx   = (int16_t)(right - 4 * cell);
            right_text(right, y, c->ui, c->muted, "next");
            y = (int16_t)(y + line_h);
            for (i = 0; i < 16; i++) {
                if (!occupied(bits, i)) continue;
                eos_display_fill(eos_rect((int16_t)(nx + (i & 3) * cell),
                                          (int16_t)(y + (i >> 2) * cell),
                                          (int16_t)(cell - 1), (int16_t)(cell - 1)),
                                 eos_theme_cube_index(PIECE_RGB[T.next]));
            }
        }
    }

    if (!T.started)
        right_text((int16_t)(r.x + r.w), (int16_t)(by + bh - line_h),
                   c->ui, c->accent, "enter");
    else if (T.over)
        right_text((int16_t)(r.x + r.w), (int16_t)(by + bh - line_h),
                   c->ui, c->warn, "over");
}
