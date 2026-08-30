// test_integration — the checks that no single component can make about itself.
//
// Every component here has its own test suite and every one of them passes in
// isolation. That is not the same as the components agreeing with each other,
// and the failures that matter on a repo built in parallel are all of the
// second kind: two enums that were meant to be in lockstep drifting apart, a
// palette index claimed by two headers, a struct field that moved after the
// generator last read it. None of those show up in a component's own tests,
// because a component's own tests never include anybody else's header.
//
// So this file includes ALL of them, in one translation unit, and asserts the
// contracts that are written down in comments but enforced nowhere. If it does
// not compile, two components have collided. If it compiles and fails, two
// components disagree.
//
// The one non-obvious constraint: the board registry's C headers are generated
// build output and are not in the tree, so this file reaches them through
// __has_include and fails loudly when they are absent rather than skipping.
// Not covering that seam is what let a second eos_board_t live in the generated
// headers for as long as it did: every component compiled, every suite passed,
// and the one translation unit that needed both the HAL API and the board data
// — the boot glue — was the only thing that could not be written.
//
// Build (regenerate the board headers first):
//   python3 tools/gen_board_header.py --all
//   cc -std=c99 -Wall -Wextra -pedantic -O1 \
//      -Ikernel/wm/include -Ikernel/hal/include -Ikernel/theme/include \
//      -Ikernel/avatar/include -Ikernel/shell/include -Ikernel/svc/include \
//      -Iboards/generated \
//      kernel/test/test_integration.c kernel/wm/eos_wm.c kernel/theme/eos_theme.c \
//      kernel/shell/eos_bar.c kernel/avatar/eos_buddy.c kernel/avatar/eos_vox.c \
//      -lm -o t && ./t

#include <stdio.h>
#include <string.h>

// Include order is deliberately not dependency order. These headers must
// compose in any order and survive being included twice; a clashing include
// guard or a duplicated typedef fails the build right here.
#include "eos_brain.h"
#include "eos_bar.h"
#include "eos_keys.h"
#include "eos_buddy.h"
#include "eos_vox.h"
#include "eos_theme.h"
#include "eos_storage.h"
#include "eos_input.h"
#include "eos_display.h"
#include "eos_board.h"
#include "eos_wm.h"
#include "eos_wm.h"      /* twice, on purpose */
#include "eos_display.h" /* twice, on purpose */

// The registry, all six of it, in the same translation unit as the HAL that
// declares the type they fill. This is the include that used to be impossible.
// The bring-up board comes first, so it is the one that owns the plain EOS_*
// macros and the one the macro cross-check below reads.
#if defined(__has_include)
#  if __has_include("waveshare-c6-lcd-13.h")
#    define EOS_HAVE_BOARDS 1
#  endif
#endif

#ifdef EOS_HAVE_BOARDS
#include "waveshare-c6-lcd-13.h"
#include "cyd-2432s024n.h"
#include "waveshare-c5-lcd-147.h"
#include "wavvy-ili9488-35.h"
#include "wavvy-ili9488-40.h"
#include "wavvy-oled-c5.h"
#include "waveshare-c6-lcd-13.h"   /* twice, on purpose */
#endif

static int checks = 0, failed = 0;

static void ck(const char *what, int ok)
{
    checks++;
    if (!ok) { failed++; printf("  FAIL  %s\n", what); }
}

// ---------------------------------------------------------------------------
// The bar renders the buddy's mood by casting one enum to the other rather than
// through a switch it would forget to update. eos_bar.h says so and cannot
// check it, because it deliberately does not include eos_buddy.h.
static void moods(void)
{
    printf("bar mood <-> buddy state\n");
    ck("IDLE",      (int)EOS_MOOD_IDLE      == (int)EOS_BUDDY_IDLE);
    ck("THINKING",  (int)EOS_MOOD_THINKING  == (int)EOS_BUDDY_THINKING);
    ck("TALKING",   (int)EOS_MOOD_TALKING   == (int)EOS_BUDDY_TALKING);
    ck("LISTENING", (int)EOS_MOOD_LISTENING == (int)EOS_BUDDY_LISTENING);
    ck("SLEEPING",  (int)EOS_MOOD_SLEEPING  == (int)EOS_BUDDY_SLEEPING);
    ck("HAPPY",     (int)EOS_MOOD_HAPPY     == (int)EOS_BUDDY_HAPPY);
    ck("CONFUSED",  (int)EOS_MOOD_CONFUSED  == (int)EOS_BUDDY_CONFUSED);
    ck("no eighth buddy state the bar cannot name",
       (int)EOS_BUDDY_STATE_COUNT == (int)EOS_MOOD_CONFUSED + 1);

    // Every mood must render as something. A NULL here is a blank status bar.
    {
        int i;
        for (i = 0; i < (int)EOS_BUDDY_STATE_COUNT; i++) {
            const char *g = eos_bar_mood_glyph((eos_bar_mood_t)i);
            ck("mood glyph exists", g != NULL && g[0] != 0);
        }
    }
}

// ---------------------------------------------------------------------------
// eos_bar.h keeps EOS_WORKSPACE_PIPS as a literal so the bar model builds with
// no window manager present. That is fine right up until somebody changes
// EOS_WORKSPACES and the bar starts drawing pips for workspaces that cannot
// exist, or stops drawing one that can.
static void workspaces(void)
{
    printf("workspace pips <-> window manager\n");
    ck("EOS_WORKSPACE_PIPS == EOS_WORKSPACES", EOS_WORKSPACE_PIPS == EOS_WORKSPACES);
}

