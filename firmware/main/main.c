// app_main — the boot path. Board descriptor, panel, theme, radios, and then
// either the provisioning screen or the tiled desktop.
//
// Everything above this file is pure: eos_wm hands out rectangles, eos_bar
// hands out positioned text, eos_theme hands out palette indices, eos_font
// hands out glyph bits, eos_net owns the credential state machine, eos_ble owns
// the keyboard and eos_httpd owns the API — and none of them knows the others
// exist. Nothing in the kernel joins them up, and nothing in the kernel is
// allowed to call IDF. This file is the join, and it is the only file in ESP-OS
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

#include "eos_boot_theme.h"
#include "eos_shell_draw.h"
#include "eos_shell_input.h"
#include "eos_setup_screen.h"
#include "eos_web_embed.h"

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

// The shell state lives for the life of the image and is not small
// (eos_wm_t alone is about 900 bytes), so it is static rather than stacked on
// a task that also runs the compositor.
static eos_wm_t          wm;
static eos_theme_t       theme;
static eos_keymap_t      keys;
static eos_shell_state_t shell;
static eos_shell_input_t input;
static eos_bar_status_t  bar;

// The services. Both are meant for BSS and say so in their own headers:
// eos_net_t is about 1.2 KB and eos_httpd_t about 4.7 KB, and neither
// allocates outside its own start call.
static eos_net_t   net;
static eos_httpd_t httpd;
static bool        httpd_up;

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

    eos_shell_status_sync(&wm, &bar, eos_shell_app_names(), EOS_APP_COUNT);
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
    screen_t screen = SCREEN_SETUP, last_screen = SCREEN_PASSKEY;
    uint32_t last_passkey = 0;
    bool dirty, net_ok;

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
    heap_step("storage");

    // 3. The theme, and the palette it implies. A missing or corrupt file is a
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

    // The desktop's windows exist from here on even while SETUP owns the glass.
    // Opening them later would mean the first desktop frame declares damage for
    // tiles the backend has not banded.
    open_windows(b);
    heap_step("shell");

    // 5. Something on the panel before anything slow. eos_net_start() below can
    //    block for the whole fifteen second join budget.
    eos_setup_screen_message(&theme, "ESP-OS", "starting the radios");

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
    nerr = eos_net_init(&net, &ncfg);
    net_ok = (nerr == EOS_NET_OK);
    if (!net_ok)
        ESP_LOGE(TAG, "net    init failed: %s", eos_net_err_name(nerr));
    ESP_LOGI(TAG, "net    scans and joins are %s against BLE",
             eos_net_radio_serialised() ? "serialised" : "NOT SERIALISED");

    if (net_ok && eos_net_has_credentials(&net))
        eos_setup_screen_message(&theme, "ESP-OS", "joining the stored network");

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
    if (eos_httpd_start(&httpd) == 0) {
        httpd_up = true;
        ESP_LOGI(TAG, "httpd  up on port %u in %s mode", (unsigned)hcfg.port,
                 hcfg.mode == EOS_HTTPD_MODE_SETUP ? "setup" : "run");
    } else {
        ESP_LOGE(TAG, "httpd  refused to start - the API and the web app are down");
    }
    heap_step("httpd");

    ESP_LOGI(TAG, "heap   boot cost %" PRId32 " B of the %" PRIu32 " free at app_main; "
                  "%" PRIu32 " left, largest block %" PRIu32,
             (int32_t)heap_boot - (int32_t)heap_free(), heap_boot,
             heap_free(), heap_largest());

    // 9. The scene the desktop draws when it is the one on screen.
    eos_bar_status_init(&bar);
    bar.brain_up = false;
    bar.mood     = EOS_MOOD_IDLE;

    memset(&view, 0, sizeof view);
    view.theme = &theme;
    view.wm    = &wm;
    view.bar   = &bar;
    view.keys  = &keys;
    board_lines(b, &view, &net);

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
                ESP_LOGI(TAG, "setup  ap \"%s\" pass \"%s\" at %s, qr %s: %s",
                         setup.ap_ssid, setup.ap_psk, url,
                         eos_setup_screen_had_qr() ? "on screen" : "TEXT ONLY",
                         status);
            }
            break;

        case SCREEN_DESKTOP:
        default:
            refresh_status(now);
            view.uptime_ms    = now;
            board_lines(b, &view, &net);
            view.heap_free    = bar.free_heap;
            view.heap_largest = heap_largest();

            if (eos_shell_input_pump(&input)) {
                // A move can change every tile and the pips at once, so the
                // whole screen is declared rather than guessed at rect by rect.
                eos_shell_damage_all();
                dirty = true;
            } else if (dirty) {
                eos_shell_damage_all();     // arriving from another screen
            } else if (sec != last_sec || net_dirty) {
                // The three things that move on their own: the bar, the clock
                // window, and the WiFi glyph. Declaring them separately is what
                // keeps an idle board pushing 27 KB a second instead of 115 KB.
                eos_shell_damage_bar(&view);
                eos_shell_damage_app(&view, EOS_APP_CLOCK);
                dirty = true;
            }
            net_dirty = false;

            if (dirty) {
                last_sec = sec;
                eos_shell_draw_frame(&view);
            }
            break;
        }

        // Keystrokes are drained on every pass, not only on the desktop, and
        // off the desktop they are drained and DISCARDED. Two reasons: a
        // keyboard paired while the setup screen is up would otherwise deliver
        // its first thirty-two keys the instant the desktop appeared, and a
        // super+q dispatched behind a screen the user cannot see closes a window
        // they never asked to close.
        if (screen != SCREEN_DESKTOP) {
            eos_event_t drop;
            while (eos_shell_input_next(&drop)) { }
        }

        vTaskDelay(pdMS_TO_TICKS(IDLE_TICK_MS));
    }
}
