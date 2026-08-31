// app_main — the boot path. Board descriptor, panel, theme, radios, and then
// either the provisioning screen or the tiled desktop.
//
// Everything above this file is pure: eos_wm hands out rectangles, eos_bar
// hands out positioned text, eos_theme hands out palette indices, eos_font
// hands out glyph bits, eos_net owns the credential state machine, eos_ble owns
// the keyboard and eos_httpd owns the API — and none of them knows the others
// exist. Nothing in the kernel joins them up, and nothing in the kernel is
// allowed to call IDF. This file is the join, and it is the only file in penguinOS
// that does both.
//
// The one non-obvious constraint: the order below is load-bearing and is not
// alphabetical.
//
//   display before theme   eos_display_init() seeds its colour LUT from the
//                          compiled-in default, so a board that finds no theme
//                          file still draws in real colours.
//   storage before theme   eos_boot_theme_load() reads /int/theme.json, and an
//                          unmounted /int means it silently never finds one.
//                          It comes after the panel and not before it because
//                          a first boot formats a blank 960 KB partition, and
//                          that is a stall the glass should be lit for.
//   theme before eos_wm    gap, bar_h and tab_h are the theme's, min_tile_w and
//                          min_tile_h are the board's, and eos_wm_cfg_t wants
//                          all five at once.
//   theme before buddy     the avatar's shade table maps model colours onto
//                          DISPLAY palette indices, so it can only be built
//                          once the theme's palette is the one loaded. That is
//                          also why a live theme switch rebuilds it.
//   BLE before WiFi        the NimBLE controller wants a large contiguous block
//                          and the WiFi stack fragments the heap. On the tighter
//                          boards this order is the difference between booting
//                          and an out-of-memory.
//   windows before damage  a banded backend fixes its bands when the frame opens
//                          and cannot be told about a window that appeared
//                          halfway down the screen. They are opened before the
//                          first frame even when SETUP is what gets drawn.
//   panel before network   eos_net_start() blocks for up to fifteen seconds on
//                          the boot join. Something has to be on the glass for
//                          that whole time or the board reads as dead, so the
//                          "joining" message is drawn first and the radio comes
//                          up underneath it.
//
// The second constraint is memory, and it is why every init step here logs the
// free heap and what it cost. WiFi, NimBLE, an HTTP server and a 38,400 byte
// DMA framebuffer all live in the same arena; when one of them does not fit,
// the log has to say which one rather than leaving it to be guessed at from a
// board that reboots.

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

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
#include "eos_input.h"
#include "eos_storage.h"
#include "eos_ble.h"
#include "eos_net.h"
#include "eos_httpd.h"
#include "eos_apps.h"

#include "eos_buddy.h"
#include "eos_stroll.h"
#include "eos_buddy_model.h"

#include "eos_boot_theme.h"
#include "eos_brain_bridge.h"
#include "eos_shell_draw.h"
#include "eos_shell_input.h"
#include "eos_app_registry.h"
#include "eos_led.h"
#include "eos_setup_screen.h"
#include "eos_settings.h"
#include "eos_settings_bind.h"
#include "eos_web_embed.h"
#include "eos_seed_buddy.h"

static const char *TAG = "eos";

// Below this the heap segment in the bar turns WARN. It is not a limit and
// nothing enforces it: with both radios up the board still has hundreds of
// kilobytes, so the threshold is set where a leak would be obvious rather than
// where the board would be in trouble.
#define HEAP_WARN_BYTES 65536u

// How long the loop sleeps when nothing is damaged. The clock in the focused
// tile ticks in seconds, so a quarter of that is enough to never miss one and
// still leave the CPU idle for 96% of the time.
#define IDLE_TICK_MS 250

// And how long it sleeps while the avatar's tile is visible. The buddy bobs,
// blinks and turns; 4 fps reads as a stutter and 10 fps does not. It buys that
// with about 20 KB of SPI per frame — the tile's own rect, not a full-width
// band — and nothing else on this board animates, so the rate goes back to
// IDLE_TICK_MS the moment the buddy is behind a tab.
#define BUDDY_TICK_MS 100

// And how long it sleeps while the trackpad's cursor is awake. 33 ms is 30 fps
// and is what a cursor has to hit before it stops feeling attached to the
// finger pushing it. It buys that with two 7x11 damage rects - under 400 bytes
// of SPI a frame, less than the buddy costs at a third of the rate - and it
// lasts only as long as EOS_POINTER_IDLE_MS after the last report.
#define POINTER_TICK_MS 33

// The shell state lives for the life of the image and is not small
// (eos_wm_t alone is about 900 bytes), so it is static rather than stacked on
// a task that also runs the compositor.
static eos_wm_t          wm;
static eos_theme_t       theme;
static eos_keymap_t      keys;
static eos_shell_state_t shell;
static eos_shell_input_t input;
static eos_launcher_t    launcher;
static eos_bar_status_t  bar;

// The services. Both are meant for BSS and say so in their own headers:
// eos_net_t is about 1.2 KB and eos_httpd_t about 4.7 KB, and neither
// allocates outside its own start call.
static eos_net_t   net;
static eos_httpd_t httpd;
static bool        httpd_up;

// The persisted settings. About 440 bytes of values plus its ports; it holds no
// buffer of its own, so the document it is written from is eos_settings.c's
// single BSS scratch and not this file's.
static eos_settings_store_t settings;

// The avatar. It drives two things: the bar's mood glyph, which is
// eos_buddy_state_t by another name, and the EOS_APP_BUDDY window, which is
// what the whole project is named for.
//
// The model is whichever one the gallery resolves to, and eos_apps is the only
// thing that decides: the slug in /int/buddy/active, else the first entry in
// the gallery, else the pre-gallery /int/buddy/buddy.vox that a board which has
// not been updated yet still carries. With none of those — a board nobody has
// given a buddy — the compiled-in shape in eos_buddy_model.c is adopted, so the
// tile is never empty and the Buddy tab's "there is no model yet, build one" is
// an offer rather than an apology. This file never opens a model path itself;
// buddy_adopt() takes whatever eos_apps_buddy_model() is holding.
static eos_buddy_t buddy;

// And what he does with himself between questions. The mood machine says how
// he feels; this says where he is standing, which foot he is on and whether
// he is about to hop. It is a separate object on purpose: a mood arrives from
// the megabrain and a stroll does not, and folding the second into the first
// would put a walk cycle inside the thing the HTTP client drives.
//
// buddy.json's idle.behaviour chooses the preset, by name — "still" spends no
// pixels and draws exactly the avatar this board drew before it could walk.
static eos_stroll_t stroll;
static char        brain_model[EOS_BRAIN_MODEL_MAX];

// The files, console, buddy and apps endpoints. eos_apps_t does not exist:
// that component owns one upload handle, one log ring and one buddy image-wide,
// because eos_httpd serialises dispatch and a second of any of them could not
// be in flight anyway. What is here is only what the boot glue has to hold.
//
// buddy_gen is how a model uploaded through /api/fs/write reaches the avatar
// without a reboot. The reload happens on an HTTP worker; the state machine is
// ticked on this task. Re-adopting the model HERE, when the generation moves,
// is what stops a swap landing inside a half-drawn frame.
static uint32_t buddy_gen;

