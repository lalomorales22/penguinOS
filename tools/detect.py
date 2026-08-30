#!/usr/bin/env python3
# detect.py - work out which board is on the other end of a serial cable, as far
# as that is honestly possible.
#
# esptool will tell you the chip, the flash size, whether there is embedded
# PSRAM and the MAC. It will not tell you what is on the other end of the SPI
# bus, and boards/README.md explains at length why nothing here should pretend
# otherwise: the ILI9488 answers register 0xD3 with 00 7F DF, the Waveshare C5
# does not wire MISO to the panel at all, and two pairs of boards in the
# registry are electrically identical. So this tool narrows, it does not decide.
# It reports one of four outcomes - pinned, unique, ambiguous, none - and the
# caller is responsible for asking a human whenever that is not `pinned`.
#
# The one non-obvious constraint: detection always talks to the board at 115200,
# never at the profile's upload baud. The wavvy CP2102 cable fails with "Invalid
# head of packet (0xFF)" at 460800 and 921600 for plain reads, not just writes,
# so a detector that used the upload baud would fail to identify the one board
# whose upload baud matters most.
#
# Side-effect free by design: detection never writes the MAC cache. The caller
# calls --remember once a human has confirmed, so a confirmation is always
# something a person did, never something a probe inferred.
#
# python3, standard library only.

import argparse
import glob
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

SCHEMA = 1
TOOL = "detect.py"

# Detection baud. Not negotiable, and not the same thing as the upload baud.
DETECT_BAUD = 115200

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_BOARDS = os.path.join(REPO, "boards")
DEFAULT_CACHE = os.path.join(os.path.expanduser("~"), ".esp-os", "board-cache.json")

# Serial devices that are always present on a Mac and are never a board. Matched
# against the basename, case-insensitively, as a substring.
NOT_A_BOARD = (
    "bluetooth-incoming-port",
    "bluetooth-modem",
    "blth",
    "debug-console",
    "wlan-debug",
)

# USB bridges we can name from the descriptor. The registry records a
# `flashing.usb_bridge` per profile and these are the values it uses.
USB_BRIDGES = {
    (0x1A86, 0x7523): "CH340",
    (0x1A86, 0x7522): "CH340",
    (0x1A86, 0x5523): "CH341",
    (0x1A86, 0x55D4): "CH9102",
    (0x10C4, 0xEA60): "CP2102",
    (0x10C4, 0xEA70): "CP2105",
    (0x0403, 0x6001): "FTDI",
    (0x0403, 0x6010): "FTDI",
    (0x0403, 0x6015): "FTDI",
}
# Espressif's own vendor id. Anything under it on a serial port is the chip's
# native USB, which the registry calls usb_serial_jtag.
ESPRESSIF_VID = 0x303A

CHIP_RE = re.compile(r"(?:Chip is|Chip type:)\s+([A-Za-z0-9][A-Za-z0-9\-]*)"
                     r"(?:\s*\(revision\s*([^)]+)\))?")
DETECTING_RE = re.compile(r"Detecting chip type\.\.\.\s*([A-Za-z0-9\-]+)")
FEATURES_RE = re.compile(r"Features:\s*(.+)")
MAC_RE = re.compile(r"\bMAC:\s*([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})")
FLASH_RE = re.compile(r"(?:Detected flash size|Flash size):\s*(\d+)\s*(MB|KB)", re.I)
CRYSTAL_RE = re.compile(r"Crystal (?:is|frequency:)\s*(\d+)\s*MHz", re.I)
# ESP32-C5 / -S3 / -H2 / -P4 style part numbers. ESP32-D0WD and ESP32-PICO are
# plain esp32 and must not match here.
VARIANT_RE = re.compile(r"^ESP32-((?:C|S|H|P)\d+)\b", re.I)

# idf target -> the arduino-cli FQBN the prober is built with. Only the targets
# the registry actually contains are listed; anything else is reported as
# unknown rather than guessed.
FQBN = {
    "esp32": "esp32:esp32:esp32",
    "esp32c5": "esp32:esp32:esp32c5",
    "esp32s3": "esp32:esp32:esp32s3",
}


class Fail(Exception):
    """A condition worth printing as one clear line, never as a traceback."""

    def __init__(self, message, code=1):
        super().__init__(message)
        self.message = message
        self.code = code


# --------------------------------------------------------------- the registry


def load_profiles(boards_dir):
    if not os.path.isdir(boards_dir):
        raise Fail("no board registry at %s - pass --boards" % boards_dir, 2)
    out = []
    for path in sorted(glob.glob(os.path.join(boards_dir, "*.json"))):
        if os.path.basename(path) == "schema.json":
            continue
        try:
            with open(path, "r") as fh:
                raw = json.load(fh)
        except ValueError as exc:
            raise Fail("%s is not valid JSON: %s" % (path, exc), 1)
        except OSError as exc:
            raise Fail("cannot read %s: %s" % (path, exc), 1)
        if "id" not in raw or "chip" not in raw:
            # Not a board profile. Skip quietly; the directory may hold other
            # things one day.
            continue
        raw["_path"] = path
        out.append(raw)
    if not out:
        raise Fail("no board profiles found in %s" % boards_dir, 1)
    return out


def profile_by_id(profiles, pid):
    for p in profiles:
        if p["id"] == pid:
            return p
    return None


# ------------------------------------------------------------- serial ports


def enumerate_ports():
    """Every serial device that could plausibly be a board, sorted."""
    system = platform.system()
    if system == "Darwin":
        found = glob.glob("/dev/cu.*")
    elif system == "Linux":
        found = (glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
                 + glob.glob("/dev/serial/by-id/*"))
    else:
        found = glob.glob("/dev/cu.*") + glob.glob("/dev/ttyUSB*")
    keep = []
    for dev in found:
        base = os.path.basename(dev).lower()
        if any(bad in base for bad in NOT_A_BOARD):
            continue
        keep.append(dev)
    return sorted(set(keep))