// ---------------------------------------------------------------------------
// The transparency sentinel and the theme palette both want an index, and there
// are only 256. eos_display.h reserves 255; eos_theme.h must not hand it out.
// A colour that resolves to 255 silently does not draw, which is invisible in
// the theme's own tests and very visible on a panel.
static void palette_sentinel(void)
{
    eos_theme_t t;
    eos_rgb_t white = { 255, 255, 255 };
    int r, g, b;
    long cube_hits = 0, search_hits = 0;

    printf("palette sentinel\n");
    eos_theme_default(&t);

    ck("EOS_COLOR_NONE is the theme's reserved slot",
       (int)EOS_COLOR_NONE == EOS_PAL_CUBE_NONE);
    ck("the cube does not hand out the reserved slot for white",
       eos_theme_cube_index(white) != EOS_COLOR_NONE);
    ck("the nearest-match search does not either",
       eos_theme_index(&t, white) != EOS_COLOR_NONE);
    ck("palette_len leaves exactly one slot out",
       EOS_PALETTE_MAX - 1 == EOS_PAL_SIZE - 1);

    for (r = 0; r < 256; r += 3)
        for (g = 0; g < 256; g += 3)
            for (b = 0; b < 256; b += 3) {
                eos_rgb_t c;
                c.r = (uint8_t)r; c.g = (uint8_t)g; c.b = (uint8_t)b;
                if (eos_theme_cube_index(c) == EOS_COLOR_NONE) cube_hits++;
                if (eos_theme_index(&t, c) == EOS_COLOR_NONE) search_hits++;
            }
    printf("      swept %d colours: cube hit the sentinel %ld times, search %ld\n",
           86 * 86 * 86, cube_hits, search_hits);
    ck("no colour anywhere resolves to the sentinel", cube_hits == 0 && search_hits == 0);

    // White still has to be IN the palette, just not reachable as a draw index.
    ck("pal565[255] is still white", eos_theme_palette565(&t)[255] == 0xFFFF);
}

// ---------------------------------------------------------------------------
// eos_theme.h says gap/bar_h/tab_h are "copied straight into eos_wm_cfg_t", and
// that min_tile_w/min_tile_h are deliberately absent because they belong to the
// panel. That leaves the shell assembling one struct from two sources, so the
// types have to line up and the result has to lay out.
static void theme_drives_wm(void)
{
    eos_theme_t th;
    eos_wm_cfg_t cfg;
    eos_wm_t wm;
    eos_rect_t screen = { 0, 0, 320, 240 };
    eos_tile_t tiles[EOS_MAX_WINDOWS];
    int n, i;

    printf("theme metrics -> eos_wm_cfg_t\n");
    eos_theme_default(&th);

    // From the board. 80x40 is what boards/*.json carries for the CYD.
    cfg.min_tile_w = 80;
    cfg.min_tile_h = 40;
    // From the theme, verbatim. If these are ever not assignable, this breaks.
    cfg.gap   = th.m.gap;
    cfg.bar_h = th.m.bar_h;
    cfg.tab_h = th.m.tab_h;

    ck("gap survived the copy",   cfg.gap   == th.m.gap);
    ck("bar_h survived the copy", cfg.bar_h == th.m.bar_h);
    ck("tab_h survived the copy", cfg.tab_h == th.m.tab_h);

    eos_wm_init(&wm, &cfg);
    for (i = 0; i < 5; i++)
        ck("window opened", eos_wm_open(&wm, (uint16_t)i, screen) != EOS_NONE);

    n = eos_wm_layout(&wm, screen, tiles, EOS_MAX_WINDOWS);
    ck("all five laid out", n == 5);
    for (i = 0; i < n; i++) {
        if (!tiles[i].visible) continue;
        ck("tile is on screen",
           tiles[i].rect.x >= 0 && tiles[i].rect.y >= 0 &&
           tiles[i].rect.x + tiles[i].rect.w <= screen.w &&
           tiles[i].rect.y + tiles[i].rect.h <= screen.h);
        ck("tile clears the theme's status bar", tiles[i].rect.y >= th.m.bar_h);
        ck("tile is not a sliver",
           tiles[i].rect.w >= cfg.min_tile_w && tiles[i].rect.h >= cfg.min_tile_h);
    }

    // The theme's own clamps must stay inside what a 16-bit layout can take.
    ck("gap is sane",   th.m.gap   >= 0 && th.m.gap   <= 32);
    ck("border is sane",th.m.border>= 0 && th.m.border<= 8);
    ck("bar_h is sane", th.m.bar_h >= 0 && th.m.bar_h <= 64);
    ck("tab_h is sane", th.m.tab_h >= 0 && th.m.tab_h <= 64);
}

