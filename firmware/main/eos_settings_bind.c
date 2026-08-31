// The board's answers to /api/settings, /api/system and /api/themes.
// See eos_settings_bind.h for why this lives beside app_main.

#include "eos_settings_bind.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "eos_apps.h"
#include "eos_boot_theme.h"
#include "eos_brain_bridge.h"
#include "eos_display.h"
#include "eos_net.h"
#include "eos_storage.h"

static const char *TAG = "eos.set";

// The image's own version. There is no build system field for it yet, so it is
// a string here rather than a lie somewhere else.
#ifndef EOS_FW_VERSION
#define EOS_FW_VERSION "0.1.0"
#endif
#ifndef EOS_FW_BUILT
#define EOS_FW_BUILT __DATE__ " " __TIME__
#endif

// Where a theme file lives on either mount. The picker's names are the file
// STEMS, not the "name" field inside the file: the stem is what the loader has
// to build a path from, and a file whose stem and inner name disagree would
// give a picker entry that vanishes the moment it is chosen.
#define THEME_DIR "themes"

// How many theme files the picker offers beyond the active one. It is a
// snapshot taken when the response starts, so a card with fifty files gives a
// short list rather than a document that changes under the writer.
#define THEME_LIST_MAX 12

static struct {
    eos_httpd_t          *h;
    eos_settings_store_t *st;
    const eos_board_t    *b;
    eos_theme_t          *theme;
    eos_net_t            *net;

    bool     redraw;          // a live theme switch happened

    bool     reboot_armed;
    uint32_t reboot_at_ms;

    // The theme scan, taken on the i == 0 call and read by the rest. eos_httpd
    // serialises dispatch, so one snapshot for the image is enough.
    char     tnames[THEME_LIST_MAX][EOS_SETTINGS_THEME_MAX];
    char     tpaths[THEME_LIST_MAX][EOS_HTTPD_PATH_MAX];
    int      tcount;
} S;

// ==========================================================================
// The settings store's own ports
// ==========================================================================

static bool p_wifi_ssid(void *ctx, char *out, int cap)
{
    const char *s;
    (void)ctx;
    if (!S.net) return false;
    s = eos_net_ssid(S.net);
    if (!s) return false;
    snprintf(out, (size_t)cap, "%s", s);
    return true;
}

static bool p_wifi_psk_set(void *ctx)
{
    (void)ctx;
    return S.net ? eos_net_has_credentials(S.net) : false;
}

// The one place the Settings page can move a board onto another network.
//
// It does NOT write anything itself. It queues through eos_httpd's existing
// wifi_join port, which is drained by eos_httpd_pump() on the OS loop and
// which calls eos_net_try() and then eos_net_commit() only if that succeeded.
// Reusing it rather than calling eos_net directly is what keeps the
// try-then-commit rule in one place: a second joiner would be a second chance
// to get save-then-try the wrong way round.
//
// A new SSID with no passphrase is refused rather than guessed at. The stored
// passphrase belongs to the network being left, and joining a different network
// with it would fail in a way the page would report as a wrong password.
static int p_wifi_set(void *ctx, const char *ssid, const char *psk)
{
    const char *use_ssid;
    char cur[EOS_NET_SSID_MAX];

    (void)ctx;
    if (!S.h || !S.h->ports.wifi_join || !S.net) return (int)EOS_ERR_UNSUPPORTED;

    if (!ssid) {
        // A corrected passphrase for the network already named. Common enough
        // to be worth handling: it is what someone does after a typo.
        snprintf(cur, sizeof cur, "%s", eos_net_ssid(S.net) ? eos_net_ssid(S.net) : "");
        if (!cur[0]) return (int)EOS_ERR_ARG;
        use_ssid = cur;
    } else {
        if (!ssid[0]) return (int)EOS_ERR_ARG;
        use_ssid = ssid;
    }

    if (!psk) return (int)EOS_ERR_ARG;   // see the note above

    return S.h->ports.wifi_join(S.h->ctx, (const uint8_t *)use_ssid,
                                (int)strlen(use_ssid), psk, (int)strlen(psk));
}

