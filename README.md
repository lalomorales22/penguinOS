# penguinOS

A small operating system for cheap ESP32 boards with LCD screens.

You buy a $10–15 development board with a screen on it, flash penguinOS, and get
a tiling window manager, a themeable desktop, a little voxel penguin who wanders
around, a web app for managing the board from your phone or laptop, and a chat
window wired to a language model running on your own computer.

It is written in C on **ESP-IDF** — not Arduino — and runs on boards with as
little as **148 KB of usable RAM**.

---

## What you get

**A tiling window manager.** Windows split the screen rather than overlapping,
Omarchy/i3 style. When a split would make a tile too small to be useful, the
window manager stops splitting and turns that region into a **tab group**
instead — so the layout degrades into something usable rather than into slivers.
On a 240×320 panel that happens quickly, which is exactly why it exists.

**Ten apps**, opened from the launcher with `super+space`:

| | |
|---|---|
| `clock` | uptime in a large face |
| `board` | what this board is, and its address once it has joined |
| `heap` | live memory, which matters more than you'd think on these parts |
| `keys` | every keybinding, on the glass |
| `buddy` | the voxel avatar |
| `chat` | talk to your model |
| `settings` | network, Bluetooth, theme, model host |
| `files` | browse the board's filesystem |
| `media` | |
| `party` | |

There is deliberately **no terminal** — there's no shell to run in one.

**A status bar and themes.** Seven themes ship, switchable from the board with
`super+t` or from the web app.

**A voxel buddy.** A little penguin called Pip lives on the desktop and wanders
about. Four ship — a penguin, a cat, an owl and a robot — and you can upload
your own MagicaVoxel `.vox` file from the web app.

**A web app.** Once the board joins your network it serves a page for browsing
its filesystem, changing themes, swapping the buddy, editing settings and
chatting with your model.

