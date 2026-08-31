#include "eos_keys.h"
#include <string.h>
#include <stdio.h>

// ------------------------------------------------------------------- input

// Left and right sides of a modifier fold together: the table stores, and
// compares, whole groups. Running this over an already-collapsed byte is a
// no-op, so every entry point can call it without knowing where the byte
// came from.
uint8_t eos_keys_mods_from_hid(uint8_t hid_mods)
{
    uint8_t m = 0;
    if (hid_mods & EOS_MOD_CTRL)  m |= EOS_MOD_CTRL;
    if (hid_mods & EOS_MOD_SHIFT) m |= EOS_MOD_SHIFT;
    if (hid_mods & EOS_MOD_ALT)   m |= EOS_MOD_ALT;
    if (hid_mods & EOS_MOD_SUPER) m |= EOS_MOD_SUPER;
    return m;
}

// ---------------------------------------------------------------- name maps

typedef struct { const char *name; uint16_t key; } keyname_t;

// Letters and digits are computed, so this only carries the rest. The first
// spelling of a key is the canonical one - eos_keys_format prints that.
static const keyname_t KEYNAMES[] = {
    { "return",    EOS_KEY_ENTER    }, { "enter",  EOS_KEY_ENTER },
    { "escape",    EOS_KEY_ESC    }, { "esc",    EOS_KEY_ESC },
    { "space",     EOS_KEY_SPACE     },
    { "tab",       EOS_KEY_TAB       },
    { "backspace", EOS_KEY_BKSP },
    { "delete",    EOS_KEY_DELETE    }, { "del",    EOS_KEY_DELETE },
    { "insert",    EOS_KEY_INSERT    },
    { "home",      EOS_KEY_HOME      },
    { "end",       EOS_KEY_END       },
    { "pageup",    EOS_KEY_PGUP    },
    { "pagedown",  EOS_KEY_PGDN  },
    { "minus",     EOS_KEY_MINUS     },
    { "equal",     EOS_KEY_EQUAL     },
    { "comma",     EOS_KEY_COMMA     },
    { "period",    EOS_KEY_PERIOD    },
    { "slash",     EOS_KEY_SLASH     },
    { "semicolon", EOS_KEY_SEMICOLON },
    { "quote",     EOS_KEY_QUOTE     },
    { "grave",     EOS_KEY_GRAVE     },
    { "backslash", EOS_KEY_BACKSLASH },
    { "bracketleft",  EOS_KEY_LBRACKET },
    { "bracketright", EOS_KEY_RBRACKET },
    { "left",  EOS_KEY_LEFT  },
    { "right", EOS_KEY_RIGHT },
    { "up",    EOS_KEY_UP    },
    { "down",  EOS_KEY_DOWN  },
    { "capslock", EOS_KEY_CAPSLOCK },
    { "f1", EOS_KEY_F1 }, { "f2",  EOS_KEY_F2  }, { "f3",  EOS_KEY_F3  }, { "f4",  EOS_KEY_F4  },
    { "f5", EOS_KEY_F5 }, { "f6",  EOS_KEY_F6  }, { "f7",  EOS_KEY_F7  }, { "f8",  EOS_KEY_F8  },
    { "f9", EOS_KEY_F9 }, { "f10", EOS_KEY_F10 }, { "f11", EOS_KEY_F11 }, { "f12", EOS_KEY_F12 },
    { "boot",  EOS_KEY_BOOT  },   // the BOOT/IO0 button, and the board's own keys
    { "user1", EOS_KEY_USER1 }, { "user2", EOS_KEY_USER2 }, { "user3", EOS_KEY_USER3 }
};
#define NKEYNAMES ((int)(sizeof(KEYNAMES) / sizeof(KEYNAMES[0])))

static const char *ACTNAMES[EOS_ACT__COUNT] = {
    "none", "spawn", "close", "split_cols", "split_rows",
    "focus_left", "focus_right", "focus_up", "focus_down",
    "move_left", "move_right", "move_up", "move_down",
    "workspace", "move_to_workspace", "tab_next", "resize",
    "launcher", "toggle_bar", "cycle_theme", "lock"
};

const char *eos_keys_action_name(eos_action_t a)
{
    int i = (int)a;
    return (i >= 0 && i < EOS_ACT__COUNT) ? ACTNAMES[i] : "?";
}

