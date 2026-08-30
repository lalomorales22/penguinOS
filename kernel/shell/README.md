# kernel/shell — keybinds and the status bar model

Two pieces of pure logic. No allocation, no hardware, no LVGL, no drawing.

| File | What |
|---|---|
| `include/eos_keys.h` | keybind table, action enum, dispatch, keys.json |
| `include/eos_bar.h` | status bar segment model and the width fitter |
| `eos_keys.c` | table, JSON parser, dispatch against `eos_wm` |
| `eos_bar.c` | segment builders and the two-pass fitter |
| `test/test_shell.c` | host test: scripted keypresses + printed bars |

```bash
cc -std=c99 -Wall -Wextra -O1 -Ikernel/wm/include -Ikernel/hal/include \
   -Ikernel/shell/include kernel/wm/eos_wm.c kernel/shell/eos_keys.c \
   kernel/shell/eos_bar.c kernel/shell/test/test_shell.c -o /tmp/test_shell \
   && /tmp/test_shell
```

1052 checks, 0 failed.

## What this component depends on

| Header | Why |
|---|---|
| `kernel/wm/include/eos_wm.h` | every action dispatches against it; never modified |
| `kernel/hal/include/eos_input.h` | the keycode and modifier space, not redefined here |

`eos_bar.h` depends on nothing but `stdint`/`stdbool` on purpose — the bar model
has to build and be testable with no theme, no avatar and no HAL present. Two
enums it defines are shaped to match their owners so the glue is trivial:

| eos_bar.h | maps to | how |
|---|---|---|
| `eos_bar_role_t` | `eos_role_t` (theme) | `FG`→`EOS_ROLE_BAR_FG`, `MUTED`→`EOS_ROLE_MUTED`, `ACCENT`→`EOS_ROLE_ACCENT`, `OK`→`EOS_ROLE_OK`, `WARN`→`EOS_ROLE_WARN` |
| `eos_bar_mood_t` | `eos_buddy_state_t` (avatar) | same seven values in the same order — a cast |

Three names had to move to keep every kernel header includable in one
translation unit: `EOS_ROLE_BAR_FG` belongs to the theme, so the bar's roles are
`EOS_BAR_ROLE_*`; `eos_bind_t` belongs to the HAL's event matcher, so the table
entry is `eos_keybind_t`; and the `EOS_KEY_*`/`EOS_MOD_*` constants are the
HAL's, included rather than duplicated.

## eos_keys

Keycodes are **USB HID usage ids**, not ASCII and not a per-board scancode map.
That is what a BLE HID keyboard (the K809) reports, so nothing translates on the
way in. The constants come from `eos_input.h`; this component does not define a second
keycode space. `eos_keys_mods_from_hid()` folds a raw report modifier byte into
the collapsed group form the table stores — left and right sides collapse
together, because nothing in the shell wants them apart — and it is idempotent,
so every entry point runs it without caring where the byte came from.

Matching is **exact**, not "at least these modifiers". `super+shift+1` must not
also fire `super+1`, and an unexpected `alt` must not silently fall through to
the plainer bind. That is why the table does not use the HAL's
`eos_bind_match()`, which is the looser matcher the arcade-style apps want.

```c
eos_keymap_t km; eos_wm_t wm; eos_shell_state_t st;
eos_keys_defaults(&km);
eos_keys_load_file(&km, "/sd/keys.json", scratch, sizeof scratch);   // optional
eos_shell_state_init(&st, theme_count);
...
eos_key_result_t r = eos_keys_feed(&km, &wm, &st, screen,
                                   eos_keys_mods_from_hid(rpt[0]), rpt[2]);
if (r.changed) redraw();
```

### Default table

45 binds of the 72 slots.

| Chord | Action | Arg |
|---|---|---|
| `super+return` | spawn | app 0 (terminal) |
| `super+q` | close focused window | |
| `super+h` `super+j` `super+k` `super+l` | focus left/down/up/right | |
| `super+left` `super+down` `super+up` `super+right` | same | |
| `super+shift+`(hjkl or arrows) | move window in that direction | |
| `super+ctrl+h` | force next split side by side | |
| `super+ctrl+v` | force next split stacked | |
| `super+1`..`super+9` | switch workspace | 0..8 |
| `super+shift+1`..`9` | move window to workspace | 0..8 |
| `super+tab` | next window inside a tab group | |
| `super+minus` / `super+equal` | resize focused tile | -50 / +50 permille |
| `super+space` | toggle app launcher | |
| `super+b` | toggle status bar | |
| `super+t` | cycle theme | |
| `super+escape` | lock / sleep | |