// ---------------------------------------------------------------------------
// eos_bar.h maps its five roles onto eos_role_t "itself", in a comment. The bar
// cannot check that; the renderer that does the mapping does not exist yet. The
// least this can do is prove every role the bar emits has somewhere to land.
static void bar_roles(void)
{
    static const eos_role_t MAP[] = {
        EOS_ROLE_BAR_FG, EOS_ROLE_MUTED, EOS_ROLE_ACCENT,
        EOS_ROLE_OK,     EOS_ROLE_WARN
    };
    eos_theme_t t;
    int i;

    printf("bar roles -> theme roles\n");
    eos_theme_default(&t);
    ck("the map covers every bar role",
       (int)(sizeof MAP / sizeof MAP[0]) == (int)EOS_BAR_ROLE_WARN + 1);
    for (i = 0; i < (int)(sizeof MAP / sizeof MAP[0]); i++) {
        ck("bar role names a real theme role", MAP[i] < EOS_ROLE_COUNT);
        ck("that role sits in the role block of the palette",
           eos_theme_role_index(&t, MAP[i]) == (uint8_t)(EOS_PAL_ROLE_BASE + MAP[i]));
        ck("that role is not the sentinel",
           eos_theme_role_index(&t, MAP[i]) != EOS_COLOR_NONE);
    }
}

// ---------------------------------------------------------------------------
// eos_brain's events are documented as mapping one-for-one onto the buddy's.
// Nothing wires them together yet, so the most this can assert is that the
// buddy has a distinct destination for each brain event that claims one, and
// that driving the buddy through a whole request lifecycle actually moves it.
static void brain_drives_buddy(void)
{
    eos_buddy_t b;
    eos_buddy_cfg_t cfg;

    printf("brain lifecycle -> buddy moods\n");
    eos_buddy_default_cfg(&cfg);
    eos_buddy_init(&b, NULL, &cfg);

    eos_buddy_event(&b, EOS_BUDDY_EV_USER_TYPING);
    ck("typing -> LISTENING", eos_buddy_state(&b) == EOS_BUDDY_LISTENING);

    eos_buddy_event(&b, EOS_BUDDY_EV_REQUEST_SENT);      /* EV_SUBMITTED   */
    ck("submitted -> THINKING", eos_buddy_state(&b) == EOS_BUDDY_THINKING);

    eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_FIRST);      /* EV_FIRST_TOKEN */
    ck("first token -> TALKING", eos_buddy_state(&b) == EOS_BUDDY_TALKING);

    // The gap that lapses a reply back to IDLE must be longer than one frame,
    // or the buddy drops out of TALKING between chunks of a live stream.
    eos_buddy_tick(&b, 30);
    ck("a 30ms frame does not lapse the reply", eos_buddy_state(&b) == EOS_BUDDY_TALKING);

    eos_buddy_event(&b, EOS_BUDDY_EV_STREAM_DONE);       /* EV_DONE        */
    ck("done -> HAPPY", eos_buddy_state(&b) == EOS_BUDDY_HAPPY);

    eos_buddy_event(&b, EOS_BUDDY_EV_ERROR);             /* EV_FAILED      */
    ck("error -> CONFUSED", eos_buddy_state(&b) == EOS_BUDDY_CONFUSED);

    // The bar reads the same value the buddy is in.
    ck("mood cast is meaningful",
       (int)(eos_bar_mood_t)eos_buddy_state(&b) == (int)EOS_MOOD_CONFUSED);
}

// ---------------------------------------------------------------------------
// The board registry against the descriptors generated from it.
//
// boards/*.json is the source of truth and tools/gen_board_header.py is the one
// thing that reads it, so a mistake in the generator's field mapping — the
// wrong pin off the wrong sub-object, a rotation applied twice, a byte count
// that forgot the bytes — produces a header that compiles perfectly and drives
// the wrong GPIO. The table below is that JSON, read out once by hand. It
// disagrees with the descriptor exactly when the mapping has drifted.
//
// The waveshare-c6-lcd-13 row is the bring-up pinout: recovered from the GPIO
// matrix over JTAG and then confirmed by drawing to the panel. Those numbers
// are measurements, not datasheet values, and several published pinouts for
// that board contradict them. Anything that disagrees with this row is wrong.
#ifdef EOS_HAVE_BOARDS

typedef struct {
    const eos_board_t *b;
    const char *id;

    uint8_t     soc, cores, tier;
    uint32_t    flash, psram;

    uint8_t     panel, bus;
    int16_t     nw, nh;
    uint8_t     rot;
    int16_t     sw, sh;                  /* native, after rotation */
    uint8_t     depth, wire;
    bool        bgr, invert;
    uint32_t    hz;
    int16_t     colo, rowo;
    eos_pin_t   sck, mosi, miso, dc, cs, rst;
    uint8_t     spi_host;
    eos_pin_t   sda, scl;
    uint8_t     i2c_addr;
    bool        bl_low, bl_pwm;
    eos_pin_t   bl;

    uint8_t     comp;
    bool        lvgl;
    uint16_t    pal;
    bool        full;
    int16_t     band, mtw, mth;
    uint32_t    heap;

    bool        sd;
    uint8_t     sd_bus;
    const char *sd_point, *int_label, *int_point;
    uint32_t    sd_hz;
    bool        sd_shares;

    uint8_t     led;
    eos_pin_t   led_r, led_g, led_b, led_data;
    uint8_t     led_n;
    bool        led_low;

    uint8_t     audio;
    eos_pin_t   audio_pin, ldr;
    uint8_t     ldr_unit;
    int8_t      ldr_chan;

    uint8_t     touch, touch_bus;
    bool        ble, web;
    uint8_t     btns;
} regrow_t;

