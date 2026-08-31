# kernel/shell — keybinds, the status bar model and the app launcher

Pure logic. No allocation, no hardware, no LVGL, no drawing.

| File | What |
|---|---|
| `include/eos_keys.h` | keybind table, action enum, dispatch, keys.json |
| `include/eos_bar.h` | status bar segment model and the width fitter |
| `include/eos_launcher.h` | the app list: selection, scroll, keys, hit test |
| `include/eos_pointer.h` | the cursor: acceleration, clamping, clicks, hit test, tile chrome |
| `eos_keys.c` | table, JSON parser, dispatch against `eos_wm` |
| `eos_bar.c` | segment builders and the two-pass fitter |
| `eos_launcher.c` | the list, the wrapping highlight and the scroll invariant |
| `eos_pointer.c` | relative counts in, an absolute arrow and a focus change out |
| `test/test_shell.c` | host test: scripted keypresses + printed bars |
| `test/test_launcher.c` | host test: the launcher, and that super+q still closes |
| `test/test_pointer.c` | host test: sign extension, the curve, the clamp, the click, the close box |

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
| `super+return` | spawn | app 0, which is the CLOCK. There is no terminal app; the ten registered apps are clock, board, heap, keys, buddy, chat, settings, files, media, party. |
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

## The launcher

`super+space` opens it, up/down (or `k`/`j`) move the highlight, enter opens the
selected app, escape closes it. It wraps at both ends and it scrolls, so a list
longer than the panel is still reachable; the selection can never be off screen,
because every function that moves it re-clamps the view afterwards.

It draws nothing. `firmware/main/eos_shell_draw.c` paints the overlay from the
model's stored `eos_launcher_geom_t`, and the hit test reads that same
rectangle back, so what the pointer thinks it is over and what is on the glass
cannot drift apart.

Two rules keep it from swallowing the keyboard:

* any chord carrying SUPER is handed straight back to `eos_keys`, which is what
  lets a second `super+space` close the list and `super+q` close a window from
  inside it;
* every HID usage the launcher does not bind comes back `EOS_LAUNCHER_PASS`.
  Eleven usages are launcher keys; the other 245 are not, and `test_launcher.c`
  sweeps all 256 to prove it.

Pointer support is coordinates, not a device: `eos_launcher_hover()` and
`eos_launcher_click()` take screen pixels, so a trackpad cursor, a touch panel
and a tap injected from the phone page all drive it through the same two calls.

```bash
cc -std=c99 -Wall -Wextra -O1 -Ikernel/wm/include -Ikernel/hal/include \
   -Ikernel/shell/include -Iboards/generated kernel/wm/eos_wm.c \
   kernel/shell/eos_keys.c kernel/shell/eos_bar.c kernel/shell/eos_launcher.c \
   kernel/shell/test/test_launcher.c -o /tmp/test_launcher && /tmp/test_launcher
```

204 checks, 0 failed.

## Where the dispatcher is

Everything in this component is a model: it answers questions and changes its
own state, and nothing in it reads an event queue. The one place that does is
`firmware/main/eos_shell_input.c`, which drains kernel/hal's ring and offers
each event down a fixed ladder — inactive, locked, the launcher, the keymap, the
focused window, dropped. The order and the argument for it are written at the
top of `firmware/main/eos_shell_input.h`, and `firmware/main/test/test_dispatch.c`
drives all six rungs against the real models in this directory.

Two rules from that ladder are worth knowing here, because they constrain what
these models may assume:

* **The launcher is offered every key before the keymap is.** Not because the
  default table binds no bare arrows — it does not, today — but because someone
  may bind one in `keys.json` tomorrow. `eos_launcher_key()` therefore has to
  hand back every SUPER chord and every usage it does not bind, and the host
  suite sweeps all 256 usages to prove exactly eleven are eaten.
* **A global chord that a bind claimed is consumed whether or not it moved
  anything.** `eos_key_result_t` carries `handled` and `changed` for exactly
  this reason: `changed` is about the screen and `handled` is about the key, and
  a caller that tests the wrong one sends `super+left` at the left edge on to
  the focused window as a bare left arrow.

