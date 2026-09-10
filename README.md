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

**Twelve apps**, opened from the launcher with `super+space`:

| | |
|---|---|
| `clock` | the time of day in a large face, with the date and uptime under it |
| `board` | what this board is, and its address once it has joined |
| `heap` | free heap and largest block, live — which matters more than you'd expect on these parts |
| `keys` | the compiled-in keymap, on the glass |
| `buddy` | the voxel avatar and the mood it's in |
| `chat` | ask your model, and watch the reply arrive |
| `settings` | theme, brightness, board info and model host |
| `files` | browse the internal filesystem |
| `media` | the RGB LED: colour, brightness and effects |
| `party` | the demo — the buddy dancing, the LED cycling, the colours moving |
| `camera` | a viewfinder onto a penguinOS camera node on the same network |
| `arcade` | Tetris. Arrows move and rotate, space drops, enter starts |

There is deliberately **no terminal on the glass** — there's no shell to run in
one. The web app has a small command console (`help`, `status`, `heap`,
`reboot`, `theme`, `wifi`, `brain`) and a live log, which is where that job
belongs on a board with no keyboard of its own.

**A status bar and themes.** A bar across the top carries the workspace pips,
the focused window's title, free heap, the model's state, the clock — and **the
board's IP address**, which is the one thing you cannot work out by looking at
the screen. It fits itself to the panel: every segment has a short, medium and
long form and a priority, so a 240-pixel board drops down to `35k` and `b!`
while still showing the address in full, and nothing ever overflows the edge.
Seven themes ship, switchable from the board with `super+t` or from the web
app.

**A voxel buddy.** A little penguin called Pip lives on the desktop and wanders
the whole window — picking a spot, turning, waddling over, looking about and
occasionally hopping or flapping. He stands in a **scene** (a room, grass or a
pool) and you can **give him something** — a bowl of fish, water, a ball — from
the web app or the keyboard, and he walks over to look at it. Four buddies ship
— a penguin, a cat, an owl and a robot — and you can upload your own
MagicaVoxel `.vox` file from the web app.

**A wall clock.** The board asks the network what time it is on its first join
and converts with the POSIX zone in `sys.tz`, so the clock reads local time and
files carry real timestamps instead of 1970.

**A web app.** Once the board joins your network it serves a page for browsing
its filesystem, changing themes, swapping the buddy, editing settings and
chatting with your model.