static const regrow_t registry[] = {
    { &eos_board_waveshare_c6_lcd_13, "waveshare-c6-lcd-13",
      EOS_SOC_ESP32_C6, 1, EOS_TIER_LEAN, 4194304u, 0u,
      EOS_PANEL_ST7789, EOS_BUS_SPI, 240, 240, 0, 240, 240,
      16, 2, false, true, 40000000u, 0, 0,
      7, 6, -1, 15, 14, 21, 1,
      -1, -1, 0x00, false, true,
      22,
      EOS_COMP_LVGL, true, 0, false, 40, 80, 40, 425648u,
      false, EOS_BUS_NONE, NULL, "int", "/int", 0u, false,
      EOS_LED_WS2812, -1, -1, -1, 8, 1, false,
      EOS_AUDIO_NONE, -1, -1, 0, -1,
      EOS_TOUCH_NONE, EOS_BUS_NONE, true, true, 0 },
    { &eos_board_cyd_2432s024n, "cyd-2432s024n",
      EOS_SOC_ESP32, 2, EOS_TIER_SOFT, 4194304u, 0u,
      EOS_PANEL_ILI9341, EOS_BUS_SPI, 240, 320, 1, 320, 240,
      16, 2, true, false, 40000000u, 0, 0,
      14, 13, 12, 2, 15, -1, 1,
      -1, -1, 0x00, false, true,
      27,
      EOS_COMP_INDEXED8, false, 256, true, 0, 80, 40, 98304u,
      true, EOS_BUS_SPI, "/sd", "int", "/int", 20000000u, false,
      EOS_LED_GPIO_RGB, 4, 16, 17, -1, 1, true,
      EOS_AUDIO_DAC, 26, 34, 1, 6,
      EOS_TOUCH_NONE, EOS_BUS_NONE, true, true, 1 },
    { &eos_board_waveshare_c5_lcd_147, "waveshare-c5-lcd-147",
      EOS_SOC_ESP32_C5, 1, EOS_TIER_LEAN, 4194304u, 0u,
      EOS_PANEL_ST7789, EOS_BUS_SPI, 172, 320, 1, 320, 172,
      16, 2, false, true, 40000000u, 34, 0,
      7, 6, -1, 24, 23, 26, 1,
      -1, -1, 0x00, false, true,
      10,
      EOS_COMP_LVGL, true, 0, false, 40, 80, 40, 131072u,
      true, EOS_BUS_SPI, "/sd", "int", "/int", 20000000u, true,
      EOS_LED_WS2812, -1, -1, -1, 8, 1, false,
      EOS_AUDIO_NONE, -1, -1, 0, -1,
      EOS_TOUCH_NONE, EOS_BUS_NONE, true, true, 0 },
    { &eos_board_wavvy_ili9488_35, "wavvy-ili9488-35",
      EOS_SOC_ESP32, 2, EOS_TIER_SOFT, 4194304u, 0u,
      EOS_PANEL_ILI9488, EOS_BUS_SPI, 320, 480, 0, 320, 480,
      18, 3, true, false, 40000000u, 0, 0,
      18, 23, -1, 2, 5, 4, 2,
      -1, -1, 0x00, false, false,
      -1,
      EOS_COMP_INDEXED8, false, 256, false, 16, 80, 40, 65536u,
      false, EOS_BUS_NONE, NULL, "int", "/int", 0u, false,
      EOS_LED_NONE, -1, -1, -1, -1, 0, false,
      EOS_AUDIO_NONE, -1, -1, 0, -1,
      EOS_TOUCH_NONE, EOS_BUS_NONE, true, true, 1 },
    { &eos_board_wavvy_ili9488_40, "wavvy-ili9488-40",
      EOS_SOC_ESP32, 2, EOS_TIER_SOFT, 4194304u, 0u,
      EOS_PANEL_ILI9488, EOS_BUS_SPI, 320, 480, 0, 320, 480,
      18, 3, true, false, 80000000u, 0, 0,
      18, 23, -1, 2, 5, 4, 2,
      -1, -1, 0x00, false, false,
      -1,
      EOS_COMP_INDEXED8, false, 256, false, 16, 80, 40, 65536u,
      false, EOS_BUS_NONE, NULL, "int", "/int", 0u, false,
      EOS_LED_NONE, -1, -1, -1, -1, 0, false,
      EOS_AUDIO_NONE, -1, -1, 0, -1,
      EOS_TOUCH_NONE, EOS_BUS_NONE, true, true, 1 },
    { &eos_board_wavvy_oled_c5, "wavvy-oled-c5",
      EOS_SOC_ESP32_C5, 1, EOS_TIER_SOFT, 4194304u, 0u,
      EOS_PANEL_SSD1306, EOS_BUS_I2C, 128, 64, 0, 128, 64,
      1, 0, false, false, 400000u, 0, 0,
      -1, -1, -1, -1, -1, -1, 0,
      23, 24, 0x3C, false, false,
      -1,
      EOS_COMP_MONO1, false, 0, true, 0, 60, 20, 49152u,
      false, EOS_BUS_NONE, NULL, "int", "/int", 0u, false,
      EOS_LED_NONE, -1, -1, -1, -1, 0, false,
      EOS_AUDIO_NONE, -1, -1, 0, -1,
      EOS_TOUCH_NONE, EOS_BUS_NONE, true, true, 0 },
};

