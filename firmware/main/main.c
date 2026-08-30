// app_main — the boot path. Board descriptor, panel, theme, window manager,
// four windows, and a frame loop that idles.
//
// Everything above this file is pure: eos_wm hands out rectangles, eos_bar
// hands out positioned text, eos_theme hands out palette indices, eos_font
// hands out glyph bits, and none of them knows the others exist. Nothing in
// the kernel joins them up, and nothing in the kernel is allowed to call IDF.
// This file is the join, and it is the only file in ESP-OS that does both.
//
// The one non-obvious constraint: the order below is load-bearing and is not
// alphabetical. The display must come up before the theme, because
// eos_display_init() seeds its colour LUT from the compiled-in default so that
// a board which fails to find any theme still draws in real colours; the theme
// must come up before eos_wm_init(), because gap, bar_h and tab_h are the
// theme's and min_tile_w/min_tile_h are the board's and eos_wm_cfg_t wants all
// five at once; and the windows must be opened before the first damage, because
// a banded backend fixes its bands when the frame opens and cannot be told
// about a window that appeared halfway down the screen.
//
// What is stubbed: input. eos_input.h declares a queue that nothing implements,
// so there is no BLE keyboard, no button and no touch, and the frame loop's
// call into the keybind dispatch always drains an empty queue. See
// eos_shell_input.h — the seam is one line from being real.

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "eos_board.h"
#include "eos_display.h"
#include "eos_font.h"
#include "eos_theme.h"
#include "eos_wm.h"
#include "eos_bar.h"
#include "eos_keys.h"

#include "eos_boot_theme.h"
#include "eos_shell_draw.h"
#include "eos_shell_input.h"

static const char *TAG = "eos";

// Below this the heap segment in the bar turns WARN. It is not a limit and
// nothing enforces it: the whole point of bringing the OS up on this board is
// that 400 KB free means a bug presents as a bug, so the threshold is set where
// a leak would be obvious rather than where the board would be in trouble.
#define HEAP_WARN_BYTES 65536u

// How long the loop sleeps when nothing is damaged. The clock in the focused
// tile ticks in seconds, so a quarter of that is enough to never miss one and
// still leave the CPU idle for 96% of the time.
#define IDLE_TICK_MS 250

// The shell state lives for the life of the image and is not small
// (eos_wm_t alone is about 900 bytes), so it is static rather than stacked on
// a task that also runs the compositor.
static eos_wm_t          wm;
static eos_theme_t       theme;
static eos_keymap_t      keys;
static eos_shell_state_t shell;
static eos_shell_input_t input;
static eos_bar_status_t  bar;

static char board_soc[24];
static char board_mem[24];
static char board_panel[32];
static char board_bus[32];

// ---------------------------------------------------------------- identity

// The wrong board header flashed onto the right silicon produces a panel that
// never leaves reset and no other symptom, so the three probeable facts are
// checked before anything is driven. This is the one diagnostic in the boot
// path that earns its flash.
static void log_identity(const eos_board_t *b)
{
    eos_probe_t probe;
    uint8_t bad;

    ESP_LOGI(TAG, "board  %s (%s), tier %s", b->id, b->name, eos_tier_name(b->tier));
    ESP_LOGI(TAG, "panel  %s %dx%d rot %u @ %" PRIu32 " Hz, invert %d, band %d rows",
             eos_panel_name(b->panel.panel),
             (int)eos_board_screen_w(b), (int)eos_board_screen_h(b),
             (unsigned)b->panel.rotation, b->panel.hz, (int)b->panel.invert,
             (int)b->render.band_h);
    ESP_LOGI(TAG, "pins   sck %d mosi %d cs %d dc %d rst %d bl %d",
             (int)b->panel.sck, (int)b->panel.mosi, (int)b->panel.cs,
             (int)b->panel.dc, (int)b->panel.rst, (int)b->panel.bl);

    if (eos_board_probe(&probe) != EOS_OK) {
        ESP_LOGE(TAG, "probe  failed");
        return;
    }
    bad = eos_board_check(b, &probe);
    if (bad == 0) {
        ESP_LOGI(TAG, "check  silicon agrees with the descriptor (%s, %" PRIu32 " KB flash)",
                 eos_soc_name(probe.soc), probe.flash_bytes / 1024u);
        return;
    }
    ESP_LOGE(TAG, "check  MISMATCH 0x%02x:%s%s%s - wrong board header flashed", bad,
             (bad & EOS_MISMATCH_SOC)   ? " soc"   : "",
             (bad & EOS_MISMATCH_FLASH) ? " flash" : "",
             (bad & EOS_MISMATCH_PSRAM) ? " psram" : "");
    ESP_LOGE(TAG, "check  rebuild with -DEOS_BOARD_ID=<the board you are holding>");
}