**A chat window.** Points at a language model on another machine on your
network. See [Connecting your own AI](#connecting-your-own-ai).

---

## Supported boards

Four boards are **verified on real hardware** — every pin, the colour format,
the orientation and the memory budget measured rather than read off a datasheet:

| Board | Chip | Screen | Notes |
|---|---|---|---|
| **ESP32-2432S024N** ("Cheap Yellow Display", also sold as HW-950) | ESP32 | 2.4" 240×320 | The tightest board that runs it. Resistive touch is fitted. |
| **Waveshare ESP32-C6-LCD-1.3** | ESP32-C6 | 1.3" 240×240 | Square panel, native USB. |
| **LAFVIN ESP32-C6 1.47"** | ESP32-C6 | 1.47" 320×172 | Same pinout as the Waveshare C6; only the panel differs. |
| **Waveshare ESP32-S3-Touch-LCD-1.47** | ESP32-S3 | 1.47" 320×172 | 16 MB flash, 8 MB PSRAM, capacitive touch, working microSD. The roomiest. |

Four more profiles exist in `boards/` — a Waveshare C5, two ILI9488 panels and
an OLED — written from documentation but **never run on hardware**. Treat those
as a starting point for bring-up, not as working targets.

Touch hardware is detected on two boards but **there is no touch driver yet**;
input today is a Bluetooth keyboard, the web app, or a BLE mouse.

**Your board isn't listed?** See [Adding a board](#adding-a-board). The registry
is designed for exactly that, and bringing up a new one takes minutes when the
vendor publishes a pinout.

---

## Getting started

### 1. Install ESP-IDF

penguinOS builds with **ESP-IDF v5.5** or newer.
Follow [Espressif's install guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/),
then in each new terminal:

```bash
. ~/esp/esp-idf/export.sh
```

### 2. Flash a board

Plug in the board and run:

```bash
tools/flash.sh
```

It identifies what you plugged in, tells you which profile it matched and why,
asks you to confirm, then builds and flashes. If you already know the board:

```bash
tools/flash.sh --profile cyd-2432s024n
```

Useful flags:

```bash
tools/flash.sh --list        # what's attached, and the whole registry
tools/flash.sh --identify    # identify only, write nothing
tools/flash.sh --dry-run     # print every command instead of running it
tools/flash.sh --monitor     # open a serial monitor afterwards
```

Nothing is written without a confirmation, and `--yes` authorises *writing*, not
*guessing* — if two profiles still match it stops and asks anyway.

### 3. First boot

The board comes up showing a **QR code**. Scan it, join the Wi-Fi network it
creates (`penguinos-xxxx`), and a setup page opens where you pick your network
and enter its password.

The board then joins your network and prints its address on screen. Open that in
a browser and you have the web app. It also advertises itself over mDNS, so
`http://penguinos-xxxx.local` usually works too.

Credentials are saved, so it rejoins on its own after that — and reflashing
won't wipe them.

---

## Connecting your own AI

The chat window talks to a model server on your network. There's no cloud
service and no API key; it only ever talks to a machine you control.

penguinOS asks its server a deliberately tiny question — `GET /ask?q=...`,
answered as plain streaming text — because the client has to fit in a few
kilobytes. **Ollama speaks something different**: JSON in, newline-delimited
JSON out. So there's a small bridge in this repo that sits between them.

### On the computer that will run the model

Install [Ollama](https://ollama.com), pull a model, and start the bridge:

```bash
ollama pull qwen3.5:2b
python3 tools/ollama-bridge.py
```

`qwen3.5:2b` is the default on both sides — the bridge and the board's own
firmware agree on it — so you don't have to configure the model at all. Any
Ollama model works; pass `--model <name>` to use a different one.

That's it — no dependencies beyond Python 3. It prints the address to point the
board at.

### On the board

Open the web app, go to **Settings**, and set the brain host to the **computer
running the bridge** (its LAN address) and the port to **8080**. Set the model
name to whatever you pulled.

### Notes worth knowing

**Point the board at the bridge, not at Ollama.** The bridge listens on 8080 by
default; Ollama's own port is 11434 and the board cannot talk to it directly.

**Reasoning models can look broken — including the default one.** `qwen3.5:2b`
streams its chain of thought in a separate field and produces no visible answer
until it finishes. On the board's 256-token budget it can spend every token
thinking and return **nothing at all**, which is indistinguishable from a dead
chat. The bridge turns thinking off by default for exactly this reason, which is
what makes the default model usable. Pass `--think` if you want to watch it
reason, and raise the token limit in Settings if you do.

**It works with more than Ollama.** Anything serving Ollama's API works — LM
Studio and llama.cpp both do. Point the bridge elsewhere with
`--ollama http://host:port`.

**Small models are the point.** A 2B model answers a board with a 2-inch screen
perfectly well and runs on a laptop. `qwen3.5:4b` is a good next step up if you
have the memory.

---

## Keyboard

Pair a Bluetooth keyboard from the web app's Settings tab. The bindings follow
i3/Omarchy muscle memory, where `super` is the GUI/Windows key:

| Chord | Action |
|---|---|
| `super+return` | open another window of app 0 (the clock) |
| `super+q` | close the focused window |
| `super+space` | launcher |
| `super+h` `j` `k` `l` | focus left / down / up / right (arrows work too) |
| `super+shift+h` `j` `k` `l` | move the window |
| `super+ctrl+h` / `super+ctrl+v` | force the next split to columns / rows |
| `super+1`…`super+9` | switch workspace |
| `super+shift+1`…`9` | move the window to a workspace |
| `super+tab` | next window, or next tab within a tab group |
| `super+minus` / `super+equal` | shrink / grow the focused tile |
| `super+b` | toggle the status bar |
| `super+t` | cycle theme |
| `super+escape` | lock |

`super+h` is **focus-left, not split-horizontal** — it's the key you press a
hundred times an hour, so focus wins. Both spellings collide and this is the
side the collision was resolved on.

---

## Making it yours

**Themes.** Seven ship: `carbon`, `catppuccin-mocha`, `cyd-amber`, `ember`,
`goldleaf`, `gruvbox` and `tokyonight`. Change them from the web app or with
`super+t`. They're plain JSON in `kernel/theme/themes/` — copy one, edit the
colours, upload it through the web app's file browser.

**Buddies.** Upload any MagicaVoxel `.vox` file from the Buddy tab and it joins
the gallery; pick whichever you like as the active one. Uploading never
overwrites what's already there. Keep models small — a few thousand voxels is
plenty at this screen size, and `assets/buddy/` has the Python scripts that
generate the four shipped ones.

---

## Adding a board

The board registry is the heart of this project. Every board is one JSON file in
`boards/` describing its pins, panel, memory and quirks — and every field that
cost someone debugging time carries a `_reason` explaining how it was
established, so nobody "tidies up" a value that was measured.

To bring up a new board:

1. Copy the closest existing profile in `boards/`.
2. Build the panel prober and adjust the pins until the screen lights up.
   `boards/hw-950-yellow/probe/` is a worked example: it identifies the panel,
   settles the colour format and finds the touch controller, and its comments
   explain what each test distinguishes and why the obvious version of it
   misleads.
3. Run `python3 tools/gen_board_header.py --check boards/<your-board>.json`.
   The validator is strict and explains what it wants.
4. `tools/flash.sh --profile <your-board>`.

`boards/README.md` documents every field. It's worth reading before guessing at
one: several of them are non-obvious, and the file records what each mistake
looks like on the glass rather than in a log.

---

## Building from source

```bash
tools/host_tests.sh                       # 34,000+ checks, no hardware needed
python3 tools/gen_board_header.py --all   # regenerate board headers
```

Building for a specific board directly, without the flasher:

```bash
idf.py -B build/<board-id> -DEOS_BOARD_ID=<board-id> \
       -DSDKCONFIG=build/<board-id>/sdkconfig set-target <target>
idf.py -B build/<board-id> -DEOS_BOARD_ID=<board-id> \
       -DSDKCONFIG=build/<board-id>/sdkconfig build
```

Both `-D` flags matter. `EOS_BOARD_ID` selects the board; a separate `SDKCONFIG`
per build directory is required once you build for more than one *silicon
target*, because `set-target` rewrites a shared one. See `firmware/README.md`.

---

## How it's put together

```
kernel/          the OS. No malloc, no ESP-IDF dependency, all host-testable.
  wm/            tiling window manager
  shell/         desktop, launcher, status bar, keybindings, pointer
  svc/           web server, storage, Bluetooth, network, model client
  avatar/        voxel renderer and the buddy's behaviour
  hal/           board abstraction and display backends
boards/          one JSON profile per board, plus the generator and validator
firmware/        the ESP-IDF project that ties it together
web/             the web app
tools/           flasher, detector, header generator, tests, Ollama bridge
```

The kernel doesn't know it's on an ESP32. It's plain C99 with no dynamic
allocation, which is why 34,000+ checks run on a laptop in a few seconds — and
why bugs get caught before a board is involved.

**Three render tiers** match the hardware. Tier 0 is an indexed software
compositor for boards with almost no RAM; tier 1 adds LVGL with banded drawing;
tier 2 is for boards with PSRAM. Which tier a board gets is a measured decision
recorded in its profile, not a guess.

---

## Status

Working on four boards: display, Wi-Fi provisioning, web app, filesystem,
themes, buddy gallery, Bluetooth keyboard, and the model chat.

Not done yet: touch input (hardware detected on two boards, no driver), the
microSD in the web app (the card mounts, it isn't exposed yet), and render tier 2
(the memory is there, the path is unproven).

`STATUS.md` carries the honest table, including what's known to be broken.

---

## License

**No license file yet**, which means default copyright applies and nobody else
has permission to use this. If you want people to be able to build on it, add
one — MIT and Apache-2.0 are the usual choices for something like this.

The **factory firmware images** dumped off each board during bring-up are
deliberately not in this repository. They're the board vendors' code, not ours,
and republishing them isn't ours to do. Each board profile records the SHA-256 of
its dump so a restore is still verifiable if you made your own backup.