#define REGISTRY_COUNT ((int)(sizeof registry / sizeof registry[0]))

static const char *ck_id = "";

static void ckb(const char *what, int ok)
{
    checks++;
    if (!ok) { failed++; printf("  FAIL  %s: %s\n", ck_id, what); }
}

static int streq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return a == b;
    return strcmp(a, b) == 0;
}

// Everything the JSON says, held against what the header emitted.
static void registry_row(const regrow_t *e)
{
    const eos_board_t *b = e->b;
    ck_id = e->id;

    ckb("id",           streq(b->id, e->id));
    ckb("name is set",  b->name != NULL && b->name[0] != 0);
    ckb("variant is set", b->variant != NULL && b->variant[0] != 0);
    ckb("soc",          b->soc == e->soc);
    ckb("cores",        b->cores == e->cores);
    ckb("tier",         b->tier == e->tier);
    ckb("flash_bytes",  b->flash_bytes == e->flash);
    ckb("psram_bytes",  b->psram_bytes == e->psram);

    ckb("panel",        b->panel.panel == e->panel);
    ckb("panel bus",    b->panel.bus == e->bus);
    ckb("native size",  b->panel.native_w == e->nw && b->panel.native_h == e->nh);
    ckb("rotation",     b->panel.rotation == e->rot);
    ckb("color_depth",  b->panel.color_depth == e->depth);
    ckb("wire_bytes",   b->panel.wire_bytes == e->wire);
    ckb("bgr",          b->panel.bgr == e->bgr);
    ckb("invert",       b->panel.invert == e->invert);
    ckb("bus clock",    b->panel.hz == e->hz);
    ckb("ram offsets",  b->panel.col_offset == e->colo && b->panel.row_offset == e->rowo);
    ckb("sck",          b->panel.sck == e->sck);
    ckb("mosi",         b->panel.mosi == e->mosi);
    ckb("miso",         b->panel.miso == e->miso);
    ckb("dc",           b->panel.dc == e->dc);
    ckb("cs",           b->panel.cs == e->cs);
    ckb("rst",          b->panel.rst == e->rst);
    ckb("spi_host",     b->panel.spi_host == e->spi_host);
    ckb("sda",          b->panel.sda == e->sda);
    ckb("scl",          b->panel.scl == e->scl);
    ckb("i2c_addr",     b->panel.i2c_addr == e->i2c_addr);
    ckb("backlight pin", b->panel.bl == e->bl);
    ckb("backlight polarity", b->panel.bl_active_low == e->bl_low);
    ckb("backlight pwm", b->panel.bl_pwm == e->bl_pwm);

    ckb("compositor",   b->render.compositor == e->comp);
    ckb("lvgl",         b->render.lvgl == e->lvgl);
    ckb("palette_entries", b->render.palette_entries == e->pal);
    ckb("full_framebuffer", b->render.full_framebuffer == e->full);
    ckb("band_h",       b->render.band_h == e->band);
    ckb("min tile",     b->render.min_tile_w == e->mtw && b->render.min_tile_h == e->mth);
    ckb("heap_budget",  b->render.heap_budget == e->heap);

    ckb("sd present",   b->storage.sd == e->sd);
    ckb("sd bus",       b->storage.sd_bus == e->sd_bus);
    ckb("sd clock",     b->storage.sd_hz == e->sd_hz);
    ckb("sd shares the panel bus", b->storage.sd_shares_bus == e->sd_shares);
    ckb("sd mount point", streq(b->storage.sd_point, e->sd_point));
    ckb("internal fs",  streq(b->storage.int_label, e->int_label)
                        && streq(b->storage.int_point, e->int_point));

    ckb("led kind",     b->extras.led == e->led);
    ckb("led pins",     b->extras.led_r == e->led_r && b->extras.led_g == e->led_g
                        && b->extras.led_b == e->led_b && b->extras.led_data == e->led_data);
    ckb("led count",    b->extras.led_count == e->led_n);
    ckb("led polarity", b->extras.led_active_low == e->led_low);
    ckb("audio",        b->extras.audio == e->audio && b->extras.audio_pin == e->audio_pin);
    ckb("ldr",          b->extras.ldr == e->ldr && b->extras.ldr_adc_unit == e->ldr_unit
                        && b->extras.ldr_adc_channel == e->ldr_chan);

    ckb("touch",        b->input.touch == e->touch && b->input.touch_bus == e->touch_bus);
    ckb("ble keyboard", b->input.ble_keyboard == e->ble);
    ckb("web input",    b->input.web_input == e->web);
    ckb("button count", b->input.button_count == e->btns);

    // Not in the registry: the two fields the generator fills with a documented
    // default. If either ever arrives from the JSON, this is where it shows up.
    {
        int i, zeroed = 1;
        for (i = 0; i < (int)b->input.button_count; i++)
            if (b->input.buttons[i].key != 0) zeroed = 0;
        ckb("button keymap is left for the board component", zeroed);
    }
    ckb("sd_slot defaults to 0", b->storage.sd_slot == 0);
    ckb("no board claims to identify itself", b->auto_detectable == false);
    ckb("first boot has a question to ask",
        b->confirm_prompt != NULL && b->confirm_prompt[0] != 0);
}

