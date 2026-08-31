#!/usr/bin/env python3
"""Generates assets/buddy/cat.vox - Mochi, the third buddy in the gallery.

A sitting cat, because the pose is the whole silhouette: heavy haunches, a
narrow chest, a round head with two triangles on it, and a tail wrapped
around the base. At fifteen voxels tall that is still four distinguishable
parts, which is more than a standing cat manages.

The rules are make_penguin.py's, unchanged: Z up, face on the y = 0 slice,
SOLID because the culler hollows it, and the markings painted onto the front
SURFACE column by column rather than chosen with a test in 3-D.

Two things here are worth knowing before tuning the tables.

The tail is not a list of coordinates. It is traced along the body's own
outline at z = 0 and 1 - the same rule the penguin's flippers taught, for the
same reason: a tail written down as fixed x values detaches from the body the
moment the haunches change radius, and the gap between them is background.

The ears sit at the crown's outer corners with the tip directly above the
base, so each one rests on something. An ear leaning outward into air would
show the panel through its underside, because a voxel's bottom face is never
camera-facing and never drawn.
"""
import pathlib
from voxbuild import Model, write_vox, report

X, Y, Z = 15, 13, 19
CX, CY = 7.0, 6.0

CI_FUR, CI_CREAM, CI_PINK, CI_EYE, CI_LID, CI_DARK = 1, 2, 3, 4, 5, 6
PALETTE = {
    CI_FUR:   (0x8e, 0x94, 0xa0),   # grey tabby
    CI_CREAM: (0xf0, 0xe8, 0xdc),   # bib, muzzle, paws
    CI_PINK:  (0xd8, 0x86, 0x92),   # nose and inner ear
    CI_EYE:   (0x86, 0xd8, 0x5e),   # green, which is what makes it a cat
    CI_LID:   (0x8e, 0x94, 0xa0),   # same RGB as CI_FUR, on purpose
    CI_DARK:  (0x5a, 0x60, 0x6c),   # forehead marks and tail tip
}

# Half-axes per level. Haunches at the bottom, a real waist at the chest, a
# head that is wider than the neck it sits on: that is the sitting pose, and
# tuning the cat means tuning this table.
PROFILE = {
    0: (4.6, 4.0),                                                # sits ON the floor
    1: (5.2, 4.6), 2: (5.5, 4.8), 3: (5.5, 4.8), 4: (5.2, 4.5),   # haunches
    5: (4.7, 4.1), 6: (4.2, 3.7), 7: (3.9, 3.4), 8: (3.7, 3.2),   # chest
    9: (3.6, 3.1), 10: (3.6, 3.1),                                # neck
    11: (4.2, 3.6), 12: (4.7, 4.0), 13: (4.9, 4.2),               # head
    14: (4.8, 4.1), 15: (4.4, 3.7), 16: (3.6, 3.0),
}

m = Model(X, Y, Z)

for z, (rx, ry) in PROFILE.items():
    m.ellipse(z, CX, CY, rx, ry, CI_FUR)

# Bib and muzzle. The bib is inset so grey wraps the flanks and the cat still
# has a back; the muzzle is narrow so the head does not turn into a mask.
m.paint_front(range(5, 10), CI_CREAM, half_width=2.2, depth=3, cx=CX)
m.paint_front(range(11, 13), CI_CREAM, half_width=2.0, depth=2, cx=CX)

# Nose on the muzzle, eyes above it on grey - which is why CI_LID is grey.
m.paint_dot(7, 12, CI_PINK)
for x in (4, 5, 9, 10):
    m.paint_dot(x, 13, CI_EYE)

# The tabby's forehead marks, two of them, above the eyes.
for x in (6, 8):
    m.paint_dot(x, 15, CI_DARK)

# Ears: base pair at the crown's corners, tip directly above the outer voxel
# of each base so nothing overhangs. The inner face gets pink.
for y in range(3, 7):
    for x in (4, 5, 9, 10):
        m.fill_default(x, y, 17, CI_FUR)
    for x in (4, 10):
        m.fill_default(x, y, 18, CI_FUR)
for x in (5, 9):
    m.paint_dot(x, 17, CI_PINK)

# Front paws. They reach one voxel further forward than the haunches, which
# is the whole of the sitting pose; both levels are filled so the overhanging
# level rests on the one below it rather than on air.
for z in (0, 1):
    for x in list(range(4, 7)) + list(range(8, 11)):
        for y in range(1, 4):
            m.set(x, y, z, CI_CREAM)

# The tail, traced along the body's real right-hand outline and hooked in at
# the front. Following the edge is what keeps it touching the cat.
tail = []
for y in range(3, 11):
    span = m.span_x(y, 1)
    if span:
        tail.append((span[1] + 1, y))
span2 = m.span_x(3, 1)
if span2:
    tail.append((span2[1] + 1, 2))
    tail.append((span2[1], 1))
for i, (x, y) in enumerate(tail):
    ci = CI_DARK if i >= len(tail) - 2 else CI_FUR
    for z in (0, 1):
        m.fill_default(x, y, z, ci)

path, nbytes, nvox = write_vox(pathlib.Path(__file__).with_name("cat.vox"), m, PALETTE)
report(path, nbytes, nvox, m)