## The pointer

The K809's trackpad sends three-byte HID boot-mouse reports on a handle of its
own. `kernel/svc/eos_ble.c` decodes them with `eos_ble_decode_mouse()` and hands
the signed counts to `eos_pointer_feed()`, which is the only place in penguinOS
that knows where the cursor is.

`byte 1` and `byte 2` are **signed**. Reading them as `uint8_t` gives a cursor
that can travel right and down and never left or up, and it pins itself in the
bottom-right corner on the first swipe. The decode is in the portable half of
`eos_ble.c` so the host suite can hand it `0xFF` and check it comes back `-1`.

Acceleration is two regimes and all integer, because the C6 has no FPU:

* at or below **2 counts** in one report the gain is exactly **1.0** — a slow
  drag moves one pixel per count and can be parked on a one-pixel tile border;
* above that it rises **0.625x per extra count**, capped at **5.0x**, so a ten
  count flick moves 40 px and crosses this 240 px panel in four reports.

Sub-pixel remainders are carried in Q4, so a fractional gain does not quantise
slow motion away; the division truncates toward zero rather than shifting, so
left and right cover exactly the same ground.

A click is a press and a release within `EOS_POINTER_SLOP` pixels of each
other. On a tiling window manager it means exactly three things — focus the tile
under it, raise the tab under it, or **close the window whose x it landed on** —
and all three are lookups against the layout `eos_wm_layout()` already computed.
There is no drag and no resize: a tiling layout comes from a tree, and a dragged
window would have nowhere to land.

### The close box

The owner asked for windows that can be closed, with a trackpad in their hand.
`super+q` was already the keyboard half; the x at the right of every visible
tile's header is the other one. Both end in `eos_wm_close()`, so the two doors
lead to one implementation and `eos_wm` keeps deciding which sibling absorbs the
space.

`eos_pointer.c` cannot see a font, so the renderer hands it the three numbers it
needs as an `eos_pointer_chrome_t`: the theme's border, the UI face's height and
the box's width. `firmware/main/eos_shell_draw.c` fills one in with
`eos_shell_tile_chrome()` at boot and again on a theme change — a hit box
measured on the old face's grid is a hit box in the wrong place.

`eos_pointer_close_box()` computes the rectangle, and **both** sides call it: the
painter draws the cross inside whatever it returns, and the hit test tests
against whatever it returns. They are the same rectangle by construction rather
than by agreement, and `firmware/main/test/test_shell_draw.c` closes the loop
from the other end by reading the composited panel back and counting ink inside
the box the dispatcher would use.

The box is `close_w` wide or it does not exist. A tile that cannot spare it and
still keep half its header for the window's name gets none: an earlier version
shrank to fit, and since `draw_close_x()` refuses to paint anything under
`EOS_POINTER_CLOSE_MIN` pixels, that produced a rectangle that closed a window
with nothing on the glass to say so.

A NULL chrome, or a `close_w` of zero, disables the box everywhere and changes
nothing else — which is what a panel too narrow to spare the pixels should do,
and what every caller written before there was one still sees.

The hit test answers the close box **before** the tile it sits in, because it is
drawn inside that tile's rect and every point in it is also a point in the tile.
Tab strips do not overlap it, so the order between those two is free; this one
is not.

Motion events coalesce in the HAL ring. A swipe is hundreds of reports and the
ring is 32 events, so without that the click at the end of a swipe would be the
event that got dropped.

```bash
cc -std=c99 -Wall -Wextra -O1 -Ikernel/hal/include -Ikernel/wm/include \
   -Ikernel/shell/include -Ikernel/svc/include -Iboards/generated \
   kernel/shell/eos_pointer.c kernel/wm/eos_wm.c kernel/hal/eos_input.c \
   kernel/svc/eos_ble.c kernel/shell/test/test_pointer.c -o /tmp/test_pointer \
   && /tmp/test_pointer
```

604 checks, 0 failed.
