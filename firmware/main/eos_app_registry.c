// The table, and the four calls the OS loop makes into it. See
// eos_app_registry.h for why the list is a table and not a switch.

#include "eos_app_registry.h"
#include "eos_shell_draw.h"
#include "eos_led.h"

#include <string.h>

// ============================================================== THE TABLE
//
// Adding a window is this row and the function it names. Nothing else: the tab
// label, the launcher entry, /api/apps and sys.autostart all read it from
// here, and the host suite fails the build if two rows claim the same id or a
// row has no draw function.
//
// `name` is the tab label and is deliberately short. A collapsed group of
// eight tabs on this panel gives each cell fourteen pixels — two characters of
// the 6x8 face — so anything longer than one word is going to be cut, and a
// cut word reads as a rendering bug rather than as a long name.
//
// `summary` is a sentence and is never drawn on the glass. It is what the
// launcher's second column and the web app's picker show.

static const eos_app_t APPS[EOS_APP_COUNT] = {
    { "clock", "clock", "uptime, in the large face",
      0, eos_app_draw_clock,    NULL },

    { "board", "board", "what this board is, or its address once it has joined",
      0, eos_app_draw_board,    NULL },

    { "heap",  "heap",  "free heap and largest block, live",
      0, eos_app_draw_heap,     NULL },

    { "keys",  "keys",  "the compiled-in keymap",
      0, eos_app_draw_keys,     NULL },

    { "buddy", "buddy", "Pip, and the mood megabrain has put him in",
      0, eos_app_draw_buddy,    NULL },

    { "chat",  "chat",  "ask megabrain, and watch the reply arrive",
      0, eos_app_draw_chat,     eos_app_chat_key },

    { "settings", "settings", "theme, brightness, board and megabrain. read-only here.",
      0, eos_app_draw_settings, NULL },

    { "files", "files", "browse the internal filesystem. read-only here.",
      0, eos_app_draw_files,    eos_app_files_key },

    // Named for what the hardware is. This board has a WS2812 and no speaker,
    // and a window called "media" that a person opens expecting audio is a
    // promise the silicon cannot keep, so the summary says light in its first
    // four words.
    { "media", "media", "the RGB LED: colour, brightness and effects. no audio on this board.",
      0, eos_app_draw_media,    eos_app_media_key },

    { "party", "party", "the demo: Pip dancing, the LED cycling, the colours moving",
      0, eos_app_draw_party,    eos_app_party_key },

    { "camera", "camera", "a viewfinder onto a penguinOS camera node",
      0, eos_app_draw_camera,   NULL },
};

// ============================================================ the lookups

int eos_app_count(void) { return EOS_APP_COUNT; }

const eos_app_t *eos_app_at(int i)
{
    if (i < 0 || i >= EOS_APP_COUNT) return NULL;
    return &APPS[i];
}

int eos_app_index_of(const char *id)
{
    int i;
    if (!id || !id[0]) return -1;
    for (i = 0; i < EOS_APP_COUNT; i++)
        if (strcmp(APPS[i].id, id) == 0) return i;
    return -1;
}

const eos_app_t *eos_app_by_id(const char *id)
{
    int i = eos_app_index_of(id);
    return i < 0 ? NULL : &APPS[i];
}

bool eos_app_table_ok(void)
{
    int i, j;

    for (i = 0; i < EOS_APP_COUNT; i++) {
        if (!APPS[i].id || !APPS[i].id[0]) return false;
        if (!APPS[i].name || !APPS[i].name[0]) return false;
        if (!APPS[i].summary) return false;
        if (!APPS[i].draw) return false;
        for (j = 0; j < i; j++)
            if (strcmp(APPS[i].id, APPS[j].id) == 0) return false;
    }
    return true;
}

// ================================================================ the helper

int16_t eos_app_text(int16_t x, int16_t y, const eos_font_t *f,
                     eos_color_t c, const char *s, int16_t max_w)
{
    int n;
    if (!f || !s || max_w <= 0) return 0;
    n = eos_text_fit(f, s, -1, (int)max_w);
    if (n <= 0) return 0;
    return (int16_t)eos_display_text(x, y, f, c, s, n);
}

// ================================================================== wiring

// Whether anything on the glass is mid-animation. Written by the tick, read by
// the loop when it decides how long to sleep. Two flags rather than one call
// into each window, because the answer has to be cheap: the loop asks it on
// every pass.
static bool s_fast;
static bool s_led_shown;

static const eos_settings_t *s_set;
static const eos_board_t    *s_board;
static eos_buddy_t          *s_buddy;

const eos_settings_t *eos_app_settings(void) { return s_set; }
const eos_board_t    *eos_app_board(void)    { return s_board; }