**The one deviation from the brief.** `super+h` cannot be both focus-left and
force-a-horizontal-split. Focus wins, because that is the key you press a
hundred times an hour, so the split force moved to `super+ctrl+h` /
`super+ctrl+v`. Two lines of `keys.json` put the i3 spelling back:

```json
{ "binds": [ { "keys": "super+h", "action": "split_cols" },
             { "keys": "super+v", "action": "split_rows" } ] }
```

### keys.json

Lives on the SD card. Applied **on top of** the compiled-in defaults, so the
file only lists what changes and a missing card costs nothing. `"action":
"none"` unbinds. Rebinding an existing chord replaces it in place.

```json
{ "binds": [
    { "keys": "super+q",       "action": "spawn", "arg": 4 },
    { "keys": "SUPER+F",       "action": "toggle_bar" },
    { "keys": "super+shift+b", "action": "none" }
] }
```

A bare top-level array works too. Chords are case-insensitive; modifier
spellings are `super`/`mod`/`win`/`cmd`/`gui`, `shift`, `ctrl`/`control`,
`alt`/`opt`. Key names are single characters plus `return` `escape` `space`
`tab` `backspace` `delete` `insert` `home` `end` `pageup` `pagedown` `minus`
`equal` `comma` `period` `slash` `semicolon` `quote` `grave` `backslash`
`bracketleft` `bracketright` `left` `right` `up` `down` `f1`..`f12`.

Parsing happens into a private copy of the table and is committed only if the
**whole file** parses. A truncated card cannot leave the board with a table you
can no longer type your way out of. Errors report a byte offset:

| Error | Cause |
|---|---|
| `EOS_KEYS_ERR_SYNTAX` | malformed json, or a bind object missing `keys`/`action` |
| `EOS_KEYS_ERR_KEYNAME` | a chord token that is not a modifier or a known key |
| `EOS_KEYS_ERR_ACTION` | an action name we do not implement |
| `EOS_KEYS_ERR_FULL` | more than `EOS_MAX_BINDS` binds |
| `EOS_KEYS_ERR_IO` | file missing or unreadable |
| `EOS_KEYS_ERR_TOO_BIG` | file larger than the caller's scratch buffer |

`eos_keys_load_file` reads into a caller-owned buffer. It never allocates and
never truncates: a file that does not fit is refused. 4 KB of scratch is plenty
for a few dozen overrides.

### Moving a window

`eos_wm` has no swap primitive and is not allowed to grow one. So
`EOS_ACT_MOVE_*` focuses the neighbour with the WM's own `eos_wm_focus_dir`,
then exchanges the two leaves' window ids and the `win[].node` back-pointers.
The tree shape, the split ratios and the tab groups are untouched — only which
window sits in which hole changes. Focus then follows the window that moved,
not the slot it left. This is the only place the shell writes to `eos_wm_t`
directly, and it is why the host test asserts that the tile count and rects are
unchanged across a move.

### Modal rules

- **Locked**: every bind except `EOS_ACT_LOCK` is swallowed — reported
  `handled`, but nothing moves. The lock screen gets a quiet keyboard for free.
- **Launcher open**: only chords carrying SUPER are dispatched. Everything else
  comes back `handled == false` so the launcher's own text field gets it.

## eos_bar

The bar produces a list of positioned segments — text, a theme colour role, an
alignment, an x and a w in pixels. The renderer walks the list; the model never
draws. Colour **roles**, not colours: the bar deliberately does not include the
theme header, so it builds and is testable with no theme present.

| Segment | Priority | Align | Forms, shortest first |
|---|---|---|---|
| pips | 100 | left | `3` / `12[3]5` / `1 2 [3] 5` |
| clock | 90 | right | `14:32` (`--:--` with no time source) |
| wifi | 80 | right | `=` / `-58` / `wifi -58` |
| title | 60 | left | flexible: truncated to whatever is left |
| brain | 40 | right | `b` / `qwen3.5:2b` / `brain qwen3.5:2b` |
| heap | 30 | right | `21k` / `heap 21k` |
| mood | 20 | right | `:]` / `buddy :]` — always two cells wide |

