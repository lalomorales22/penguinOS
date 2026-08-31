#!/usr/bin/env python3
"""Generates assets/buddy/robot.vox - Bolt, the fourth buddy in the gallery.

The other three are round, so this one is square. A voxel grid flatters a
machine: chamfered slabs, a visor instead of a face, an antenna to break the
head's outline against the wallpaper. At fifteen voxels tall the antenna is
what tells you which one this is from across a desk.

Same rules as make_penguin.py: Z up, face on the y = 0 slice, model SOLID,
and the visor and chest strip painted onto the FRONT SURFACE column by column
rather than picked out with a test in 3-D.

The non-obvious one here is the blink pair. Bolt's eyes sit on a dark visor,
not on skin, so CI_LID holds the visor's RGB rather than the panel's - the
lid has to match whatever the eye is surrounded BY, or a blink punches two
light holes in the middle of the visor instead of closing it.

Everything above the waist rests on something: the collar is wider than the
neck so the head lands on it, and the antenna sits directly on the crown. The
arms are the one deliberate overhang, the same one the penguin's flippers
are, and they hang from the body's real edge at each level rather than from a
fixed x, so they cannot drift away from the torso.
"""
import pathlib
from voxbuild import Model, write_vox, report

X, Y, Z = 15, 11, 20
CX, CY = 7.0, 5.0

CI_PANEL, CI_METAL, CI_CYAN, CI_EYE, CI_LID = 1, 2, 3, 4, 5
PALETTE = {
    CI_PANEL: (0xb4, 0xbc, 0xc8),   # torso and head shell
    CI_METAL: (0x3c, 0x44, 0x50),   # legs, arms, neck, visor
    CI_CYAN:  (0x46, 0xd0, 0xdc),   # chest strip and antenna tip
    CI_EYE:   (0xf4, 0xb0, 0x3c),   # amber, on the visor
    CI_LID:   (0x3c, 0x44, 0x50),   # same RGB as CI_METAL, on purpose
}

# Half-width and half-depth per level, same idea as the penguin's radius
# profile but squared off. The narrow top and bottom level of each block is
# what keeps the robot from being two crates on legs.
TORSO = {
    5: (4.0, 3.0),
    6: (4.5, 3.5), 7: (4.5, 3.5), 8: (4.5, 3.5), 9: (4.5, 3.5), 10: (4.5, 3.5),
    11: (4.0, 3.0),
}
HEAD = {
    13: (3.0, 2.5),
    14: (3.5, 3.0), 15: (3.5, 3.0), 16: (3.5, 3.0),
    17: (3.0, 2.5),
}

m = Model(X, Y, Z)

for z, (rx, ry) in TORSO.items():
    m.rect(z, CX, CY, rx, ry, CI_PANEL, chamfer=1)

# Collar: wider than a neck so the head has something to land on, which is
# the difference between a head and a head hovering over a gap.
m.rect(12, CX, CY, 2.0, 2.0, CI_METAL)

for z, (rx, ry) in HEAD.items():
    m.rect(z, CX, CY, rx, ry, CI_PANEL, chamfer=1)

# Feet and legs, as two blocks with daylight between them. The gap is the
# silhouette: one solid plinth reads as a fridge.
m.box(3, 6, 2, 7, 0, 1, CI_METAL)
m.box(8, 11, 2, 7, 0, 1, CI_METAL)
m.box(4, 6, 3, 6, 2, 4, CI_METAL)
m.box(8, 10, 3, 6, 2, 4, CI_METAL)

# Arms, grown from the torso's real edge so they stay welded to it, with a
# wider mitt at the bottom.
m.grow_from_edge(range(7, 11), CI_METAL, reach=1, y_range=range(3, 8))
m.grow_from_edge(range(5, 7), CI_METAL, reach=2, y_range=range(3, 8))

# Visor across the face, then the eyes on it.
m.paint_front(range(15, 17), CI_METAL, half_width=3.0, depth=1, cx=CX)
for x in (5, 9):
    for z in (15, 16):
        m.paint_dot(x, z, CI_EYE)

# Chest strip: the one lit thing below the face, so the eye is drawn up.
for x in (6, 7, 8):
    m.paint_dot(x, 8, CI_CYAN)

# Antenna, standing on the crown rather than beside it.
for y in (4, 5):
    m.set(7, y, 18, CI_METAL)
    m.set(7, y, 19, CI_CYAN)

path, nbytes, nvox = write_vox(pathlib.Path(__file__).with_name("robot.vox"), m, PALETTE)
report(path, nbytes, nvox, m)