// ------------------------------------------------------------------ scene

// The four lines the "board" window shows. Built once: they are descriptor
// reads, and the renderer runs six times per frame.
static void board_lines(const eos_board_t *b, eos_shell_view_t *v)
{
    snprintf(board_soc, sizeof board_soc, "%s", eos_soc_name(b->soc));
    snprintf(board_mem, sizeof board_mem, "%" PRIu32 "MB flash",
             b->flash_bytes / (1024u * 1024u));
    snprintf(board_panel, sizeof board_panel, "%s %dx%d",
             eos_panel_name(b->panel.panel),
             (int)eos_board_screen_w(b), (int)eos_board_screen_h(b));
    snprintf(board_bus, sizeof board_bus, "spi%u %" PRIu32 "MHz",
             (unsigned)(b->panel.spi_host + 1u), b->panel.hz / 1000000u);

    v->board_line[0] = board_soc;
    v->board_line[1] = board_mem;
    v->board_line[2] = board_panel;
    v->board_line[3] = board_bus;
}

// Four windows on workspace 1 and one on workspace 2. The count and the order
// are chosen to produce the layout this board is meant to demonstrate: the
// third split cannot give both children min_tile_w (80 px in a 117 px tile),
// so it COLLAPSES INTO A TAB GROUP, which is the one rule that makes tiling
// usable on a 2.4 inch panel and the one thing a screenshot has to show.
static void open_windows(const eos_board_t *b)
{
    eos_rect_t screen = eos_board_screen(b);

    eos_wm_open(&wm, EOS_APP_CLOCK, screen);   // full width
    eos_wm_open(&wm, EOS_APP_BOARD, screen);   // splits it side by side
    eos_wm_open(&wm, EOS_APP_HEAP,  screen);   // stacks under "board"
    eos_wm_open(&wm, EOS_APP_KEYS,  screen);   // too narrow to split: tabs

    // Workspace 2 gets one window so the bar's pips have something to show
    // besides the workspace you are standing on.
    eos_wm_goto_workspace(&wm, 1);
    eos_wm_open(&wm, EOS_APP_CLOCK, screen);
    eos_wm_goto_workspace(&wm, 0);
}

// Everything in the bar that changes while the board sits there. The clock is
// UPTIME, not wall time: this image has no RTC, no NTP and no radios, and a
// counter that visibly advances is worth more during bring-up than a correct
// "--:--" would be. Say so here rather than letting someone read 00:07 as the
// time of day.
static void refresh_status(uint32_t now_ms)
{
    uint32_t minutes = now_ms / 60000u;

    bar.free_heap  = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    bar.heap_warn  = HEAP_WARN_BYTES;
    bar.hour       = (uint8_t)((minutes / 60u) % 24u);
    bar.minute     = (uint8_t)(minutes % 60u);
    bar.clock_valid = true;

    eos_shell_status_sync(&wm, &bar, eos_shell_app_names(), EOS_APP_COUNT);
}

// ------------------------------------------------------------------- boot