// Facts the descriptor has to satisfy whatever the JSON says. These are
// computed, not transcribed, so they catch a generator that is consistently
// wrong as well as one that is wrong in one place.
static void registry_invariants(const regrow_t *e)
{
    const eos_board_t *b = e->b;
    uint32_t w, h, rows, fb, band;
    ck_id = e->id;

    ckb("rotation lands the screen the right way round",
        eos_board_screen_w(b) == e->sw && eos_board_screen_h(b) == e->sh);
    {
        eos_rect_t r = eos_board_screen(b);
        ckb("the screen rect starts at the origin and is the screen",
            r.x == 0 && r.y == 0 && r.w == e->sw && r.h == e->sh);
    }

    w = (uint32_t)e->sw;
    h = (uint32_t)e->sh;
    fb = (e->comp == EOS_COMP_MONO1) ? ((w + 7u) / 8u) * h : w * h;
    rows = e->full ? h : (uint32_t)e->band;
    band = (e->comp == EOS_COMP_MONO1) ? ((w + 7u) / 8u) * rows : w * rows;
    ckb("fb_bytes is one screen of compositor pixels", eos_board_fb_bytes(b) == fb);
    ckb("band_bytes is one strip of them", eos_board_band_bytes(b) == band);
    ckb("the strip fits the heap budget", eos_board_band_bytes(b) <= b->render.heap_budget);

    ckb("band_h is set exactly when the frame is banded",
        b->render.full_framebuffer ? b->render.band_h == 0 : b->render.band_h > 0);
    ckb("a palette exists exactly for the indexed compositor",
        (b->render.compositor == EOS_COMP_INDEXED8) ? b->render.palette_entries == 256
                                                    : b->render.palette_entries == 0);
    ckb("lvgl and the compositor agree",
        b->render.lvgl == (b->render.compositor == EOS_COMP_LVGL));
    ckb("eos_board_has_lvgl reads the same flag", eos_board_has_lvgl(b) == b->render.lvgl);
    ckb("two min tiles fit across the screen",
        (int)b->render.min_tile_w * 2 <= eos_board_screen_w(b));
    ckb("two min tiles fit down the screen",
        (int)b->render.min_tile_h * 2 <= eos_board_screen_h(b));

    ckb("wire_bytes follows color_depth",
        b->panel.wire_bytes == (e->depth == 1 ? 0 : (e->depth == 16 ? 2 : 3)));
    ckb("eos_board_is_mono agrees with the depth",
        eos_board_is_mono(b) == (b->panel.color_depth == 1));
    ckb("eos_board_panel_16bit agrees with the depth",
        eos_board_panel_16bit(b) == (b->panel.color_depth == 16));
    ckb("mono panels use the mono compositor",
        eos_board_is_mono(b) == (b->render.compositor == EOS_COMP_MONO1));
    ckb("eos_board_has_touch agrees with the touch enum",
        eos_board_has_touch(b) == (b->input.touch != EOS_TOUCH_NONE));
    ckb("only a RICH board claims psram",
        (b->tier == EOS_TIER_RICH) == (b->psram_bytes != 0));

    if (b->panel.bus == EOS_BUS_SPI) {
        ckb("an spi panel has a clock and a data line",
            eos_pin_ok(b->panel.sck) && eos_pin_ok(b->panel.mosi));
        ckb("an spi panel names a host", b->panel.spi_host == 1 || b->panel.spi_host == 2);
        ckb("an spi panel has no i2c wiring",
            !eos_pin_ok(b->panel.sda) && !eos_pin_ok(b->panel.scl) && b->panel.i2c_addr == 0);
    } else {
        ckb("an i2c panel has both wires",
            eos_pin_ok(b->panel.sda) && eos_pin_ok(b->panel.scl));
        ckb("an i2c panel has an address", b->panel.i2c_addr != 0);
        ckb("an i2c panel has no spi wiring",
            !eos_pin_ok(b->panel.sck) && !eos_pin_ok(b->panel.mosi) && b->panel.spi_host == 0);
    }

    // Two panel signals on one pin is the failure that looks like a dead board.
    {
        eos_pin_t p[9];
        int n = 0, i, j, dup = 0;
        if (eos_pin_ok(b->panel.sck))  p[n++] = b->panel.sck;
        if (eos_pin_ok(b->panel.mosi)) p[n++] = b->panel.mosi;
        if (eos_pin_ok(b->panel.miso)) p[n++] = b->panel.miso;
        if (eos_pin_ok(b->panel.dc))   p[n++] = b->panel.dc;
        if (eos_pin_ok(b->panel.cs))   p[n++] = b->panel.cs;
        if (eos_pin_ok(b->panel.rst))  p[n++] = b->panel.rst;
        if (eos_pin_ok(b->panel.sda))  p[n++] = b->panel.sda;
        if (eos_pin_ok(b->panel.scl))  p[n++] = b->panel.scl;
        if (eos_pin_ok(b->panel.bl))   p[n++] = b->panel.bl;
        for (i = 0; i < n; i++)
            for (j = i + 1; j < n; j++)
                if (p[i] == p[j]) dup = 1;
        ckb("no two panel signals share a pin", !dup);
    }

    {
        int ok;
        switch (b->extras.led) {
        case EOS_LED_NONE:
            ok = !eos_pin_ok(b->extras.led_r) && !eos_pin_ok(b->extras.led_g)
                 && !eos_pin_ok(b->extras.led_b) && !eos_pin_ok(b->extras.led_data);
            break;
        case EOS_LED_GPIO_RGB:
            ok = eos_pin_ok(b->extras.led_r) && eos_pin_ok(b->extras.led_g)
                 && eos_pin_ok(b->extras.led_b) && !eos_pin_ok(b->extras.led_data);
            break;
        default:
            ok = eos_pin_ok(b->extras.led_data) && !eos_pin_ok(b->extras.led_r);
            break;
        }
        ckb("led pins match the led kind", ok);
    }

    ckb("a card has a mount point exactly when it exists",
        (b->storage.sd_point != NULL) == b->storage.sd);
    ckb("the internal filesystem is always named",
        b->storage.int_label != NULL && b->storage.int_point != NULL);

    // eos_board_check is the only thing standing between a wrong flash and a
    // confident boot on the wrong board, so it gets exercised per board.
    {
        eos_probe_t p;
        memset(&p, 0, sizeof p);
        p.soc = b->soc;
        p.flash_bytes = b->flash_bytes;
        p.psram_bytes = b->psram_bytes;
        ckb("check passes against matching silicon", eos_board_check(b, &p) == 0);
        p.soc = (uint8_t)(b->soc + 1u);
        ckb("check catches the wrong soc",
            (eos_board_check(b, &p) & EOS_MISMATCH_SOC) != 0);
        p.soc = b->soc;
        p.flash_bytes = b->flash_bytes / 2u;
        ckb("check catches the wrong flash size",
            (eos_board_check(b, &p) & EOS_MISMATCH_FLASH) != 0);
        p.flash_bytes = b->flash_bytes;
        p.psram_bytes = b->psram_bytes ? 0u : 8u * 1024u * 1024u;
        ckb("check catches psram appearing or vanishing",
            (eos_board_check(b, &p) & EOS_MISMATCH_PSRAM) != 0);
    }
}