void eos_app_bind(eos_httpd_t *h, const eos_settings_t *set,
                  const eos_board_t *b, eos_buddy_t *buddy)
{
    s_set   = set;
    s_board = b;
    s_buddy = buddy;
    eos_app_chat_bind(h);
    // Claims the RMT channel, and answers NODEV on a board with no WS2812 —
    // which is not a failure. The Media window draws "no LED on this board"
    // and the Party window drops the light half of its demo.
    eos_led_init(b);
}

// ---------------------------------------------------------------- the tick

// Which apps have a visible tile on the current workspace, answered with ONE
// layout pass rather than one per app. eos_shell_app_visible() would run the
// whole tree per question and there are ten questions.
static void visible_set(const eos_shell_view_t *v, bool *out)
{
    eos_tile_t tiles[EOS_MAX_WINDOWS * 2];
    const eos_display_info_t *info;
    int n, i;

    memset(out, 0, EOS_APP_COUNT * sizeof(bool));
    if (!v || !v->wm) return;

    info = eos_display_info();
    n = eos_wm_layout(v->wm, eos_rect(0, 0, info->w, info->h),
                      tiles, EOS_MAX_WINDOWS * 2);
    for (i = 0; i < n; i++)
        if (tiles[i].visible && tiles[i].app_id < EOS_APP_COUNT)
            out[tiles[i].app_id] = true;
}

void eos_app_tick(const eos_shell_view_t *v, uint32_t now_ms)
{
    bool vis[EOS_APP_COUNT];

    visible_set(v, vis);

    // Chat drains whether or not it is on the glass. The relay's ring is a
    // kilobyte and the brain task STOPS PUMPING when it fills, so a chat window
    // that only drained while visible would leave a reply wedged against a
    // switched-off consumer and a megabrain socket held open behind it.
    eos_app_chat_tick(now_ms);

    // Files does its directory scan only while it is being looked at. A
    // LittleFS readdir is flash reads with the instruction cache off, and
    // paying for one every 250 ms for a window behind a tab is a stall nobody
    // asked for.
    eos_app_files_tick(vis[EOS_APP_FILES], now_ms);

    // Party takes the LED and the buddy's mood while it is on screen and hands
    // both back when it is not, which is why it is told rather than asked.
    eos_app_party_tick(vis[EOS_APP_PARTY], now_ms, s_buddy);

    // The camera fetches from another board over the network, and does it
    // inside its draw call - so it is told whether anyone is looking before
    // it asks for the redraw that triggers the fetch.
    eos_app_camera_tick(vis[EOS_APP_CAMERA], now_ms);

    // And the light, once, after both windows have had their say. It writes
    // nothing when the colour has not moved, so a SOLID effect costs one
    // comparison per pass and no RMT traffic at all.
    eos_led_tick(now_ms);

    s_fast      = vis[EOS_APP_PARTY];
    s_led_shown = vis[EOS_APP_MEDIA];
}

bool eos_app_damage(const eos_shell_view_t *v)
{
    bool any = false;

    // Each take_dirty() is called unconditionally, because it CLEARS the flag.
    // Guarding it on visibility would leave a window that streamed while it was
    // behind a tab permanently marked dirty.
    if (eos_app_chat_take_dirty()  && eos_shell_damage_app(v, EOS_APP_CHAT))  any = true;
    if (eos_app_files_take_dirty() && eos_shell_damage_app(v, EOS_APP_FILES)) any = true;
    if (eos_app_media_take_dirty() && eos_shell_damage_app(v, EOS_APP_MEDIA)) any = true;
    if (eos_app_party_active()     && eos_shell_damage_app(v, EOS_APP_PARTY)) any = true;
    if (eos_app_camera_take_dirty() && eos_shell_damage_app(v, EOS_APP_CAMERA)) any = true;
    return any;
}

bool eos_app_key(uint16_t app_id, const eos_event_t *e)
{
    const eos_app_t *a = eos_app_at((int)app_id);
    if (!a || !a->key || !e) return false;
    return a->key(e);
}

bool eos_app_wants_fast(void)
{
    eos_led_state_t st;

    // The party, and any window whose light is mid-effect. The loop normally
    // sleeps 250 ms and drops to 100 ms while the avatar is visible; a strobe
    // sampled four times a second is not a strobe, and a rainbow stepped four
    // times a second is a slideshow. This is what lets those two windows ask
    // for the same rate the buddy already gets, and only while they are up.
    if (s_fast) return true;
    eos_led_get(&st);
    return s_led_shown && eos_led_fx_animated(st.fx);
}

uint32_t eos_app_bss_bytes(void)
{
    return eos_app_chat_bytes() + eos_app_files_bytes();
}