const char *eos_keys_err_str(eos_keys_err_t e)
{
    switch (e) {
    case EOS_KEYS_OK:          return "ok";
    case EOS_KEYS_ERR_SYNTAX:  return "malformed json";
    case EOS_KEYS_ERR_KEYNAME: return "unknown key name";
    case EOS_KEYS_ERR_ACTION:  return "unknown action";
    case EOS_KEYS_ERR_FULL:    return "bind table full";
    case EOS_KEYS_ERR_IO:      return "cannot read file";
    case EOS_KEYS_ERR_TOO_BIG: return "file larger than scratch buffer";
    }
    return "?";
}

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static uint16_t key_from_name(const char *n)
{
    if (n[0] && !n[1]) {
        char c = n[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') return (uint16_t)(EOS_KEY_A + (c - 'a'));
        if (c >= '1' && c <= '9') return (uint16_t)(EOS_KEY_1 + (c - '1'));
        if (c == '0')             return EOS_KEY_0;
    }
    for (int i = 0; i < NKEYNAMES; i++)
        if (streq(KEYNAMES[i].name, n)) return KEYNAMES[i].key;
    return EOS_KEY_NONE;
}

static void key_to_name(uint16_t k, char *out, int n)
{
    if (k >= EOS_KEY_A && k <= EOS_KEY_Z) { snprintf(out, (size_t)n, "%c", 'a' + (k - EOS_KEY_A)); return; }
    if (k >= EOS_KEY_1 && k <= EOS_KEY_9) { snprintf(out, (size_t)n, "%c", '1' + (k - EOS_KEY_1)); return; }
    if (k == EOS_KEY_0)                   { snprintf(out, (size_t)n, "0"); return; }
    for (int i = 0; i < NKEYNAMES; i++)
        if (KEYNAMES[i].key == k) { snprintf(out, (size_t)n, "%s", KEYNAMES[i].name); return; }
    snprintf(out, (size_t)n, "0x%02x", (unsigned)k);
}

bool eos_keys_parse_chord(const char *s, uint8_t *mods, uint16_t *key)
{
    char tok[24];
    int  ti = 0;
    *mods = 0;
    *key  = EOS_KEY_NONE;

    for (;;) {
        char c = *s;
        if (c != '+' && c != 0) {
            if (ti < (int)sizeof(tok) - 1) {
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                tok[ti++] = c;
            }
            s++;
            continue;
        }
        tok[ti] = 0;
        if (ti > 0) {
            if (streq(tok, "super") || streq(tok, "mod") || streq(tok, "win") ||
                streq(tok, "cmd")   || streq(tok, "gui")) {
                *mods |= EOS_MOD_SUPER;
            } else if (streq(tok, "shift")) {
                *mods |= EOS_MOD_SHIFT;
            } else if (streq(tok, "ctrl") || streq(tok, "control")) {
                *mods |= EOS_MOD_CTRL;
            } else if (streq(tok, "alt") || streq(tok, "opt")) {
                *mods |= EOS_MOD_ALT;
            } else {
                if (*key != EOS_KEY_NONE) return false;   // two non-modifiers
                *key = key_from_name(tok);
                if (*key == EOS_KEY_NONE) return false;
            }
        }
        ti = 0;
        if (c == 0) break;
        s++;
    }
    return *key != EOS_KEY_NONE;
}

int eos_keys_format(const eos_keybind_t *b, char *out, int n)
{
    char kn[16];
    if (!out || n <= 0) return 0;
    key_to_name(b->key, kn, (int)sizeof(kn));
    int want = snprintf(out, (size_t)n, "%s%s%s%s%s",
                        (b->mods & EOS_MOD_SUPER) ? "super+" : "",
                        (b->mods & EOS_MOD_CTRL)  ? "ctrl+"  : "",
                        (b->mods & EOS_MOD_ALT)   ? "alt+"   : "",
                        (b->mods & EOS_MOD_SHIFT) ? "shift+" : "",
                        kn);
    // snprintf reports what it WANTED to write. Callers advance a cursor by
    // this, so hand back what actually landed in the buffer.
    if (want < 0) { out[0] = 0; return 0; }
    return (want < n) ? want : n - 1;
}

// ------------------------------------------------------------------- table

void eos_keys_clear(eos_keymap_t *km) { memset(km, 0, sizeof(*km)); }

const eos_keybind_t *eos_keys_lookup(const eos_keymap_t *km, uint8_t mods, uint16_t key)
{
    mods = eos_keys_mods_from_hid(mods);
    for (int i = 0; i < km->count; i++)
        if (km->binds[i].mods == mods && km->binds[i].key == key)
            return &km->binds[i];
    return NULL;
}

bool eos_keys_bind(eos_keymap_t *km, uint8_t mods, uint16_t key,
                   eos_action_t action, int16_t arg)
{
    mods = eos_keys_mods_from_hid(mods);
    for (int i = 0; i < km->count; i++) {
        if (km->binds[i].mods != mods || km->binds[i].key != key) continue;
        if (action == EOS_ACT_NONE) {                 // unbind: close the hole
            for (int j = i; j + 1 < km->count; j++) km->binds[j] = km->binds[j + 1];
            km->count--;
        } else {
            km->binds[i].action = (uint8_t)action;
            km->binds[i].arg    = arg;
        }
        return true;
    }
    if (action == EOS_ACT_NONE) return true;          // unbinding what is not bound
    if (km->count >= EOS_MAX_BINDS) return false;
    km->binds[km->count].mods   = mods;
    km->binds[km->count].key    = key;
    km->binds[km->count].action = (uint8_t)action;
    km->binds[km->count].arg    = arg;
    km->count++;
    return true;
}

// The compiled-in table. Omarchy/Hyprland muscle memory, with one deviation:
// super+h is focus-left, so forcing the next split direction sits on
// super+ctrl+h and super+ctrl+v. Rebind either in keys.json if you want the
// i3 spelling back.
void eos_keys_defaults(eos_keymap_t *km)
{
    static const uint16_t HJKL[4]  = { EOS_KEY_H, EOS_KEY_J, EOS_KEY_K, EOS_KEY_L };
    static const uint16_t ARROW[4] = { EOS_KEY_LEFT, EOS_KEY_DOWN, EOS_KEY_UP, EOS_KEY_RIGHT };
    static const uint8_t  FOCUS[4] = { EOS_ACT_FOCUS_LEFT, EOS_ACT_FOCUS_DOWN,
                                       EOS_ACT_FOCUS_UP,   EOS_ACT_FOCUS_RIGHT };
    static const uint8_t  MOVE[4]  = { EOS_ACT_MOVE_LEFT,  EOS_ACT_MOVE_DOWN,
                                       EOS_ACT_MOVE_UP,    EOS_ACT_MOVE_RIGHT };
    const uint8_t S  = EOS_MOD_SUPER;
    const uint8_t SS = EOS_MOD_SUPER | EOS_MOD_SHIFT;
    const uint8_t SC = EOS_MOD_SUPER | EOS_MOD_CTRL;

    eos_keys_clear(km);

    eos_keys_bind(km, S, EOS_KEY_ENTER, EOS_ACT_SPAWN, 0);   // app 0 = terminal
    eos_keys_bind(km, S, EOS_KEY_Q,      EOS_ACT_CLOSE, 0);

    eos_keys_bind(km, SC, EOS_KEY_H, EOS_ACT_SPLIT_COLS, 0);
    eos_keys_bind(km, SC, EOS_KEY_V, EOS_ACT_SPLIT_ROWS, 0);

    for (int i = 0; i < 4; i++) {
        eos_keys_bind(km, S,  HJKL[i],  (eos_action_t)FOCUS[i], 0);
        eos_keys_bind(km, S,  ARROW[i], (eos_action_t)FOCUS[i], 0);
        eos_keys_bind(km, SS, HJKL[i],  (eos_action_t)MOVE[i],  0);
        eos_keys_bind(km, SS, ARROW[i], (eos_action_t)MOVE[i],  0);
    }

    for (int i = 0; i < EOS_WORKSPACES; i++) {
        uint16_t k = (uint16_t)(EOS_KEY_1 + i);
        eos_keys_bind(km, S,  k, EOS_ACT_WORKSPACE, (int16_t)i);
        eos_keys_bind(km, SS, k, EOS_ACT_MOVE_TO_WS, (int16_t)i);
    }

    eos_keys_bind(km, S, EOS_KEY_TAB,    EOS_ACT_TAB_NEXT, 0);
    eos_keys_bind(km, S, EOS_KEY_MINUS,  EOS_ACT_RESIZE, -50);
    eos_keys_bind(km, S, EOS_KEY_EQUAL,  EOS_ACT_RESIZE,  50);
    eos_keys_bind(km, S, EOS_KEY_SPACE,  EOS_ACT_LAUNCHER, 0);
    eos_keys_bind(km, S, EOS_KEY_B,      EOS_ACT_TOGGLE_BAR, 0);
    eos_keys_bind(km, S, EOS_KEY_T,      EOS_ACT_CYCLE_THEME, 0);
    eos_keys_bind(km, S, EOS_KEY_ESC, EOS_ACT_LOCK, 0);
}

// -------------------------------------------------------------------- json

typedef struct { const char *s; int i; eos_keys_err_t err; } jp_t;

static void jp_ws(jp_t *p) { while (p->s[p->i] == ' ' || p->s[p->i] == '\t' ||
                                    p->s[p->i] == '\n' || p->s[p->i] == '\r') p->i++; }

static bool jp_eat(jp_t *p, char c)
{
    jp_ws(p);
    if (p->s[p->i] != c) { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
    p->i++;
    return true;
}

static bool jp_string(jp_t *p, char *out, int n)
{
    jp_ws(p);
    if (p->s[p->i] != '"') { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
    p->i++;
    int o = 0;
    for (;;) {
        char c = p->s[p->i];
        if (c == 0) { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
        p->i++;
        if (c == '"') break;
        if (c == '\\') {
            char e = p->s[p->i];
            if (e == 0) { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
            p->i++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = e;    break;    // \" \\ \/ and anything else, verbatim
            }
        }
        if (o < n - 1) out[o++] = c;
    }
    out[o] = 0;
    return true;
}

static bool jp_int(jp_t *p, int *out)
{
    jp_ws(p);
    int sign = 1, got = 0;
    long v = 0;
    if (p->s[p->i] == '-') { sign = -1; p->i++; }
    else if (p->s[p->i] == '+') p->i++;
    while (p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        v = v * 10 + (p->s[p->i] - '0');
        if (v > 100000L) v = 100000L;
        p->i++;
        got = 1;
    }
    if (!got) { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
    *out = (int)(sign * v);
    return true;
}

static bool jp_skip_value(jp_t *p, int depth)
{
    char buf[8];
    jp_ws(p);
    char c = p->s[p->i];
    if (depth > 8) { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
    if (c == '"') { char t[32]; return jp_string(p, t, (int)sizeof(t)); }
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        p->i++;
        for (;;) {
            jp_ws(p);
            if (p->s[p->i] == close) { p->i++; return true; }
            if (p->s[p->i] == 0) { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (c == '{') {                       // "name": value
                char t[32];
                if (!jp_string(p, t, (int)sizeof(t))) return false;
                if (!jp_eat(p, ':')) return false;
            }
            if (!jp_skip_value(p, depth + 1)) return false;
        }
    }
    if (c == 't' || c == 'f' || c == 'n') {       // true / false / null
        int o = 0;
        while (p->s[p->i] >= 'a' && p->s[p->i] <= 'z' && o < (int)sizeof(buf) - 1)
            buf[o++] = p->s[p->i++];
        buf[o] = 0;
        if (streq(buf, "true") || streq(buf, "false") || streq(buf, "null")) return true;
        p->err = EOS_KEYS_ERR_SYNTAX;
        return false;
    }
    { int v; return jp_int(p, &v); }
}

// One { "keys": ..., "action": ..., "arg": ... } object, applied to `km`.
static bool jp_bind(jp_t *p, eos_keymap_t *km)
{
    char keys[32] = {0}, act[32] = {0};
    int  arg = 0;
    bool have_keys = false, have_act = false;

    if (!jp_eat(p, '{')) return false;
    for (;;) {
        jp_ws(p);
        if (p->s[p->i] == '}') { p->i++; break; }
        if (p->s[p->i] == ',') { p->i++; continue; }
        char name[32];
        if (!jp_string(p, name, (int)sizeof(name))) return false;
        if (!jp_eat(p, ':')) return false;
        if (streq(name, "keys") || streq(name, "bind")) {
            if (!jp_string(p, keys, (int)sizeof(keys))) return false;
            have_keys = true;
        } else if (streq(name, "action")) {
            if (!jp_string(p, act, (int)sizeof(act))) return false;
            have_act = true;
        } else if (streq(name, "arg")) {
            if (!jp_int(p, &arg)) return false;
        } else if (!jp_skip_value(p, 0)) {
            return false;
        }
    }
    if (!have_keys || !have_act) { p->err = EOS_KEYS_ERR_SYNTAX; return false; }

    uint8_t  mods;
    uint16_t key;
    if (!eos_keys_parse_chord(keys, &mods, &key)) { p->err = EOS_KEYS_ERR_KEYNAME; return false; }

    eos_action_t a = EOS_ACT__COUNT;
    for (int i = 0; i < EOS_ACT__COUNT; i++)
        if (streq(ACTNAMES[i], act)) { a = (eos_action_t)i; break; }
    if (a == EOS_ACT__COUNT) { p->err = EOS_KEYS_ERR_ACTION; return false; }

    if (arg < -32768) arg = -32768;
    if (arg >  32767) arg =  32767;
    if (!eos_keys_bind(km, mods, key, a, (int16_t)arg)) {
        p->err = EOS_KEYS_ERR_FULL;
        return false;
    }
    return true;
}

static bool jp_bind_array(jp_t *p, eos_keymap_t *km, int *applied)
{
    if (!jp_eat(p, '[')) return false;
    for (;;) {
        jp_ws(p);
        if (p->s[p->i] == ']') { p->i++; return true; }
        if (p->s[p->i] == ',') { p->i++; continue; }
        if (p->s[p->i] != '{') { p->err = EOS_KEYS_ERR_SYNTAX; return false; }
        if (!jp_bind(p, km)) return false;
        (*applied)++;
    }
}

eos_keys_load_t eos_keys_load_json(eos_keymap_t *km, const char *json)
{
    eos_keys_load_t r = { EOS_KEYS_OK, 0, 0 };
    eos_keymap_t    tmp = *km;              // commit only if the whole file parses
    jp_t p = { json ? json : "", 0, EOS_KEYS_OK };
    bool ok = true;

    jp_ws(&p);
    if (p.s[p.i] == '[') {
        ok = jp_bind_array(&p, &tmp, &r.applied);
    } else if (p.s[p.i] == '{') {
        p.i++;
        bool seen = false;
        for (;;) {
            jp_ws(&p);
            if (p.s[p.i] == '}') { p.i++; break; }
            if (p.s[p.i] == ',') { p.i++; continue; }
            char name[32];
            if (!(ok = jp_string(&p, name, (int)sizeof(name)))) break;
            if (!(ok = jp_eat(&p, ':'))) break;
            if (streq(name, "binds")) {
                seen = true;
                if (!(ok = jp_bind_array(&p, &tmp, &r.applied))) break;
            } else if (!(ok = jp_skip_value(&p, 0))) {
                break;
            }
        }
        if (ok && !seen) { p.err = EOS_KEYS_ERR_SYNTAX; ok = false; }
    } else {
        p.err = EOS_KEYS_ERR_SYNTAX;
        ok = false;
    }
    if (ok) { jp_ws(&p); if (p.s[p.i] != 0) { p.err = EOS_KEYS_ERR_SYNTAX; ok = false; } }

    r.offset = p.i;
    if (!ok) { r.err = (p.err == EOS_KEYS_OK) ? EOS_KEYS_ERR_SYNTAX : p.err; return r; }
    *km = tmp;
    return r;
}

eos_keys_load_t eos_keys_load_file(eos_keymap_t *km, const char *path,
                                   char *scratch, int scratch_len)
{
    eos_keys_load_t r = { EOS_KEYS_OK, 0, 0 };
    FILE *f;
    size_t n;

    if (!scratch || scratch_len < 2) { r.err = EOS_KEYS_ERR_TOO_BIG; return r; }
    f = fopen(path, "rb");
    if (!f) { r.err = EOS_KEYS_ERR_IO; return r; }

    n = fread(scratch, 1, (size_t)scratch_len - 1, f);
    if (ferror(f)) { fclose(f); r.err = EOS_KEYS_ERR_IO; return r; }
    // A full buffer with more still to come means the file does not fit.
    if (n == (size_t)scratch_len - 1 && fgetc(f) != EOF) {
        fclose(f);
        r.err = EOS_KEYS_ERR_TOO_BIG;
        return r;
    }
    fclose(f);
    scratch[n] = 0;
    return eos_keys_load_json(km, scratch);
}

// ---------------------------------------------------------------- dispatch

void eos_shell_state_init(eos_shell_state_t *st, uint8_t theme_count)
{
    memset(st, 0, sizeof(*st));
    st->bar_visible = true;
    st->theme_count = theme_count ? theme_count : 1;
}

// eos_wm has no swap primitive and must not grow one, so a move is done here:
// focus the neighbour the normal way, then exchange the two leaves' window
// ids. The tree shape, the ratios and the tab groups are all untouched -
// only which window sits in which hole changes.
static bool move_focused(eos_wm_t *wm, eos_dir_t dir, eos_rect_t screen)
{
    int me = wm->focus;
    if (me < 0 || me >= EOS_MAX_WINDOWS || !wm->win[me].alive) return false;
    if (!eos_wm_focus_dir(wm, dir, screen)) return false;

    int other = wm->focus;
    if (other == me) return false;

    int na = wm->win[me].node;
    int nb = wm->win[other].node;
    wm->nodes[na].win   = (int16_t)other;
    wm->nodes[nb].win   = (int16_t)me;
    wm->win[me].node    = (int16_t)nb;
    wm->win[other].node = (int16_t)na;

    eos_wm_focus_win(wm, me);      // focus follows the window that moved
    return true;
}

// Next visible window on this workspace, wrapping. Built from the layout the
// window manager already computes rather than from its tree, so the order the
// user tabs through is the order they see on the glass.
static bool focus_next_window(eos_wm_t *wm, eos_rect_t screen)
{
    eos_tile_t t[EOS_MAX_WINDOWS * 2];
    int n, i, cur = -1, first = -1, next = -1;

    if (!wm) return false;
    n = eos_wm_layout(wm, screen, t, (int)(sizeof t / sizeof t[0]));

    for (i = 0; i < n; i++) {
        if (!t[i].visible) continue;
        if (first < 0) first = i;
        if (cur >= 0 && next < 0) next = i;
        if (t[i].win == wm->focus) cur = i;
    }
    if (first < 0) return false;                    // nothing on this workspace
    if (next < 0 || cur < 0) next = first;          // wrapped, or nothing focused
    if (t[next].win == wm->focus) return false;     // only one window

    eos_wm_focus_win(wm, t[next].win);
    return true;
}

eos_key_result_t eos_keys_apply(eos_wm_t *wm, eos_shell_state_t *st,
                                eos_rect_t screen, eos_action_t action, int16_t arg)
{
    eos_key_result_t r;
    r.handled = true;
    r.changed = false;
    r.action  = action;
    r.arg     = arg;
    r.win     = EOS_NONE;

    switch (action) {
    case EOS_ACT_NONE:
        r.handled = false;
        break;

    case EOS_ACT_SPAWN: {
        int w = eos_wm_open(wm, (uint16_t)arg, screen);
        r.win     = (int16_t)w;
        r.changed = (w != EOS_NONE);
        break;
    }
    case EOS_ACT_CLOSE: {
        int w = wm->focus;
        if (w != EOS_NONE && eos_wm_close(wm, w)) { r.win = (int16_t)w; r.changed = true; }
        break;
    }
    case EOS_ACT_SPLIT_COLS: eos_wm_set_split(wm, EOS_SPLIT_COLS); r.changed = true; break;
    case EOS_ACT_SPLIT_ROWS: eos_wm_set_split(wm, EOS_SPLIT_ROWS); r.changed = true; break;

    case EOS_ACT_FOCUS_LEFT:  r.changed = eos_wm_focus_dir(wm, EOS_DIR_LEFT,  screen); break;
    case EOS_ACT_FOCUS_RIGHT: r.changed = eos_wm_focus_dir(wm, EOS_DIR_RIGHT, screen); break;
    case EOS_ACT_FOCUS_UP:    r.changed = eos_wm_focus_dir(wm, EOS_DIR_UP,    screen); break;
    case EOS_ACT_FOCUS_DOWN:  r.changed = eos_wm_focus_dir(wm, EOS_DIR_DOWN,  screen); break;

    case EOS_ACT_MOVE_LEFT:  r.changed = move_focused(wm, EOS_DIR_LEFT,  screen); break;
    case EOS_ACT_MOVE_RIGHT: r.changed = move_focused(wm, EOS_DIR_RIGHT, screen); break;
    case EOS_ACT_MOVE_UP:    r.changed = move_focused(wm, EOS_DIR_UP,    screen); break;
    case EOS_ACT_MOVE_DOWN:  r.changed = move_focused(wm, EOS_DIR_DOWN,  screen); break;

    case EOS_ACT_WORKSPACE:
        if (arg >= 0 && arg < EOS_WORKSPACES && arg != wm->ws) {
            eos_wm_goto_workspace(wm, arg);
            r.changed = true;
        }
        break;

    case EOS_ACT_MOVE_TO_WS:
        if (wm->focus != EOS_NONE)
            r.changed = eos_wm_move_to_workspace(wm, wm->focus, arg);
        break;

    case EOS_ACT_TAB_NEXT:
        // Inside a collapsed group, cycle the group - the specific and more
        // useful meaning. Everywhere else cycle the workspace's windows,
        // because eos_wm_focus_tab_next() answers false when the focused
        // window is not in a group, and super+tab then did nothing at all.
        // Two windows on a narrow panel collapse into a group and it worked;
        // a third changed the layout and it stopped, which reads as broken
        // rather than as a rule.
        r.changed = eos_wm_focus_tab_next(wm, screen);
        if (!r.changed) r.changed = focus_next_window(wm, screen);
        break;
    case EOS_ACT_RESIZE:   r.changed = eos_wm_resize(wm, arg);            break;

    case EOS_ACT_LAUNCHER:   st->launcher_open = !st->launcher_open; r.changed = true; break;
    case EOS_ACT_TOGGLE_BAR: st->bar_visible   = !st->bar_visible;   r.changed = true; break;
    case EOS_ACT_LOCK:       st->locked        = !st->locked;        r.changed = true; break;

    case EOS_ACT_CYCLE_THEME:
        if (st->theme_count == 0) st->theme_count = 1;
        st->theme = (uint8_t)((st->theme + 1) % st->theme_count);
        r.changed = true;
        break;

    default:
        r.handled = false;
        break;
    }
    return r;
}

eos_key_result_t eos_keys_feed(const eos_keymap_t *km, eos_wm_t *wm,
                               eos_shell_state_t *st, eos_rect_t screen,
                               uint8_t mods, uint16_t key)
{
    eos_key_result_t miss = { false, false, EOS_ACT_NONE, 0, EOS_NONE };
    const eos_keybind_t *b;

    mods = eos_keys_mods_from_hid(mods);
    b    = eos_keys_lookup(km, mods, key);
    if (!b) return miss;

    eos_action_t a = (eos_action_t)b->action;

    // Locked: the only key that still means anything is the one that unlocks.
    if (st->locked && a != EOS_ACT_LOCK) {
        eos_key_result_t swallowed = { true, false, a, b->arg, EOS_NONE };
        return swallowed;
    }
    // Launcher open: plain keys belong to its text field, not to the shell.
    if (st->launcher_open && !(mods & EOS_MOD_SUPER)) return miss;

    return eos_keys_apply(wm, st, screen, a, b->arg);
}

void eos_shell_status_sync(const eos_wm_t *wm, eos_bar_status_t *st,
                           const char *const *app_names, int app_count)
{
    st->ws_occupied = 0;
    for (int i = 0; i < EOS_MAX_WINDOWS; i++)
        if (wm->win[i].alive && wm->win[i].ws >= 0 && wm->win[i].ws < EOS_WORKSPACES)
            st->ws_occupied |= (uint16_t)(1u << wm->win[i].ws);
    st->ws_active = (uint8_t)wm->ws;

    st->title = NULL;
    if (wm->focus >= 0 && wm->focus < EOS_MAX_WINDOWS && wm->win[wm->focus].alive) {
        int id = (int)wm->win[wm->focus].app_id;
        if (app_names && id >= 0 && id < app_count) st->title = app_names[id];
    }
}