// A theme name is a filename STEM and this is the only thing that turns one
// into a path. It arrives off the network - POST /api/settings carries it - and
// eos_settings only ever checked its LENGTH, so this is where the rest of the
// rule lives: one component, no separators, no leading dot. LittleFS resolves
// ".." inside a path for itself, so "../settings" here would have read
// /int/settings.json and called it a theme; it could not leave the partition,
// and a name that addresses a different directory than the picker offered is
// still a name that means something other than what it says.
static bool name_is_stem(const char *n)
{
    int i;
    if (!n || !n[0] || n[0] == '.') return false;
    for (i = 0; n[i]; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c == '/' || c == '\\') return false;
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

// Loads a theme by stem from either mount and puts it on the panel. The card
// first, same order as the boot search, so a theme dropped on a card wins over
// one flashed into /int.
static eos_err_t apply_theme(const char *name)
{
    char path[EOS_HTTPD_PATH_MAX];
    eos_theme_metrics_t before;
    eos_theme_err_t e = EOS_THEME_ERR_EMPTY;

    if (!S.theme || !S.b || !name_is_stem(name)) return EOS_ERR_ARG;
    before = S.theme->m;

    if (S.b->storage.sd && S.b->storage.sd_point) {
        snprintf(path, sizeof path, "%s/%s/%s.json",
                 S.b->storage.sd_point, THEME_DIR, name);
        e = eos_boot_theme_read(path, S.theme);
    }
    if (e != EOS_THEME_OK && S.b->storage.int_point) {
        snprintf(path, sizeof path, "%s/%s/%s.json",
                 S.b->storage.int_point, THEME_DIR, name);
        e = eos_boot_theme_read(path, S.theme);
    }

    if (e != EOS_THEME_OK) {
        // The theme the panel is wearing is untouched — eos_boot_theme_read()
        // guarantees that — so this is a refused setting and not a broken
        // screen. The store rolls the key back on a negative return.
        ESP_LOGW(TAG, "theme  \"%s\": %s", name, eos_theme_strerror(e));
        return EOS_ERR_NOTFOUND;
    }

    // Colours are live: the compositor holds palette INDICES and the CLUT is
    // what turns them into pixels, so reprogramming it restyles everything
    // already on the glass at the next flush.
    eos_boot_theme_upload(S.theme);
    eos_boot_theme_prefer(name);
    S.redraw = true;
    ESP_LOGI(TAG, "theme  now \"%s\"", name);

    // Metrics are not. gap, bar_h and tab_h went into eos_wm_cfg_t at boot and
    // the window manager lays out from them; changing them under a live tree
    // would move every tile without telling the compositor which bands moved.
    // Saying so is what makes the page's "(reboot)" label honest instead of
    // decorative.
    if (before.gap   != S.theme->m.gap   || before.border != S.theme->m.border ||
        before.bar_h != S.theme->m.bar_h || before.tab_h  != S.theme->m.tab_h)
        return EOS_ERR_UNSUPPORTED;      // kept and applied; metrics need a reboot

    return EOS_OK;
}

static eos_err_t p_apply(void *ctx, int key, const eos_settings_t *s)
{
    (void)ctx;

    switch (key) {
    case EOS_SET_UI_BRIGHT: {
        // The backlight HAL takes percent and the setting is 0..255, because
        // that is what the web contract fixed and what a PWM duty reads as.
        eos_err_t e = eos_display_backlight((uint8_t)(((unsigned)s->ui_bright * 100u + 127u) / 255u));
        // A board with no backlight pin says NODEV. That is not a reason to
        // refuse the value: it is stored, it means nothing here, and it will
        // mean something on the next board this settings file is copied to.
        return (e == EOS_OK || e == EOS_ERR_NODEV) ? EOS_OK : e;
    }

    case EOS_SET_UI_THEME:
        return apply_theme(s->ui_theme);

    case EOS_SET_SYS_TZ:
        // The clock is UTC until something sets this. An empty string clears
        // it rather than leaving the previous zone in place, which is what an
        // owner emptying the field means.
        if (s->sys_tz[0]) setenv("TZ", s->sys_tz, 1);
        else              unsetenv("TZ");
        tzset();
        return EOS_OK;

    case EOS_SET_BRAIN_HOST:
    case EOS_SET_BRAIN_PORT:
    case EOS_SET_BRAIN_MODEL:
    case EOS_SET_BRAIN_MAX:
    case EOS_SET_BRAIN_SYSTEM:
        // The five keys web/README.md marks "live", and this is what makes
        // that word true. The bridge applies the change between requests, so a
        // host edited while a reply is streaming takes effect on the next ask
        // and never mid-stream — which is why this is not "reboot" and also
        // why it is not instantaneous.
        //
        // All five are handed over together rather than one at a time.
        // eos_brain_bridge_configure() takes a whole config, and a patch that
        // changed the host and the model would otherwise apply the host
        // against the previous model for the length of one call.
        eos_brain_bridge_from_settings(s);
        return EOS_OK;

    // net.host and sys.autostart are already marked (reboot) by eos_settings:
    // the hostname went into eos_net_cfg_t before the radio came up, and
    // autostart is read once, when app_main decides which window has the
    // focus. Nothing to do here, and saying EOS_OK is what puts them in the
    // store so the next boot sees them.
    default:
        return EOS_OK;
    }
}

void eos_settings_bind_ports(eos_settings_store_t *st, const eos_board_t *b,
                             eos_theme_t *theme)
{
    eos_settings_ports_t p;

    if (!st) return;
    S.st    = st;
    S.b     = b;
    S.theme = theme;

    memset(&p, 0, sizeof p);
    p.wifi_set     = p_wifi_set;
    p.wifi_ssid    = p_wifi_ssid;
    p.wifi_psk_set = p_wifi_psk_set;
    p.apply        = p_apply;
    st->ports = p;
}

// ==========================================================================
// /api/settings
// ==========================================================================

static bool b_settings_get(void *ctx, int i, eos_httpd_kv_t *out)
{
    char val[EOS_HTTPD_VALUE_MAX];
    const char *name;
    int n;

    (void)ctx;
    if (!S.st || !out) return false;
    if (i < 0 || i >= EOS_SET_COUNT) return false;

    name = eos_settings_key_name(i);
    if (!name) return false;

    memset(out, 0, sizeof *out);

    // A write-only key is reported as a slot with no name, which the handler
    // skips. Returning false would end the enumeration at wifi.psk and lose
    // every key after it.
    n = eos_settings_get(S.st, i, val, (int)sizeof val);
    if (n < 0) return true;

    snprintf(out->key, sizeof out->key, "%s", name);
    switch (eos_settings_key_type(i)) {
    case EOS_SET_T_INT:
        out->type = EOS_HTTPD_VAL_INT;
        out->n    = strtol(val, NULL, 10);
        break;
    case EOS_SET_T_BOOL:
        out->type = EOS_HTTPD_VAL_BOOL;
        out->n    = (strcmp(val, "true") == 0) ? 1 : 0;
        break;
    default:
        out->type = EOS_HTTPD_VAL_STR;
        snprintf(out->s, sizeof out->s, "%s", val);
        break;
    }
    return true;
}

static int b_settings_set(void *ctx, const char *key, const char *val,
                          bool is_num, long num, bool *reboot)
{
    int k;

    (void)ctx;
    if (!S.st) return (int)EOS_ERR_UNSUPPORTED;

    k = eos_settings_key_of(key);
    if (k < 0) return (int)EOS_ERR_NOTFOUND;

    if (is_num) return (int)eos_settings_set_num(S.st, k, num, reboot);
    return (int)eos_settings_set_str(S.st, k, val, reboot);
}

static void b_settings_commit(void *ctx)
{
    (void)ctx;
    if (S.st) (void)eos_settings_route_commit(S.st);
}

// ==========================================================================
// /api/system
// ==========================================================================

static bool b_sys_info(void *ctx, eos_httpd_sys_t *o)
{
    const eos_board_t *b = S.b;
    const eos_display_info_t *di;
    esp_chip_info_t ci;
    uint8_t mac[6];
    time_t now;

    (void)ctx;
    if (!o || !b) return false;
    memset(o, 0, sizeof *o);

    snprintf(o->board_id,   sizeof o->board_id,   "%s", b->id   ? b->id   : "");
    snprintf(o->board_name, sizeof o->board_name, "%s", b->name ? b->name : "");
    // The registry has no summary field, so it is built from the two facts a
    // person uses to tell two boards apart on a bench: the panel and its size.
    snprintf(o->board_summary, sizeof o->board_summary, "%s %dx%d",
             eos_panel_name(b->panel.panel),
             (int)eos_board_screen_w(b), (int)eos_board_screen_h(b));

    esp_chip_info(&ci);
    snprintf(o->chip_target,  sizeof o->chip_target,  "%s", eos_soc_name(b->soc));
    snprintf(o->chip_variant, sizeof o->chip_variant, "%s",
             b->variant ? b->variant : eos_soc_name(b->soc));
    o->chip_cores = b->cores;
    o->chip_rev   = (uint8_t)(ci.revision / 100);
    o->flash_mb   = b->flash_bytes / (1024u * 1024u);
    o->psram_present = (b->psram_bytes != 0);
    o->psram_mb      = b->psram_bytes / (1024u * 1024u);
    snprintf(o->psram_type, sizeof o->psram_type, "%s", o->psram_present ? "spi" : "none");
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) memcpy(o->mac, mac, 6);

    o->render_tier = b->tier;
    snprintf(o->compositor, sizeof o->compositor, "%s", eos_comp_name(b->render.compositor));
    o->lvgl = b->render.lvgl;

    snprintf(o->disp_controller, sizeof o->disp_controller, "%s",
             eos_panel_name(b->panel.panel));
    o->disp_w        = eos_board_screen_w(b);
    o->disp_h        = eos_board_screen_h(b);
    o->disp_rotation = b->panel.rotation;
    snprintf(o->disp_bus, sizeof o->disp_bus,
             b->panel.bus == EOS_BUS_I2C ? "i2c" : "spi%u",
             (unsigned)(b->panel.spi_host + 1u));
    o->disp_clock_hz = b->panel.hz;
    di = eos_display_info();
    o->backlight = di ? ((di->caps & EOS_CAP_BACKLIGHT) != 0) : false;

    o->heap_free     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    o->heap_min_free = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    o->heap_largest  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    o->heap_total    = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_8BIT);

    o->uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);

    now = time(NULL);
    o->epoch = (uint32_t)now;
    // Before SNTP the RTC reads 1970 and reporting that as a synced clock is
    // how a file listing ends up claiming every file is 55 years old. Anything
    // before 2020 is a clock that has never been set.
    o->time_synced = (now > 1577836800L);
    if (S.st) snprintf(o->tz, sizeof o->tz, "%s", S.st->v.sys_tz);

    snprintf(o->fw_version, sizeof o->fw_version, "%s", EOS_FW_VERSION);
    snprintf(o->fw_idf,     sizeof o->fw_idf,     "%s", esp_get_idf_version());
    snprintf(o->fw_built,   sizeof o->fw_built,   "%s", EOS_FW_BUILT);

    // The one group the client changes behaviour on. chunk_max is the request
    // body this server will accept, not a number chosen here: a bigger one is
    // an allocation failure on a worker's stack rather than a slow upload.
    //
    // Every one of them is asked of eos_apps rather than restated here.
    // eos_apps.c is what enforces them: a chunk_max reported as 4096 by a
    // board that refuses anything over 512 turns every upload into a 413 that
    // looks like a network fault, and the only way that number can be wrong is
    // if two files hold it.
    o->chunk_max  = eos_apps_chunk_max();
    o->path_max   = eos_apps_path_max();
    o->name_max   = eos_apps_name_max();
    o->list_max   = eos_apps_list_max();
    o->open_files = eos_apps_open_files();
    return true;
}

