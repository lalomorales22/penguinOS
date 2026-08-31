#!/usr/bin/env python3
"""Generates assets/buddy/owl.vox - Hoot, the second buddy in the gallery.

An owl because the silhouette survives being fifteen voxels tall: a wide
egg, a waist that barely dips, and two ear tufts. Nothing else on the shelf
has tufts, so at a glance the gallery reads penguin / owl / cat / robot
rather than four rounded blobs.

Same rules as make_penguin.py, and for the same reasons: Z up, the face on
the y = 0 slice, the model SOLID because the culler hollows it, and the ear
tufts stacked so each one sits on something rather than over air. The face
disc, the belly and the eyes are painted onto the FRONT SURFACE column by
column - see voxbuild.paint_front for what testing an ellipse in 3-D does to
a marking at the flanks.

The one non-obvious thing here is the eye pair. Hoot's eyes are 2x2 because
an owl reads as an owl by its eyes, and all four voxels of each eye carry
CI_EYE so the blink swaps the whole disc; CI_LID holds the same RGB as the
cream face around them, which is what makes that swap look like an eyelid
instead of a hole.
"""
import pathlib
from voxbuild import Model, write_vox, report

X, Y, Z = 15, 11, 20
CX, CY = 7.0, 5.0

CI_BROWN, CI_CREAM, CI_ORANGE, CI_EYE, CI_LID, CI_TAN = 1, 2, 3, 4, 5, 6
PALETTE = {
    CI_BROWN:  (0x6b, 0x4a, 0x30),   # back, crown, flanks
    CI_CREAM:  (0xe8, 0xd8, 0xb4),   # face disc and breast
    CI_ORANGE: (0xe8, 0x96, 0x3c),   # beak and talons
    CI_EYE:    (0x14, 0x10, 0x0c),
    CI_LID:    (0xe8, 0xd8, 0xb4),   # same RGB as CI_CREAM, on purpose
    CI_TAN:    (0x9a, 0x74, 0x4c),   # folded wings and tufts
}

# Half-axes of the cross-section at each z. The whole bird is this table:
# a body that swells to z=7, the shallowest of waists at z=10, and a head
# that swells again so the crown overhangs nothing.
PROFILE = {
    2: (4.4, 3.4), 3: (5.0, 3.9), 4: (5.5, 4.3), 5: (5.8, 4.5),
    6: (5.9, 4.6), 7: (5.8, 4.5), 8: (5.6, 4.4), 9: (5.2, 4.1),
    10: (4.6, 3.7), 11: (4.4, 3.5),                    # shoulders, then neck
    12: (4.8, 3.8), 13: (5.1, 4.0), 14: (5.1, 4.0),    # head
    15: (4.8, 3.8), 16: (4.4, 3.5), 17: (3.8, 3.0),    # flat crown, for the tufts
}

m = Model(X, Y, Z)

for z, (rx, ry) in PROFILE.items():
    m.ellipse(z, CX, CY, rx, ry, CI_BROWN)

# Breast, then the face disc. Both inset from the silhouette so the brown
# wraps the flanks and the bird still has a back when it turns.
m.paint_front(range(3, 10), CI_CREAM, half_width=2.8, depth=3, cx=CX)
m.paint_front(range(11, 17), CI_CREAM,
              half_width=lambda z: {11: 2.6, 12: 3.4, 13: 3.8, 14: 3.8, 15: 3.2, 16: 2.0}[z],
              depth=2, cx=CX)

# Eyes: two by two, inside the disc, one voxel of cream between them and the
# rim so the disc still frames them.
for x in (4, 5, 9, 10):
    for z in (13, 14):
        m.paint_dot(x, z, CI_EYE)

# Beak: painted on the front plane, not protruding. A beak sticking into
# empty space is exactly the overhang the README warns about.
m.paint_dot(7, 12, CI_ORANGE)
m.paint_dot(7, 11, CI_ORANGE)

# Folded wings, grown from the body's real edge at each level so they cannot
# float away from it when the profile narrows.
m.grow_from_edge(range(4, 11), CI_TAN, reach=1, y_range=range(2, 8))

# Ear tufts. Two voxels wide at the base and one at the tip, so the tip rests
# on the base and the base rests on the crown.
for y in range(3, 6):
    for x in (4, 5, 9, 10):
        m.fill_default(x, y, 18, CI_TAN)
    for x in (4, 10):
        m.fill_default(x, y, 19, CI_TAN)

# Talons, forward of the body at ground level.
for z in (0, 1):
    for x in list(range(3, 7)) + list(range(8, 12)):
        for y in range(1, 6):
            m.set(x, y, z, CI_ORANGE)

path, nbytes, nvox = write_vox(pathlib.Path(__file__).with_name("owl.vox"), m, PALETTE)
report(path, nbytes, nvox, m)