// The console's `reboot`. Armed from an HTTP worker, performed from the loop:
// esp_restart() inside a handler drops the 202 the person is waiting on, and
// they are left unable to tell a reboot from a crash.
static volatile bool     reboot_armed;
static volatile uint32_t reboot_at_ms;

static char board_soc[24];
static char board_mem[24];
static char board_panel[32];
static char board_bus[32];

// ---------------------------------------------------------------- the heap
//
// Every init step below reports what it cost. This is the whole answer to
// "does WiFi plus NimBLE plus an HTTP server plus a framebuffer fit", and it is
// printed rather than reasoned about because the first flash is the only place
// the real numbers exist.

static uint32_t heap_mark;

static uint32_t heap_free(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

static uint32_t heap_largest(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

static void heap_step(const char *what)
{
    uint32_t now = heap_free();
    int32_t  cost = (int32_t)heap_mark - (int32_t)now;

    ESP_LOGI(TAG, "heap   after %-9s %7" PRIu32 " free, %7" PRIu32 " largest, this step took %" PRId32,
             what, now, heap_largest(), cost);
    heap_mark = now;
}

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

// The four lines the "board" window shows.
//
// Once the board is on a network these become the ONLY place it says where to
// browse to, so the address outranks the chip name and takes line 0, which is
// the one drawn in the 8x13 face. A board that has joined a network and cannot
// tell you its address is a board you have to go and find with an arp scan,
// which is what happened the first time this ran.
//
// Rebuilt every frame rather than once: the address is not known at boot and
// changes when the network does. It is four snprintfs against a descriptor.
static void board_lines(const eos_board_t *b, eos_shell_view_t *v,
                        const eos_net_t *net)
{
    if (net && eos_net_mode(net) == EOS_NET_STA) {
        char ip[16];
        eos_net_ip_str(eos_net_ip(net), ip, sizeof ip);
        snprintf(board_soc,   sizeof board_soc,   "%s.local", eos_net_hostname(net));
        snprintf(board_mem,   sizeof board_mem,   "%s", ip);
        snprintf(board_panel, sizeof board_panel, "%s", eos_net_ssid(net));
        snprintf(board_bus,   sizeof board_bus,   "%s %ddBm",
                 eos_soc_name(b->soc), (int)eos_net_rssi(net));
        v->board_line[0] = board_soc;
        v->board_line[1] = board_mem;
        v->board_line[2] = board_panel;
        v->board_line[3] = board_bus;
        return;
    }
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

// Five windows on workspace 1 and one on workspace 2. The count and the order
// are chosen to produce the layout this board is meant to demonstrate: the
// third split cannot give both children min_tile_w (80 px in a 117 px tile),
// so it COLLAPSES INTO A TAB GROUP, which is the one rule that makes tiling
// usable on a 2.4 inch panel and the one thing a screenshot has to show.
//
// The buddy is opened LAST, and that is deliberate on both counts. Last means
// it lands in that tab group as the visible tab, so the avatar is on the glass
// from the first frame without anybody pressing anything; and it means the
// four windows that were verified on hardware keep the rects they had, because
// a fifth window here only lengthens the tab strip.
//
// win_of[] is what sys.autostart focuses. It is filled here rather than
// re-derived from a layout later, because eos_wm_focus_win() takes the window
// handle eos_wm_open() returned and there is no other way back to it.
static int win_of[EOS_APP_COUNT];

static void open_windows(const eos_board_t *b)
{
    eos_rect_t screen = eos_board_screen(b);
    int i;

    for (i = 0; i < EOS_APP_COUNT; i++) win_of[i] = -1;

    win_of[EOS_APP_CLOCK] = eos_wm_open(&wm, EOS_APP_CLOCK, screen); // full width
    win_of[EOS_APP_BOARD] = eos_wm_open(&wm, EOS_APP_BOARD, screen); // splits it side by side
    win_of[EOS_APP_HEAP]  = eos_wm_open(&wm, EOS_APP_HEAP,  screen); // stacks under "board"
    win_of[EOS_APP_KEYS]  = eos_wm_open(&wm, EOS_APP_KEYS,  screen); // too narrow to split: tabs
    win_of[EOS_APP_BUDDY] = eos_wm_open(&wm, EOS_APP_BUDDY, screen); // and the face of it

    // Workspace 2 gets one window so the bar's pips have something to show
    // besides the workspace you are standing on.
    eos_wm_goto_workspace(&wm, 1);
    eos_wm_open(&wm, EOS_APP_CLOCK, screen);
    eos_wm_goto_workspace(&wm, 0);
}

// sys.autostart. web/README.md calls it "an id from /api/apps, or empty", and
// on a board whose windows are all open from boot the honest meaning of that
// is which one you are looking at when the desktop appears — not launching a
// process, because nothing here has one to launch.
//
// An id that names no window is left alone rather than reported: the setting is
// marked (reboot) in the contract, so the page has already been told the value
// took, and a boot that refused it would leave the store and the board
// disagreeing with nothing on screen to say so. It goes in the log instead.
static void apply_autostart(const char *id)
{
    int i;

    if (!id || !id[0]) return;

    // Resolved through eos_app_index_of(), which matches the registry's `id`
    // column — the same column /api/apps publishes and the same one
    // web/README.md tells the page to send back. It used to be matched against
    // eos_shell_app_names(), which is the `name` column: the TAB LABEL, which
    // a row is free to spell differently and which is not what anybody was
    // ever offered to choose from. They agree for every row in the table
    // today, so this fixed nothing visible and closed the one seam left where
    // the app list existed twice.
    i = eos_app_index_of(id);
    if (i >= 0 && i < EOS_APP_COUNT && win_of[i] >= 0) {
        eos_wm_focus_win(&wm, win_of[i]);
        ESP_LOGI(TAG, "start  autostart focuses \"%s\" (window %d)", id, win_of[i]);
        return;
    }
    ESP_LOGW(TAG, "start  sys.autostart \"%s\" names no window on this board", id);
}

// Everything in the bar that changes while the board sits there. The clock is
// UPTIME, not wall time: this image has no RTC and no NTP, and a counter that
// visibly advances is worth more during bring-up than a correct "--:--" would
// be. Say so here rather than letting someone read 00:07 as the time of day.
static void refresh_status(uint32_t now_ms)
{
    uint32_t minutes = now_ms / 60000u;

    bar.free_heap  = heap_free();
    bar.heap_warn  = HEAP_WARN_BYTES;
    bar.hour       = (uint8_t)((minutes / 60u) % 24u);
    bar.minute     = (uint8_t)(minutes % 60u);
    bar.clock_valid = true;
    bar.wifi       = (uint8_t)eos_net_bar_wifi(&net);

    // The two segments that were hardcoded false and IDLE until megabrain was
    // wired up. brain_model is a copy rather than a borrow: the bar holds the
    // pointer for the length of a build and the brain task can rewrite its
    // live config between two frames.
    bar.brain_up    = eos_brain_bridge_reachable();
    bar.brain_model = brain_model[0] ? brain_model : NULL;
    bar.mood        = (eos_bar_mood_t)eos_buddy_state(&buddy);

    eos_shell_status_sync(&wm, &bar, eos_shell_app_names(), EOS_APP_COUNT);
}

// -------------------------------------------------------------- the buddy

// Picks the model the avatar wears and rebuilds everything downstream of it.
//
// Called at boot, on every /api/buddy/reload, and after a live theme switch.
// The theme is in that list because the shade table maps model colours onto
// DISPLAY palette indices, and a theme switch reprograms the display palette
// under it: without this the buddy would keep wearing the old theme's nearest
// colours until something uploaded a new model.
//
// The state machine survives: eos_buddy_init() resets it, so the state is read
// out first and put back. A model swap is a change of clothes, not amnesia —
// re-adopting mid-reply must not drop the buddy out of THINKING and leave the
// glass claiming the mini went quiet.
static void buddy_adopt(void)
{
    eos_vox_model_t     *m   = eos_apps_buddy_model();
    const eos_vox_pal_t *pal = eos_apps_buddy_palette();
    const eos_buddy_cfg_t *from = eos_apps_buddy_cfg();
    eos_buddy_state_t was = eos_buddy_state(&buddy);
    eos_stroll_preset_t preset;
    eos_buddy_cfg_t cfg;

    if (m && m->count) {
        // buddy.json's fields, whichever of them it carried.
        if (from) cfg = *from;
        else      eos_buddy_default_cfg(&cfg);
    } else {
        m   = eos_buddy_model_default();
        pal = eos_buddy_model_default_palette();
        eos_buddy_model_default_cfg(&cfg);
    }
    if (!pal) pal = m->pal;

    // How much of the tile he gives up so that there is floor under him. It
    // has to be set BEFORE eos_buddy_init(), because the scale and the stage
    // are the same division of the box and only one of them can go first.
    preset = eos_stroll_preset_from_name(eos_apps_idle_name(eos_apps_buddy_behaviour()));
    cfg.roam_q8 = eos_stroll_roam_q8(preset);

    eos_shell_buddy_shade(pal, &cfg);
    eos_buddy_init(&buddy, m, &cfg);
    eos_buddy_set_state(&buddy, was);
    // Seeded from the model, not from a clock: two boards wearing the same
    // buddy should not be doing the same hop at the same instant, and two
    // reloads of the same model on one board should not replay one either.
    eos_stroll_init(&stroll, &buddy, preset,
                    0x5EED1E55u ^ (eos_apps_buddy_generation() * 2654435761u)
                                ^ ((uint32_t)m->count << 7));
}

// ------------------------------------------------------------ megabrain

// The avatar, driven from the request lifecycle, and the two facts the bar
// reads. It runs on EVERY pass of the loop and not only on the desktop: the
// buddy's TALKING state lapses on a 900 ms timer and THINKING has no timer at
// all, so a machine ticked only while its glyph happens to be visible would be
// right by accident. Returns true when the mood changed, which is a redraw the
// one-second bar refresh would otherwise be up to a second late for.
static uint32_t brain_last_tick;

static bool brain_tick(uint32_t now)
{
    uint8_t before = (uint8_t)eos_buddy_state(&buddy);
    int ev;

    // Drained in a loop, not one per frame. A fast model produces hundreds of
    // chunks between two frames; the queue coalesces those but keeps every
    // state change, and a caller that read one at a time would fall a whole
    // reply behind.
    while ((ev = eos_brain_bridge_next_event()) >= 0)
        eos_buddy_event(&buddy, (eos_buddy_event_t)ev);

    if (brain_last_tick && now > brain_last_tick) {
        uint32_t dt = now - brain_last_tick;
        eos_buddy_tick(&buddy, dt);
        // After the mood, never before it: the stroll reads the state the
        // mood machine has just settled on, and a walk driven off last
        // frame's mood is a lean applied to the wrong animation on exactly
        // the frame a mood changes.
        eos_stroll_tick(&stroll, dt);
    }
    brain_last_tick = now;

    // No health probe until there is a network to probe over. In SETUP every
    // candidate would time out in turn and the only thing that would buy is
    // eleven seconds of a task walking a list nothing is on.
    eos_brain_bridge_set_online(eos_net_mode(&net) == EOS_NET_STA);
    eos_brain_bridge_model(brain_model, sizeof brain_model);

    return (uint8_t)eos_buddy_state(&buddy) != before;
}

// -------------------------------------------------------------- eos_apps

// The console's seven read-only commands, answered from the things only this
// file can reach. One function rather than seven ports: the whole point of a
// closed table is that adding a topic is adding a line here, in the file that
// already knows what a heap and a radio are.
//
// A topic this board cannot answer returns negative and the console says so.
// That is not an error: an image built without megabrain still runs `brain`.
static int apps_describe(void *ctx, const char *topic, char *out, int cap)
{
    const eos_board_t *b = eos_board_get();

    (void)ctx;
    if (strcmp(topic, "board") == 0)
        return snprintf(out, (size_t)cap, "board %s, %s, %" PRIu32 "MB flash, tier %s",
                        b->id, eos_soc_name(b->soc),
                        b->flash_bytes / (1024u * 1024u), eos_tier_name(b->tier));

    if (strcmp(topic, "heap") == 0)
        return snprintf(out, (size_t)cap, "heap %" PRIu32 " free, %" PRIu32 " largest block",
                        heap_free(), heap_largest());

    if (strcmp(topic, "wifi") == 0) {
        char ip[16];
        if (eos_net_mode(&net) != EOS_NET_STA)
            return snprintf(out, (size_t)cap, "wifi %s, ap \"%s\"",
                            eos_net_mode_name(eos_net_mode(&net)), eos_net_ap_ssid(&net));
        eos_net_ip_str(eos_net_ip(&net), ip, sizeof ip);
        return snprintf(out, (size_t)cap, "wifi \"%s\" %ddBm, %s, %s.local",
                        eos_net_ssid(&net), (int)eos_net_rssi(&net), ip,
                        eos_net_hostname(&net));
    }

    if (strcmp(topic, "theme") == 0)
        return snprintf(out, (size_t)cap, "theme \"%s\", font %s, bright %u",
                        theme.name, eos_theme_font(&theme),
                        (unsigned)settings.v.ui_bright);

    if (strcmp(topic, "brain") == 0)
        return snprintf(out, (size_t)cap, "brain %s, model %s",
                        eos_brain_bridge_reachable() ? "reachable" : "not reachable",
                        brain_model[0] ? brain_model : "none");

    return -1;
}

static void apps_reboot(void *ctx, uint32_t in_ms)
{
    (void)ctx;
    reboot_at_ms = (uint32_t)(esp_timer_get_time() / 1000) + in_ms;
    reboot_armed = true;
}

// What /api/apps reports. It is a restatement of the app registry in
// eos_httpd's vocabulary and it invents nothing: the id, the name, the summary
// and the tier all come out of the same table the tab labels, the launcher and
// sys.autostart read. That is the whole point of the table — the summaries
// used to live here, in a second array that had to be kept in the same order
// as an enum in another file, and adding a window meant remembering to.
//
// The strings are borrowed, not copied, which eos_apps.h asks for: they are
// string literals in eos_app_registry.c and they outlive the image.
static eos_apps_app_t app_catalog[EOS_APP_COUNT];

static void build_app_catalog(const eos_board_t *b)
{
    int i, n = eos_app_count();

    if (n > EOS_APP_COUNT) n = EOS_APP_COUNT;
    for (i = 0; i < n; i++) {
        const eos_app_t *a = eos_app_at(i);
        if (!a) continue;
        app_catalog[i].id       = a->id;
        app_catalog[i].name     = a->name;
        app_catalog[i].summary  = a->summary;
        app_catalog[i].tier_min = a->tier_min;
    }
    (void)b;
    eos_apps_set_apps(app_catalog, n);
}

// ------------------------------------------------------- pairing passkey

// Fires from the NimBLE host task, so it does no drawing and takes no locks:
// it logs, and leaves the panel to read eos_ble_status() on its next frame.
// The screen that matters is drawn by the loop below, which is the only thing
// in this image allowed to touch the panel.
// The advertised name of the device being paired. eos_ble_status() cannot
// supply it: its bond record is zeroed until the bond exists, and the whole
// point of this screen is the moment before that. It is written here and read
// by the loop; the worst a torn read costs is one frame of a wrong name on a
// screen that is redrawn on the next state change, which is cheaper than a lock
// held across the NimBLE host task.
static char pair_peer[EOS_BLE_NAME_MAX];

static void on_passkey(uint32_t passkey, const eos_ble_dev_t *peer, void *user)
{
    (void)user;
    pair_peer[0] = '\0';
    if (peer && peer->name[0]) {
        snprintf(pair_peer, sizeof pair_peer, "%s", peer->name);
    }
    ESP_LOGW(TAG, "pair   %s: type %06u on the keyboard, then Enter",
             pair_peer[0] ? pair_peer : "keyboard", (unsigned)passkey);
    ESP_LOGW(TAG, "pair   %s", eos_ble_pair_warning());
}

// --------------------------------------------------------------- net events

// eos_net calls this from whatever task moved the state, which for everything
// except the boot join is this one. It only raises a flag; the redraw happens
// in the loop, where the display is safe to touch.
static volatile bool net_dirty = true;

static void on_net_event(eos_net_event_t ev, const eos_net_t *n, void *ud)
{
    (void)ud;
    ESP_LOGI(TAG, "net    %s: mode %s, cred %s", eos_net_event_name(ev),
             eos_net_mode_name(eos_net_mode(n)), eos_net_cred_name(eos_net_cred(n)));
    net_dirty = true;
}

// ------------------------------------------------------------ setup screen

// What the foot of the setup screen says. It is a separate function because the
// wording of one line here is the difference between a recoverable board and a
// support call: after a refused join it has to say that NOTHING WAS SAVED, or
// the owner reasonably assumes the wrong password is now stored and that the
// board is bricked.
static bool setup_status(char *out, size_t cap)
{
    if (eos_net_trying(&net)) {
        snprintf(out, cap, "trying %s", eos_net_ssid(&net));
        return false;
    }
    if (eos_net_cred(&net) == EOS_NET_CRED_TRIED_FAIL) {
        snprintf(out, cap, "that network refused - nothing saved");
        return true;
    }
    if (eos_net_has_credentials(&net)) {
        snprintf(out, cap, "%s is out of reach", eos_net_ssid(&net));
        return true;
    }
    snprintf(out, cap, "scan the code, then open the page");
    return false;
}

// ------------------------------------------------------------------- boot

// Which of the three scenes the panel is showing. The passkey outranks both of
// the others: it is the only one with a human waiting on it, it lasts seconds,
// and it can arrive on a board that is already at the desktop.
typedef enum { SCREEN_SETUP = 0, SCREEN_DESKTOP, SCREEN_PASSKEY } screen_t;

void app_main(void)
{
    const eos_board_t *b = eos_board_get();
    eos_shell_view_t view;
    const eos_display_info_t *info;
    eos_wm_cfg_t cfg;
    eos_net_cfg_t ncfg;
    eos_httpd_cfg_t hcfg;
    eos_setup_view_t setup;
    eos_ble_status_t bst;
    eos_theme_src_t src;
    eos_err_t err;
    eos_net_err_t nerr;
    char qr[EOS_NET_QR_MAX];
    char url[32];
    char status[48];
    char last_status[48];
    uint32_t heap_boot, last_sec = 0;
    bool input_moved = false;
    screen_t screen = SCREEN_SETUP, last_screen = SCREEN_PASSKEY;
    uint32_t last_passkey = 0;
    bool dirty, net_ok, mood_moved;
    // Whether the avatar's tile was on screen last pass: it sets both the
    // redraw rate and whether the tile is damaged at all, and it is false the
    // moment the buddy goes behind a tab or on to another workspace.
    bool buddy_shown = false;

    // 0. The console ring, before anything logs. It is BSS and touches no
    //    hardware, and installing the log hook here is the difference between
    //    the Console tab showing this whole boot and showing only whatever was
    //    typed into it afterwards. It also registers the files, console, buddy
    //    and apps routes with eos_httpd, which is why it is before the server
    //    rather than next to it.
    {
        eos_apps_ports_t aports;
        memset(&aports, 0, sizeof aports);
        aports.describe = apps_describe;
        aports.reboot   = apps_reboot;
        eos_apps_init(&aports, NULL);
        eos_apps_log_install();
    }

    log_identity(b);

    heap_boot = heap_free();
    heap_mark = heap_boot;
    ESP_LOGI(TAG, "heap   at app_main   %7" PRIu32 " free, %7" PRIu32 " largest",
             heap_boot, heap_largest());

    // 1. The panel. This is the only call in the whole image that takes memory
    //    and keeps it, and it takes it once.
    err = eos_display_init();
    if (err != EOS_OK) {
        ESP_LOGE(TAG, "display init failed: %s (%d)", eos_strerr(err), (int)err);
        return;    // A panel that will not start is not retried; see the backend.
    }
    info = eos_display_info();
    ESP_LOGI(TAG, "display %dx%d, %d bands of %d rows, caps 0x%04x, palette %u",
             (int)info->w, (int)info->h, (int)info->max_bands, (int)info->band_h,
             (unsigned)info->caps, (unsigned)info->palette_len);
    heap_step("display");

    // 2. The filesystem. A board whose internal filesystem will not mount is
    //    not stopped by it: every caller above storage falls back to something
    //    compiled in, and a log line naming the partition is more use than a
    //    board that refuses to come up. A missing card is not even a log line -
    //    it is a mount that reports itself absent, which is what /api/system
    //    shows.
    err = eos_storage_init();
    if (err != EOS_OK) {
        ESP_LOGE(TAG, "storage init failed: %s (%d) - settings and files are off",
                 eos_strerr(err), (int)err);
    } else {
        eos_mount_t mnt[EOS_MOUNT_MAX];
        int i, nm = eos_storage_mounts(mnt, EOS_MOUNT_MAX);
        for (i = 0; i < nm && i < EOS_MOUNT_MAX; i++) {
            uint64_t total = 0, used = 0;
            if (mnt[i].mounted) eos_storage_usage(mnt[i].point, &total, &used);
            ESP_LOGI(TAG, "fs     %-4s %-8s %-11s %llu of %llu bytes used",
                     mnt[i].point,
                     mnt[i].fs == EOS_FS_LITTLEFS ? "littlefs"
                       : mnt[i].fs == EOS_FS_FAT  ? "fat" : "none",
                     mnt[i].mounted ? "mounted" : "not present",
                     (unsigned long long)used, (unsigned long long)total);
        }
    }
    // Put the shipped buddy on the card before asking for one. Without this
    // the panel wears a penguin the web app cannot see - the avatar is
    // compiled in, GET /api/buddy reports what is on /int, and /int is empty
    // on a new board. Seeding it means the Buddy tab opens on the buddy that
    // is actually on screen, so editing him starts from him rather than from
    // an empty grid. It writes once and never overwrites.
    heap_step("storage");

    // The gallery: the shipped models put back if any are missing, a
    // pre-gallery buddy.vox migrated in, and the active pointer written if
    // nothing valid is chosen. Measured on its own rather than folded into
    // storage, because this is the step that grew this pass and the one whose
    // cost anybody reading a boot log will want to find. It should be ~0: the
    // seeder stages nothing, comparing the legacy model against the shipped
    // one in 128-byte bites rather than holding 7 KB to answer one question.
    eos_seed_buddy();

    // The avatar, off the filesystem.
    {
        eos_err_t be = eos_apps_buddy_reload();
        if (be == EOS_OK)
            ESP_LOGI(TAG, "buddy  \"%s\" %u voxels, %s",
                     eos_apps_buddy_name()[0] ? eos_apps_buddy_name() : "unnamed",
                     (unsigned)eos_apps_buddy_model()->count,
                     eos_apps_idle_name(eos_apps_buddy_behaviour()));
        else if (be == EOS_ERR_NOTFOUND)
            ESP_LOGI(TAG, "buddy  none on %s yet - the Buddy tab builds one",
                     EOS_APPS_BUDDY_DIR);
        else
            ESP_LOGW(TAG, "buddy  %s: %s", eos_strerr(be),
                     eos_apps_buddy_error() ? eos_apps_buddy_error() : "");
    }
    // The voxel pool is BSS and was taken before app_main ran, so what this
    // measures is the parse and the metadata, not the model.
    heap_step("gallery");

    // 2b. The settings. Between storage and theme because it names the theme,
    //     and before the network because it names the mDNS host. A file that is
    //     truncated, garbled, empty or missing leaves the store holding
    //     defaults and the board booting - the same guarantee kernel/theme
    //     makes, for the same reason: the board whose config file is bad is the
    //     board that needs a serial cable.
    eos_settings_bind_ports(&settings, b, &theme);
    {
        eos_settings_err_t se = eos_settings_load(&settings);
        ESP_LOGI(TAG, "config %s (%s), theme \"%s\", bright %u, tz \"%s\"",
                 EOS_SETTINGS_PATH, eos_settings_strerror(se),
                 settings.v.ui_theme, (unsigned)settings.v.ui_bright,
                 settings.v.sys_tz[0] ? settings.v.sys_tz : "UTC");
        if (settings.bad_fields)
            ESP_LOGW(TAG, "config %u keys were unusable and kept their defaults",
                     (unsigned)__builtin_popcount(settings.bad_fields));
    }
    if (settings.v.sys_tz[0]) { setenv("TZ", settings.v.sys_tz, 1); tzset(); }
    eos_display_backlight((uint8_t)(((unsigned)settings.v.ui_bright * 100u + 127u) / 255u));

    // 3. The theme, and the palette it implies. A missing or corrupt file is a
    //    log line, never a stop - eos_theme.h guarantees the caller is left
    //    holding a usable theme whatever happened.
    eos_boot_theme_prefer(settings.v.ui_theme);
    src = eos_boot_theme_load(b, &theme);
    eos_boot_theme_upload(&theme);
    ESP_LOGI(TAG, "theme  \"%s\" from %s (gap %d, border %d, bar %d, tab %d, font %s)",
             theme.name, eos_boot_theme_src_name(src),
             (int)theme.m.gap, (int)theme.m.border, (int)theme.m.bar_h,
             (int)theme.m.tab_h, eos_theme_font(&theme));
    ESP_LOGI(TAG, "fonts  %" PRIu32 " B of flash, ui face \"%s\"",
             eos_font_flash_bytes(),
             eos_font_name(eos_font_id_from_name(eos_theme_font(&theme))));

    // 4. The window manager. The split across these five fields is the whole
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

    // The app launcher. Its list is the app registry, unfiltered and in
    // registry order — the same table /api/apps and the tab labels come from —
    // so an app added there appears in the overlay without anyone remembering
    // to add it twice. The registry's strings are static storage that outlives
    // everything, which is what makes the launcher borrowing rather than
    // copying them safe here; a caller handing it stack strings would not be.
    //
    // A registry longer than EOS_LAUNCHER_MAX is refused item by item rather
    // than truncated silently, so the log line below is the place that would
    // say so: the count it prints is what the overlay will actually show.
    {
        eos_launcher_geom_t lg;
        int i, n = eos_app_count();

        eos_launcher_init(&launcher);
        for (i = 0; i < n; i++) {
            const eos_app_t *a = eos_app_at(i);
            if (a) eos_launcher_add(&launcher, a->name, a->summary, (uint16_t)i);
        }

        // Geometry needs the panel and the theme's UI face, so it is computed
        // by the scene and handed back. Recomputed on a live theme change,
        // below, because a theme may name a different face and the row height
        // is derived from the face's height.
        eos_shell_launcher_geom(&theme, &lg);
        eos_launcher_set_geom(&launcher, &lg);
        eos_shell_input_launcher(&input, &launcher);
        ESP_LOGI(TAG, "shell  launcher %d of %d apps, %d rows of %d px, panel %dx%d",
                 eos_launcher_count(&launcher), n, (int)lg.rows, (int)lg.row_h,
                 (int)lg.w, (int)lg.h);
    }

    // The cursor. It needs the panel's size to clamp itself and gets it here,
    // from the same board rect the window manager was just handed, so the
    // arrow and the tiles can never disagree about where the edge is. It draws
    // nothing until the trackpad actually says something, so a board with no
    // pointing device paired is not carrying an arrow it cannot move.
    //
    // The chrome beside it is what makes the x in a tile's header clickable:
    // the same numbers the scene paints the box from, handed to the dispatcher
    // that tests clicks against it. Recomputed on a theme change, below, for
    // the same reason the launcher's geometry is.
    {
        eos_rect_t scr = eos_board_screen(b);
        eos_pointer_chrome_t ch;

        eos_pointer_init(eos_pointer_shared(), scr.w, scr.h);
        eos_shell_tile_chrome(&theme, &ch);
        eos_shell_input_chrome(&input, &ch);

        // The three things between eos_shell_input_init() above and
        // heap_step("shell") below take no heap at all: the dispatcher, the
        // launcher and the cursor are file statics claimed before app_main
        // ran, so the step's delta will read zero and there would otherwise be
        // no line saying where the bytes actually went. It prints them here
        // for the same reason eos_shell_buddy_bytes() and eos_app_bss_bytes()
        // print theirs.
        ESP_LOGI(TAG, "shell  input %u B, launcher %u B, cursor %u B of static RAM; "
                      "close box %dx%d px, border %d",
                 (unsigned)sizeof input, (unsigned)sizeof launcher,
                 (unsigned)sizeof(eos_pointer_t),
                 (int)ch.close_w, (int)(ch.border + 1 + ch.hdr_h), (int)ch.border);
    }

    // The desktop's windows exist from here on even while SETUP owns the glass.
    // Opening them later would mean the first desktop frame declares damage for
    // tiles the backend has not banded.
    open_windows(b);
    heap_step("shell");

    // 5. Something on the panel before anything slow. eos_net_start() below can
    //    block for the whole fifteen second join budget.
    eos_setup_screen_message(&theme, "penguinOS", "starting the radios");

    // 6. Input, which is what brings up the NimBLE HID host. Before WiFi: see
    //    the file header. NULL takes eos_input_defaults().
    eos_ble_on_passkey(on_passkey, NULL);
    err = eos_input_init(NULL);
    if (err != EOS_OK)
        ESP_LOGW(TAG, "input  init returned %s (%d)", eos_strerr(err), (int)err);
    ESP_LOGI(TAG, "input  ring %d events, ble host %s, %u buttons",
             EOS_INPUT_QUEUE, b->input.ble_keyboard ? "up" : "not on this board",
             (unsigned)b->input.button_count);
    heap_step("ble");

    // 7. The network. eos_net_idf_defaults() fills in the driver, the NVS store
    //    and the timings out of docs/provisioning.md; everything set after it
    //    is this board's.
    eos_net_idf_defaults(&ncfg);
    ncfg.on_event = on_net_event;
    // net.host. Empty leaves eos_net's own rule in place, which derives the
    // label from the MAC so six boards do not all claim penguinos.local.
    if (settings.v.net_host[0])
        snprintf(ncfg.hostname, sizeof ncfg.hostname, "%s", settings.v.net_host);
    nerr = eos_net_init(&net, &ncfg);
    net_ok = (nerr == EOS_NET_OK);
    if (!net_ok)
        ESP_LOGE(TAG, "net    init failed: %s", eos_net_err_name(nerr));
    ESP_LOGI(TAG, "net    scans and joins are %s against BLE",
             eos_net_radio_serialised() ? "serialised" : "NOT SERIALISED");

    if (net_ok && eos_net_has_credentials(&net))
        eos_setup_screen_message(&theme, "penguinOS", "joining the stored network");

    // The three-state boot. It returns OK for both landing states: reaching
    // SETUP because a join failed is an outcome, not an error.
    if (net_ok) {
        nerr = eos_net_start(&net);
        if (nerr != EOS_NET_OK)
            ESP_LOGE(TAG, "net    start failed: %s", eos_net_err_name(nerr));
    }
    ESP_LOGI(TAG, "net    mode %s, credentials %s, ap \"%s\"",
             eos_net_mode_name(eos_net_mode(&net)),
             eos_net_cred_name(eos_net_cred(&net)), eos_net_ap_ssid(&net));
    heap_step("wifi");

    // 8. The server. It binds to eos_net and eos_ble, so it goes up after both.
    //    The three file ports go to eos_web_embed, not to eos_storage. There
    //    IS a storage backend now and /int is mounted, but it is empty on
    //    every board that exists: nothing deploys the five web files onto it
    //    yet. Serving from a filesystem that has no index.html would trade a
    //    working app in flash for a 404. When a deploy path exists, the rule
    //    from web/README.md applies - prefer the file, fall back to the
    //    embedded copy, and never leave the board with nothing to serve.
    eos_httpd_cfg_default(&hcfg);
    hcfg.mode = (eos_net_mode(&net) == EOS_NET_SETUP) ? EOS_HTTPD_MODE_SETUP
                                                      : EOS_HTTPD_MODE_RUN;
    eos_httpd_init(&httpd, NULL, NULL, &hcfg);
    // NULL when the network never came up, which is what makes the four WiFi
    // endpoints answer 501 instead of failing one call at a time against a
    // service that was never initialised.
    eos_httpd_idf_bind(&httpd, net_ok ? &net : NULL);
    eos_web_embed_bind(&httpd);   // the app lives in flash, not on a card
    // And then eos_apps in front of it, which is what turns the comment above
    // into the rule web/README.md states: a real file on /int is preferred, the
    // embedded copy answers when there is not one, and the board is never left
    // with nothing to serve. It is also what /api/fs/read streams through -
    // with the fallback switched off, so a GET for a file that is not there is
    // a 404 and not the contents of a same-named asset in the image.
    eos_apps_bind_files(&httpd);
    build_app_catalog(b);

    // 8a. The settings, system and theme ports. After both binds above for the
    //     same reason megabrain's are: eos_httpd_idf_bind() assigns the whole
    //     port table by value and would wipe anything set before it.
    eos_settings_bind(&httpd, &settings, b, &theme, net_ok ? &net : NULL);

    // 8b. Megabrain. It goes in after both binds above because
    //     eos_httpd_idf_bind() assigns the whole port table by value and would
    //     wipe these four; it goes in before the server starts so that the
    //     first request cannot arrive at a half-wired table.
    //
    //     The five brain.* keys are "live" in web/README.md, and this is the
    //     first half of what makes that true: the store's values are handed
    //     over HERE at boot, so a configured mini survives a reboot. The other
    //     half is eos_settings_bind.c's apply hook, which calls the same
    //     function on every POST /api/settings that touches one of them.
    //     eos_brain_bridge_from_settings() starts from the compiled-in
    //     defaults and overwrites only the fields the store actually holds, so
    //     a settings file that names two of the five cannot blank the rest.
    {
        eos_brain_bridge_cfg_t bcfg;
        eos_brain_bridge_defaults(&bcfg);
        if (eos_brain_bridge_start() == 0) {
            eos_brain_bridge_from_settings(&settings.v);
            eos_brain_bridge_bind(&httpd);
            eos_brain_bridge_model(brain_model, sizeof brain_model);
            ESP_LOGI(TAG, "brain  client up, host %s:%u, model %s",
                     settings.v.brain_host[0] ? settings.v.brain_host : bcfg.host,
                     (unsigned)(settings.v.brain_port ? settings.v.brain_port : bcfg.port),
                     brain_model[0] ? brain_model : bcfg.model);
        } else {
            ESP_LOGE(TAG, "brain  client refused to start - the bar stays \"no brain\"");
        }
    }

    // 8c. The windows. After the brain block and OUTSIDE it, both deliberately.
    //     After, because the Chat window reaches megabrain through the same
    //     four port pointers the web app does and they are assigned above.
    //     Outside, because a board whose brain task refused to start still has
    //     nine other windows and a Chat tile that should read "no megabrain
    //     client on this board" rather than not existing.
    //
    //     This is also where the WS2812 on GPIO8 is claimed. A board that
    //     declares no LED, or one whose RMT channels are all spoken for, comes
    //     back absent and the Media and Party windows say so.
    eos_app_bind(&httpd, &settings.v, b, &buddy);
    ESP_LOGI(TAG, "apps   %d windows%s, %" PRIu32 " B of static RAM, led %s",
             eos_app_count(), eos_app_table_ok() ? "" : " - THE TABLE IS BROKEN",
             eos_app_bss_bytes(), eos_led_present() ? "up on GPIO8" : "absent");
    heap_step("brain");

    if (eos_httpd_start(&httpd) == 0) {
        httpd_up = true;
        ESP_LOGI(TAG, "httpd  up on port %u in %s mode", (unsigned)hcfg.port,
                 hcfg.mode == EOS_HTTPD_MODE_SETUP ? "setup" : "run");
    } else {
        ESP_LOGE(TAG, "httpd  refused to start - the API and the web app are down");
    }
    heap_step("httpd");

    // 9. The scene the desktop draws when it is the one on screen.
    eos_bar_status_init(&bar);
    // The avatar. buddy_adopt() takes whichever model exists — the one
    // eos_apps_buddy_reload() found on /int back at step 2, or the compiled-in
    // one — and builds the shade table against the palette the theme just
    // uploaded, which is why it is here and not next to the reload.
    buddy_gen = eos_apps_buddy_generation();
    buddy_adopt();
    // The buddy window claims no heap at all — the box, the shade table and the
    // compiled-in model are BSS, taken before app_main ran. heap_step("buddy")
    // below will therefore report ~0, which is the point; this line is the one
    // that says where the static kilobytes went.
    ESP_LOGI(TAG, "buddy  %s model, %u voxels, %ux%u px box, %" PRIu32 " B of BSS",
             eos_apps_buddy_model() ? "flash" : "compiled-in",
             (unsigned)(buddy.model ? buddy.model->count : 0u),
             (unsigned)EOS_SHELL_BUDDY_PX, (unsigned)EOS_SHELL_BUDDY_PX,
             eos_shell_buddy_bytes());
    bar.brain_up = eos_brain_bridge_reachable();
    bar.mood     = EOS_MOOD_IDLE;

    memset(&view, 0, sizeof view);
    view.theme = &theme;
    view.wm    = &wm;
    view.bar   = &bar;
    view.keys  = &keys;
    view.buddy = &buddy;
    view.pointer  = eos_pointer_shared();
    view.launcher = &launcher;
    board_lines(b, &view, &net);

    // sys.autostart, after the windows exist and before the first frame. It
    // moves the focus, which changes which tab of the collapsed group is
    // visible, so doing it later would show one window for a frame and then
    // another.
    apply_autostart(settings.v.sys_autostart);
    heap_step("buddy");

    ESP_LOGI(TAG, "heap   boot cost %" PRId32 " B of the %" PRIu32 " free at app_main; "
                  "%" PRIu32 " left, largest block %" PRIu32,
             (int32_t)heap_boot - (int32_t)heap_free(), heap_boot,
             heap_free(), heap_largest());


    // 10. And the one SETUP draws. The QR payload is generated once: the AP name
    //     and password do not change while the board is up, and eos_net keeps
    //     the password across a forget precisely so the screen someone is
    //     reading does not go stale under them.
    memset(&setup, 0, sizeof setup);
    qr[0] = '\0';
    if (eos_net_ap_qr(&net, qr, sizeof qr) < 0) {
        qr[0] = '\0';
        ESP_LOGW(TAG, "setup  no QR payload - the panel falls back to text");
    }
    snprintf(url, sizeof url, "http://%s", EOS_NET_AP_IP_STR);
    setup.theme    = &theme;
    setup.ap_ssid  = eos_net_ap_ssid(&net);
    setup.ap_psk   = eos_net_ap_psk(&net);
    setup.url      = url;
    setup.qr       = qr[0] ? qr : NULL;
    last_status[0] = '\0';

    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint32_t sec = now / 1000u;

        // Buttons, key repeat, expiring injected holds, and the BLE reconnect
        // backoff. Everything time-based in the input path happens here; the
        // pump below only drains what this produced.
        eos_input_tick(now);

        // The AP teardown after a commit, the address and RSSI poll, and the
        // rejoin after a link drop.
        eos_net_pump(&net, now);

        // The queued join and the queued rescan. This is the ONLY place in the
        // image where credentials reach flash, and it does it by calling
        // eos_net_try() and then eos_net_commit() only when that succeeded.
        // It also blocks for as long as the join takes, which is why it is here
        // and not on an HTTP worker.
        if (httpd_up) eos_httpd_pump(&httpd, now);

        // The upload handle nobody came back to, and the console's clock. Here
        // rather than in a handler because an upload times out precisely while
        // no request is arriving.
        eos_apps_tick(now);

        // The windows that move on their own: the chat draining the megabrain
        // relay, the files list rescanning, the party driving the LED and the
        // avatar's mood. It is here and not inside the desktop branch because
        // the chat drain must keep running whatever is on the glass — the relay
        // stops pumping its socket when its ring fills, and a consumer that
        // paused during a passkey screen would wedge a reply behind it.
        eos_app_tick(&view, now);

        // A model uploaded through /api/fs/write and reloaded on an HTTP worker
        // is adopted HERE, on the task that ticks the state machine and draws
        // it, so the swap cannot land inside a frame. That is the whole
        // synchronisation and it is enough: one writer, one reader, and a
        // generation counter between them.
        if (eos_apps_buddy_generation() != buddy_gen) {
            buddy_gen = eos_apps_buddy_generation();
            buddy_adopt();
            ESP_LOGI(TAG, "buddy  reloaded: %s model, %u voxels",
                     eos_apps_buddy_model() ? "flash" : "compiled-in",
                     (unsigned)(buddy.model ? buddy.model->count : 0u));
        }
        // One writer, on this task, so /api/buddy reports the state the panel
        // is actually in rather than a second machine that drifts from it.
        eos_apps_buddy_set_state(eos_buddy_state(&buddy));

        if (reboot_armed && (int32_t)(now - reboot_at_ms) >= 0) {
            ESP_LOGW(TAG, "reboot from the console");
            esp_restart();
        }

        // The debounced settings write and the armed reboot. Both are here and
        // not on an HTTP worker: a LittleFS sync is a sector erase with the
        // instruction cache off, and a restart inside a handler drops the
        // response the person is waiting on.
        eos_settings_bind_pump(now);

        // The avatar and the two bar segments megabrain owns. Not inside the
        // desktop branch: see brain_tick().
        mood_moved = brain_tick(now);

        eos_ble_status(&bst);
        if (bst.passkey_shown)                       screen = SCREEN_PASSKEY;
        else if (eos_net_mode(&net) == EOS_NET_SETUP) screen = SCREEN_SETUP;
        else                                          screen = SCREEN_DESKTOP;

        // The server's document root follows the radio. Nothing reads it on its
        // own, and a board still answering captive-portal probes after it has
        // joined a real network is how a phone decides the network is broken.
        if (httpd_up)
            httpd.cfg.mode = (screen == SCREEN_SETUP) ? EOS_HTTPD_MODE_SETUP
                                                      : EOS_HTTPD_MODE_RUN;

        dirty = (screen != last_screen);
        last_screen = screen;

        // ONE drain of the event ring, on every pass, whatever is on the glass.
        // It used to be two: this call inside the desktop branch, and a
        // discard loop at the foot of the loop for every other screen. Two
        // consumers of one queue is two places that have to agree about what
        // an event off the desktop means, and the order between them was
        // wrong — the discard ran after the desktop branch, so the first pass
        // that reached the desktop dispatched the whole backlog the setup
        // screen had collected. eos_shell_input_set_active() moved that rule
        // inside the dispatcher, where it is one branch of the ladder in
        // eos_shell_input.h and not a second code path.
        //
        // The answer only means anything to the desktop, which is the only
        // scene an event can move; the other two ignore it because the pump
        // has already thrown their events away.
        eos_shell_input_set_active(&input, screen == SCREEN_DESKTOP);
        input_moved = eos_shell_input_pump(&input);

        // A full ring drops the NEWEST event and counts it, which is the right
        // trade — losing a key-up latches nothing, because the HAL's held table
        // is updated whether or not the event made it into the queue — but a
        // dropped EOS_EV_CLICK is a click that simply did not happen, and until
        // this line nothing in the image ever read the counter. Logged on the
        // change and not per drop: a burst is one line, and a board that never
        // overflows never says anything.
        {
            static uint32_t last_drop;
            uint32_t d = eos_input_dropped();
            if (d != last_drop) {
                ESP_LOGW(TAG, "input  ring full, %u event%s dropped in total",
                         (unsigned)d, d == 1 ? "" : "s");
                last_drop = d;
            }
        }

        switch (screen) {
        case SCREEN_PASSKEY:
            if (bst.passkey != last_passkey) dirty = true;
            last_passkey = bst.passkey;
            if (dirty)
                eos_setup_screen_passkey(&theme, bst.passkey,
                                         pair_peer[0] ? pair_peer : NULL,
                                         eos_ble_pair_warning());
            break;

        case SCREEN_SETUP:
            setup.status_warn = setup_status(status, sizeof status);
            if (net_dirty || strcmp(status, last_status) != 0) dirty = true;
            net_dirty = false;
            snprintf(last_status, sizeof last_status, "%s", status);
            setup.status = status;
            if (dirty) {
                eos_setup_screen_draw(&setup);
                // The passphrase is on the GLASS and in the QR code and it is
                // deliberately not here. eos_apps_log_install() puts every log
                // line into the console ring, and GET /api/console/log serves
                // that ring to anybody who can reach the board - which in SETUP
                // is anyone on this access point and in RUN is anyone on the
                // house network. There is no login on a device provisioned by
                // pointing a camera at a QR code, so the ring is public, and a
                // credential that reaches it is a credential that stays
                // readable for the rest of the boot.
                ESP_LOGI(TAG, "setup  ap \"%s\" at %s, qr %s: %s",
                         setup.ap_ssid, url,
                         eos_setup_screen_had_qr() ? "on screen" : "TEXT ONLY",
                         status);
            }
            break;

        case SCREEN_DESKTOP:
        default:
            // Asked once, used twice: it decides whether the avatar's tile is
            // damaged at all and how long the loop sleeps afterwards.
            buddy_shown = eos_shell_app_visible(&view, EOS_APP_BUDDY);
            refresh_status(now);
            view.uptime_ms    = now;
            board_lines(b, &view, &net);
            view.heap_free    = bar.free_heap;
            view.heap_largest = heap_largest();

            if (input_moved) {
                // A move can change every tile and the pips at once, so the
                // whole screen is declared rather than guessed at rect by rect.
                eos_shell_damage_all();
                dirty = true;
            } else if (eos_settings_bind_take_redraw()) {
                // A live theme switch reprogrammed the CLUT. Every pixel on the
                // glass is a palette index into it, so the whole screen changed
                // colour and none of it declared damage.
                //
                // The buddy is the one thing on screen that does NOT follow
                // for free: its shade table was resolved against the old
                // palette and holds indices, not colours, so it has to be
                // rebuilt or the avatar keeps wearing the previous theme.
                buddy_adopt();
                // And the launcher's rows are sized from the theme's UI face,
                // which a theme is allowed to change. Recomputing here is what
                // keeps the highlight bar and the hit test on the same grid as
                // the glyphs after a switch.
                //
                // And the close box, for the same reason: it is measured from
                // the face's height, and a hit box on the old face's grid is a
                // hit box that is not where the x is.
                {
                    eos_launcher_geom_t lg;
                    eos_pointer_chrome_t ch;

                    eos_shell_launcher_geom(&theme, &lg);
                    eos_launcher_set_geom(&launcher, &lg);
                    eos_shell_tile_chrome(&theme, &ch);
                    eos_shell_input_chrome(&input, &ch);
                }
                eos_shell_damage_all();
                dirty = true;
            } else if (dirty) {
                eos_shell_damage_all();     // arriving from another screen
            } else {
                // The things that move on their own: the bar, the clock window,
                // the WiFi glyph, and the avatar. Declaring them separately is
                // what keeps an idle board pushing 27 KB a second instead of
                // 115 KB.
                if (sec != last_sec || net_dirty || mood_moved) {
                    eos_shell_damage_bar(&view);
                    eos_shell_damage_app(&view, EOS_APP_CLOCK);
                    dirty = true;
                }
                // The buddy bobs, blinks and eases its yaw, so its tile is
                // damaged on every pass it is visible rather than once a
                // second. The band engine sizes its strips from the damage
                // rect, so this costs the tile's own 114x91 and not a
                // full-width band: about 20 KB of SPI, which is why the loop
                // is allowed to run faster while it is up.
                if (buddy_shown) {
                    eos_shell_damage_app(&view, EOS_APP_BUDDY);
                    dirty = true;
                }
            }
            net_dirty = false;

            // And whatever an app changed: a token that arrived, a directory
            // that was rescanned, an effect that is mid-animation. Each of them
            // is one tile-sized rect and none of them is a full screen, which
            // is why this is a separate call and not folded into the branch
            // above. Safe after eos_shell_damage_all() for the same reason the
            // pointer is: an overlapping rect unions in and changes nothing.
            if (eos_app_damage(&view)) dirty = true;

            // The arrow, last and on its own. It moves independently of every
            // branch above - a trackpad report changes no tile and no bar - so
            // its two rects are declared here rather than inside any of them,
            // and they are two rects and not a screen: 154 pixels against
            // 57,600, on a banded backend that could not push the second
            // number at the rate a trackpad reports.
            //
            // Safe after eos_shell_damage_all(): a rect that overlaps the
            // full-screen entry unions into it and changes nothing.
            view.pointer_ms = now;

            // ONE read of the cursor, held for the whole frame. Its position
            // is written from the NimBLE host task, which preempts this one,
            // and a frame reads it three times: here for the damage rects,
            // once per band while the scene is replayed, and again at the
            // commit below. A trackpad report landing between any two of those
            // leaves them disagreeing, and the arrow painted at the position
            // no rect covered stays on the glass. The latch is what makes all
            // three the same number; motion that arrives mid-frame moves the
            // cursor as it always did and is picked up by the next latch.
            eos_pointer_latch(eos_pointer_shared(), now);
            if (eos_shell_damage_pointer(&view)) dirty = true;

            if (dirty) {
                last_sec = sec;
                eos_shell_draw_frame(&view);
                // AFTER the draw, never before. The damage above is the
                // difference between where the arrow was and where it is, and
                // committing early would collapse that difference to nothing
                // and leave the old arrow on the glass.
                eos_pointer_commit(eos_pointer_shared(), now);
            }
            break;
        }

        // The avatar is the only thing on this board that animates, so it is
        // the only thing that earns a faster loop. A tile-sized damage rect is
        // cheap; a whole screen at 10 Hz would not be, and neither would the
        // setup screen at 10 Hz, so this is gated on the buddy actually being
        // on the glass.
        // A moving cursor is the second thing that earns a faster loop, and it
        // earns a faster one than the buddy: 10 fps reads as an animation and
        // as lag. It costs two 7x11 rects a frame and it stops on its own when
        // the trackpad goes quiet for EOS_POINTER_IDLE_MS, so an idle board is
        // still an idle board. The first report after a quiet spell waits out
        // whatever tick was already running - up to 250 ms - because nothing
        // here wakes on a BLE notification; every one after it is at this rate.
        {
            uint32_t tick = IDLE_TICK_MS;
            if (screen == SCREEN_DESKTOP) {
                if (eos_pointer_visible(eos_pointer_shared(), now))
                    tick = POINTER_TICK_MS;
                else if (buddy_shown || eos_app_wants_fast())
                    tick = BUDDY_TICK_MS;
            }
            vTaskDelay(pdMS_TO_TICKS(tick));
        }
    }
}