static bool b_fs_info(void *ctx, int i, eos_httpd_fs_t *o)
{
    eos_mount_t m[EOS_MOUNT_MAX];
    uint64_t total = 0, used = 0;
    int n;

    (void)ctx;
    if (!o) return false;
    n = eos_storage_mounts(m, EOS_MOUNT_MAX);
    if (i < 0 || i >= n) return false;

    memset(o, 0, sizeof *o);
    snprintf(o->point, sizeof o->point, "%s", m[i].point ? m[i].point : "");
    snprintf(o->fs, sizeof o->fs, "%s",
             m[i].fs == EOS_FS_LITTLEFS ? "littlefs" :
             m[i].fs == EOS_FS_FAT      ? "fat" : "none");
    o->mounted   = m[i].mounted;
    o->writable  = m[i].writable;
    o->removable = m[i].removable;

    // usage() walks the block allocation, which is why eos_storage keeps it
    // separate from mounts(). /api/system is polled every five seconds by an
    // open Settings tab, and on a 960 KB LittleFS that walk is short enough to
    // pay for; on a card it would not be, which is why an unmounted mount is
    // never asked.
    if (m[i].mounted && eos_storage_usage(m[i].point, &total, &used) == EOS_OK) {
        o->total = total;
        o->used  = used;
    } else {
        o->total = m[i].total;
        o->used  = m[i].used;
    }
    return true;
}