void app_main(void)
{
    const eos_board_t *b = eos_board_get();
    eos_shell_view_t view;
    const eos_display_info_t *info;
    eos_wm_cfg_t cfg;
    eos_theme_src_t src;
    eos_err_t err;
    uint32_t heap_before, heap_after, last_sec = 0;
    bool dirty;

    log_identity(b);

    heap_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);

    // 1. The panel. This is the only call in the whole image that takes memory
    //    and keeps it, and it takes it once.
    err = eos_display_init();
    if (err != EOS_OK) {
        ESP_LOGE(TAG, "display init failed: %s (%d)", eos_strerr(err), (int)err);
        return;    // A panel that will not start is not retried; see the backend.
    }
    heap_after = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    info = eos_display_info();
    ESP_LOGI(TAG, "display %dx%d, %d bands of %d rows, caps 0x%04x, palette %u",
             (int)info->w, (int)info->h, (int)info->max_bands, (int)info->band_h,
             (unsigned)info->caps, (unsigned)info->palette_len);
    ESP_LOGI(TAG, "heap   %" PRIu32 " -> %" PRIu32 " (display took %" PRIu32 " B), largest block %u",
             heap_before, heap_after, heap_before - heap_after,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // 2. The theme, and the palette it implies. A missing or corrupt file is a
    //    log line, never a stop - eos_theme.h guarantees the caller is left
    //    holding a usable theme whatever happened.
    src = eos_boot_theme_load(b, &theme);
    eos_boot_theme_upload(&theme);
    ESP_LOGI(TAG, "theme  \"%s\" from %s (gap %d, border %d, bar %d, tab %d, font %s)",
             theme.name, eos_boot_theme_src_name(src),
             (int)theme.m.gap, (int)theme.m.border, (int)theme.m.bar_h,
             (int)theme.m.tab_h, eos_theme_font(&theme));
    ESP_LOGI(TAG, "fonts  %" PRIu32 " B of flash, ui face \"%s\"",
             eos_font_flash_bytes(),
             eos_font_name(eos_font_id_from_name(eos_theme_font(&theme))));

    // 3. The window manager. The split across these five fields is the whole
    //    reason eos_theme_metrics_t does not carry min_tile_*: a minimum tile
    //    is a property of the PANEL - below it a tile is unreadable whatever
    //    the theme says - and gap, bar_h and tab_h are a property of the LOOK.
    //    They meet here and nowhere else.
    memset(&cfg, 0, sizeof cfg);
    cfg.min_tile_w = b->render.min_tile_w;
    cfg.min_tile_h = b->render.min_tile_h;
    cfg.gap        = theme.m.gap;
    cfg.bar_h      = theme.m.bar_h;
    cfg.tab_h      = theme.m.tab_h;
    eos_wm_init(&wm, &cfg);

    eos_keys_defaults(&keys);
    eos_shell_state_init(&shell, 1);
    eos_shell_input_init(&input, &wm, &shell, &keys,
                         eos_board_screen(b), theme.m.bar_h);

    eos_bar_status_init(&bar);
    bar.wifi     = EOS_WIFI_OFF;    // no radios in this image
    bar.brain_up = false;
    bar.mood     = EOS_MOOD_IDLE;

    open_windows(b);

    memset(&view, 0, sizeof view);
    view.theme = &theme;
    view.wm    = &wm;
    view.bar   = &bar;
    view.keys  = &keys;
    board_lines(b, &view);

    // 4. One full frame, then only what changed.
    refresh_status(0);
    view.uptime_ms    = 0;
    view.heap_free    = bar.free_heap;
    view.heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    eos_shell_damage_all();
    eos_shell_draw_frame(&view);
    ESP_LOGI(TAG, "first frame drawn, %d windows, free heap %" PRIu32,
             EOS_APP_COUNT + 1, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t sec = now / 1000u;

        refresh_status(now);
        view.uptime_ms    = now;
        view.heap_free    = bar.free_heap;
        view.heap_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        // The keybind dispatch. Empty today; see eos_shell_input.h.
        dirty = eos_shell_input_pump(&input);
        if (dirty) {
            // A move can change every tile and the pips at once, so the whole
            // screen is declared rather than guessed at rect by rect.
            eos_shell_damage_all();
        } else if (sec != last_sec) {
            // The two things that move on their own: the bar, and the clock
            // window. Declaring them separately is what keeps an idle board
            // pushing 27 KB a second instead of 115 KB.
            eos_shell_damage_bar(&view);
            eos_shell_damage_app(&view, EOS_APP_CLOCK);
            dirty = true;
        }

        if (dirty) {
            last_sec = sec;
            eos_shell_draw_frame(&view);
        }

        vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
    }
}