// The macros and the initialiser are two separate emitters walking the same
// profile. They can only disagree if one of them read the wrong field, which is
// the bug class this whole file exists for. Only the first header included in a
// translation unit owns the unsuffixed names, so this covers that one board.
static void registry_macros(void)
{
    const eos_board_t *b = &EOS_BOARD;
    ck_id = EOS_BOARD_ACTIVE;

    ckb("the active board is the one that took the macros", streq(b->id, EOS_BOARD_ID));
    ckb("EOS_BOARD_ACTIVE names it", streq(EOS_BOARD_ACTIVE, EOS_BOARD_ID));
    ckb("the header says it was generated", EOS_BOARD_GENERATED == 1);
    ckb("name", streq(b->name, EOS_BOARD_NAME));
    ckb("variant", streq(b->variant, EOS_CHIP_VARIANT));
    ckb("cores", b->cores == EOS_CHIP_CORES);
    ckb("flash", b->flash_bytes == (uint32_t)EOS_FLASH_MB * 1024u * 1024u);
    ckb("psram", b->psram_bytes == (uint32_t)EOS_PSRAM_MB * 1024u * 1024u);
    ckb("psram flag", (b->psram_bytes != 0) == (EOS_HAS_PSRAM != 0));
    ckb("tier", b->tier == EOS_TIER);
    ckb("compositor", b->render.compositor == EOS_COMPOSITOR);
    ckb("lvgl", b->render.lvgl == (EOS_USE_LVGL != 0));
    ckb("palette", b->render.palette_entries == EOS_PALETTE_ENTRIES);
    ckb("full framebuffer", b->render.full_framebuffer == (EOS_FB_FULL != 0));
    ckb("band height", b->render.band_h == EOS_BAND_H);
    ckb("min tile", b->render.min_tile_w == EOS_MIN_TILE_W
                    && b->render.min_tile_h == EOS_MIN_TILE_H);
    ckb("double buffer", b->render.double_buffer == (EOS_DOUBLE_BUFFER != 0));
    ckb("animations", b->render.animations == (EOS_ANIMATIONS != 0));

    ckb("native size", b->panel.native_w == EOS_LCD_NATIVE_W
                       && b->panel.native_h == EOS_LCD_NATIVE_H);
    ckb("rotated size", eos_board_screen_w(b) == EOS_LCD_W
                        && eos_board_screen_h(b) == EOS_LCD_H);
    ckb("rotation", b->panel.rotation == EOS_LCD_ROTATION);
    ckb("depth", b->panel.color_depth == EOS_LCD_DEPTH);
    ckb("bytes per pixel", b->panel.wire_bytes == EOS_LCD_BPP);
    ckb("16-bit pixels", eos_board_panel_16bit(b) == (EOS_LCD_16BIT != 0));
    ckb("clock", b->panel.hz == EOS_LCD_CLOCK_HZ);
    ckb("offsets", b->panel.col_offset == EOS_LCD_COL_OFFSET
                   && b->panel.row_offset == EOS_LCD_ROW_OFFSET);
    ckb("invert", b->panel.invert == (EOS_LCD_INVERT != 0));
    ckb("sck", b->panel.sck == EOS_LCD_PIN_SCK);
    ckb("mosi", b->panel.mosi == EOS_LCD_PIN_MOSI);
    ckb("miso", b->panel.miso == EOS_LCD_PIN_MISO);
    ckb("dc", b->panel.dc == EOS_LCD_PIN_DC);
    ckb("cs", b->panel.cs == EOS_LCD_PIN_CS);
    ckb("rst", b->panel.rst == EOS_LCD_PIN_RST);
    ckb("backlight", b->panel.bl == EOS_LCD_PIN_BL
                     && b->panel.bl_active_low == (EOS_LCD_BL_ACTIVE_LOW != 0)
                     && b->panel.bl_pwm == (EOS_LCD_BL_PWM != 0));

    ckb("buttons", b->input.button_count == EOS_BTN_COUNT);
    ckb("touch", eos_board_has_touch(b) == (EOS_HAS_TOUCH != 0));
    ckb("ble keyboard", b->input.ble_keyboard == (EOS_HAS_BT_KEYBOARD != 0));
    ckb("card", b->storage.sd == (EOS_HAS_SD != 0));
#ifdef EOS_LED_PIN_DATA
    ckb("led data pin", b->extras.led_data == EOS_LED_PIN_DATA);
    ckb("led count", b->extras.led_count == EOS_LED_COUNT);
#endif
#ifdef EOS_FLASH_PORT_HINT
    ckb("port", streq(b->port, EOS_FLASH_PORT_HINT));
#endif
    ckb("upload baud", b->upload_baud == EOS_UPLOAD_BAUD);
    ckb("monitor baud", b->monitor_baud == EOS_MONITOR_BAUD);
    ckb("confirm prompt", streq(b->confirm_prompt, EOS_ID_CONFIRM_PROMPT));
    ckb("auto detectable", b->auto_detectable == (EOS_ID_AUTO_DETECTABLE != 0));

    // The derived byte counts, which live only as macros because the HAL
    // computes them from the panel instead of storing them.
    ckb("wire frame bytes",
        (uint32_t)EOS_WIRE_FRAME_BYTES ==
        (EOS_LCD_BPP ? (uint32_t)EOS_LCD_W * (uint32_t)EOS_LCD_H * (uint32_t)EOS_LCD_BPP
                     : (uint32_t)((EOS_LCD_W + 7) / 8) * (uint32_t)EOS_LCD_H));
    ckb("render ram is the framebuffer plus the staging buffer",
        (uint32_t)EOS_RENDER_RAM_BYTES ==
        (uint32_t)EOS_FB_BYTES + (uint32_t)EOS_BLIT_BYTES);
}