// Armed here, acted on by the pump. Never restarts inside the handler: the
// response has not been written when this returns.
static int b_reboot(void *ctx, int in_ms)
{
    (void)ctx;
    if (in_ms < 0) in_ms = 0;
    S.reboot_at_ms  = (uint32_t)(esp_timer_get_time() / 1000) + (uint32_t)in_ms;
    S.reboot_armed  = true;
    ESP_LOGW(TAG, "reboot in %d ms, on request", in_ms);
    return 0;
}

// ==========================================================================
// /api/themes
// ==========================================================================

static void hex6(char *out, eos_rgb_t c)
{
    static const char H[] = "0123456789abcdef";
    out[0] = '#';
    out[1] = H[(c.r >> 4) & 0xF]; out[2] = H[c.r & 0xF];
    out[3] = H[(c.g >> 4) & 0xF]; out[4] = H[c.g & 0xF];
    out[5] = H[(c.b >> 4) & 0xF]; out[6] = H[c.b & 0xF];
    out[7] = '\0';
}

static bool b_theme_active(void *ctx, eos_httpd_theme_t *o)
{
    int i;

    (void)ctx;
    if (!o || !S.theme) return false;
    memset(o, 0, sizeof *o);

    // The name is the SETTINGS name, not theme->name. The picker matches on it
    // and the loader builds a path from it, so it has to be the file stem; the
    // "name" inside the file is the theme author's label and the two are free
    // to disagree. eos_settings only ever holds a stem that loaded, because a
    // stem that did not is rolled back by the store.
    snprintf(o->name, sizeof o->name, "%s",
             S.st && S.st->v.ui_theme[0] ? S.st->v.ui_theme : S.theme->name);
    if (S.b && S.b->storage.int_point)
        snprintf(o->path, sizeof o->path, "%s/%s/%s.json",
                 S.b->storage.int_point, THEME_DIR, o->name);

    o->has_colors = true;
    for (i = 0; i < EOS_HTTPD_THEME_ROLES && i < EOS_ROLE_COUNT; i++)
        hex6(o->color[i], eos_theme_role_rgb(S.theme, (eos_role_t)i));

    o->gap    = S.theme->m.gap;
    o->border = S.theme->m.border;
    o->bar_h  = S.theme->m.bar_h;
    o->tab_h  = S.theme->m.tab_h;
    o->radius = S.theme->m.radius;
    return true;
}

