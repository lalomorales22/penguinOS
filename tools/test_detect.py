#!/usr/bin/env python3
"""Host tests for tools/detect.py's chip-string parsing.

This file exists because of one character. VARIANT_RE carried a trailing \b,
which silently mis-identified every ESP32-C6 as a plain esp32 - "ESP32-C6FH4"
has a word character right after the digit, so the boundary never matched. The
detector then rejected both C6 profiles as wrong-target and offered the operator
a menu that could not contain the board in front of them.

detect.py had no tests at all, which is why a one-character bug survived in the
identification path for the whole fleet. These are cheap and they are the ones
that would have caught it.
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import detect

checks = 0
fails = 0


def ck(cond, msg):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("  FAIL %s" % msg)


# Exactly what esptool prints for the silicon this fleet actually contains,
# plus the parts the registry names but nobody has held.
CASES = [
    ("ESP32-C6FH4 (QFN32)",       "esp32c6", "the LAFVIN and the Waveshare C6 - the regression"),
    ("ESP32-C6FH4",               "esp32c6", "same part without the package suffix"),
    ("ESP32-S3 (QFN56)",          "esp32s3", "the Waveshare S3"),
    ("ESP32-S3R8",                "esp32s3", "the S3 with its PSRAM suffix"),
    ("ESP32-D0WD-V3",             "esp32",   "the CYD"),
    ("ESP32-D0WDQ6",              "esp32",   "the wavvy ILI9488 profiles"),
    ("ESP32-PICO-D4",             "esp32",   "a plain ESP32 in a module"),
    ("ESP32",                     "esp32",   "the bare name"),
    ("ESP32-C5 (QFN40)",          "esp32c5", "the C5 profiles"),
    ("ESP32-C3",                  "esp32c3", ""),
    ("ESP32-H2",                  "esp32h2", ""),
    ("ESP32-P4",                  "esp32p4", ""),
    ("ESP32-C61",                 "esp32c61", "two digits must survive"),
    ("ESP32-S2",                  "esp32s2", ""),
]

for desc, want, note in CASES:
    got = detect.target_from_description(desc)
    ck(got == want, "%-22s -> %-9s (got %s)%s"
       % (desc, want, got, "  " + note if note else ""))

# A D0WD is NOT a C-series part however it is spelled; the D must not be read
# as a variant letter.
for desc in ("ESP32-D0WD", "ESP32-D0WD-V3", "ESP32-DOWD"):
    ck(detect.target_from_description(desc) == "esp32",
       "%s is a plain esp32, not a variant" % desc)

# Nothing that is not an ESP32 should be claimed.
for desc in ("", "STM32F407", "RP2040", "nRF52840"):
    ck(detect.target_from_description(desc) != "esp32",
       "%r is not claimed as an esp32" % desc)

# The trailing-\b regression, stated directly: a word character immediately
# after the digit must not defeat the match.
_m = detect.VARIANT_RE.match("ESP32-C6FH4")
ck(_m is not None,
   "VARIANT_RE matches when a letter follows the digit (the \\b bug)")
# Guarded, so reintroducing the bug reports a FAILURE rather than a traceback -
# a crash hides the message that says what actually broke.
ck(_m is not None and _m.group(1).lower() == "c6",
   "and it captures just the variant, not the package code")

# ---------------------------------------------------------------- MAC parsing
# The EUI-64 regression. On the 802.15.4 parts esptool prints an eight-octet
# EUI-64 on the "MAC:" line and the real six-octet MAC on a "BASE MAC:" line
# below it. The old regex took the first six octets of the EUI-64, which is
# three real bytes, the ff:fe padding, and one real byte - an identifier that
# is in no profile's mac_allowlist and belongs to no board.
C5_OUTPUT = """Detecting chip type... ESP32-C5
Chip is ESP32-C5 (revision v1.0)
Features: Wi-Fi 6 (dual-band), BT 5 (LE), IEEE802.15.4, Single Core + LP Core, 240MHz
Crystal is 48MHz
MAC: 38:44:be:ff:fe:0e:9c:38
BASE MAC: 38:44:be:0e:9c:38
MAC_EXT: ff:fe
"""

_f = detect.parse_esptool(C5_OUTPUT)
ck(_f.get("mac") == "38:44:be:0e:9c:38",
   "the C5's BASE MAC is taken, not the first six octets of its EUI-64")
ck(_f.get("mac") != "38:44:be:ff:fe:0e",
   "the ff:fe EUI-64 padding never reaches the MAC field")

# The plain single-line form must still work, and must not be affected by the
# lookahead added to defend against the EUI-64.
ESP32_OUTPUT = """Chip is ESP32-D0WD-V3 (revision v3.1)
Features: WiFi, BT, Dual Core, 240MHz, VRef calibration in efuse, Coding Scheme None
Crystal is 40MHz
MAC: e0:8c:fe:2e:d6:2c
"""
ck(detect.parse_esptool(ESP32_OUTPUT).get("mac") == "e0:8c:fe:2e:d6:2c",
   "a six-octet-only MAC line still parses")

# The S3 prints its MAC twice, after a warning. Both are the same value.
S3_OUTPUT = """Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
MAC: ac:27:6e:a7:a6:dc
Warning: ESP32-S3 has no Chip ID. Reading MAC instead.
MAC: ac:27:6e:a7:a6:dc
"""
ck(detect.parse_esptool(S3_OUTPUT).get("mac") == "ac:27:6e:a7:a6:dc",
   "a repeated MAC line parses to that MAC")

print("\n=== %d checks, %d failed ===" % (checks, fails))
sys.exit(1 if fails else 0)