static void boards(void)
{
    int i;
    printf("board registry -> eos_board_t (%d boards)\n", REGISTRY_COUNT);
    for (i = 0; i < REGISTRY_COUNT; i++) registry_row(&registry[i]);
    for (i = 0; i < REGISTRY_COUNT; i++) registry_invariants(&registry[i]);
    registry_macros();
    ck_id = "registry";
    {
        int j, dup = 0;
        for (i = 0; i < REGISTRY_COUNT; i++)
            for (j = i + 1; j < REGISTRY_COUNT; j++)
                if (streq(registry[i].b->id, registry[j].b->id)) dup = 1;
        ckb("no two descriptors carry the same id", !dup);
    }
}

#else  /* !EOS_HAVE_BOARDS */

static void boards(void)
{
    printf("board registry -> eos_board_t\n");
    ck("boards/generated is present (run: python3 tools/gen_board_header.py --all)", 0);
}

#endif /* EOS_HAVE_BOARDS */

// ---------------------------------------------------------------------------
// Sizes that the tier-0 heap budget depends on. Not a correctness check so much
// as a tripwire: these are the numbers the RAM arithmetic in the docs uses.
static void footprints(void)
{
    printf("footprints\n");
    printf("      eos_wm_t %zu  eos_theme_t %zu  eos_brain_t %zu  eos_buddy_t %zu\n",
           sizeof(eos_wm_t), sizeof(eos_theme_t), sizeof(eos_brain_t), sizeof(eos_buddy_t));
    printf("      eos_board_t %zu  eos_event_t %zu  eos_rect_t %zu\n",
           sizeof(eos_board_t), sizeof(eos_event_t), sizeof(eos_rect_t));

    ck("eos_event_t is still 16 bytes", sizeof(eos_event_t) == 16);
    ck("eos_rect_t is four int16s", sizeof(eos_rect_t) == 8);
    ck("eos_theme_t fits a tier-0 budget", sizeof(eos_theme_t) <= 1024);
    // eos_rect_t comes from eos_wm.h and is used by the HAL. If the HAL ever
    // declares its own, this stops being one type and the WM stops agreeing
    // with the display about where anything is.
    {
        eos_rect_t a = eos_rect(1, 2, 3, 4);
        eos_tile_t t;
        memset(&t, 0, sizeof t);
        t.rect = a;   /* will not compile if these are two different types */
        ck("the HAL and the WM share one rect type",
           t.rect.x == 1 && t.rect.y == 2 && t.rect.w == 3 && t.rect.h == 4);
    }
}

int main(void)
{
    printf("== esp-os cross-component integration ==\n\n");
    moods();
    workspaces();
    palette_sentinel();
    theme_drives_wm();
    bar_roles();
    brain_drives_buddy();
    boards();
    footprints();
    printf("\n=== %d checks, %d failed ===\n", checks, failed);
    return failed ? 1 : 0;
}