// Adds every "*.json" under <mount>/themes to the snapshot, by stem.
static void scan_themes(const char *mount)
{
    char dir[EOS_HTTPD_PATH_MAX];
    eos_dirh_t *d;
    eos_dirent_t ent;

    if (!mount) return;
    snprintf(dir, sizeof dir, "%s/%s", mount, THEME_DIR);

    d = eos_storage_opendir(dir);
    if (!d) return;      // no themes directory is the normal case, not an error

    while (S.tcount < THEME_LIST_MAX && eos_storage_readdir(d, &ent)) {
        size_t n = strlen(ent.name);
        int j;
        bool dup = false;

        if (ent.is_dir) continue;
        if (n < 6 || strcmp(ent.name + n - 5, ".json") != 0) continue;
        n -= 5;
        if (n >= EOS_SETTINGS_THEME_MAX) continue;   // a stem no setting can hold

        for (j = 0; j < S.tcount; j++)
            if (strncmp(S.tnames[j], ent.name, n) == 0 && S.tnames[j][n] == '\0')
                dup = true;                          // same stem on both mounts
        if (dup) continue;

        // The full path has to fit EOS_PATH_MAX or the theme cannot be opened
        // anyway, so a name that would truncate is skipped rather than listed
        // under a path that names a different file.
        if (strlen(dir) + 1 + strlen(ent.name) >= sizeof S.tpaths[0]) continue;

        memcpy(S.tnames[S.tcount], ent.name, n);
        S.tnames[S.tcount][n] = '\0';
        memcpy(S.tpaths[S.tcount], dir, strlen(dir));
        S.tpaths[S.tcount][strlen(dir)] = '/';
        memcpy(S.tpaths[S.tcount] + strlen(dir) + 1, ent.name, strlen(ent.name) + 1);
        S.tcount++;
    }
    eos_storage_closedir(d);
}

