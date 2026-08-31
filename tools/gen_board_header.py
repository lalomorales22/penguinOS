#!/usr/bin/env python3
# gen_board_header.py - turns one board profile into the C header the build bakes in.
#
# The registry is JSON because humans edit it and the flasher reads it, but the
# firmware needs macros it can #if on and one struct it can pass around. This
# closes that gap. It is also the only real validator the profiles get: a JSON
# Schema can check shapes, but it cannot tell you that a DAC speaker is pinned to
# a GPIO with no DAC behind it, that a backlight is bound to an input-only pin,
# or that the SD card is quietly sharing three wires with the panel. Those checks
# live here and they are the point of the tool.
#
# The header it writes contains DATA and nothing else: the EOS_* macros and one
# initialiser for the eos_board_t declared in kernel/hal/include/eos_board.h. It
# used to be able to emit its own copy of those types so a header would compile
# standalone, and that copy is what made the boot glue impossible to write - one
# translation unit cannot hold two eos_board_t definitions, and the enumerators
# clashed on top of that. The HAL header is the authority now; when the registry
# grows a field the runtime needs, it goes there and the tables in the HAL MODE
# section below map the registry's strings onto its enums.
#
# The one non-obvious constraint: nothing the header emits may allocate. Every
# string is a literal, every array is fixed length, and the macros cost nothing
# at all unless something references them - which matters, because the tightest
# board this targets has about 20KB of heap and 4MB of flash.
#
# python3, standard library only.

import argparse
import hashlib
import json
import os
import re
import sys

SCHEMA_VERSION = 1
# Mirrors EOS_MAX_BUTTONS in kernel/hal/include/eos_board.h. That array is the
# real limit; this constant only exists so the failure is a validation message
# instead of a compile error in generated code. Raise it there first.
MAX_BUTTONS = 6
# Bad baud rates never reach the runtime struct - they are flasher advice and
# live only as macros - so this cap is the generator's own.
MAX_BAD_BAUDS = 4

# What each silicon target will actually let you do. input_only pins can be read
# but never driven, which is the trap that eats an evening when a backlight or a
# chip select lands on one.
TARGETS = {
    "esp32":   {"max_gpio": 39, "input_only": set(range(34, 40)), "dac": (25, 26), "cores": (1, 2)},
    "esp32c5": {"max_gpio": 28, "input_only": set(),              "dac": (),       "cores": (1,)},
    "esp32c6": {"max_gpio": 30, "input_only": set(),              "dac": (),       "cores": (1,)},
    "esp32s3": {"max_gpio": 48, "input_only": set(),              "dac": (),       "cores": (1, 2)},
}

# ADC1 is the only unit usable while WiFi is up; ADC2 is owned by the radio.
ESP32_ADC1 = {36: 0, 37: 1, 38: 2, 39: 3, 32: 4, 33: 5, 34: 6, 35: 7}

BUS_SPI, BUS_I2C = 1, 2
COMP = {"indexed8": 0, "mono1": 1, "lvgl": 2}
LED = {"none": 0, "gpio_rgb": 1, "ws2812": 2}
SPK = {"none": 0, "dac": 1, "pwm": 2, "i2s": 3}
BT = {"none": 0, "nimble": 1, "bluedroid": 2}

