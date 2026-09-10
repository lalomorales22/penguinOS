#!/usr/bin/env python3
"""Turn a picture into something a penguinOS board can put on its glass.

The board does not decode anything. There is no JPEG decoder in the display
firmware - the one in the tree belongs to firmware-cam, which has 8MB of PSRAM
to decode into, and the boards with screens have about 30KB of largest free
block. So the decoding happens HERE, once, on a machine with room for it, and
the board is left with bytes it can hand straight to the panel.

The output is raw little-endian RGB565 behind an eight-byte header. That is
EOS_PIXFMT_RGB565, the format eos_display_blit() already accepts, and it is the
same wire format the camera app streams from the camera node - so the viewer on
the board is the camera's strip loop with a file where the socket was.

Usage:
    tools/mkimg.py photo.jpg                 # fits the default 240x240 panel
    tools/mkimg.py photo.jpg --board waveshare-c6-lcd-13
    tools/mkimg.py photo.jpg -o /tmp/pip.565 --width 320 --height 170
    tools/mkimg.py *.png --board lilygo-t-display-c5 --outdir out/

Then upload the .565 files through the web app's Files page and open them in
the files window on the board.
"""

import argparse
import json
import os
import struct
import sys

MAGIC = b"E5"
VERSION = 1
HDR = 8          # magic(2) version(1) flags(1) width(2) height(2)

HERE = os.path.dirname(os.path.abspath(__file__))
BOARDS = os.path.join(os.path.dirname(HERE), "boards")


def board_size(board_id):
    """Active pixel size of a board, from the registry rather than a table
    here: the profile already states native_width, native_height and rotation,
    and a second copy of that is a second thing to keep in step."""
    path = os.path.join(BOARDS, board_id + ".json")
    if not os.path.exists(path):
        sys.exit("no board profile %s in %s" % (board_id, BOARDS))
    with open(path) as fh:
        d = json.load(fh)
    disp = d["display"]
    w, h = disp["native_width"], disp["native_height"]
    if disp["rotation"] % 2 == 1:
        w, h = h, w
    return w, h


def convert(src, dst, box_w, box_h, fit):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("this needs Pillow for the decoding:  pip3 install pillow\n"
                 "(only on this machine - the board decodes nothing)")

    im = Image.open(src)
    im = im.convert("RGB")

    if fit == "cover":
        # Fill the panel and crop the overflow. What you want for a photo.
        scale = max(box_w / im.width, box_h / im.height)
        nw, nh = max(1, round(im.width * scale)), max(1, round(im.height * scale))
        im = im.resize((nw, nh), Image.LANCZOS)
        left, top = (nw - box_w) // 2, (nh - box_h) // 2
        im = im.crop((left, top, left + box_w, top + box_h))
    elif fit == "contain":
        # Whole picture, letterboxed. What you want for a diagram.
        scale = min(box_w / im.width, box_h / im.height)
        nw, nh = max(1, round(im.width * scale)), max(1, round(im.height * scale))
        im = im.resize((nw, nh), Image.LANCZOS)
    else:  # stretch
        im = im.resize((box_w, box_h), Image.LANCZOS)

    w, h = im.size
    out = bytearray(struct.pack("<2sBBHH", MAGIC, VERSION, 0, w, h))

    # RGB888 -> RGB565, little endian, because EOS_PIXFMT_RGB565 says "native
    # colour, little endian" and the blit path byte-swaps per board if the panel
    # wants it the other way round. Doing it here would swap it twice.
    px = im.tobytes()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out += struct.pack("<H", v)

    with open(dst, "wb") as fh:
        fh.write(out)
    return w, h, len(out)


def main():
    ap = argparse.ArgumentParser(description="picture -> penguinOS .565")
    ap.add_argument("images", nargs="+")
    ap.add_argument("--board", help="size from a boards/*.json profile")
    ap.add_argument("--width", type=int)
    ap.add_argument("--height", type=int)
    ap.add_argument("-o", "--out", help="output file (single input only)")
    ap.add_argument("--outdir", default=".", help="where to write (default: here)")
    ap.add_argument("--fit", choices=("cover", "contain", "stretch"),
                    default="cover", help="default: cover")
    a = ap.parse_args()

    if a.board:
        w, h = board_size(a.board)
    elif a.width and a.height:
        w, h = a.width, a.height
    else:
        w, h = 240, 240
        print("no --board or --width/--height: assuming %dx%d" % (w, h))

    if a.out and len(a.images) != 1:
        sys.exit("-o takes exactly one input; use --outdir for several")

    os.makedirs(a.outdir, exist_ok=True)
    for src in a.images:
        dst = a.out or os.path.join(
            a.outdir, os.path.splitext(os.path.basename(src))[0] + ".565")
        ow, oh, n = convert(src, dst, w, h, a.fit)
        print("%-28s -> %-28s %dx%d, %d bytes" %
              (os.path.basename(src), dst, ow, oh, n))


if __name__ == "__main__":
    main()