static bool b_theme_list(void *ctx, int i, eos_httpd_theme_t *o)
{
    (void)ctx;
    if (!o) return false;

    // The scan is taken once, at the top of the response. It holds one of the
    // two directory handles in the whole OS for the length of one readdir walk
    // and gives it straight back — a scan left open across the response would
    // be a handle held for as long as a phone took to read the reply.
    if (i == 0) {
        S.tcount = 0;
        if (S.b) {
            if (S.b->storage.sd && S.b->storage.sd_point) scan_themes(S.b->storage.sd_point);
            scan_themes(S.b->storage.int_point);
        }
    }
    if (i < 0 || i >= S.tcount) return false;

    memset(o, 0, sizeof *o);
    snprintf(o->name, sizeof o->name, "%s", S.tnames[i]);
    snprintf(o->path, sizeof o->path, "%s", S.tpaths[i]);
    o->has_colors = false;
    return true;
}

// ==========================================================================
// Bind and pump
// ==========================================================================

// eos_httpd restates the sixteen role names rather than including eos_theme.h
// into the portable half. That is only safe while the two lists agree, and the
// order is what makes color[i] mean anything at all.
_Static_assert(EOS_HTTPD_THEME_ROLES == EOS_ROLE_COUNT,
               "eos_httpd_role_names has drifted from eos_role_t");

void eos_settings_bind(eos_httpd_t *h, eos_settings_store_t *st,
                       const eos_board_t *b, eos_theme_t *theme, void *net)
{
    if (!h) return;

    S.h     = h;
    S.st    = st;
    S.b     = b;
    S.theme = theme;
    S.net   = (eos_net_t *)net;

    // Assigned one at a time, not as a whole table: eos_httpd_idf_bind() has
    // already filled in the radios and eos_web_embed the files, and copying a
    // fresh eos_httpd_ports_t over the top would silently unbind both.
    h->ports.settings_get    = st ? b_settings_get : NULL;
    h->ports.settings_set    = st ? b_settings_set : NULL;
    h->ports.settings_commit = st ? b_settings_commit : NULL;
    h->ports.sys_info        = b_sys_info;
    h->ports.fs_info         = b_fs_info;
    h->ports.reboot          = b_reboot;
    h->ports.theme_active    = theme ? b_theme_active : NULL;
    h->ports.theme_list      = b_theme_list;
}

bool eos_settings_bind_take_redraw(void)
{
    bool r = S.redraw;
    S.redraw = false;
    return r;
}

void eos_settings_bind_pump(uint32_t now_ms)
{
    if (S.st) eos_settings_pump(S.st, now_ms);

    if (S.reboot_armed && (int32_t)(now_ms - S.reboot_at_ms) >= 0) {
        S.reboot_armed = false;
        // The settings the request may have changed go out first. This is the
        // one place a flush is not debounced, because there is no later.
        if (S.st) (void)eos_settings_flush(S.st);
        ESP_LOGW(TAG, "restarting");
        esp_restart();
    }
}