def _ioreg_usb_map():
    """port -> USB descriptor facts, from ioreg. macOS only, best effort.

    ioreg -a emits an XML plist, which plistlib parses, which keeps this
    stdlib-only. The USB identity lives on an ancestor of the serial node, so
    the walk carries the nearest USB device's properties down to whichever
    child finally owns an IOCalloutDevice.
    """
    try:
        import plistlib
    except ImportError:
        return {}
    try:
        raw = subprocess.run(
            ["ioreg", "-a", "-r", "-c", "IOUSBHostDevice", "-l"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=15).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    if not raw:
        return {}
    try:
        tree = plistlib.loads(raw)
    except Exception:
        return {}

    out = {}

    def usb_facts(node):
        vid = node.get("idVendor")
        pid = node.get("idProduct")
        if vid is None or pid is None:
            return None
        loc = node.get("locationID")
        bridge = USB_BRIDGES.get((int(vid), int(pid)))
        if bridge is None and int(vid) == ESPRESSIF_VID:
            bridge = "usb_serial_jtag"
        return {
            "vid": "0x%04x" % int(vid),
            "pid": "0x%04x" % int(pid),
            "bridge": bridge,
            "vendor": node.get("USB Vendor Name"),
            "product": node.get("USB Product Name"),
            "serial": node.get("USB Serial Number"),
            "location_id": ("0x%08x" % int(loc)) if isinstance(loc, int) else None,
        }

    def walk(node, carried):
        if not isinstance(node, dict):
            return
        mine = usb_facts(node) or carried
        callout = node.get("IOCalloutDevice")
        if isinstance(callout, str) and mine:
            out[callout] = mine
        for child in node.get("IORegistryEntryChildren", []) or []:
            walk(child, mine)

    for root in (tree if isinstance(tree, list) else [tree]):
        walk(root, None)
    return out


def _sysfs_usb_map(ports):
    """port -> USB descriptor facts, from sysfs. Linux only, best effort."""
    out = {}
    for dev in ports:
        base = os.path.basename(dev)
        node = "/sys/class/tty/%s/device" % base
        if not os.path.exists(node):
            continue
        # The USB device with the descriptors is one or two levels up.
        cur = os.path.realpath(node)
        facts = None
        for _ in range(4):
            vid_p = os.path.join(cur, "idVendor")
            pid_p = os.path.join(cur, "idProduct")
            if os.path.exists(vid_p) and os.path.exists(pid_p):
                def rd(name):
                    try:
                        with open(os.path.join(cur, name), "r") as fh:
                            return fh.read().strip()
                    except OSError:
                        return None
                vid, pid = rd("idVendor"), rd("idProduct")
                try:
                    ivid, ipid = int(vid, 16), int(pid, 16)
                except (TypeError, ValueError):
                    break
                bridge = USB_BRIDGES.get((ivid, ipid))
                if bridge is None and ivid == ESPRESSIF_VID:
                    bridge = "usb_serial_jtag"
                facts = {
                    "vid": "0x%04x" % ivid,
                    "pid": "0x%04x" % ipid,
                    "bridge": bridge,
                    "vendor": rd("manufacturer"),
                    "product": rd("product"),
                    "serial": rd("serial"),
                    "location_id": None,
                }
                break
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
        if facts:
            out[dev] = facts
    return out


def usb_map(ports):
    if platform.system() == "Darwin":
        return _ioreg_usb_map()
    if platform.system() == "Linux":
        return _sysfs_usb_map(ports)
    return {}


def usb_for(umap, port):
    facts = umap.get(port)
    if facts:
        return dict(facts)
    # Nothing authoritative. The device name still carries a weak hint: native
    # USB enumerates as usbmodem, a bridge chip as usbserial. Say so, and say
    # that it is a guess.
    base = os.path.basename(port).lower()
    bridge = None
    if "usbmodem" in base:
        bridge = "usb_serial_jtag"
    return {"vid": None, "pid": None, "bridge": bridge, "vendor": None,
            "product": None, "serial": None, "location_id": None,
            "from_name_only": True}


# ------------------------------------------------------------------ esptool


def find_esptool(explicit=None):
    """(argv_prefix, version_string, subcommand_style) or raise Fail."""
    cands = []
    if explicit:
        cands.append([explicit])
    else:
        for name in ("esptool", "esptool.py"):
            found = shutil.which(name)
            if found:
                cands.append([found])
        cands.append([sys.executable, "-m", "esptool"])

    for argv in cands:
        try:
            run = subprocess.run(argv + ["version"], stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=30)
        except (OSError, subprocess.SubprocessError):
            continue
        if run.returncode != 0:
            continue
        text = run.stdout.decode("utf-8", "replace")
        m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
        version = m.group(0) if m else "unknown"
        major = int(m.group(1)) if m else 0
        # esptool 5 renamed every subcommand to hyphens and only warns about the
        # old spelling. esptool 4 and older only know the underscores.
        style = "hyphen" if major >= 5 else "underscore"
        return argv, version, style

    raise Fail(
        "esptool not found. Install it with `pip3 install esptool` (or "
        "`brew install esptool`), or point at it with --esptool /path/to/esptool.",
        4)


def esptool_cmd(style, name):
    return name if style == "hyphen" else name.replace("-", "_")


def probe_port(argv, style, port, baud, timeout):
    """One esptool connection. Returns (ok, combined_output, error_or_None).

    `flash-id` is used because a single connection prints the whole banner -
    chip, features, crystal, MAC - and then the flash manufacturer and size. Two
    facts for the price of one reset.
    """
    base = argv + ["--port", port, "--baud", str(baud), "--after", "no-reset"]
    cmd = base + ["--connect-attempts", "2", esptool_cmd(style, "flash-id")]
    for attempt in (cmd, base + [esptool_cmd(style, "flash-id")]):
        try:
            run = subprocess.run(attempt, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            return False, "", ("esptool timed out after %ds on %s" % (timeout, port))
        except OSError as exc:
            return False, "", ("cannot run esptool: %s" % exc)
        text = run.stdout.decode("utf-8", "replace")
        if run.returncode == 0:
            return True, text, None
        # Older esptool does not know --connect-attempts; retry without it once.
        if attempt is cmd and re.search(r"no such option|unrecognized argument|"
                                        r"Got unexpected extra argument", text, re.I):
            continue
        return False, text, first_useful_line(text) or ("esptool exited %d" % run.returncode)
    return False, "", "esptool failed"


def first_useful_line(text):
    """The line a human should read out of an esptool failure."""
    interesting = (
        "Invalid head of packet",
        "Failed to connect",
        "could not open port",
        "No serial data received",
        "Serial port",
        "Permission denied",
        "Wrong boot mode",
        "Device or resource busy",
    )
    for line in text.splitlines():
        line = line.strip()
        for needle in interesting:
            if needle.lower() in line.lower():
                return line
    for line in reversed(text.splitlines()):
        if line.strip():
            return line.strip()
    return None


def target_from_description(desc):
    if not desc:
        return None
    m = VARIANT_RE.match(desc)
    if m:
        return ("esp32" + m.group(1)).lower()
    if desc.upper().startswith("ESP32"):
        return "esp32"
    if desc.upper().startswith("ESP8266"):
        return "esp8266"
    return None


def parse_esptool(text):
    """esptool's banner, in both the v4 and the v5 wording."""
    facts = {
        "description": None, "revision": None, "target": None,
        "features": [], "psram": False, "psram_certain": False,
        "flash_size_mb": None, "mac": None, "crystal_mhz": None,
    }
    m = CHIP_RE.search(text)
    if m:
        facts["description"] = m.group(1).strip()
        if m.group(2):
            facts["revision"] = m.group(2).strip()
    if not facts["description"]:
        m = DETECTING_RE.search(text)
        if m:
            facts["description"] = m.group(1).strip()
    facts["target"] = target_from_description(facts["description"])

    m = FEATURES_RE.search(text)
    if m:
        feats = [f.strip() for f in m.group(1).split(",") if f.strip()]
        facts["features"] = feats
        for f in feats:
            if "psram" in f.lower():
                facts["psram"] = True
        # esptool only ever sees PSRAM that is inside the chip package. An
        # external PSRAM chip on the module is invisible to it. So "yes" is a
        # fact and "no" is only the absence of one, which is why the narrowing
        # below never rejects a profile for claiming PSRAM.
        facts["psram_certain"] = facts["psram"]

    m = FLASH_RE.search(text)
    if m:
        size = int(m.group(1))
        facts["flash_size_mb"] = size if m.group(2).upper() == "MB" else max(1, size // 1024)

    m = MAC_RE.search(text)
    if m:
        facts["mac"] = m.group(1).lower()

    m = CRYSTAL_RE.search(text)
    if m:
        facts["crystal_mhz"] = int(m.group(1))
    return facts


# ---------------------------------------------------------------- narrowing


def narrow(facts, usb, profiles):
    """Split the registry into survivors and rejects.

    Hard signals reject: chip target, flash size, and PSRAM but only in the one
    direction esptool can actually prove. Soft signals never reject on their own;
    they rank, and they are reported so a human can see why one candidate looks
    more likely than another.
    """
    survivors, rejected = [], []

    for p in profiles:
        chip = p.get("chip", {})
        ident = p.get("identification", {})
        flashing = p.get("flashing", {})
        hard = []

        want_target = chip.get("target")
        if facts["target"] and want_target and facts["target"] != want_target:
            hard.append("chip target is %s, profile wants %s"
                        % (facts["target"], want_target))

        want_flash = chip.get("flash_size_mb")
        if facts["flash_size_mb"] and want_flash and facts["flash_size_mb"] != want_flash:
            hard.append("flash is %dMB, profile wants %dMB"
                        % (facts["flash_size_mb"], want_flash))

        want_psram = bool(chip.get("psram", {}).get("present"))
        if facts["psram_certain"] and not want_psram:
            hard.append("chip reports embedded PSRAM, profile says none")

        if hard:
            rejected.append({"id": p["id"], "name": p.get("name"), "reasons": hard})
            continue

        soft_match, soft_conflict = [], []

        want_desc = (ident.get("esptool_reports") or {}).get("chip")
        if facts["description"] and want_desc:
            if facts["description"].upper() == want_desc.upper():
                soft_match.append("esptool chip description %s" % want_desc)
            else:
                soft_conflict.append("registry expects %s, esptool says %s"
                                     % (want_desc, facts["description"]))

        want_bridge = flashing.get("usb_bridge")
        got_bridge = usb.get("bridge")
        if want_bridge and got_bridge and not usb.get("from_name_only"):
            if got_bridge.upper() == want_bridge.upper():
                soft_match.append("USB bridge %s" % want_bridge)
            else:
                soft_conflict.append("registry expects a %s bridge, this is a %s"
                                     % (want_bridge, got_bridge))

        want_usb_serial = ident.get("usb_serial")
        got_usb_serial = usb.get("serial")
        if want_usb_serial and got_usb_serial:
            if str(got_usb_serial) == str(want_usb_serial):
                soft_match.append("USB serial number %s" % want_usb_serial)
            else:
                soft_conflict.append("registry expects USB serial %s, this reports %s"
                                     % (want_usb_serial, got_usb_serial))

        survivors.append({
            "id": p["id"],
            "name": p.get("name"),
            "summary": p.get("summary"),
            "tier": (p.get("render") or {}).get("tier"),
            "controller": (p.get("display") or {}).get("controller"),
            "soft_matches": soft_match,
            "soft_conflicts": soft_conflict,
            "confirm_prompt": ident.get("confirm_prompt"),
            "distinguishing_notes": ident.get("distinguishing_notes") or [],
            "mac_allowlist": [m.lower() for m in (ident.get("mac_allowlist") or [])],
            "upload_baud": flashing.get("upload_baud"),
            "monitor_baud": flashing.get("monitor_baud"),
            "bad_baud_rates": flashing.get("bad_baud_rates") or [],
            "flash_mode": flashing.get("flash_mode"),
            "flash_freq_mhz": flashing.get("flash_freq_mhz"),
            "target": chip.get("target"),
            "fqbn": FQBN.get(chip.get("target")),
            "path": p["_path"],
        })

    # Rank: no conflicts first, then most positive evidence.
    survivors.sort(key=lambda c: (len(c["soft_conflicts"]), -len(c["soft_matches"]), c["id"]))
    return survivors, rejected


def decide(facts, survivors, cache_entry):
    """pinned | unique | ambiguous | none, plus the reason and what is still owed."""
    mac = facts.get("mac")

    # The registry outranks the local cache: a MAC written into a profile's
    # mac_allowlist was put there deliberately by a human editing a checked-in
    # file, which is a stronger statement than a cache line this tool wrote.
    if mac:
        for c in survivors:
            if mac in c["mac_allowlist"]:
                return ("pinned", c["id"], "MAC %s is in %s identification.mac_allowlist"
                        % (mac, c["id"]), False, False)

    if cache_entry and cache_entry.get("profile"):
        pinned = cache_entry["profile"]
        if any(c["id"] == pinned for c in survivors):
            return ("pinned", pinned,
                    "MAC %s was confirmed as %s on %s and cached locally"
                    % (mac, pinned, cache_entry.get("confirmed_at", "an earlier run")),
                    False, False)
        # The cache disagrees with what is plugged in. Say so rather than
        # quietly trusting either side.
        return ("ambiguous", None,
                "cache says %s for MAC %s but that profile does not match this chip; "
                "the cache entry is stale" % (pinned, mac), False, True)

    if len(survivors) == 1:
        return ("unique", survivors[0]["id"],
                "exactly one profile in the registry matches this chip", False, True)

    if not survivors:
        return ("none", None,
                "no profile in the registry matches this chip", False, False)

    clean = [c for c in survivors if not c["soft_conflicts"]]
    if len(clean) == 1:
        return ("unique", clean[0]["id"],
                "narrowed to one by %s" % ", ".join(clean[0]["soft_matches"] or ["elimination"]),
                False, True)

    return ("ambiguous", None,
            "%d profiles match everything esptool can see; they differ only in "
            "things it cannot" % len(survivors), True, True)


# -------------------------------------------------------------------- cache


class Cache(object):
    """MAC -> confirmed profile. Lives outside the repo: it is per-machine
    state about physical boards, not source."""

    def __init__(self, path):
        self.path = path
        self.data = {"schema": SCHEMA, "boards": {}}
        self.loaded = False
        self.error = None
        self.load()

    def load(self):
        if not self.path or not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r") as fh:
                data = json.load(fh)
            if isinstance(data, dict) and isinstance(data.get("boards"), dict):
                self.data = data
                self.data.setdefault("schema", SCHEMA)
                self.loaded = True
        except (ValueError, OSError) as exc:
            # A corrupt cache is a nuisance, never a failure. Detection still
            # works without it; it just asks again.
            self.error = "ignoring unreadable cache %s: %s" % (self.path, exc)

    def get(self, mac):
        if not mac:
            return None
        return self.data.get("boards", {}).get(mac.lower())

    def set(self, mac, profile, chip=None, port=None, by="human"):
        entry = {
            "profile": profile,
            "confirmed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "confirmed_by": by,
        }
        if chip:
            entry["chip"] = chip
        if port:
            entry["last_port"] = port
        self.data.setdefault("boards", {})[mac.lower()] = entry
        self.save()

    def forget(self, mac):
        boards = self.data.setdefault("boards", {})
        if mac.lower() in boards:
            del boards[mac.lower()]
            self.save()
            return True
        return False

    def save(self):
        self.data["schema"] = SCHEMA
        self.data["note"] = ("Written by esp-os tools/detect.py. Maps a board's "
                             "MAC to the profile a human confirmed for it.")
        directory = os.path.dirname(self.path)
        try:
            if directory and not os.path.isdir(directory):
                os.makedirs(directory)
            tmp = self.path + ".tmp"
            with open(tmp, "w") as fh:
                json.dump(self.data, fh, indent=2, sort_keys=True)
                fh.write("\n")
            os.rename(tmp, self.path)
        except OSError as exc:
            raise Fail("cannot write cache %s: %s" % (self.path, exc), 1)


# ------------------------------------------------------------------ reports


def inspect(port, argv, style, umap, profiles, cache, baud, timeout, do_probe):
    usb = usb_for(umap, port)
    result = {
        "port": port,
        "usb": usb,
        "probe": {"ok": False, "error": None, "attempted": bool(do_probe)},
        "chip": None,
        "cached": None,
        "candidates": [],
        "rejected": [],
        "decision": "unknown",
        "profile": None,
        "reason": None,
        "needs_probe": False,
        "needs_confirm": False,
    }
    if not do_probe:
        result["reason"] = "not probed (--no-probe)"
        return result

    ok, text, err = probe_port(argv, style, port, baud, timeout)
    result["probe"]["ok"] = ok
    result["probe"]["error"] = err
    if not ok:
        result["decision"] = "unreachable"
        result["reason"] = err or "esptool could not talk to this port"
        return result

    facts = parse_esptool(text)
    result["chip"] = facts
    entry = cache.get(facts.get("mac"))
    if entry:
        result["cached"] = dict(entry)

    survivors, rejected = narrow(facts, usb, profiles)
    result["candidates"] = survivors
    result["rejected"] = rejected

    decision, profile, reason, needs_probe, needs_confirm = decide(facts, survivors, entry)
    result["decision"] = decision
    result["profile"] = profile
    result["reason"] = reason
    result["needs_probe"] = needs_probe
    result["needs_confirm"] = needs_confirm
    return result


def build_report(args, profiles):
    cache = Cache(None if args.no_cache else args.cache)

    ports = enumerate_ports()
    if args.port:
        ports = [args.port] if args.port in ports or os.path.exists(args.port) else []

    report = {
        "schema": SCHEMA,
        "tool": TOOL,
        "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "boards_dir": args.boards,
        "profiles": [{"id": p["id"], "name": p.get("name"),
                      "target": p.get("chip", {}).get("target"),
                      "tier": (p.get("render") or {}).get("tier"),
                      "controller": (p.get("display") or {}).get("controller"),
                      "upload_baud": (p.get("flashing") or {}).get("upload_baud")}
                     for p in profiles],
        "cache_path": None if args.no_cache else args.cache,
        "cache_warning": cache.error,
        "detect_baud": args.baud,
        "esptool": {"present": False, "version": None, "argv": None},
        "ports": ports,
        "results": [],
        "status": "ok",
        "message": "",
    }

    # Look esptool up before anything else, so the report is honest about the
    # prerequisites even when there is no board to point it at.
    esptool_error = None
    if not args.no_probe:
        try:
            argv, version, style = find_esptool(args.esptool)
            report["esptool"] = {"present": True, "version": version,
                                 "argv": argv, "style": style}
        except Fail as exc:
            esptool_error = exc.message

    if args.port and not ports:
        report["status"] = "no-such-port"
        report["message"] = ("%s is not a serial device on this machine. Run with "
                             "--list-ports to see what is." % args.port)
        return report, cache

    if not ports:
        report["status"] = "no-ports"
        report["message"] = (
            "No board found. No USB serial device is attached (Bluetooth and the "
            "internal debug consoles are filtered out). Plug a board in, and if it "
            "still does not appear check the cable - a charge-only USB cable "
            "enumerates nothing at all.")
        return report, cache

    umap = usb_map(ports)

    if args.no_probe:
        report["status"] = "ports-only"
        report["message"] = "%d serial device(s) found; not probed (--no-probe)." % len(ports)
        for port in ports:
            report["results"].append(
                inspect(port, None, None, umap, profiles, cache,
                        args.baud, args.timeout, False))
        return report, cache

    if esptool_error:
        report["status"] = "no-esptool"
        report["message"] = esptool_error
        return report, cache

    argv = report["esptool"]["argv"]
    style = report["esptool"]["style"]
    for port in ports:
        report["results"].append(
            inspect(port, argv, style, umap, profiles, cache,
                    args.baud, args.timeout, True))

    identified = [r for r in report["results"] if r["probe"]["ok"]]
    if not identified:
        report["status"] = "no-boards"
        report["message"] = (
            "%d serial device(s) found but none answered as an ESP32. They may be "
            "something else entirely, or the board may be held in reset." % len(ports))
    else:
        report["status"] = "ok"
        report["message"] = "%d board(s) identified." % len(identified)
    return report, cache


# ------------------------------------------------------------ human output


def w(line=""):
    sys.stdout.write(line + "\n")


def print_human(report):
    w("esp-os board detection")
    w("=" * 62)
    et = report["esptool"]
    w("registry   %s (%d profiles)" % (report["boards_dir"], len(report["profiles"])))
    w("esptool    %s" % ("%s %s" % (" ".join(et["argv"]), et["version"])
                         if et["present"] else "not found"))
    w("cache      %s" % (report["cache_path"] or "disabled"))
    w("baud       %d (detection only - the upload baud comes from the profile)"
      % report["detect_baud"])
    if report.get("cache_warning"):
        w("note       %s" % report["cache_warning"])
    w("")

    if report["status"] in ("no-ports", "no-such-port", "no-esptool"):
        w(report["message"])
        w("")
        print_registry(report)
        return

    w("serial devices (%d)" % len(report["ports"]))
    for p in report["ports"]:
        w("  %s" % p)
    w("")

    for r in report["results"]:
        print_result(r)

    if report["status"] == "no-boards":
        w(report["message"])
        w("")
    print_registry(report)


def print_result(r):
    w("-" * 62)
    w("port       %s" % r["port"])
    usb = r["usb"]
    if usb.get("bridge") or usb.get("vid"):
        bits = []
        if usb.get("bridge"):
            bits.append(usb["bridge"] + (" (guessed from the device name)"
                                         if usb.get("from_name_only") else ""))
        if usb.get("vid"):
            bits.append("%s:%s" % (usb["vid"], usb["pid"]))
        if usb.get("serial"):
            bits.append("serial %s" % usb["serial"])
        if usb.get("location_id"):
            bits.append("location %s" % usb["location_id"])
        w("usb        %s" % "  ".join(bits))

    if not r["probe"]["ok"]:
        w("chip       did not answer")
        w("           %s" % (r["probe"]["error"] or "unknown error"))
        w("")
        return

    c = r["chip"]
    w("chip       %s%s  target %s" % (
        c["description"] or "?",
        (" rev %s" % c["revision"]) if c["revision"] else "",
        c["target"] or "?"))
    w("flash      %s" % ("%dMB" % c["flash_size_mb"] if c["flash_size_mb"] else "?"))
    w("psram      %s" % ("yes" if c["psram"] else
                         "none reported (esptool cannot see external PSRAM)"))
    w("mac        %s" % (c["mac"] or "?"))
    if r["cached"]:
        w("cached     %s, confirmed %s by %s" % (
            r["cached"].get("profile"), r["cached"].get("confirmed_at", "?"),
            r["cached"].get("confirmed_by", "?")))
    w("")
    w("decision   %s" % r["decision"].upper())
    w("           %s" % (r["reason"] or ""))
    if r["profile"]:
        w("profile    %s" % r["profile"])

    if r["candidates"]:
        w("")
        w("candidates")
        for cand in r["candidates"]:
            mark = "->" if cand["id"] == r["profile"] else "  "
            w("  %s %-22s tier %s  %-8s upload %s baud"
              % (mark, cand["id"], cand["tier"], cand["controller"],
                 cand["upload_baud"]))
            for m in cand["soft_matches"]:
                w("       + %s" % m)
            for m in cand["soft_conflicts"]:
                w("       - %s" % m)
    if r["rejected"]:
        w("")
        w("ruled out")
        for rej in r["rejected"]:
            w("     %-22s %s" % (rej["id"], "; ".join(rej["reasons"])))

    if r["decision"] == "ambiguous":
        w("")
        w("These cannot be told apart by anything esptool can read. Either pick one")
        w("with --profile, or flash tools/probe/probe.ino and read the screen.")
        for cand in r["candidates"]:
            for note in cand["distinguishing_notes"][:1]:
                w("  %-22s %s" % (cand["id"], note))
    elif r["decision"] == "unique" and r["needs_confirm"]:
        cand = r["candidates"][0] if r["candidates"] else None
        if cand and cand.get("confirm_prompt"):
            w("")
            w("One-time confirmation still owed:")
            w("  %s" % cand["confirm_prompt"])
    elif r["decision"] == "none":
        w("")
        w("This chip matches no profile. Add one under boards/ - boards/README.md")
        w("has the checklist.")
    w("")


def print_registry(report):
    w("registry profiles")
    for p in report["profiles"]:
        w("  %-22s tier %s  %-8s %-10s upload %s baud"
          % (p["id"], p["tier"], p["target"], p["controller"], p["upload_baud"]))


# ------------------------------------------------------------ shell output


def shq(value):
    """POSIX single-quote. Safe to eval in bash 3.2."""
    if value is None:
        return "''"
    text = str(value)
    return "'" + text.replace("'", "'\\''") + "'"


def pick_result(report, want_port):
    """The one result --shell describes, or None with a status."""
    results = report.get("results") or []
    if want_port:
        for r in results:
            if r["port"] == want_port:
                return r, None
        return None, "no-such-port"
    live = [r for r in results if r["probe"]["ok"]]
    if len(live) == 1:
        return live[0], None
    if len(live) > 1:
        return None, "multiple-boards"
    if len(results) == 1:
        return results[0], None
    if len(results) > 1:
        return None, "multiple-ports"
    return None, report.get("status", "no-ports")


def print_shell(report, want_port):
    """The same facts as --json, flattened. bash 3.2 has neither a JSON parser
    nor associative arrays, so flash.sh eats this instead."""
    out = []

    def put(key, value):
        out.append("EOS_%s=%s" % (key, shq(value)))

    def put_raw(key, value):
        out.append("EOS_%s=%s" % (key, value))

    result, why = pick_result(report, want_port)
    status = report["status"] if why is None else why
    if result and not result["probe"]["ok"] and status == "ok":
        status = "unreachable"

    put("DETECT_STATUS", status)
    put("DETECT_MESSAGE", report["message"] if why is None else shell_status_message(report, why))
    put("BOARDS_DIR", report["boards_dir"])
    put("CACHE_PATH", report["cache_path"] or "")
    put("DETECT_BAUD", report["detect_baud"])
    put("PORTS", " ".join(report["ports"]))
    put_raw("PORT_COUNT", len(report["ports"]))
    et = report["esptool"]
    put("ESPTOOL", " ".join(et["argv"]) if et["argv"] else "")
    put("ESPTOOL_VERSION", et["version"] or "")
    put("ESPTOOL_STYLE", et.get("style") or "hyphen")
    put("PROFILE_IDS", " ".join(p["id"] for p in report["profiles"]))

    if result is None:
        put("PORT", "")
        put("PROFILE", "")
        put("DECISION", status)
        put_raw("CANDIDATE_COUNT", 0)
        put("CANDIDATES", "")
        put("CANDIDATES_CLEAN", "")
        put_raw("CANDIDATE_CLEAN_COUNT", 0)
        put_raw("NEEDS_CONFIRM", 0)
        put_raw("NEEDS_PROBE", 0)
        put("MAC", "")
        put("CHIP_TARGET", "")
        put("PROBE_FQBN", "")
        put_raw("SAFE_UPLOAD_BAUD", 115200)
        sys.stdout.write("\n".join(out) + "\n")
        return

    put("PORT", result["port"])
    usb = result["usb"]
    put("USB_BRIDGE", usb.get("bridge") or "")
    put("USB_VID", usb.get("vid") or "")
    put("USB_PID", usb.get("pid") or "")
    put("USB_SERIAL", usb.get("serial") or "")
    put("USB_LOCATION", usb.get("location_id") or "")

    chip = result["chip"] or {}
    put("CHIP_DESC", chip.get("description") or "")
    put("CHIP_REV", chip.get("revision") or "")
    put("CHIP_TARGET", chip.get("target") or "")
    put_raw("FLASH_MB", chip.get("flash_size_mb") or 0)
    put_raw("PSRAM", 1 if chip.get("psram") else 0)
    put_raw("PSRAM_CERTAIN", 1 if chip.get("psram_certain") else 0)
    put("MAC", chip.get("mac") or "")
    put("PROBE_ERROR", result["probe"].get("error") or "")

    put("DECISION", result["decision"])
    put("DECISION_REASON", result["reason"] or "")
    put("PROFILE", result["profile"] or "")
    put("CACHED_PROFILE", (result["cached"] or {}).get("profile") or "")
    put_raw("NEEDS_CONFIRM", 1 if result["needs_confirm"] else 0)
    put_raw("NEEDS_PROBE", 1 if result["needs_probe"] else 0)

    cands = result["candidates"]
    put("CANDIDATES", " ".join(c["id"] for c in cands))
    put_raw("CANDIDATE_COUNT", len(cands))
    # The subset with nothing arguing against it. When esptool cannot separate
    # two boards this is the pair a human actually has to choose between.
    clean = [c for c in cands if not c["soft_conflicts"]]
    put("CANDIDATES_CLEAN", " ".join(c["id"] for c in clean))
    put_raw("CANDIDATE_CLEAN_COUNT", len(clean))

    # The fastest baud that is safe for every candidate: the lowest upload baud
    # any of them asks for, minus every rate any of them is known to fail at.
    # The prober has to be flashed before the board is identified, so it cannot
    # use one profile's baud and hope.
    pool = clean or cands
    bad = set()
    for c in pool:
        for b in c.get("bad_baud_rates", []):
            if b.get("baud"):
                bad.add(int(b["baud"]))
    wanted = [int(c["upload_baud"]) for c in pool if c.get("upload_baud")]
    safe = min([b for b in wanted if b not in bad] or [115200])
    put_raw("SAFE_UPLOAD_BAUD", safe)
    put("PROBE_FQBN", (pool[0].get("fqbn") if pool else "") or "")

    chosen = None
    for c in cands:
        if c["id"] == result["profile"]:
            chosen = c
    if chosen is None and len(cands) == 1:
        chosen = cands[0]
    if chosen:
        put("CONFIRM_PROMPT", chosen.get("confirm_prompt") or "")
        put_raw("UPLOAD_BAUD", chosen.get("upload_baud") or 115200)
        put_raw("MONITOR_BAUD", chosen.get("monitor_baud") or 115200)
        put("FLASH_MODE", chosen.get("flash_mode") or "dio")
        put_raw("FLASH_FREQ", chosen.get("flash_freq_mhz") or 40)
        put("PROFILE_TARGET", chosen.get("target") or "")
        put("PROFILE_FQBN", chosen.get("fqbn") or "")
        put("PROFILE_PATH", chosen.get("path") or "")
        put_raw("PROFILE_TIER", chosen.get("tier") if chosen.get("tier") is not None else -1)
        put("BAD_BAUDS", " ".join(str(b.get("baud")) for b in chosen.get("bad_baud_rates", [])))
    else:
        put("CONFIRM_PROMPT", "")
        put_raw("UPLOAD_BAUD", 115200)
        put_raw("MONITOR_BAUD", 115200)
        put("FLASH_MODE", "dio")
        put_raw("FLASH_FREQ", 40)
        put("PROFILE_TARGET", (result["chip"] or {}).get("target") or "")
        put("PROFILE_FQBN", FQBN.get((result["chip"] or {}).get("target"), ""))
        put("PROFILE_PATH", "")
        put_raw("PROFILE_TIER", -1)
        put("BAD_BAUDS", "")

    sys.stdout.write("\n".join(out) + "\n")


def shell_status_message(report, why):
    if why == "multiple-ports":
        return ("%d serial devices are attached and none was named. Pass --port."
                % len(report["ports"]))
    if why == "multiple-boards":
        return ("More than one board answered. Pass --port to say which one. "
                "Both wavvy CP2102 bridges report USB serial 0001, so the device "
                "names collide; unplug one, or tell them apart by USB location id.")
    if why == "no-such-port":
        return "That port is not among the serial devices found."
    return report.get("message", "")


# ------------------------------------------------------------ profile facts


def print_profile_facts(profiles, pid, as_json):
    p = profile_by_id(profiles, pid)
    if p is None:
        raise Fail("no profile called %s. Known: %s"
                   % (pid, ", ".join(x["id"] for x in profiles)), 2)
    flashing = p.get("flashing", {})
    display = p.get("display", {})
    render = p.get("render", {})
    chip = p.get("chip", {})
    ident = p.get("identification", {})
    facts = {
        "id": p["id"],
        "name": p.get("name"),
        "summary": p.get("summary"),
        "target": chip.get("target"),
        "fqbn": FQBN.get(chip.get("target")),
        "tier": render.get("tier"),
        "controller": display.get("controller"),
        "upload_baud": flashing.get("upload_baud"),
        "monitor_baud": flashing.get("monitor_baud"),
        "bad_baud_rates": flashing.get("bad_baud_rates") or [],
        "flash_mode": flashing.get("flash_mode"),
        "flash_freq_mhz": flashing.get("flash_freq_mhz"),
        "flash_size_mb": chip.get("flash_size_mb"),
        "partition_scheme": flashing.get("partition_scheme"),
        "confirm_prompt": ident.get("confirm_prompt"),
        "path": p["_path"],
    }
    if as_json:
        json.dump(facts, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return
    for key in ("id", "name", "target", "fqbn", "tier", "controller",
                "upload_baud", "monitor_baud", "flash_mode", "flash_freq_mhz",
                "flash_size_mb", "partition_scheme"):
        w("%-16s %s" % (key, facts[key]))
    if facts["bad_baud_rates"]:
        w("%-16s %s" % ("bad_baud_rates",
                        ", ".join(str(b["baud"]) for b in facts["bad_baud_rates"])))
    w("%-16s %s" % ("confirm", facts["confirm_prompt"]))


def print_profile_shell(profiles, pid):
    p = profile_by_id(profiles, pid)
    if p is None:
        raise Fail("no profile called %s. Known: %s"
                   % (pid, ", ".join(x["id"] for x in profiles)), 2)
    flashing = p.get("flashing", {})
    chip = p.get("chip", {})
    ident = p.get("identification", {})
    lines = [
        "EOS_PROFILE=%s" % shq(p["id"]),
        "EOS_PROFILE_NAME=%s" % shq(p.get("name")),
        "EOS_PROFILE_TARGET=%s" % shq(chip.get("target")),
        "EOS_PROFILE_FQBN=%s" % shq(FQBN.get(chip.get("target"), "")),
        "EOS_PROFILE_TIER=%d" % ((p.get("render") or {}).get("tier", -1)),
        "EOS_PROFILE_PATH=%s" % shq(p["_path"]),
        "EOS_UPLOAD_BAUD=%d" % (flashing.get("upload_baud") or 115200),
        "EOS_MONITOR_BAUD=%d" % (flashing.get("monitor_baud") or 115200),
        "EOS_FLASH_MODE=%s" % shq(flashing.get("flash_mode") or "dio"),
        "EOS_FLASH_FREQ=%d" % (flashing.get("flash_freq_mhz") or 40),
        "EOS_FLASH_MB=%d" % (chip.get("flash_size_mb") or 4),
        "EOS_CONFIRM_PROMPT=%s" % shq(ident.get("confirm_prompt") or ""),
        "EOS_BAD_BAUDS=%s" % shq(" ".join(
            str(b.get("baud")) for b in (flashing.get("bad_baud_rates") or []))),
    ]
    sys.stdout.write("\n".join(lines) + "\n")


# --------------------------------------------------------------------- main


def build_parser():
    p = argparse.ArgumentParser(
        prog="detect.py",
        description="Identify an attached ESP32 board and narrow it to board "
                    "profiles under boards/.",
        epilog="Detection always connects at %d baud. The profile's upload baud "
               "is for writing only." % DETECT_BAUD)
    p.add_argument("--port", help="only look at this serial device")
    p.add_argument("--boards", default=DEFAULT_BOARDS, metavar="DIR",
                   help="board registry directory (default: %(default)s)")
    p.add_argument("--cache", default=os.environ.get("EOS_BOARD_CACHE", DEFAULT_CACHE),
                   metavar="PATH", help="MAC -> profile cache (default: %(default)s)")
    p.add_argument("--no-cache", action="store_true",
                   help="ignore the cache entirely, read and write")
    p.add_argument("--esptool", metavar="PATH", help="esptool to use")
    p.add_argument("--baud", type=int, default=DETECT_BAUD,
                   help="detection baud (default: %(default)s; you should not need this)")
    p.add_argument("--timeout", type=int, default=45, metavar="S",
                   help="give up on one esptool call after this long (default: %(default)s)")
    p.add_argument("--no-probe", action="store_true",
                   help="enumerate ports but do not talk to anything")

    out = p.add_mutually_exclusive_group()
    out.add_argument("--json", action="store_true", help="machine-readable report")
    out.add_argument("--shell", action="store_true",
                     help="the same facts as --json, as EOS_* shell assignments")
    out.add_argument("--list-ports", action="store_true",
                     help="print candidate serial devices, one per line, and stop")
    out.add_argument("--list-profiles", action="store_true",
                     help="print the registry and stop")

    p.add_argument("--from-json", metavar="FILE",
                   help="with --shell: flatten a report captured earlier "
                        "(- for stdin) instead of probing again")
    p.add_argument("--profile", metavar="ID",
                   help="print this profile's flashing facts and stop; "
                        "combines with --json or --shell")
    p.add_argument("--remember", metavar="MAC=PROFILE",
                   help="record a human's confirmation in the cache and stop")
    p.add_argument("--forget", metavar="MAC",
                   help="drop a cache entry and stop")
    p.add_argument("--cache-list", action="store_true",
                   help="print the cache and stop")
    return p


def do_remember(args, profiles):
    if "=" not in args.remember:
        raise Fail("--remember wants MAC=PROFILE, e.g. "
                   "--remember 24:6f:28:aa:bb:cc=cyd-2432s024n", 2)
    mac, pid = args.remember.split("=", 1)
    mac, pid = mac.strip().lower(), pid.strip()
    if not re.match(r"^[0-9a-f]{2}(:[0-9a-f]{2}){5}$", mac):
        raise Fail("%s does not look like a MAC address" % mac, 2)
    if profile_by_id(profiles, pid) is None:
        raise Fail("no profile called %s. Known: %s"
                   % (pid, ", ".join(p["id"] for p in profiles)), 2)
    if args.no_cache:
        raise Fail("--remember and --no-cache contradict each other", 2)
    cache = Cache(args.cache)
    cache.set(mac, pid, port=args.port)
    w("remembered %s as %s in %s" % (mac, pid, args.cache))
    w("")
    w("That is this machine's record. To pin the board for everyone who clones")
    w("the repo, add the MAC to identification.mac_allowlist in")
    w("%s." % os.path.join(args.boards, pid + ".json"))
    return 0


def do_cache_list(args):
    cache = Cache(args.cache)
    if cache.error:
        w(cache.error)
    boards = cache.data.get("boards", {})
    if not boards:
        w("cache %s is empty - no board has been confirmed on this machine yet."
          % args.cache)
        return 0
    w("cache %s" % args.cache)
    for mac in sorted(boards):
        e = boards[mac]
        w("  %s  %-22s confirmed %s by %s"
          % (mac, e.get("profile"), e.get("confirmed_at", "?"),
             e.get("confirmed_by", "?")))
    return 0


def main(argv):
    args = build_parser().parse_args(argv)

    if args.list_ports:
        for port in enumerate_ports():
            w(port)
        return 0

    if args.cache_list:
        return do_cache_list(args)

    if args.forget:
        cache = Cache(args.cache)
        w("forgot %s" % args.forget if cache.forget(args.forget)
          else "%s was not in the cache" % args.forget)
        return 0

    profiles = load_profiles(args.boards)

    if args.remember:
        return do_remember(args, profiles)

    if args.profile:
        if args.shell:
            print_profile_shell(profiles, args.profile)
        else:
            print_profile_facts(profiles, args.profile, args.json)
        return 0

    if args.list_profiles:
        report = {"profiles": [{"id": p["id"], "name": p.get("name"),
                                "target": p.get("chip", {}).get("target"),
                                "tier": (p.get("render") or {}).get("tier"),
                                "controller": (p.get("display") or {}).get("controller"),
                                "upload_baud": (p.get("flashing") or {}).get("upload_baud")}
                               for p in profiles]}
        if args.json:
            json.dump(report, sys.stdout, indent=2, sort_keys=True)
            sys.stdout.write("\n")
        else:
            print_registry(report)
        return 0

    if args.from_json:
        if not args.shell:
            raise Fail("--from-json only makes sense with --shell", 2)
        try:
            if args.from_json == "-":
                report = json.load(sys.stdin)
            else:
                with open(args.from_json, "r") as fh:
                    report = json.load(fh)
        except ValueError as exc:
            raise Fail("that is not a detect.py --json report: %s" % exc, 2)
        except OSError as exc:
            raise Fail("cannot read %s: %s" % (args.from_json, exc), 1)
        print_shell(report, args.port)
        return 0

    report, _cache = build_report(args, profiles)

    if args.json:
        json.dump(report, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    elif args.shell:
        print_shell(report, args.port)
    else:
        print_human(report)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Fail as exc:
        sys.stderr.write("detect.py: %s\n" % exc.message)
        sys.exit(exc.code)
    except KeyboardInterrupt:
        sys.stderr.write("\ndetect.py: interrupted\n")
        sys.exit(130)
    except Exception as exc:  # never show the user a traceback
        sys.stderr.write("detect.py: unexpected failure: %s: %s\n"
                         % (type(exc).__name__, exc))
        sys.stderr.write("detect.py: this is a bug in detect.py, not in your board.\n")
        sys.exit(1)