**A chat window.** Points at a language model on another machine on your
network. See [Connecting your own AI](#connecting-your-own-ai).

---

## Supported boards

All seven are **verified on real hardware** — every pin, the colour format,
the orientation and the memory budget measured rather than read off a datasheet:

| Board | Chip | Screen | Notes |
|---|---|---|---|
| **ESP32-2432S024N** ("Cheap Yellow Display", also sold as HW-950) | ESP32 | 2.4" 240×320 | The tightest board that runs it. Resistive touch is fitted. |
| **ESP32-4832S040** (the 4.0" Cheap Yellow Display) | ESP32 | 4.0" 480×320 | ST7796. Resistive touch (XPT2046 on the panel's own bus), and a **working microSD** on its own SPI host, so a card read cannot stall a frame. |
| **Waveshare ESP32-C6-LCD-1.3** | ESP32-C6 | 1.3" 240×240 | Square panel, native USB. |
| **LAFVIN ESP32-C6 1.47"** | ESP32-C6 | 1.47" 320×172 | Same pinout as the Waveshare C6; only the panel differs. |
| **Waveshare ESP32-S3-Touch-LCD-1.47** | ESP32-S3 | 1.47" 320×172 | 16 MB flash, 8 MB PSRAM, capacitive touch, working microSD. The roomiest. |
| **Waveshare ESP32-C5-LCD-1.47** | ESP32-C5 | 1.47" 320×172 | Wi-Fi 6, dual-band 2.4/5 GHz. No touch. microSD shares the panel's SPI bus. The tightest heap in the fleet after boot. |
| **LILYGO T-Display C5** | ESP32-C5 | 1.9" 320×170 | 16 MB flash and a 12 MB filesystem — the roomiest storage in the fleet. Battery management and a Qwiic port, neither used yet. |

### Photos

Two per board. Drop a file into `docs/photos/` with the name below and it
appears here — nothing else needs editing. See `docs/photos/README.md`.

| Board | Running it | Detail |
|---|---|---|
| **ESP32-2432S024N (Cheap Yellow Display, 2.4in, N variant)**<br>ESP32-D0WD-V3, 240×320 | <img src="docs/photos/cyd-2432s024n-1.jpg" alt="ESP32-2432S024N (Cheap Yellow Display, 2.4in, N variant) running penguinOS" width="260"> | <img src="docs/photos/cyd-2432s024n-2.jpg" alt="ESP32-2432S024N (Cheap Yellow Display, 2.4in, N variant), detail" width="260"> |
| **ESP32-4832S040 (Cheap Yellow Display, 4.0in, resistive touch)**<br>ESP32-WROOM-32E, 480×320 | <img src="docs/photos/cyd-4832s040-1.jpg" alt="ESP32-4832S040 (Cheap Yellow Display, 4.0in, resistive touch) running penguinOS" width="260"> | <img src="docs/photos/cyd-4832s040-2.jpg" alt="ESP32-4832S040 (Cheap Yellow Display, 4.0in, resistive touch), detail" width="260"> |
| **Waveshare ESP32-C6-LCD-1.3**<br>ESP32-C6FH4 (QFN32) rev v0.2, 240×240 | <img src="docs/photos/waveshare-c6-lcd-13-1.jpg" alt="Waveshare ESP32-C6-LCD-1.3 running penguinOS" width="260"> | <img src="docs/photos/waveshare-c6-lcd-13-2.jpg" alt="Waveshare ESP32-C6-LCD-1.3, detail" width="260"> |
| **LAFVIN ESP32-C6 1.47inch LCD**<br>ESP32-C6FH4 (QFN32) rev v0.2, 320×172 | <img src="docs/photos/lafvin-c6-lcd-147-1.jpg" alt="LAFVIN ESP32-C6 1.47inch LCD running penguinOS" width="260"> | <img src="docs/photos/lafvin-c6-lcd-147-2.jpg" alt="LAFVIN ESP32-C6 1.47inch LCD, detail" width="260"> |
| **Waveshare ESP32-S3-Touch-LCD-1.47**<br>ESP32-S3 (QFN56) rev v0.2, 320×172 | <img src="docs/photos/waveshare-s3-touch-lcd-147-1.jpg" alt="Waveshare ESP32-S3-Touch-LCD-1.47 running penguinOS" width="260"> | <img src="docs/photos/waveshare-s3-touch-lcd-147-2.jpg" alt="Waveshare ESP32-S3-Touch-LCD-1.47, detail" width="260"> |
| **Waveshare ESP32-C5-LCD-1.47**<br>ESP32-C5, 320×172 | <img src="docs/photos/waveshare-c5-lcd-147-1.jpg" alt="Waveshare ESP32-C5-LCD-1.47 running penguinOS" width="260"> | <img src="docs/photos/waveshare-c5-lcd-147-2.jpg" alt="Waveshare ESP32-C5-LCD-1.47, detail" width="260"> |
| **LILYGO T-Display C5**<br>ESP32-C5, 320×170 | <img src="docs/photos/lilygo-t-display-c5-1.jpg" alt="LILYGO T-Display C5 running penguinOS" width="260"> | <img src="docs/photos/lilygo-t-display-c5-2.jpg" alt="LILYGO T-Display C5, detail" width="260"> |
| **penguinOS camera node**<br>XIAO ESP32-S3 Sense, no screen | <img src="docs/photos/xiao-esp32s3-sense-1.jpg" alt="the camera node" width="260"> | <img src="docs/photos/xiao-esp32s3-sense-2.jpg" alt="the camera node, detail" width="260"> |
Three more profiles exist in `boards/` — two ILI9488 panels and an OLED —
written from documentation but **never run on hardware**. Treat those as a
starting point for bring-up, not as working targets.

Every verified row above means a board that was held, flashed, booted and
looked at: the colour format confirmed by drawing red and checking it came out
red, the orientation confirmed by reading text off the glass, the memory budget
read out of the boot log rather than estimated. Where two boards carry the same
panel and disagree — and two pairs of them do — both answers are recorded with
what was measured, because the panel does not decide it; the wiring does.

Touch hardware is fitted to three of these and its wiring is recorded, but
**there is no touch driver yet** — the injection path is complete and nothing
calls it. Input today is a Bluetooth keyboard, a BLE trackpad, or the web app.

**A camera node.** A second, much smaller program in `firmware-cam/` turns a
Seeed XIAO ESP32-S3 Sense into a camera that serves frames over HTTP, which the
`camera` window points at. It has no board profile on purpose: it is a different
program rather than a board with the display switched off. Flash it with
`tools/flash.sh --camera`; `boards/xiao-esp32s3-sense/README.md` has the
measured facts about the unit and the one diagnostic that matters when frames
come back black.

**It is already penguinOS.** The node provisions through the same captive
portal, answers on the same `penguinos-xxxx.local` pattern and serves the same
`/api/*` namespace as every board with a screen — it just serves frames instead
of a desktop. There is nothing separate to install.

**Pointing a board at it.** Every board has its own `cam.host` setting, in the
web app's Settings tab. Put the camera node's address in it — its `.local` name
or its IP — and that board's `camera` window starts drawing. A board with
`cam.host` empty says `set cam.host` on the glass rather than failing quietly.
The setting is per board, so each one is aimed independently and several can
watch the same camera.

**How many at once.** The node runs the same HTTP server as everything else,
with **four worker sockets**, and a viewing board asks for one horizontal strip
roughly every 120 ms — eight strips to a picture. One or two viewers are
comfortable; four is the ceiling the sockets impose, and past that the
least-recently-used connection is dropped to make room, so viewers start
stealing frames from each other. It is a camera for a couple of screens at a
time, not a broadcast.

**Why strips and not pictures.** The node has 8 MB of PSRAM and the boards
watching it have about 30 KB of largest free block. A 240×320 RGB565 frame is
153,600 bytes, so no board with a screen can hold one. The node therefore
decodes the JPEG, scales and rotates it to exactly the size asked for, and
serves **raw RGB565** — and the viewer blits it 40 rows at a time without ever
holding a whole picture. All eight strips come out of one captured frame, so
the image does not tear across strip boundaries.

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
tools/flash.sh --erase       # erase the whole flash first (never implied)
```

Nothing is written without a confirmation, and `--yes` authorises *writing*, not
*guessing* — if two profiles still match it stops and asks anyway.

**The camera node is flashed by the same script**, with `--camera`:

```bash
tools/flash.sh --camera
```

It has no board profile on purpose — the registry is built around a panel and
every profile must carry a display controller, pins, a render tier and a band
height, all of which a screenless device would have to invent. So `--camera`
identifies nothing, generates no board header and stamps no profile into NVS; it
just builds `firmware-cam/` and writes it. Plug the node in without that flag and
the script now says so rather than telling you to go and write a profile.

### 3. First boot

The board comes up showing a **QR code**. Scan it, join the Wi-Fi network it
creates (`penguinos-xxxx`), and a setup page opens where you pick your network
and enter its password.

The board then joins your network and prints its address on screen. Open that in
a browser and you have the web app. It also advertises itself over mDNS, so
`http://penguinos-xxxx.local` usually works too.

Credentials are saved, so it rejoins on its own after that — and reflashing
won't wipe them.

### 4. Updating a board you already flashed

Plug it in and run the same command. **You do not need to know which board it
is.**

```bash
tools/flash.sh --yes
```

Every board that has ever been flashed had its MAC written into its profile's
`identification.mac_allowlist`, so the flasher recognises it on sight and says
so before it writes anything:

```
decision  pinned - MAC 38:44:be:0e:9c:38 is in waveshare-c5-lcd-147 ...
```

If it cannot tell — two profiles that differ only in what is soldered on, and a
board it has never seen — it stops and asks rather than guessing. `--yes`
authorises *writing*, never *guessing*.

**A reflash keeps everything the board has learned.** It writes three regions —
the bootloader, the partition table and the app — and neither of the two that
hold state is among them:

| Region | Holds | Touched by a reflash? |
|---|---|---|
| `nvs` | Wi-Fi credentials, BLE keyboard bonds, which profile this board is | almost never — see below |
| `int` | themes, buddies, `settings.json`, anything uploaded from the web app | **no** |

So a board picks up every new app and fix, rejoins your network on its own, and
still has its buddy and its theme. Use `--erase` only when you deliberately want
a board back to nothing; it is never implied.

**The one exception, and it happens once per board.** The flasher writes a small
stamp into `nvs` saying which profile a board is. It skips that write when the
stamp is already right — which is why a reflash normally keeps your network. But
a board flashed by a penguinOS old enough to predate stamping has no stamp at
all, and writing one rewrites the whole `nvs` partition, credentials included.
Such a board comes back up in **setup mode** and needs its network again.

It is a one-time cost: the stamp written on that first reflash makes every later
one take the skip. The flasher says so before it does it. `--no-nvs` avoids it
entirely, at the price of leaving the board unstamped — so it pays the same cost
next time instead.

---

## Connecting your own AI

The chat window talks to a model server on your network. There's no cloud
service and no API key; it only ever talks to a machine you control.

penguinOS asks its server a deliberately tiny question — `GET /ask?q=...`,
answered as plain streaming text — because the client has to fit in a few
kilobytes. **Ollama speaks something different**: JSON in, newline-delimited
JSON out. So there's a small bridge in this repo that sits between them.

### On the computer that will run the model

Install Ollama:

```bash
curl -fsSL https://ollama.com/install.sh | sh
```

That one-liner is Linux. On **macOS** use `brew install ollama` (or the app from
[ollama.com/download](https://ollama.com/download)); on **Windows** use the
installer from the same page. Then, on any of them, pull a model and start the
bridge:

```bash
ollama pull qwen3.5:2b
python3 tools/ollama-bridge.py
```

`qwen3.5:2b` is the default on both sides — the bridge and the board's own
firmware agree on it — so you don't have to configure the model at all. Any
Ollama model works; pass `--model <name>` to use a different one.

That's it — no dependencies beyond Python 3. It prints the address to point the
board at.

**You do not have to expose Ollama to your network.** The bridge runs on the
same machine and reaches Ollama over `127.0.0.1`, so Ollama keeps its default
local-only binding and the only thing listening on the LAN is the bridge. There
is no `OLLAMA_HOST` to set.

The bridge runs in the foreground and stops when you close the terminal, which
is usually what you want while trying things out. To leave it running, start it
under `nohup`, `screen`, `tmux`, or whatever service manager you already use.

### On the board

Open the web app, go to **Settings**, and fill in three things:

| Field | What to put |
|---|---|
| **Host** | The **hostname of the computer running the bridge** — see below |
| **Port** | `8080` |
| **Model** | `qwen3.5:2b`, or whatever you pulled |

#### What "host" means here

It's the name or address of **your computer** — the one running Ollama and the
bridge — not the board, and not a website. The board has to reach across your
network to find it.

The easiest thing to type is your computer's own name with `.local` on the end.
The board resolves those, so you don't have to go hunting for an IP address:

**macOS** — this prints it:

```bash
echo "$(scutil --get LocalHostName).local"
```

**Linux** — usually your hostname plus `.local` (needs `avahi-daemon`, which
most desktop distributions run already):

```bash
echo "$(hostname).local"
```

**Windows** — `.local` names need Bonjour installed, so an IP address is the
safer choice. Get it with `ipconfig` and use the IPv4 address of the adapter
you're actually connected through.

An IP address works everywhere and is the fallback if a `.local` name doesn't
resolve — find it on macOS or Linux with `ipconfig getifaddr en0` or
`hostname -I`. The cost is that many routers hand out a different one after a
reboot, and then the board quietly stops finding your model until you update it.
A `.local` name follows the computer around, which is why it's worth trying
first.

**Check it before you type it into the board.** From the computer running the
bridge:

```bash
curl "http://$(scutil --get LocalHostName).local:8080/health"
```

If that returns JSON listing your models, the board will reach it too. If it
doesn't, the board won't either, and you'll save yourself debugging the wrong
end.

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
| `super+ctrl+←` `↓` `→` `↑` | shrink / grow the focused tile |
| `super+minus` / `super+equal` | the same, on a full keyboard |
| `super+b` | toggle the status bar |
| `super+t` | cycle theme |
| `super+escape` | lock |

Windows also take keys of their own while focused. `arcade` is arrows, space
and enter; `buddy` takes `space` to change the scene, `f` `w` `b` to give him
something and `n` to clear the floor.

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
