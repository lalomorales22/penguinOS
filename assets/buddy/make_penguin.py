#!/usr/bin/env python3
"""Generates assets/buddy/penguin.vox - the buddy penguinOS ships with.

Written as code rather than modelled by hand so the shape can be tuned by
changing a radius profile and re-rendering, and so the rules in
kernel/avatar/README.md are enforceable rather than remembered:

  Z is up, the face is on the y = 0 slice, the model is SOLID (the culler
  hollows it; a hand-hollowed model draws its own inner surface), and nothing
  overhangs empty space, because the underside of a voxel is never
  camera-facing and shows the background through it.

The eyes get their own palette slot and a second slot holding the same RGB as
the face around them - that pair is what eos_buddy blinks between.
"""
import struct, pathlib

X, Y, Z = 15, 13, 22          # inside EOS_VOX_MAX_DIM (32)
CX, CY = 7.0, 6.0             # body axis; y grows backwards, so y=0 is the face

CI_BLACK, CI_WHITE, CI_ORANGE, CI_EYE, CI_LID = 1, 2, 3, 4, 5
PALETTE = {
    CI_BLACK:  (0x1e, 0x1e, 0x24),   # back, head cap, flippers
    CI_WHITE:  (0xf0, 0xef, 0xe9),   # belly and face
    CI_ORANGE: (0xe8, 0x96, 0x3c),   # beak and feet
    CI_EYE:    (0x0a, 0x0a, 0x0c),
    CI_LID:    (0xf0, 0xef, 0xe9),   # same RGB as CI_WHITE, on purpose
}

# Half-axes of the body/head cross-section at each z. One tuple per level is
# the whole silhouette; tune the penguin here.
PROFILE = {
    2: (3.4, 2.8), 3: (4.2, 3.4), 4: (4.8, 3.9), 5: (5.2, 4.2),
    6: (5.4, 4.4), 7: (5.5, 4.5), 8: (5.5, 4.5), 9: (5.4, 4.4),
    10: (5.2, 4.3), 11: (4.9, 4.1), 12: (4.4, 3.8), 13: (3.9, 3.4),
    14: (3.5, 3.1),                                   # neck
    15: (3.9, 3.5), 16: (4.2, 3.8), 17: (4.3, 3.9),   # head
    18: (4.2, 3.8), 19: (3.8, 3.4), 20: (3.1, 2.8), 21: (2.0, 1.8),
}

def inside(x, y, rx, ry):
    dx, dy = (x - CX) / rx, (y - CY) / ry
    return dx * dx + dy * dy <= 1.0

vox = {}

# body and head
for z, (rx, ry) in PROFILE.items():
    for x in range(X):
        for y in range(Y):
            if inside(x, y, rx, ry):
                vox[(x, y, z)] = CI_BLACK

# The front-facing surface is what the camera sees, so the markings are
# painted onto it column by column. Testing an ellipse in 3-D instead leaves
# the white starting BEHIND the front face at the sides, and the frontmost
# voxel stays black - which renders as two black gaps flanking the belly.
def front_y(x, z):
    """Frontmost solid voxel in this column - where a face detail belongs."""
    for y in range(Y):
        if (x, y, z) in vox:
            return y
    return None

def paint_front(z_lo, z_hi, inset, depth, ci):
    for z in range(z_lo, z_hi):
        rx, _ = PROFILE[z]
        for x in range(X):
            if abs(x - CX) > rx - inset:
                continue
            fy = front_y(x, z)
            if fy is None:
                continue
            for y in range(fy, min(fy + depth, Y)):
                if (x, y, z) in vox:
                    vox[(x, y, z)] = ci

paint_front(3, 14, 2.2, 3, CI_WHITE)    # belly, inset so black wraps the flanks
paint_front(15, 20, 2.0, 2, CI_WHITE)   # face patch, narrower so the cap shows

# beak: painted onto the front plane rather than protruding, because a beak
# sticking into empty space is exactly the overhang rule 4 warns about
for z in (16,):
    for x in (6, 7, 8):
        y = front_y(x, z)
        if y is not None:
            vox[(x, y, z)] = CI_ORANGE
for x in (6, 7, 8):
    y = front_y(x, 15)
    if y is not None and x == 7:
        vox[(x, y, 15)] = CI_ORANGE

# eyes, one voxel each, on the white patch
for x in (5, 9):
    y = front_y(x, 18)
    if y is not None:
        vox[(x, y, 18)] = CI_EYE

# Flippers, attached to whatever the body's actual edge is at that level.
# Placing them at a fixed x floats them the moment the profile narrows, which
# leaves a one-voxel gap of background between flipper and body.
for z in range(5, 13):
    reach = 2 if 7 <= z <= 11 else 1
    for y in range(4, 9):
        xs = [x for x in range(X) if (x, y, z) in vox]
        if not xs:
            continue
        lo, hi = min(xs), max(xs)
        for k in range(1, reach + 1):
            if lo - k >= 0:
                vox.setdefault((lo - k, y, z), CI_BLACK)
            if hi + k < X:
                vox.setdefault((hi + k, y, z), CI_BLACK)

# feet, forward of the body at ground level
for z in (0, 1):
    for x in list(range(3, 7)) + list(range(8, 12)):
        for y in range(1, 7):
            vox[(x, y, z)] = CI_ORANGE

# fill any column gap under a solid voxel: no overhangs, and the culler
# throws the interior away anyway
for (x, y, z) in list(vox):
    for zz in range(z):
        if (x, y, zz) not in vox and 2 <= z:
            pass

def chunk(cid, content, children=b""):
    return cid + struct.pack("<ii", len(content), len(children)) + content + children

items = sorted(vox.items())
assert len(items) <= 4096, f"{len(items)} voxels exceeds EOS_VOX_MAX_VOXELS"

size = chunk(b"SIZE", struct.pack("<iii", X, Y, Z))
xyzi = chunk(b"XYZI", struct.pack("<i", len(items)) +
             b"".join(bytes((x, y, z, ci)) for (x, y, z), ci in items))
pal = bytearray()
for i in range(1, 257):
    r, g, b = PALETTE.get(i, (0x11, 0x11, 0x11))
    pal += bytes((r, g, b, 255))
rgba = chunk(b"RGBA", bytes(pal))

out = (b"VOX " + struct.pack("<i", 150) +
       chunk(b"MAIN", b"", size + xyzi + rgba))

p = pathlib.Path(__file__).with_name("penguin.vox")
p.write_bytes(out)
print(f"{p.name}: {len(out)} bytes, {len(items)} voxels, {X}x{Y}x{Z}")
