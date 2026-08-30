#!/usr/bin/env python3
"""Turn the ASCII glyph art in art/*.txt into kernel/font/eos_font_data.inc.

The art is the source of truth. Every glyph is drawn in a fixed cell whose
width IS the pen advance, because eos_bar.c measures the status bar as
char_w * strlen and a face whose advance differs from its cell breaks bar
fitting on every board. The generator enforces that: it refuses any glyph
whose ink leaves the declared ink box, which is what keeps the rightmost
column(s) of the cell empty and the advance honest.

Packing: rows top to bottom, each row padded to whole bytes, bit 7 of the
first byte is the leftmost pixel. Glyphs are concatenated in codepoint order.
That is exactly EOS_PIXFMT_MONO1 with stride = (cell_w + 7) / 8, so the
compositor can point an eos_bitmap_t straight at a glyph with no repacking.

Usage: python3 tools/gen_font.py [--check]
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, "art")
OUT = os.path.abspath(os.path.join(HERE, os.pardir, "eos_font_data.inc"))

FIRST, LAST = 32, 126
FALLBACK = ord('?')

# id order must match eos_font_id_t in eos_display.h
FACES = ["tiny", "small", "med", "big"]
ENUM = {"tiny": "EOS_FONT_TINY", "small": "EOS_FONT_SMALL",
        "med": "EOS_FONT_MED", "big": "EOS_FONT_BIG"}
# The advance contract, restated here so a typo in the art header is an error
# and not a silently broken status bar.
CONTRACT = {"tiny": (4, 6), "small": (6, 8), "med": (8, 13), "big": (12, 20)}

NAMES = {
    32: "space", 33: "!", 34: '"', 35: "#", 36: "$", 37: "%", 38: "&",
    39: "'", 40: "(", 41: ")", 42: "*", 43: "+", 44: ",", 45: "-", 46: ".",
    47: "/", 58: ":", 59: ";", 60: "<", 61: "=", 62: ">", 63: "?", 64: "@",
    91: "[", 92: "backslash", 93: "]", 94: "^", 95: "_", 96: "`",
    123: "{", 124: "|", 125: "}", 126: "~",
}


class Fail(Exception):
    pass


def parse(path):
    face = {"cell_w": 0, "cell_h": 0, "ink_w": 0, "ink_h": 0, "glyphs": {}}
    code = None
    rows = []
    lineno = 0

    def close():
        if code is None:
            return
        if code in face["glyphs"]:
            raise Fail("%s:%d: codepoint %d appears twice" % (path, lineno, code))
        face["glyphs"][code] = rows

    with open(path, "r") as fh:
        for raw in fh:
            lineno += 1
            line = raw.rstrip("\n")
            # '#' is a comment only in the header. Once a glyph is open it is
            # ink, and treating it as a comment silently blanks the art.
            if code is None and (line.startswith("#") or not line.strip()):
                continue
            if line.startswith("@"):
                close()
                tok = line[1:].split(None, 1)
                code = int(tok[0])
                rows = []
                continue
            if code is None:
                tok = line.split()
                if not tok:
                    continue
                if tok[0] == "name":
                    face["name"] = tok[1]
                elif tok[0] == "cell":
                    face["cell_w"], face["cell_h"] = int(tok[1]), int(tok[2])
                elif tok[0] == "ink":
                    face["ink_w"], face["ink_h"] = int(tok[1]), int(tok[2])
                else:
                    raise Fail("%s:%d: unknown header %r" % (path, lineno, tok[0]))
                continue
            if not line.strip():
                continue
            bad = set(line) - set(".#")
            if bad:
                raise Fail("%s:%d: art may only contain '.' and '#', saw %r"
                           % (path, lineno, sorted(bad)))
            rows.append(line)
    close()
    return face


def validate(face, path):
    cw, ch = face["cell_w"], face["cell_h"]
    iw, ih = face["ink_w"] or cw, face["ink_h"] or ch
    if iw > cw or ih > ch:
        raise Fail("%s: ink box %dx%d does not fit cell %dx%d" % (path, iw, ih, cw, ch))
    want = CONTRACT[face["name"]]
    if (cw, ch) != want:
        raise Fail("%s: cell is %dx%d, the eos_font_id_t contract says %dx%d"
                   % (path, cw, ch, want[0], want[1]))
    for c in range(FIRST, LAST + 1):
        if c not in face["glyphs"]:
            raise Fail("%s: no glyph for codepoint %d" % (path, c))
    for c, rows in sorted(face["glyphs"].items()):
        if c < FIRST or c > LAST:
            raise Fail("%s: codepoint %d is outside %d..%d" % (path, c, FIRST, LAST))
        if len(rows) > ih:
            raise Fail("%s: glyph %d has %d rows, ink box allows %d"
                       % (path, c, len(rows), ih))
        for y, r in enumerate(rows):
            if len(r) > iw:
                raise Fail("%s: glyph %d row %d is %d wide, ink box allows %d "
                           "(the spare column is the inter-glyph gap)"
                           % (path, c, y, len(r), iw))


def bitmap(face, code):
    cw, ch = face["cell_w"], face["cell_h"]
    rb = (cw + 7) // 8
    out = bytearray(rb * ch)
    for y, row in enumerate(face["glyphs"][code]):
        for x, ch_ in enumerate(row):
            if ch_ == "#":
                out[y * rb + (x >> 3)] |= 0x80 >> (x & 7)
    return bytes(out)


def emit(faces):
    L = []
    A = L.append
    A("// Generated by kernel/font/tools/gen_font.py from art/*.txt. Do not edit.")
    A("// Regenerate with: python3 kernel/font/tools/gen_font.py")
    A("")
    total = 0
    for f in faces:
        cw, ch = f["cell_w"], f["cell_h"]
        rb = (cw + 7) // 8
        n = LAST - FIRST + 1
        total += rb * ch * n
    for f in faces:
        name = f["name"]
        cw, ch = f["cell_w"], f["cell_h"]
        rb = (cw + 7) // 8
        n = LAST - FIRST + 1
        A("// ---------------------------------------------------------------- %s" % name)
        A("// %d glyphs x %d rows x %d byte%s = %d bytes of flash."
          % (n, ch, rb, "" if rb == 1 else "s", n * ch * rb))
        A("")
        A("static const uint8_t eos_font_%s_bits[%d] = {" % (name, n * ch * rb))
        for c in range(FIRST, LAST + 1):
            label = NAMES.get(c, chr(c))
            A("    // %d %s" % (c, label))
            b = bitmap(f, c)
            for y in range(ch):
                row = b[y * rb:(y + 1) * rb]
                art = "".join("#" if (row[i >> 3] >> (7 - (i & 7))) & 1 else "."
                              for i in range(cw))
                A("    %s  // %s" % ("".join("0x%02X," % v for v in row), art))
        A("};")
        A("")
    A("// ------------------------------------------------------------------ table")
    A("//")
    A("// Order matches eos_font_id_t. gap is 0 and leading is 0 on every face:")
    A("// the blank column and blank row are drawn into the cell itself, so the")
    A("// pen advance is exactly cell_w and the line pitch is exactly h.")
    A("static const eos_font_t eos_font_table[EOS_FONT_COUNT] = {")
    for f in faces:
        name = f["name"]
        A("    { /* %s */" % ENUM[name])
        A("        .first = %d, .last = %d, .fallback = %d," % (FIRST, LAST, FALLBACK))
        A("        .cell_w = %d, .h = %d, .gap = 0, .leading = 0,"
          % (f["cell_w"], f["cell_h"]))
        A("        .bits = eos_font_%s_bits, .widths = NULL, .offsets = NULL" % name)
        A("    },")
    A("};")
    A("")
    A("#define EOS_FONT_FLASH_BYTES %d" % total)
    A("")
    return "\n".join(L) + "\n"


def main():
    faces = []
    for name in FACES:
        path = os.path.join(ART, name + ".txt")
        f = parse(path)
        if f.get("name") != name:
            raise Fail("%s: header name is %r, expected %r" % (path, f.get("name"), name))
        validate(f, path)
        faces.append(f)
    text = emit(faces)
    if "--check" in sys.argv:
        old = open(OUT).read() if os.path.exists(OUT) else None
        if old != text:
            print("eos_font_data.inc is stale; run tools/gen_font.py")
            return 1
        print("eos_font_data.inc up to date")
        return 0
    with open(OUT, "w") as fh:
        fh.write(text)
    for f in faces:
        rb = (f["cell_w"] + 7) // 8
        n = LAST - FIRST + 1
        print("%-6s %2dx%-2d  %d glyphs  %5d bytes"
              % (f["name"], f["cell_w"], f["cell_h"], n, n * f["cell_h"] * rb))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print("gen_font: %s" % e, file=sys.stderr)
        sys.exit(2)