Priority is not "what is interesting", it is **what is not already on the
screen**. The pips and the clock cannot be read off the windows themselves; the
focused title mostly can, which is why it ranks below wifi; the buddy is charm
and goes first.

Mood glyphs, in `eos_buddy_state_t` order: `:|` idle, `:?` thinking, `:o`
talking, `:^` listening, `zz` sleeping, `:]` happy, `:/` confused.

Only occupied workspaces and the active one appear in the pips — nine empty
digits are nine wasted characters. The wifi minimum is a one-character signal
ramp: `#` ≥ -55, `=` ≥ -67, `-` ≥ -78, `_` below, `x` down, `-` off, `?`
joining. Heap turns `WARN` at or below `heap_warn` (default 12 KB, because the
CYD idles near 20 KB with WiFi and BLE up and 12 KB is the cliff).

### The fitter

Two passes, and the invariant is the point:

1. Take segments in **strict priority order at their shortest form** until one
   no longer fits. The first refusal ends the run, so the survivors are always a
   *prefix* of the priority order — never a hole in the middle, and never a low
   segment outliving a high one.
2. Spend what is left, again in priority order, one segment at a time. Each
   takes the longest form it can still afford; the flexible title takes as much
   of its natural width as remains and truncates with `~`.

Truncation measures, it does not divide. LVGL fonts are proportional and the
arithmetic answer is wrong by a few pixels exactly when it matters, so
`eos_bar_metrics_t.measure` takes the font's own measure function. Leave it
NULL on tier 0 and `char_w` is used instead.

`eos_bar_build` costs about 900 bytes of stack for its candidate table and
allocates nothing.

### What survives, per panel

Measured at `char_w 6, pad 6`. `~` marks a truncated title.

| Panel | Bar px | Result |
|---|---|---|
| ILI9488 480x320 landscape | 480 | `1 2 [3] 5 megabrain          buddy :] heap 21k brain qwen3.5:2b wifi -58 14:32` |
| ILI9341 320x240 | 320 | `1 2 [3] 5 megabrain  :] 21k qwen3.5:2b wifi -58 14:32` |
| ST7789 320x172 | 172 | `12[3]5 meg~ :] 21k b = 14:32` |
| SSD1306 128x64 | 128 | `3 mega~ 21k b = 14:32` — mood is the first casualty |
| (stress) | 96 | `3 meg~ b = 14:32` — heap goes too |
| (stress) | 64 | `3  = 14:32` — pips, wifi, clock and nothing else |

On the 128px OLED the pips collapse to the active workspace digit alone and the
title keeps four characters. That is the honest floor: you still know where you
are, whether the radio is up, how much heap is left and what time it is.

Filling the status struct:

```c
eos_bar_status_t st;
eos_bar_status_init(&st);
eos_shell_status_sync(&wm, &st, app_names, n_apps);   // pips + title from the wm
st.wifi = ...; st.brain_up = ...; st.free_heap = esp_get_free_heap_size();
int n = eos_bar_build(&st, &metrics, bar_w, segs, EOS_BAR_SEGS);
```

`eos_shell_status_sync` fills only what the window manager knows — which
workspaces hold windows, which is current, and the focused window's title taken
from `app_names[app_id]`. Everything else comes from elsewhere and is left
alone.

## Host test

`test/test_shell.c` drives a real `eos_wm` through the chords the owner will
actually type and asserts the tree lands where it should — opening, forced
splits, focus, moving a window, workspaces, tab groups on a 128x64 panel,
resize, close, lock, launcher, unbound keys. Then it parses good and bad
`keys.json`, and finally prints the bar at 480/320/172/128/96/64 px against
four different system states plus a proportional font, so a human can read what
survives. The asserts guard the two things the eye cannot: the bar never runs
past its width, and it never keeps a segment while dropping a higher-priority
one.

The test writes and removes `eos_shell_test_keys.json` in the working directory
to exercise the file path.