ID_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
SLUG_RE = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
NAME_RE = re.compile(r"^[a-z0-9_]+$")
MAC_RE = re.compile(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$")


class BadProfile(Exception):
    pass


def dig(obj, path):
    """Resolve a dotted path. Raises KeyError with the path on any miss."""
    cur = obj
    for part in path.split("."):
        if isinstance(cur, list):
            if not part.isdigit() or int(part) >= len(cur):
                raise KeyError(path)
            cur = cur[int(part)]
        elif isinstance(cur, dict):
            if part not in cur:
                raise KeyError(path)
            cur = cur[part]
        else:
            raise KeyError(path)
    return cur


def ascii_strings(obj, path, sink):
    """The header is C99. A stray en dash in a reason string breaks the build in
    a way that points at the wrong file, so it is rejected here instead."""
    if isinstance(obj, str):
        for i, ch in enumerate(obj):
            if ord(ch) > 126 or (ord(ch) < 32 and ch not in "\t"):
                sink(path, "non-ASCII or control character U+%04X at offset %d; "
                           "profile strings are emitted into a C99 header and must be plain ASCII"
                     % (ord(ch), i))
                return
    elif isinstance(obj, dict):
        for k, v in obj.items():
            ascii_strings(v, "%s.%s" % (path, k) if path else k, sink)
    elif isinstance(obj, list):
        for i, v in enumerate(obj):
            ascii_strings(v, "%s.%d" % (path, i), sink)


# ------------------------------------------------------------------ validation

class Validator:
    def __init__(self, data, src):
        self.d = data
        self.src = src
        self.errors = []

    def err(self, path, msg):
        self.errors.append((path, msg))

    def get(self, path, *types, required=True, nonempty=False,
            choices=None, lo=None, hi=None, nullable=False):
        try:
            val = dig(self.d, path)
        except KeyError:
            if required:
                self.err(path, "missing")
            return None
        if val is None:
            if not nullable:
                self.err(path, "must not be null")
                return None
            return None
        if types:
            # bool is an int subclass; an integer field must not accept true.
            if int in types and bool not in types and isinstance(val, bool):
                self.err(path, "expected an integer, got a boolean")
                return None
            if not isinstance(val, types):
                self.err(path, "expected %s, got %s"
                         % ("/".join(t.__name__ for t in types), type(val).__name__))
                return None
        if nonempty and isinstance(val, str) and not val.strip():
            self.err(path, "must not be empty")
            return None
        if choices is not None and val not in choices:
            self.err(path, "must be one of %s, got %r" % (sorted(choices, key=repr), val))
            return None
        if lo is not None and isinstance(val, (int, float)) and val < lo:
            self.err(path, "must be >= %s, got %s" % (lo, val))
            return None
        if hi is not None and isinstance(val, (int, float)) and val > hi:
            self.err(path, "must be <= %s, got %s" % (hi, val))
            return None
        return val

    # -- pin helpers ------------------------------------------------------

    def pin(self, path, target, role, out_capable=True, allow_none=True):
        v = self.get(path, int, lo=-1, hi=48)
        if v is None:
            return None
        if v == -1:
            if not allow_none:
                self.err(path, "%s needs a real GPIO, got -1" % role)
            return v
        spec = TARGETS.get(target)
        if spec:
            if v > spec["max_gpio"]:
                self.err(path, "GPIO %d is out of range for target %s (max %d)"
                         % (v, target, spec["max_gpio"]))
            elif out_capable and v in spec["input_only"]:
                self.err(path, "GPIO %d is input-only on %s and cannot drive %s"
                         % (v, target, role))
        return v

    # -- the actual rules -------------------------------------------------

    def check(self):
        d = self.d
        ascii_strings(d, "", self.err)

        self.get("schema_version", int, choices={SCHEMA_VERSION})
        bid = self.get("id", str, nonempty=True)
        if bid is not None and not ID_RE.match(bid):
            self.err("id", "must be a lowercase slug like cyd-2432s024n, got %r" % bid)
        if bid is not None and self.src:
            stem = os.path.splitext(os.path.basename(self.src))[0]
            if stem != bid:
                self.err("id", "is %r but the file is named %r; the id must match the "
                               "filename stem, because the generated header is named from it"
                         % (bid, stem))
        self.get("name", str, nonempty=True)
        self.get("summary", str, nonempty=True)

        target = self.check_chip()
        w, h = self.check_display(target)
        self.check_render(w, h)
        self.check_inputs(target)
        self.check_peripherals(target)
        self.check_flashing()
        self.check_identification()
        self.check_pin_conflicts()
        self.check_paths()
        return self.errors

    def check_chip(self):
        target = self.get("chip.target", str, choices=set(TARGETS))
        self.get("chip.variant", str, nonempty=True)
        cores = self.get("chip.cores", int, lo=1, hi=2)
        if target and cores and cores not in TARGETS[target]["cores"]:
            self.err("chip.cores", "%s has %s core(s), profile says %d"
                     % (target, "/".join(str(c) for c in TARGETS[target]["cores"]), cores))
        self.get("chip.flash_size_mb", int, choices={2, 4, 8, 16, 32})
        present = self.get("chip.psram.present", bool)
        ptype = self.get("chip.psram.type", str, choices={"none", "quad", "octal"})
        psize = self.get("chip.psram.size_mb", int, lo=0)
        if present is False and (ptype not in (None, "none") or psize not in (None, 0)):
            self.err("chip.psram", "present is false, so type must be \"none\" and size_mb 0")
        if present is True and (ptype == "none" or psize == 0):
            self.err("chip.psram", "present is true, so type must not be \"none\" and size_mb must be > 0")
        return target

    def check_display(self, target):
        self.get("display.controller", str, nonempty=True)
        src = self.get("display.driver_source", str, choices={"esp_lcd", "bsp", "custom"})
        comp = self.get("display.bsp_component", str, nullable=True, required=True)
        ver = self.get("display.bsp_version", str, nullable=True, required=True)
        if src == "bsp":
            if not comp:
                self.err("display.bsp_component", "required when driver_source is \"bsp\"")
            if not ver:
                self.err("display.bsp_version", "required when driver_source is \"bsp\"")
        elif src is not None:
            if comp is not None:
                self.err("display.bsp_component", "must be null unless driver_source is \"bsp\"")
            if ver is not None:
                self.err("display.bsp_version", "must be null unless driver_source is \"bsp\"")

        nw = self.get("display.native_width", int, lo=1, hi=4096)
        nh = self.get("display.native_height", int, lo=1, hi=4096)
        rot = self.get("display.rotation", int, lo=0, hi=3)
        depth = self.get("display.color_depth", int, choices={1, 16, 18, 24})
        bpp = self.get("display.bytes_per_pixel", int, lo=0, hi=3)
        order = self.get("display.color_order", str, choices={"rgb", "bgr", "mono"})
        sixteen = self.get("display.supports_16bit_pixels", bool)
        pfr = self.get("display.pixel_format_reason", str)

        expect_bpp = {1: 0, 16: 2, 18: 3, 24: 3}
        if depth is not None and bpp is not None and bpp != expect_bpp[depth]:
            self.err("display.bytes_per_pixel",
                     "color_depth %d puts %d byte(s) on the wire, profile says %d"
                     % (depth, expect_bpp[depth], bpp))
        if depth is not None and order is not None:
            if (depth == 1) != (order == "mono"):
                self.err("display.color_order",
                         "color_depth %d and color_order %r disagree; 1bpp is the only mono depth"
                         % (depth, order))
        if depth is not None and sixteen is not None and sixteen != (depth == 16):
            self.err("display.supports_16bit_pixels",
                     "color_depth is %d, so supports_16bit_pixels must be %s"
                     % (depth, str(depth == 16).lower()))
        if sixteen is False and not (pfr or "").strip():
            self.err("display.pixel_format_reason",
                     "required and non-empty when the controller has no 16-bit pixel mode; "
                     "say what a pixel actually costs")

        bus = self.get("display.bus", str, choices={"spi", "i2c"})
        host = self.get("display.spi_host", str, nullable=True, required=True,
                        choices={"HSPI", "VSPI", "FSPI", "SPI2", "SPI3", None})
        clk = self.get("display.clock_hz", int, lo=10000, hi=80000000)
        self.get("display.clock_reason", str, nonempty=True)

        p = {}
        for k, out in (("sck", True), ("mosi", True), ("miso", False), ("dc", True),
                       ("cs", True), ("rst", True), ("sda", True), ("scl", True)):
            p[k] = self.pin("display.pins." + k, target, "display %s" % k, out_capable=out)
        addr = self.get("display.i2c_address", int, nullable=True, required=True, lo=3, hi=119)

        if bus == "spi":
            if host is None:
                self.err("display.spi_host", "required for an SPI panel")
            for k in ("sck", "mosi", "dc", "cs"):
                if p[k] == -1:
                    self.err("display.pins." + k, "required for an SPI panel")
            for k in ("sda", "scl"):
                if p[k] not in (None, -1):
                    self.err("display.pins." + k, "must be -1 on an SPI panel")
            if addr is not None:
                self.err("display.i2c_address", "must be null on an SPI panel")
            if clk is not None and clk < 1000000:
                self.err("display.clock_hz", "%d Hz is implausibly slow for an SPI panel" % clk)
        elif bus == "i2c":
            if host is not None:
                self.err("display.spi_host", "must be null on an I2C panel")
            for k in ("sda", "scl"):
                if p[k] == -1:
                    self.err("display.pins." + k, "required for an I2C panel")
            for k in ("sck", "mosi", "miso", "dc", "cs", "rst"):
                if p[k] not in (None, -1):
                    self.err("display.pins." + k, "must be -1 on an I2C panel")
            if addr is None:
                self.err("display.i2c_address", "required for an I2C panel")
            if clk is not None and clk > 1000000:
                self.err("display.clock_hz", "%d Hz is above I2C fast-mode-plus" % clk)

        blp = self.pin("display.backlight.pin", target, "the backlight")
        bal = self.get("display.backlight.active_low", bool)
        bpw = self.get("display.backlight.pwm", bool)
        if blp == -1 and (bal or bpw):
            self.err("display.backlight",
                     "pin is -1, so active_low and pwm must both be false")

        self.get("display.col_offset", int, lo=0)
        self.get("display.row_offset", int, lo=0)
        self.get("display.invert", bool)
        # Not a constant across the fleet. The C6 boards need the swap and the
        # ESP32-S3-Touch-LCD-1.47 does not; with invert also wrong, pure red
        # renders as yellow rather than as anything obviously broken, which is
        # why this is a required measured field and not a default.
        self.get("display.byte_swap", bool)
        # XORed with the rotation's own mirror bits. The rotation table assumes a
        # panel default scan order; where a panel disagrees these express it
        # without adding a fifth rotation case that means "the odd one".
        self.get("display.mirror_x", bool)
        self.get("display.mirror_y", bool)

        if nw is None or nh is None or rot is None:
            return None, None
        return (nh, nw) if rot % 2 == 1 else (nw, nh)

    def check_render(self, w, h):
        tier = self.get("render.tier", int, choices={0, 1, 2})
        comp = self.get("render.compositor", str, choices=set(COMP))
        lvgl = self.get("render.lvgl", bool)
        pal = self.get("render.palette_entries", int, lo=0, hi=256)
        full = self.get("render.full_framebuffer", bool)
        band = self.get("render.band_height", int, lo=0)
        dbl = self.get("render.double_buffer", bool)
        anim = self.get("render.animations", bool)
        self.get("render.reason", str, nonempty=True)

        if tier is not None and lvgl is not None:
            if tier == 0 and lvgl:
                self.err("render.lvgl", "tier 0 is the software compositor; LVGL does not fit")
            if tier in (1, 2) and not lvgl:
                self.err("render.lvgl", "tiers 1 and 2 are the LVGL tiers")
        if tier is not None and comp is not None:
            if tier == 0 and comp == "lvgl":
                self.err("render.compositor", "tier 0 cannot use the lvgl compositor")
            if tier in (1, 2) and comp != "lvgl":
                self.err("render.compositor", "tier %d must use the lvgl compositor, got %r"
                         % (tier, comp))
        if tier == 2 and self.d.get("chip", {}).get("psram", {}).get("present") is not True:
            self.err("render.tier", "tier 2 is the PSRAM tier and this board has none")
        if tier is not None and dbl is not None and dbl != (tier == 2):
            self.err("render.double_buffer",
                     "tier 2 double buffers and tiers 0 and 1 do not; tier is %d and "
                     "double_buffer is %s" % (tier, str(dbl).lower()))
        if tier is not None and anim and tier != 2:
            self.err("render.animations", "animations are a tier 2 budget; tier is %d" % tier)

        if comp is not None and pal is not None:
            if comp == "indexed8" and pal < 2:
                self.err("render.palette_entries", "the indexed8 compositor needs a palette")
            if comp != "indexed8" and pal != 0:
                self.err("render.palette_entries", "must be 0 for the %r compositor" % comp)
        if comp == "lvgl" and full:
            self.err("render.full_framebuffer",
                     "LVGL owns its own buffers; set this false and put the draw buffer "
                     "height in band_height")
        if full is True and band not in (None, 0):
            self.err("render.band_height", "must be 0 when full_framebuffer is true")
        if full is False and band is not None and band <= 0:
            self.err("render.band_height", "must be > 0 when full_framebuffer is false")
        if h is not None and band is not None and band > h:
            self.err("render.band_height", "%d rows is taller than the %d-row screen" % (band, h))
        if comp == "mono1" and band not in (None, 0) and band % 8 != 0:
            self.err("render.band_height",
                     "a 1bpp band must be a multiple of 8 rows; bytes pack vertically")

        mtw = self.get("render.min_tile_w", int, lo=8)
        mth = self.get("render.min_tile_h", int, lo=8)
        # These two go straight into eos_wm_cfg_t. The window manager tabs a
        # split it cannot satisfy, so a min tile WIDER than the screen is not
        # an error, it just means this panel never splits that way. A min tile
        # wider than the screen in BOTH axes means it can never split at all,
        # which is a tiling window manager that does not tile.
        if w is not None and h is not None and mtw is not None and mth is not None:
            if mtw > w and mth > h:
                self.err("render.min_tile_w",
                         "%dx%d does not fit the %dx%d screen in either axis, so no "
                         "split can ever succeed and every window would tab"
                         % (mtw, mth, w, h))

        heap = self.get("render.heap_budget_bytes", int, lo=1024)
        if heap is not None and not self.errors:
            try:
                dv = derive(self.d)
            except (KeyError, TypeError):
                dv = None
            if dv is not None and heap < dv["render_ram"]:
                self.err("render.heap_budget_bytes",
                         "%d bytes, but this profile's own buffers already need %d "
                         "(%d framebuffer + %d staging)"
                         % (heap, dv["render_ram"], dv["fb_bytes"], dv["blit_bytes"]))

    def check_inputs(self, target):
        present = self.get("inputs.bluetooth_keyboard.present", bool)
        stack = self.get("inputs.bluetooth_keyboard.stack", str, choices=set(BT))
        reason = self.get("inputs.bluetooth_keyboard.reason", str)
        if present is False and stack != "none":
            self.err("inputs.bluetooth_keyboard.stack", "must be \"none\" when present is false")
        if present is True:
            if stack == "none":
                self.err("inputs.bluetooth_keyboard.stack", "required when present is true")
            if not (reason or "").strip():
                self.err("inputs.bluetooth_keyboard.reason",
                         "required and non-empty when present is true")
        psram = self.d.get("chip", {}).get("psram", {}).get("present")
        if stack == "bluedroid" and psram is not True:
            self.err("inputs.bluetooth_keyboard.stack",
                     "Bluedroid is 83KB of heap against NimBLE's 19KB and will not fit on a "
                     "board with no PSRAM; it OOMs during BLE init")

        self.get("inputs.web_input", bool)

        tp = self.get("inputs.touch.present", bool)
        tc = self.get("inputs.touch.controller", str, nullable=True, required=True)
        tb = self.get("inputs.touch.bus", str, nullable=True, required=True,
                      choices={"spi", "i2c", None})
        ta = self.get("inputs.touch.i2c_address", int, nullable=True, required=True, lo=3, hi=119)
        tr = self.get("inputs.touch.absent_reason", str)
        tpins = {}
        for k in ("sck", "mosi", "miso", "cs", "irq", "sda", "scl"):
            tpins[k] = self.pin("inputs.touch.pins." + k, target, "touch %s" % k,
                                out_capable=(k not in ("miso", "irq")))
        if tp is False:
            if tc is not None:
                self.err("inputs.touch.controller", "must be null when present is false")
            if tb is not None:
                self.err("inputs.touch.bus", "must be null when present is false")
            if ta is not None:
                self.err("inputs.touch.i2c_address", "must be null when present is false")
            if not (tr or "").strip():
                self.err("inputs.touch.absent_reason",
                         "required and non-empty when there is no touch controller; write down "
                         "what was probed so nobody probes it again")
            for k, v in tpins.items():
                if v not in (None, -1):
                    self.err("inputs.touch.pins." + k, "must be -1 when there is no touch controller")
        elif tp is True:
            if not (tc or "").strip():
                self.err("inputs.touch.controller", "required when present is true")
            if tb is None:
                self.err("inputs.touch.bus", "required when present is true")
            if tb == "i2c" and ta is None:
                self.err("inputs.touch.i2c_address", "required for an I2C touch controller")

        btns = self.get("inputs.buttons", list)
        if btns is not None:
            if len(btns) > MAX_BUTTONS:
                self.err("inputs.buttons", "%d buttons, but eos_board_t's buttons[] "
                         "holds %d" % (len(btns), MAX_BUTTONS))
            seen = set()
            for i, b in enumerate(btns):
                base = "inputs.buttons.%d" % i
                if not isinstance(b, dict):
                    self.err(base, "expected an object")
                    continue
                nm = self.get(base + ".name", str, nonempty=True)
                if nm is not None and not NAME_RE.match(nm):
                    self.err(base + ".name", "must match [a-z0-9_]+, got %r" % nm)
                if nm in seen:
                    self.err(base + ".name", "duplicate button name %r" % nm)
                seen.add(nm)
                self.pin(base + ".gpio", target, "a button", out_capable=False, allow_none=False)
                self.get(base + ".active_low", bool)
                self.get(base + ".pull", str, choices={"none", "up", "down"})

    def check_peripherals(self, target):
        lp = self.get("peripherals.rgb_led.present", bool)
        lk = self.get("peripherals.rgb_led.kind", str, choices=set(LED))
        r = self.pin("peripherals.rgb_led.pins.r", target, "the red LED channel")
        g = self.pin("peripherals.rgb_led.pins.g", target, "the green LED channel")
        b = self.pin("peripherals.rgb_led.pins.b", target, "the blue LED channel")
        dp = self.pin("peripherals.rgb_led.data_pin", target, "the LED data line")
        cnt = self.get("peripherals.rgb_led.count", int, lo=0)
        al = self.get("peripherals.rgb_led.active_low", bool)
        if lp is not None and lk is not None and (lk == "none") == lp:
            self.err("peripherals.rgb_led.kind", "present is %s but kind is %r" % (lp, lk))
        if lk == "gpio_rgb":
            for nm, v in (("r", r), ("g", g), ("b", b)):
                if v == -1:
                    self.err("peripherals.rgb_led.pins." + nm, "required for kind gpio_rgb")
            if dp not in (None, -1):
                self.err("peripherals.rgb_led.data_pin", "must be -1 for kind gpio_rgb")
        elif lk == "ws2812":
            if dp == -1:
                self.err("peripherals.rgb_led.data_pin", "required for kind ws2812")
            for nm, v in (("r", r), ("g", g), ("b", b)):
                if v not in (None, -1):
                    self.err("peripherals.rgb_led.pins." + nm, "must be -1 for kind ws2812")
            if al:
                self.err("peripherals.rgb_led.active_low", "a WS2812 is data-driven, not active low")
            if cnt is not None and cnt < 1:
                self.err("peripherals.rgb_led.count", "a WS2812 strip needs at least one pixel")
        elif lk == "none":
            for nm, v in (("pins.r", r), ("pins.g", g), ("pins.b", b), ("data_pin", dp)):
                if v not in (None, -1):
                    self.err("peripherals.rgb_led." + nm, "must be -1 when there is no LED")
            if cnt not in (None, 0):
                self.err("peripherals.rgb_led.count", "must be 0 when there is no LED")

        sp = self.get("peripherals.speaker.present", bool)
        sk = self.get("peripherals.speaker.kind", str, choices=set(SPK))
        spin = self.pin("peripherals.speaker.pin", target, "the speaker")
        if sp is not None and sk is not None and (sk == "none") == sp:
            self.err("peripherals.speaker.kind", "present is %s but kind is %r" % (sp, sk))
        if sk == "none" and spin not in (None, -1):
            self.err("peripherals.speaker.pin", "must be -1 when there is no speaker")
        if sk in ("dac", "pwm", "i2s") and spin == -1:
            self.err("peripherals.speaker.pin", "required for kind %r" % sk)
        if sk == "dac" and target:
            dac = TARGETS[target]["dac"]
            if not dac:
                self.err("peripherals.speaker.kind",
                         "%s has no DAC at all; use pwm or i2s" % target)
            elif spin not in (None, -1) and spin not in dac:
                self.err("peripherals.speaker.pin",
                         "the %s DAC only comes out on GPIO %s, got %d"
                         % (target, " or ".join(str(x) for x in dac), spin))

        gp = self.get("peripherals.light_sensor.present", bool)
        gpin = self.pin("peripherals.light_sensor.pin", target, "the light sensor",
                        out_capable=False)
        unit = self.get("peripherals.light_sensor.adc_unit", int, lo=0, hi=2)
        chan = self.get("peripherals.light_sensor.adc_channel", int, lo=-1, hi=9)
        if gp is False:
            if gpin not in (None, -1):
                self.err("peripherals.light_sensor.pin", "must be -1 when absent")
            if unit not in (None, 0) or chan not in (None, -1):
                self.err("peripherals.light_sensor", "absent, so adc_unit must be 0 and adc_channel -1")
        elif gp is True:
            if gpin == -1:
                self.err("peripherals.light_sensor.pin", "required when present")
            if unit == 2 and target == "esp32":
                self.err("peripherals.light_sensor.adc_unit",
                         "ADC2 is owned by the radio on esp32 and reads fail while WiFi is up")
            if target == "esp32" and unit == 1 and gpin not in (None, -1):
                want = ESP32_ADC1.get(gpin)
                if want is None:
                    self.err("peripherals.light_sensor.pin",
                             "GPIO %d is not on ADC1" % gpin)
                elif chan is not None and chan != want:
                    self.err("peripherals.light_sensor.adc_channel",
                             "GPIO %d is ADC1 channel %d, profile says %d" % (gpin, want, chan))

        dp2 = self.get("peripherals.sdcard.present", bool)
        sbus = self.get("peripherals.sdcard.bus", str, nullable=True, required=True,
                        choices={"spi", "sdmmc", None})
        shost = self.get("peripherals.sdcard.spi_host", str, nullable=True, required=True,
                         choices={"HSPI", "VSPI", "FSPI", "SPI2", "SPI3", None})
        spins = {}
        for k in ("sck", "mosi", "miso", "cs"):
            spins[k] = self.pin("peripherals.sdcard.pins." + k, target, "SD %s" % k,
                                out_capable=(k != "miso"))
        self.get("peripherals.sdcard.max_clock_hz", int, lo=0)
        shares = self.get("peripherals.sdcard.shares_display_bus", bool)
        if dp2 is False:
            if sbus is not None or shost is not None:
                self.err("peripherals.sdcard", "absent, so bus and spi_host must be null")
            for k, v in spins.items():
                if v not in (None, -1):
                    self.err("peripherals.sdcard.pins." + k, "must be -1 when there is no card slot")
            if shares:
                self.err("peripherals.sdcard.shares_display_bus", "must be false when absent")
        elif dp2 is True:
            if sbus is None:
                self.err("peripherals.sdcard.bus", "required when present")
            if sbus == "spi":
                if shost is None:
                    self.err("peripherals.sdcard.spi_host", "required for an SPI card slot")
                for k in ("sck", "mosi", "miso", "cs"):
                    if spins[k] == -1:
                        self.err("peripherals.sdcard.pins." + k, "required for an SPI card slot")
                dhost = self.d.get("display", {}).get("spi_host")
                if shost is not None and dhost is not None:
                    same = (shost == dhost)
                    if same and shares is False:
                        self.err("peripherals.sdcard.shares_display_bus",
                                 "the card and the panel are both on %s, so this must be true "
                                 "and every card access has to take the panel's bus lock" % shost)
                    if not same and shares is True:
                        self.err("peripherals.sdcard.shares_display_bus",
                                 "the card is on %s and the panel on %s, so this must be false"
                                 % (shost, dhost))

        mp = self.get("peripherals.sdcard.mount_point", str, nullable=True, required=True)
        if dp2 is True and not mp:
            self.err("peripherals.sdcard.mount_point", "required when there is a card slot")
        if dp2 is False and mp is not None:
            self.err("peripherals.sdcard.mount_point", "must be null when there is no card slot")
        if mp is not None and not mp.startswith("/"):
            self.err("peripherals.sdcard.mount_point", "must be absolute, got %r" % mp)

        ifp = self.get("peripherals.internal_fs.present", bool)
        ifk = self.get("peripherals.internal_fs.kind", str,
                       choices={"none", "littlefs", "spiffs", "fatfs"})
        ifl = self.get("peripherals.internal_fs.partition_label", str, nullable=True, required=True)
        ifm = self.get("peripherals.internal_fs.mount_point", str, nullable=True, required=True)
        if ifp is not None and ifk is not None and (ifk == "none") == ifp:
            self.err("peripherals.internal_fs.kind", "present is %s but kind is %r" % (ifp, ifk))
        if ifp is True:
            if not ifl:
                self.err("peripherals.internal_fs.partition_label",
                         "required; it has to match the label in the partition CSV that "
                         "flashing.partition_scheme names")
            elif len(ifl) > 16:
                self.err("peripherals.internal_fs.partition_label",
                         "partition labels are at most 16 characters, got %d" % len(ifl))
            if not ifm:
                self.err("peripherals.internal_fs.mount_point", "required")
            elif not ifm.startswith("/"):
                self.err("peripherals.internal_fs.mount_point", "must be absolute, got %r" % ifm)
            if ifm and mp and ifm == mp:
                self.err("peripherals.internal_fs.mount_point",
                         "the card and the internal filesystem both mount at %r" % ifm)
        elif ifp is False:
            if ifl is not None or ifm is not None:
                self.err("peripherals.internal_fs",
                         "absent, so partition_label and mount_point must be null")

    def check_flashing(self):
        self.get("flashing.port_hint", str, nullable=True, required=True)
        self.get("flashing.usb_bridge", str,
                 choices={"CH340", "CP2102", "usb_serial_jtag", "unknown"})
        up = self.get("flashing.upload_baud", int,
                      choices={115200, 230400, 460800, 921600, 1500000})
        self.get("flashing.monitor_baud", int, choices={115200, 230400, 460800, 921600})
        bad = self.get("flashing.bad_baud_rates", list)
        if bad is not None:
            if len(bad) > MAX_BAD_BAUDS:
                self.err("flashing.bad_baud_rates",
                         "%d entries, but the header emits at most %d" % (len(bad), MAX_BAD_BAUDS))
            seen = set()
            for i, e in enumerate(bad):
                base = "flashing.bad_baud_rates.%d" % i
                if not isinstance(e, dict):
                    self.err(base, "expected an object")
                    continue
                b = self.get(base + ".baud", int, lo=9600)
                self.get(base + ".reason", str, nonempty=True)
                if b is not None and b == up:
                    self.err(base + ".baud",
                             "%d is listed as known-bad and also as upload_baud" % b)
                if b in seen:
                    self.err(base + ".baud", "listed twice")
                seen.add(b)
        self.get("flashing.flash_mode", str, choices={"qio", "qout", "dio", "dout"})
        self.get("flashing.flash_freq_mhz", int, choices={20, 26, 40, 80, 120})
        self.get("flashing.partition_scheme", str, nonempty=True)
        app = self.get("flashing.app_partition_kb", int, lo=256)
        flash_mb = self.d.get("chip", {}).get("flash_size_mb")
        if app is not None and isinstance(flash_mb, int) and app > flash_mb * 1024:
            self.err("flashing.app_partition_kb",
                     "%d KB of app on a %d MB part" % (app, flash_mb))
        self.get("flashing.ota_slots", int, lo=0, hi=2)
        self.get("flashing.auto_reset", bool)

    def check_identification(self):
        auto = self.get("identification.auto_detectable", bool)
        if auto is not False:
            self.err("identification.auto_detectable",
                     "must be false. esptool reports chip type, flash size, PSRAM presence and "
                     "MAC, and nothing else; the panel controller is invisible to it and the "
                     "ILI9488 answers its own ID register with 00 7F DF")
        self.get("identification.esptool_reports.chip", str, nonempty=True)
        efl = self.get("identification.esptool_reports.flash_size_mb", int, lo=1)
        eps = self.get("identification.esptool_reports.psram", bool)
        flash_mb = self.d.get("chip", {}).get("flash_size_mb")
        psram = self.d.get("chip", {}).get("psram", {}).get("present")
        if efl is not None and isinstance(flash_mb, int) and efl != flash_mb:
            self.err("identification.esptool_reports.flash_size_mb",
                     "says %d MB but chip.flash_size_mb is %d" % (efl, flash_mb))
        if eps is not None and isinstance(psram, bool) and eps != psram:
            self.err("identification.esptool_reports.psram",
                     "says %s but chip.psram.present is %s" % (eps, psram))
        self.get("identification.panel_id_register.supported", bool)
        self.get("identification.panel_id_register.register", str, nullable=True, required=True)
        self.get("identification.panel_id_register.reads_back", str, nullable=True, required=True)
        self.get("identification.panel_id_register.note", str)
        self.get("identification.usb_serial", str, nullable=True, required=True)
        macs = self.get("identification.mac_allowlist", list)
        if macs is not None:
            for i, m in enumerate(macs):
                if not isinstance(m, str) or not MAC_RE.match(m):
                    self.err("identification.mac_allowlist.%d" % i,
                             "expected a lowercase colon-separated MAC, got %r" % m)
            if len(set(macs)) != len(macs):
                self.err("identification.mac_allowlist", "contains duplicates")
        notes = self.get("identification.distinguishing_notes", list)
        if notes is not None:
            if not notes:
                self.err("identification.distinguishing_notes",
                         "at least one note; something has to tell this board from its twin")
            for i, n in enumerate(notes):
                if not isinstance(n, str) or not n.strip():
                    self.err("identification.distinguishing_notes.%d" % i, "must be a non-empty string")
        self.get("identification.confirm_prompt", str, nonempty=True)

    def check_pin_conflicts(self):
        """Every GPIO with a job, checked for double booking. The one legal
        overlap is a card slot sharing the panel's SPI wires, and only when the
        profile admits it."""
        d = self.d
        roles = []

        def add(pin, role):
            if isinstance(pin, int) and pin >= 0:
                roles.append((pin, role))

        disp = d.get("display", {})
        dpins = disp.get("pins", {}) if isinstance(disp, dict) else {}
        for k in ("sck", "mosi", "miso", "dc", "cs", "rst", "sda", "scl"):
            add(dpins.get(k), "display." + k)
        add(disp.get("backlight", {}).get("pin") if isinstance(disp.get("backlight"), dict) else None,
            "display.backlight")

        per = d.get("peripherals", {})
        led = per.get("rgb_led", {}) if isinstance(per, dict) else {}
        lpins = led.get("pins", {}) if isinstance(led, dict) else {}
        for k in ("r", "g", "b"):
            add(lpins.get(k), "rgb_led." + k)
        add(led.get("data_pin"), "rgb_led.data")
        add(per.get("speaker", {}).get("pin") if isinstance(per.get("speaker"), dict) else None,
            "speaker")
        add(per.get("light_sensor", {}).get("pin") if isinstance(per.get("light_sensor"), dict) else None,
            "light_sensor")
        sd = per.get("sdcard", {}) if isinstance(per, dict) else {}
        spins = sd.get("pins", {}) if isinstance(sd, dict) else {}
        for k in ("sck", "mosi", "miso", "cs"):
            add(spins.get(k), "sdcard." + k)

        inp = d.get("inputs", {})
        touch = inp.get("touch", {}) if isinstance(inp, dict) else {}
        tpins = touch.get("pins", {}) if isinstance(touch, dict) else {}
        for k in ("sck", "mosi", "miso", "cs", "irq", "sda", "scl"):
            add(tpins.get(k), "touch." + k)
        for i, b in enumerate(inp.get("buttons", []) if isinstance(inp, dict) else []):
            if isinstance(b, dict):
                add(b.get("gpio"), "button.%s" % b.get("name", i))

        shares = sd.get("shares_display_bus") is True
        shareable = {"display.sck", "display.mosi", "display.miso",
                     "sdcard.sck", "sdcard.mosi", "sdcard.miso"}
        by_pin = {}
        for pin, role in roles:
            by_pin.setdefault(pin, []).append(role)
        for pin in sorted(by_pin):
            names = by_pin[pin]
            if len(names) < 2:
                continue
            if shares and set(names) <= shareable:
                continue
            self.err("pins", "GPIO %d is claimed by %s" % (pin, " and ".join(sorted(names))))

    def check_paths(self):
        unv = self.get("unverified", list)
        if unv is not None:
            for i, p in enumerate(unv):
                if not isinstance(p, str):
                    self.err("unverified.%d" % i, "expected a dotted path string")
                    continue
                try:
                    dig(self.d, p)
                except KeyError:
                    self.err("unverified.%d" % i, "%r does not resolve in this profile" % p)
            if len(set(unv)) != len(unv):
                self.err("unverified", "contains duplicates")

        got = self.get("gotchas", list)
        if got is not None:
            if not got:
                self.err("gotchas", "at least one; a board with no gotchas has not been used yet")
            seen = set()
            for i, gch in enumerate(got):
                base = "gotchas.%d" % i
                if not isinstance(gch, dict):
                    self.err(base, "expected an object")
                    continue
                gid = self.get(base + ".id", str, nonempty=True)
                if gid is not None and not SLUG_RE.match(gid):
                    self.err(base + ".id", "must be a lowercase slug, got %r" % gid)
                if gid in seen:
                    self.err(base + ".id", "duplicate gotcha id %r" % gid)
                seen.add(gid)
                self.get(base + ".severity", str, choices={"critical", "high", "note"})
                self.get(base + ".text", str, nonempty=True)
                fld = gch.get("field", "\0")
                if fld == "\0":
                    self.err(base + ".field", "missing; use null when the fact is not about one field")
                elif fld is not None:
                    if not isinstance(fld, str):
                        self.err(base + ".field", "expected a dotted path or null")
                    else:
                        try:
                            dig(self.d, fld)
                        except KeyError:
                            self.err(base + ".field", "%r does not resolve in this profile" % fld)


# ------------------------------------------------------------------ derivation

def derive(d):
    """Everything the header states that the profile does not, computed once so
    a profile can never disagree with itself."""
    disp = d["display"]
    ren = d["render"]
    rot = disp["rotation"]
    nw, nh = disp["native_width"], disp["native_height"]
    w, h = (nh, nw) if rot % 2 == 1 else (nw, nh)
    bpp = disp["bytes_per_pixel"]
    comp = ren["compositor"]
    band = ren["band_height"]
    full = ren["full_framebuffer"]

    if comp == "indexed8":
        fb = w * (h if full else band)
        blit_rows = 1 if full else band
        blit = w * blit_rows * bpp
    elif comp == "mono1":
        rows = h if full else band
        fb = w * ((rows + 7) // 8)
        blit_rows = rows
        blit = 0                      # the page buffer is already the wire format
    else:                             # lvgl owns its buffers
        fb = 0
        blit_rows = band
        blit = w * band * bpp

    return {
        "w": w, "h": h,
        "fb_bytes": fb,
        "blit_rows": blit_rows,
        "blit_bytes": blit,
        "render_ram": fb + blit,
        "wire_frame_bytes": w * h * bpp if bpp else (w * ((h + 7) // 8)),
    }


# -------------------------------------------------------------------- emission

def cstr(s):
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def cbool(v):
    return "true" if v else "false"


def csuffix(board_id):
    """The board id as a C identifier fragment. ID_RE already limits ids to
    lowercase, digits and hyphens, so this only has to move the hyphens."""
    return re.sub(r"[^A-Za-z0-9]", "_", board_id)


def wrap(text, width, prefix):
    words, lines, cur = text.split(), [], ""
    for wd in words:
        if cur and len(cur) + 1 + len(wd) > width:
            lines.append(prefix + cur)
            cur = wd
        else:
            cur = (cur + " " + wd) if cur else wd
    if cur:
        lines.append(prefix + cur)
    return lines


class Out:
    def __init__(self):
        self.lines = []
        self.pending = []

    def raw(self, text=""):
        self.flush()
        self.lines.append(text)

    def comment(self, text, width=76):
        self.flush()
        self.lines.extend(wrap(text, width, "// "))

    def section(self, title):
        self.flush()
        bar = "-" * max(4, 74 - len(title))
        self.lines.append("")
        self.lines.append("// %s %s" % (bar, title))

    def d(self, name, value):
        self.pending.append((name, str(value)))

    def flush(self):
        if not self.pending:
            return
        width = max(len(n) for n, _ in self.pending)
        for n, v in self.pending:
            self.lines.append("#define %-*s %s" % (width, n, v))
        self.pending = []

    def text(self):
        self.flush()
        return "\n".join(self.lines).rstrip() + "\n"


def emit(d, src, digest):
    dv = derive(d)
    o = Out()
    disp, ren, inp, per, fl, ident = (d["display"], d["render"], d["inputs"],
                                      d["peripherals"], d["flashing"], d["identification"])
    bt = inp["bluetooth_keyboard"]
    touch = inp["touch"]
    led, spk, light, sd = per["rgb_led"], per["speaker"], per["light_sensor"], per["sdcard"]
    guard = "EOS_BOARD_%s_H" % csuffix(d["id"]).upper()
    obj = "eos_board_" + csuffix(d["id"]).lower()
    init = "EOS_BOARD_INIT_" + csuffix(d["id"]).upper()

    o.raw("// Generated by tools/gen_board_header.py from %s - do not edit."
          % (os.path.relpath(src) if src else "a board profile"))
    o.raw("//")
    o.comment(d["summary"])
    o.raw("//")
    o.comment("Board identity is not probeable. esptool reports chip, flash size, PSRAM "
              "and MAC; the panel controller, the touch chip and the LED wiring are "
              "invisible to it. This header is the answer, and it is only correct because "
              "a human confirmed the board once.")
    o.raw("//")
    o.raw("// profile sha256: %s" % digest)

    unv = d.get("unverified", [])
    o.raw("//")
    if unv:
        o.comment("Not verified on hardware - conservative defaults, not measurements. "
                  "Treat a surprise in one of these as a profile bug first:")
        for p in unv:
            o.raw("//   %s" % p)
    else:
        o.raw("// Every value in this profile was measured on hardware.")

    if d.get("gotchas"):
        o.raw("//")
        o.raw("// Gotchas, most expensive first. Comments, so they cost no flash:")
        order = {"critical": 0, "high": 1, "note": 2}
        for g in sorted(d["gotchas"], key=lambda g: order.get(g["severity"], 9)):
            head = "//   [%s] %s" % (g["severity"], g["id"])
            if g.get("field"):
                head += "  (%s)" % g["field"]
            o.raw(head)
            for ln in wrap(g["text"], 70, "//       "):
                o.raw(ln)

    o.raw()
    o.raw("#ifndef %s" % guard)
    o.raw("#define %s" % guard)
    o.raw()
    o.raw("#include <stdint.h>")
    o.raw("#include <stdbool.h>")
    o.raw("#include <stddef.h>   /* NULL */")
    o.raw()
    o.comment("eos_board_t and every enum it is built from are declared once, in "
              "kernel/hal/include/eos_board.h. This file is data: macros, one "
              "initialiser, one const instance. It declares no types of its own, so "
              "it composes with the rest of the HAL instead of colliding with it.")
    o.raw("#include \"eos_board.h\"")

    o.section("the unsuffixed names")
    o.raw()
    o.comment("A firmware image includes exactly one of these headers, so the plain "
              "EOS_* names describe its board and nothing else has a claim on them. A "
              "test that wants all six registry entries at once includes six headers "
              "in one translation unit; the first one included takes the plain names, "
              "the rest contribute only their suffixed data below. EOS_BOARD_ACTIVE "
              "says which board won, so the ambiguity is never silent.")
    o.raw("#ifndef EOS_BOARD_ACTIVE")
    o.raw("#define EOS_BOARD_ACTIVE %s" % cstr(d["id"]))

    o.section("identity")
    o.d("EOS_BOARD_GENERATED", 1)
    o.d("EOS_BOARD_ID", cstr(d["id"]))
    o.d("EOS_BOARD_NAME", cstr(d["name"]))
    o.d("EOS_BOARD_PROFILE_SHA256", cstr(digest))

    o.section("chip")
    target = d["chip"]["target"]
    o.d("EOS_TARGET", cstr(target))
    o.d("EOS_TARGET_%s" % target.upper(), 1)
    o.d("EOS_CHIP_VARIANT", cstr(d["chip"]["variant"]))
    o.d("EOS_CHIP_CORES", d["chip"]["cores"])
    o.d("EOS_FLASH_MB", d["chip"]["flash_size_mb"])
    o.d("EOS_HAS_PSRAM", 1 if d["chip"]["psram"]["present"] else 0)
    o.d("EOS_PSRAM_MB", d["chip"]["psram"]["size_mb"])

    o.section("render tier")
    o.d("EOS_TIER", ren["tier"])
    o.d("EOS_TIER_%d" % ren["tier"], 1)
    o.d("EOS_USE_LVGL", 1 if ren["lvgl"] else 0)
    o.d("EOS_COMPOSITOR", COMP[ren["compositor"]])
    o.d("EOS_COMPOSITOR_%s" % ren["compositor"].upper(), 1)
    o.d("EOS_PALETTE_ENTRIES", ren["palette_entries"])
    o.d("EOS_FB_FULL", 1 if ren["full_framebuffer"] else 0)
    o.d("EOS_BAND_H", ren["band_height"])
    o.d("EOS_MIN_TILE_W", ren["min_tile_w"])
    o.d("EOS_MIN_TILE_H", ren["min_tile_h"])
    o.d("EOS_FB_BYTES", dv["fb_bytes"])
    o.d("EOS_BLIT_ROWS", dv["blit_rows"])
    o.d("EOS_BLIT_BYTES", dv["blit_bytes"])
    o.d("EOS_RENDER_RAM_BYTES", dv["render_ram"])
    o.d("EOS_WIRE_FRAME_BYTES", dv["wire_frame_bytes"])
    o.d("EOS_DOUBLE_BUFFER", 1 if ren["double_buffer"] else 0)
    o.d("EOS_ANIMATIONS", 1 if ren["animations"] else 0)

    o.section("display")
    o.d("EOS_LCD_CONTROLLER", cstr(disp["controller"]))
    o.d("EOS_LCD_DRIVER_%s" % disp["driver_source"].upper(), 1)
    if disp["driver_source"] == "bsp":
        o.d("EOS_LCD_BSP_COMPONENT", cstr(disp["bsp_component"]))
        o.d("EOS_LCD_BSP_VERSION", cstr(disp["bsp_version"]))
    o.d("EOS_LCD_NATIVE_W", disp["native_width"])
    o.d("EOS_LCD_NATIVE_H", disp["native_height"])
    o.d("EOS_LCD_ROTATION", disp["rotation"])
    o.d("EOS_LCD_W", dv["w"])
    o.d("EOS_LCD_H", dv["h"])
    o.d("EOS_LCD_DEPTH", disp["color_depth"])
    o.d("EOS_LCD_BPP", disp["bytes_per_pixel"])
    o.d("EOS_LCD_16BIT", 1 if disp["supports_16bit_pixels"] else 0)
    o.d("EOS_LCD_COLOR_%s" % disp["color_order"].upper(), 1)
    o.d("EOS_LCD_BUS_%s" % disp["bus"].upper(), 1)
    if disp["bus"] == "spi":
        o.d("EOS_LCD_SPI_HOST", cstr(disp["spi_host"]))
        o.d("EOS_LCD_PIN_SCK", disp["pins"]["sck"])
        o.d("EOS_LCD_PIN_MOSI", disp["pins"]["mosi"])
        o.d("EOS_LCD_PIN_MISO", disp["pins"]["miso"])
        o.d("EOS_LCD_PIN_DC", disp["pins"]["dc"])
        o.d("EOS_LCD_PIN_CS", disp["pins"]["cs"])
        o.d("EOS_LCD_PIN_RST", disp["pins"]["rst"])
    else:
        o.d("EOS_LCD_PIN_SDA", disp["pins"]["sda"])
        o.d("EOS_LCD_PIN_SCL", disp["pins"]["scl"])
        o.d("EOS_LCD_I2C_ADDR", "0x%02X" % disp["i2c_address"])
    o.d("EOS_LCD_CLOCK_HZ", disp["clock_hz"])
    o.d("EOS_LCD_PIN_BL", disp["backlight"]["pin"])
    o.d("EOS_LCD_BL_ACTIVE_LOW", 1 if disp["backlight"]["active_low"] else 0)
    o.d("EOS_LCD_BL_PWM", 1 if disp["backlight"]["pwm"] else 0)
    o.d("EOS_LCD_COL_OFFSET", disp["col_offset"])
    o.d("EOS_LCD_ROW_OFFSET", disp["row_offset"])
    o.d("EOS_LCD_INVERT", 1 if disp["invert"] else 0)
    o.d("EOS_LCD_BYTE_SWAP", 1 if disp["byte_swap"] else 0)
    o.d("EOS_LCD_MIRROR_X", 1 if disp["mirror_x"] else 0)
    o.d("EOS_LCD_MIRROR_Y", 1 if disp["mirror_y"] else 0)

    o.section("inputs")
    o.d("EOS_HAS_BT_KEYBOARD", 1 if bt["present"] else 0)
    if bt["stack"] != "none":
        o.d("EOS_BT_STACK_%s" % bt["stack"].upper(), 1)
    o.d("EOS_HAS_TOUCH", 1 if touch["present"] else 0)
    if touch["present"]:
        o.d("EOS_TOUCH_CONTROLLER", cstr(touch["controller"]))
        o.d("EOS_TOUCH_BUS_%s" % touch["bus"].upper(), 1)
        for k in ("sck", "mosi", "miso", "cs", "irq", "sda", "scl"):
            if touch["pins"][k] != -1:
                o.d("EOS_TOUCH_PIN_%s" % k.upper(), touch["pins"][k])
        if touch["i2c_address"] is not None:
            o.d("EOS_TOUCH_I2C_ADDR", "0x%02X" % touch["i2c_address"])
    o.d("EOS_BTN_COUNT", len(inp["buttons"]))
    for i, b in enumerate(inp["buttons"]):
        o.d("EOS_BTN%d_NAME" % i, cstr(b["name"]))
        o.d("EOS_BTN%d_GPIO" % i, b["gpio"])
        o.d("EOS_BTN%d_ACTIVE_LOW" % i, 1 if b["active_low"] else 0)
        o.d("EOS_BTN%d_PULL_UP" % i, 1 if b["pull"] == "up" else 0)

    o.section("peripherals")
    o.d("EOS_HAS_RGB_LED", 1 if led["present"] else 0)
    if led["kind"] != "none":
        o.d("EOS_LED_KIND_%s" % led["kind"].upper(), 1)
        if led["kind"] == "gpio_rgb":
            o.d("EOS_LED_PIN_R", led["pins"]["r"])
            o.d("EOS_LED_PIN_G", led["pins"]["g"])
            o.d("EOS_LED_PIN_B", led["pins"]["b"])
        else:
            o.d("EOS_LED_PIN_DATA", led["data_pin"])
        o.d("EOS_LED_COUNT", led["count"])
        o.d("EOS_LED_ACTIVE_LOW", 1 if led["active_low"] else 0)
    o.d("EOS_HAS_SPEAKER", 1 if spk["present"] else 0)
    if spk["kind"] != "none":
        o.d("EOS_SPEAKER_KIND_%s" % spk["kind"].upper(), 1)
        o.d("EOS_SPEAKER_PIN", spk["pin"])
    o.d("EOS_HAS_LIGHT_SENSOR", 1 if light["present"] else 0)
    if light["present"]:
        o.d("EOS_LIGHT_PIN", light["pin"])
        o.d("EOS_LIGHT_ADC_UNIT", light["adc_unit"])
        o.d("EOS_LIGHT_ADC_CHANNEL", light["adc_channel"])
    o.d("EOS_HAS_SD", 1 if sd["present"] else 0)
    if sd["present"]:
        if sd["spi_host"]:
            o.d("EOS_SD_SPI_HOST", cstr(sd["spi_host"]))
        o.d("EOS_SD_PIN_SCK", sd["pins"]["sck"])
        o.d("EOS_SD_PIN_MOSI", sd["pins"]["mosi"])
        o.d("EOS_SD_PIN_MISO", sd["pins"]["miso"])
        o.d("EOS_SD_PIN_CS", sd["pins"]["cs"])
        o.d("EOS_SD_CLOCK_HZ", sd["max_clock_hz"])
        o.d("EOS_SD_SHARES_LCD_BUS", 1 if sd["shares_display_bus"] else 0)

    o.section("flashing")
    if fl["port_hint"]:
        o.d("EOS_FLASH_PORT_HINT", cstr(fl["port_hint"]))
    o.d("EOS_USB_BRIDGE", cstr(fl["usb_bridge"]))
    o.d("EOS_UPLOAD_BAUD", fl["upload_baud"])
    o.d("EOS_MONITOR_BAUD", fl["monitor_baud"])
    o.d("EOS_FLASH_MODE", cstr(fl["flash_mode"]))
    o.d("EOS_FLASH_FREQ_MHZ", fl["flash_freq_mhz"])
    o.d("EOS_PARTITION_SCHEME", cstr(fl["partition_scheme"]))
    o.d("EOS_APP_PARTITION_KB", fl["app_partition_kb"])
    o.d("EOS_OTA_SLOTS", fl["ota_slots"])
    o.d("EOS_AUTO_RESET", 1 if fl["auto_reset"] else 0)
    o.d("EOS_BAD_BAUD_COUNT", len(fl["bad_baud_rates"]))
    for i, bb in enumerate(fl["bad_baud_rates"]):
        o.d("EOS_BAD_BAUD%d" % i, bb["baud"])
        # The reason travels with the rate. It is what an operator needs when a
        # flash fails, and it costs nothing until something prints it.
        o.d("EOS_BAD_BAUD%d_REASON" % i, cstr(bb["reason"]))

    o.section("identification (for the flasher, not for the firmware)")
    o.d("EOS_ID_AUTO_DETECTABLE", 0)
    o.d("EOS_ID_ESPTOOL_CHIP", cstr(ident["esptool_reports"]["chip"]))
    o.d("EOS_ID_CONFIRM_PROMPT", cstr(ident["confirm_prompt"]))
    o.d("EOS_ID_MAC_ALLOWLIST_COUNT", len(ident["mac_allowlist"]))
    for i, m in enumerate(ident["mac_allowlist"]):
        o.d("EOS_ID_MAC%d" % i, cstr(m))

    o.section("aliases onto this board's suffixed data")
    o.d("EOS_BOARD", obj)
    o.d("EOS_BOARD_INIT", init)
    o.raw()
    o.raw("#endif /* EOS_BOARD_ACTIVE */")

    o.section("the board")
    o.raw()
    o.comment("Suffixed with the board id, so six of these can share a translation "
              "unit. The board component says &EOS_BOARD and never spells the suffix.")
    o.raw("#define %s { \\" % init)
    body = hal_init(d, dv)
    for ln in body:
        o.raw("    %s \\" % ln)
    o.raw("}")
    o.raw()
    o.comment("One copy, in rodata. Define EOS_BOARD_NO_INSTANCE and use the initialiser "
              "yourself if a translation unit wants the descriptor somewhere else.")
    o.raw("#ifndef EOS_BOARD_NO_INSTANCE")
    o.raw("static const eos_board_t %s = %s;" % (obj, init))
    o.raw("#endif")
    o.raw()
    o.raw("#endif /* %s */" % guard)
    return o.text()


# -------------------------------------------------------- the HAL descriptor
#
# kernel/hal/include/eos_board.h declares eos_board_t and names this tool as the
# thing that fills it. Its field names line up with the registry almost one for
# one, but not entirely - it groups the card and the internal filesystem into
# one storage block, keeps the LED, speaker and light sensor in "extras", and
# names panels and SoCs with enums instead of strings. So the initialiser is
# written out field by field rather than mechanically, and the tables below are
# the whole of the coupling: if that header moves, this is the part to fix.
#
# Two fields are not in the registry and are emitted as documented defaults:
# eos_button_t.key (there is no keymap in the JSON; the board component sets it)
# and eos_board_storage_t.sd_slot (no profile uses SDMMC). Registry facts with
# no struct field of their own - the IDF target string, the controller and touch
# controller names, the flashing block, and the derived framebuffer sizes - stay
# as EOS_* macros, which is where the flasher and the build system read them.

HAL_SOC = {"esp32": "EOS_SOC_ESP32", "esp32c5": "EOS_SOC_ESP32_C5",
           "esp32c6": "EOS_SOC_ESP32_C6", "esp32s3": "EOS_SOC_ESP32_S3"}
HAL_TIER = {0: "EOS_TIER_SOFT", 1: "EOS_TIER_LEAN", 2: "EOS_TIER_RICH"}
HAL_COMP = {"indexed8": "EOS_COMP_INDEXED8", "mono1": "EOS_COMP_MONO1",
            "lvgl": "EOS_COMP_LVGL"}
HAL_PANEL = {"ILI9341": "EOS_PANEL_ILI9341", "ST7789": "EOS_PANEL_ST7789",
             "ILI9488": "EOS_PANEL_ILI9488", "ST7735": "EOS_PANEL_ST7735",
             "SSD1306": "EOS_PANEL_SSD1306"}
HAL_BUS = {"spi": "EOS_BUS_SPI", "i2c": "EOS_BUS_I2C", "sdmmc": "EOS_BUS_SDMMC",
           None: "EOS_BUS_NONE"}
HAL_TOUCH = {None: "EOS_TOUCH_NONE", "XPT2046": "EOS_TOUCH_XPT2046",
             "GT911": "EOS_TOUCH_GT911", "CST816": "EOS_TOUCH_CST816",
             "AXS5106L": "EOS_TOUCH_AXS5106L"}
HAL_LED = {"none": "EOS_LED_NONE", "gpio_rgb": "EOS_LED_GPIO_RGB",
           "ws2812": "EOS_LED_WS2812"}
HAL_AUDIO = {"none": "EOS_AUDIO_NONE", "dac": "EOS_AUDIO_DAC", "i2s": "EOS_AUDIO_I2S"}
HAL_SPI_HOST = {"HSPI": 1, "SPI2": 1, "FSPI": 1, "VSPI": 2, "SPI3": 2, None: 0}

def hal_check(d):
    """What the schema cannot catch, because the HAL uses enums where the
    registry uses free strings. A controller with no eos_panel_t is a loud
    error here rather than an undefined identifier in generated code."""
    e = []
    t = d["chip"]["target"]
    if t not in HAL_SOC:
        e.append(("chip.target", "no eos_soc_t for %r; add it to HAL_SOC and to "
                                 "kernel/hal/include/eos_board.h" % t))
    c = d["display"]["controller"]
    if c not in HAL_PANEL:
        e.append(("display.controller",
                  "no eos_panel_t for %r; the HAL enum knows %s"
                  % (c, ", ".join(sorted(HAL_PANEL)))))
    tc = d["inputs"]["touch"]["controller"]
    if tc not in HAL_TOUCH:
        e.append(("inputs.touch.controller", "no eos_touch_t for %r" % tc))
    sk = d["peripherals"]["speaker"]["kind"]
    if sk not in HAL_AUDIO:
        e.append(("peripherals.speaker.kind",
                  "the HAL has no eos_audio_t for %r; it knows none, dac and i2s" % sk))
    for role, host in (("display.spi_host", d["display"]["spi_host"]),
                       ("peripherals.sdcard.spi_host", d["peripherals"]["sdcard"]["spi_host"])):
        if host not in HAL_SPI_HOST:
            e.append((role, "no HAL host index for %r" % host))
    return e


def hal_init(d, dv):
    disp, ren, inp, per, fl = (d["display"], d["render"], d["inputs"],
                               d["peripherals"], d["flashing"])
    bt, touch = inp["bluetooth_keyboard"], inp["touch"]
    led, spk, light = per["rgb_led"], per["speaker"], per["light_sensor"]
    sd, ifs = per["sdcard"], per["internal_fs"]
    p, tp = disp["pins"], touch["pins"]
    mb = 1024 * 1024
    L = []
    L.append(".id = %s," % cstr(d["id"]))
    L.append(".name = %s," % cstr(d["name"]))
    L.append(".variant = %s," % cstr(d["chip"]["variant"]))
    L.append(".soc = %s, .cores = %d, .tier = %s,"
             % (HAL_SOC[d["chip"]["target"]], d["chip"]["cores"], HAL_TIER[ren["tier"]]))
    L.append(".flash_bytes = %du," % (d["chip"]["flash_size_mb"] * mb))
    L.append(".psram_bytes = %du," % (d["chip"]["psram"]["size_mb"] * mb))
    L.append(".render = {")
    L.append("    .compositor = %s, .lvgl = %s," % (HAL_COMP[ren["compositor"]], cbool(ren["lvgl"])))
    L.append("    .palette_entries = %d," % ren["palette_entries"])
    L.append("    .full_framebuffer = %s, .band_h = %d,"
             % (cbool(ren["full_framebuffer"]), ren["band_height"]))
    L.append("    .double_buffer = %s, .animations = %s,"
             % (cbool(ren["double_buffer"]), cbool(ren["animations"])))
    L.append("    .min_tile_w = %d, .min_tile_h = %d,"
             % (ren["min_tile_w"], ren["min_tile_h"]))
    L.append("    .heap_budget = %du," % ren["heap_budget_bytes"])
    L.append("},")
    L.append(".panel = {")
    L.append("    .panel = %s, .bus = %s,"
             % (HAL_PANEL[disp["controller"]], HAL_BUS[disp["bus"]]))
    L.append("    .native_w = %d, .native_h = %d, .rotation = %d,"
             % (disp["native_width"], disp["native_height"], disp["rotation"]))
    L.append("    .color_depth = %d, .wire_bytes = %d,"
             % (disp["color_depth"], disp["bytes_per_pixel"]))
    L.append("    .bgr = %s, .invert = %s, .byte_swap = %s,"
             % (cbool(disp["color_order"] == "bgr"), cbool(disp["invert"]),
                cbool(disp["byte_swap"])))
    L.append("    .mirror_x = %s, .mirror_y = %s,"
             % (cbool(disp["mirror_x"]), cbool(disp["mirror_y"])))
    L.append("    .hz = %du," % disp["clock_hz"])
    L.append("    .col_offset = %d, .row_offset = %d," % (disp["col_offset"], disp["row_offset"]))
    L.append("    .sck = %d, .mosi = %d, .miso = %d," % (p["sck"], p["mosi"], p["miso"]))
    L.append("    .dc = %d, .cs = %d, .rst = %d," % (p["dc"], p["cs"], p["rst"]))
    L.append("    .spi_host = %d," % HAL_SPI_HOST[disp["spi_host"]])
    L.append("    .sda = %d, .scl = %d, .i2c_addr = 0x%02X,"
             % (p["sda"], p["scl"], disp["i2c_address"] or 0))
    L.append("    .bl = %d, .bl_active_low = %s, .bl_pwm = %s,"
             % (disp["backlight"]["pin"], cbool(disp["backlight"]["active_low"]),
                cbool(disp["backlight"]["pwm"])))
    L.append("},")
    L.append(".input = {")
    L.append("    .button_count = %d," % len(inp["buttons"]))
    if inp["buttons"]:
        L.append("    .buttons = {")
        for b in inp["buttons"]:
            L.append("        { .pin = %d, .active_low = %s, .pull_up = %s, .key = 0 },  /* %s */"
                     % (b["gpio"], cbool(b["active_low"]), cbool(b["pull"] == "up"),
                        b["name"]))
        L.append("    },")
    L.append("    .ble_keyboard = %s, .web_input = %s,"
             % (cbool(bt["present"]), cbool(inp["web_input"])))
    L.append("    .touch = %s, .touch_bus = %s,"
             % (HAL_TOUCH[touch["controller"]], HAL_BUS[touch["bus"]]))
    L.append("    .touch_sck = %d, .touch_mosi = %d, .touch_miso = %d,"
             % (tp["sck"], tp["mosi"], tp["miso"]))
    L.append("    .touch_cs = %d, .touch_irq = %d," % (tp["cs"], tp["irq"]))
    L.append("    .touch_sda = %d, .touch_scl = %d, .touch_addr = 0x%02X,"
             % (tp["sda"], tp["scl"], touch["i2c_address"] or 0))
    L.append("},")
    L.append(".storage = {")
    L.append("    .sd = %s, .sd_bus = %s, .sd_spi_host = %d,"
             % (cbool(sd["present"]), HAL_BUS[sd["bus"]], HAL_SPI_HOST[sd["spi_host"]]))
    L.append("    .sd_sck = %d, .sd_mosi = %d, .sd_miso = %d, .sd_cs = %d,"
             % (sd["pins"]["sck"], sd["pins"]["mosi"], sd["pins"]["miso"], sd["pins"]["cs"]))
    L.append("    .sd_slot = 0, .sd_hz = %du, .sd_shares_bus = %s,"
             % (sd["max_clock_hz"], cbool(sd["shares_display_bus"])))
    L.append("    .sd_point = %s," % (cstr(sd["mount_point"]) if sd["mount_point"] else "NULL"))
    L.append("    .int_label = %s," % (cstr(ifs["partition_label"]) if ifs["partition_label"] else "NULL"))
    L.append("    .int_point = %s," % (cstr(ifs["mount_point"]) if ifs["mount_point"] else "NULL"))
    L.append("},")
    L.append(".extras = {")
    L.append("    .led = %s," % HAL_LED[led["kind"]])
    L.append("    .led_r = %d, .led_g = %d, .led_b = %d,"
             % (led["pins"]["r"], led["pins"]["g"], led["pins"]["b"]))
    L.append("    .led_active_low = %s," % cbool(led["active_low"]))
    L.append("    .led_data = %d, .led_count = %d," % (led["data_pin"], led["count"]))
    L.append("    .audio = %s, .audio_pin = %d," % (HAL_AUDIO[spk["kind"]], spk["pin"]))
    L.append("    .ldr = %d, .ldr_adc_unit = %d, .ldr_adc_channel = %d,"
             % (light["pin"], light["adc_unit"], light["adc_channel"]))
    L.append("},")
    L.append(".auto_detectable = false,")
    L.append(".confirm_prompt = %s," % cstr(d["identification"]["confirm_prompt"]))
    L.append(".port = %s," % (cstr(fl["port_hint"]) if fl["port_hint"] else "NULL"))
    L.append(".upload_baud = %du, .monitor_baud = %du,"
             % (fl["upload_baud"], fl["monitor_baud"]))
    return L


# ------------------------------------------------------------------------ main

def load(path):
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        raise BadProfile("%s: cannot read: %s" % (path, e.strerror))
    try:
        data = json.loads(raw.decode("utf-8"))
    except UnicodeDecodeError as e:
        raise BadProfile("%s: not valid UTF-8: %s" % (path, e))
    except json.JSONDecodeError as e:
        raise BadProfile("%s: not valid JSON: line %d column %d: %s"
                         % (path, e.lineno, e.colno, e.msg))
    if os.path.basename(path) == "schema.json":
        raise BadProfile("%s: that is the JSON Schema, not a board profile. Pass one of the "
                         "board files next to it." % path)
    if not isinstance(data, dict):
        raise BadProfile("%s: expected a JSON object at the top level, got %s"
                         % (path, type(data).__name__))
    data.pop("$schema", None)
    return data, hashlib.sha256(raw).hexdigest()


def report(path, errors):
    print("%s: %d problem%s" % (path, len(errors), "" if len(errors) == 1 else "s"),
          file=sys.stderr)
    print("", file=sys.stderr)
    width = min(34, max(len(p) for p, _ in errors))
    for p, msg in errors:
        lines = wrap(msg, 76 - width - 4, "")
        if len(p) > width:
            print("  %s" % p, file=sys.stderr)
            head = ""
        else:
            head = p
        for i, ln in enumerate(lines):
            print("  %-*s  %s" % (width, head if i == 0 else "", ln), file=sys.stderr)
    print("", file=sys.stderr)


def process(src, dst, write=True):
    data, digest = load(src)
    errors = Validator(data, src).check() or hal_check(data)
    if errors:
        report(src, errors)
        return None
    text = emit(data, src, digest)
    if not write:
        return text
    parent = os.path.dirname(os.path.abspath(dst))
    try:
        os.makedirs(parent, exist_ok=True)
        with open(dst, "w", encoding="ascii") as f:
            f.write(text)
    except OSError as e:
        raise BadProfile("%s: cannot write: %s" % (dst, e.strerror))
    print("%s -> %s  (%d lines)" % (os.path.basename(src), dst, text.count("\n")))
    return text


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="gen_board_header.py",
        description="Generate the C board header from an penguinOS board profile.",
        epilog="Exit codes: 0 ok, 1 the file could not be read or is not a profile, "
               "2 bad usage or a profile that does not validate.")
    ap.add_argument("paths", nargs="*", metavar="PATH",
                    help="PROFILE.json OUTPUT.h, or one or more profiles with --check")
    ap.add_argument("--all", action="store_true",
                    help="generate a header for every profile in --boards-dir")
    ap.add_argument("--check", action="store_true",
                    help="validate only, write nothing")
    ap.add_argument("--boards-dir", default=None, metavar="DIR",
                    help="where the profiles live (default: the boards/ dir next to this tool)")
    ap.add_argument("--out-dir", default=None, metavar="DIR",
                    help="where --all writes (default: <boards-dir>/generated)")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    boards_dir = args.boards_dir or os.path.join(os.path.dirname(here), "boards")
    out_dir = args.out_dir or os.path.join(boards_dir, "generated")

    try:
        if args.all:
            if args.paths:
                ap.error("--all takes no positional arguments")
            if not os.path.isdir(boards_dir):
                print("no such boards directory: %s" % boards_dir, file=sys.stderr)
                return 1
            profiles = sorted(f for f in os.listdir(boards_dir)
                              if f.endswith(".json") and f != "schema.json")
            if not profiles:
                print("no board profiles in %s" % boards_dir, file=sys.stderr)
                return 1
            bad = 0
            for f in profiles:
                src = os.path.join(boards_dir, f)
                dst = os.path.join(out_dir, os.path.splitext(f)[0] + ".h")
                if process(src, dst, write=not args.check) is None:
                    bad += 1
            if bad:
                print("%d of %d profiles failed; nothing further was written."
                      % (bad, len(profiles)), file=sys.stderr)
                return 2
            return 0

        if args.check:
            if not args.paths:
                ap.error("--check needs at least one profile")
            # Two separate tallies, because the exit codes mean different
            # things: 1 is "that file is not a profile I can read at all",
            # 2 is "it is a profile and it is wrong". A build script keying
            # on 2 wants the second, not a typo in a path.
            unreadable = 0
            invalid = 0
            for src in args.paths:
                try:
                    data, _ = load(src)
                except BadProfile as e:
                    print(str(e), file=sys.stderr)
                    unreadable += 1
                    continue
                errors = Validator(data, src).check() or hal_check(data)
                if errors:
                    report(src, errors)
                    invalid += 1
                else:
                    print("%s: ok" % src)
            if invalid:
                return 2
            return 1 if unreadable else 0

        if len(args.paths) != 2:
            ap.error("expected PROFILE.json OUTPUT.h (got %d argument(s)); "
                     "or use --all, or --check" % len(args.paths))
        if process(args.paths[0], args.paths[1]) is None:
            print("nothing was written.", file=sys.stderr)
            return 2
        return 0
    except BadProfile as e:
        print(str(e), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
