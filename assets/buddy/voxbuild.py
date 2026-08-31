#!/usr/bin/env python3
"""Shared voxel-authoring helpers for the buddies in this directory.

make_penguin.py wrote Pip and, in doing so, worked out the two rules that
actually matter when a model is generated rather than modelled by hand. They
are both about the difference between the shape and the surface a camera
sees, and they are both easy to get wrong again, so they live here as
functions rather than as advice:

  paint_front() walks each column to its frontmost solid voxel before it
  colours anything. Choosing the marking's voxels with a 3-D test instead
  starts the colour behind the front face at the flanks, so the frontmost
  voxel keeps the body colour and the belly comes out striped.

  grow_from_edge() finds the body's real extent at each level and grows
  outward from it. An appendage placed at a fixed x floats free the moment
  the profile narrows, leaving a one-voxel line of background between the
  limb and the body.

The non-obvious constraint is in write_vox(): the RGBA chunk's 256 entries
are palette indices 1..255, so entry j in the file is index j+1 and colour
index 0 means empty. Writing the palette straight from index 0 shifts every
colour by one, which is the classic way to get a buddy in the wrong clothes.

Pip is not ported onto this module. make_penguin.py is the file that earned
these rules and it still emits the shipped penguin byte for byte; rewriting
it to prove a refactor would risk the one model the OS boots with.
"""
import struct
import pathlib


class Model:
    """A solid voxel grid keyed (x, y, z), with y growing backwards from the
    face at y = 0 and z up, which is what kernel/avatar/README.md requires."""

    def __init__(self, sx, sy, sz):
        assert 1 <= sx <= 32 and 1 <= sy <= 32 and 1 <= sz <= 32, "outside EOS_VOX_MAX_DIM"
        self.sx, self.sy, self.sz = sx, sy, sz
        self.v = {}

    # ---------------------------------------------------------------- build
    def set(self, x, y, z, ci):
        if 0 <= x < self.sx and 0 <= y < self.sy and 0 <= z < self.sz:
            self.v[(x, y, z)] = ci

    def fill_default(self, x, y, z, ci):
        """Only if the cell is empty - for appendages that must not repaint
        the body they attach to."""
        if 0 <= x < self.sx and 0 <= y < self.sy and 0 <= z < self.sz:
            self.v.setdefault((x, y, z), ci)

    def solid(self, x, y, z):
        return (x, y, z) in self.v

    def ellipse(self, z, cx, cy, rx, ry, ci):
        """One filled cross-section. A whole silhouette is one of these per
        level, which is why the shape of a buddy here is a table."""
        for x in range(self.sx):
            for y in range(self.sy):
                dx, dy = (x - cx) / rx, (y - cy) / ry
                if dx * dx + dy * dy <= 1.0:
                    self.v[(x, y, z)] = ci

    def rect(self, z, cx, cy, rx, ry, ci, chamfer=0):
        """A squared-off cross-section, for the buddies that are meant to look
        machined rather than grown. `chamfer` knocks the vertical corners off,
        which is the difference between a character and a crate."""
        for x in range(self.sx):
            for y in range(self.sy):
                dx, dy = abs(x - cx), abs(y - cy)
                if dx > rx or dy > ry:
                    continue
                if chamfer and dx > rx - chamfer and dy > ry - chamfer:
                    continue
                self.v[(x, y, z)] = ci

    def box(self, x0, x1, y0, y1, z0, z1, ci):
        for z in range(z0, z1 + 1):
            for y in range(y0, y1 + 1):
                for x in range(x0, x1 + 1):
                    self.set(x, y, z, ci)

    # -------------------------------------------------------------- surface
    def front_y(self, x, z):
        """Frontmost solid voxel in this column: where a face detail belongs."""
        for y in range(self.sy):
            if (x, y, z) in self.v:
                return y
        return None

    def top_z(self, x, y):
        for z in range(self.sz - 1, -1, -1):
            if (x, y, z) in self.v:
                return z
        return None

    def span_x(self, y, z):
        xs = [x for x in range(self.sx) if (x, y, z) in self.v]
        return (min(xs), max(xs)) if xs else None

    def paint_front(self, levels, ci, half_width, depth=2, cx=None):
        """Paints a marking onto the FRONT SURFACE over `levels`, out to
        `half_width` voxels either side of the centre line, `depth` voxels
        deep so the marking survives a quarter turn."""
        cx = (self.sx - 1) / 2.0 if cx is None else cx
        for z in levels:
            hw = half_width(z) if callable(half_width) else half_width
            for x in range(self.sx):
                if abs(x - cx) > hw:
                    continue
                fy = self.front_y(x, z)
                if fy is None:
                    continue
                for y in range(fy, min(fy + depth, self.sy)):
                    if (x, y, z) in self.v:
                        self.v[(x, y, z)] = ci

    def paint_dot(self, x, z, ci, depth=1):
        """One detail on the front surface of one column. Returns the y it
        landed on, or None if that column is empty."""
        fy = self.front_y(x, z)
        if fy is None:
            return None
        for y in range(fy, min(fy + depth, self.sy)):
            if (x, y, z) in self.v:
                self.v[(x, y, z)] = ci
        return fy

    def grow_from_edge(self, levels, ci, reach, y_range, side="both"):
        """Grows an appendage outward from the body's ACTUAL x extent at each
        level. `reach` may be an int or a function of z."""
        for z in levels:
            r = reach(z) if callable(reach) else reach
            if r <= 0:
                continue
            for y in y_range:
                span = self.span_x(y, z)
                if span is None:
                    continue
                lo, hi = span
                for k in range(1, r + 1):
                    if side in ("both", "left"):
                        self.fill_default(lo - k, y, z, ci)
                    if side in ("both", "right"):
                        self.fill_default(hi + k, y, z, ci)

    # ---------------------------------------------------------------- audit
    def unsupported(self):
        """Voxels with nothing directly beneath them. The bottom face of a
        voxel is never camera-facing, so an overhang shows the background
        through it; a few at the outer edge of a limb are what every
        appendage costs, a lot means the shape is floating."""
        return [k for k in self.v if k[2] > 0 and (k[0], k[1], k[2] - 1) not in self.v]

    def shell_count(self):
        n = 0
        for (x, y, z) in self.v:
            if not all(self.solid(x + dx, y + dy, z + dz) for dx, dy, dz in
                       ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1))):
                n += 1
        return n


def _chunk(cid, content, children=b""):
    return cid + struct.pack("<ii", len(content), len(children)) + content + children


def write_vox(path, model, palette, caps=(1536, 7264)):
    """Writes a real MagicaVoxel file. `palette` maps colour index -> (r,g,b).

    The RGBA chunk holds 256 entries that are indices 1..255: entry j is
    index j+1, index 0 is empty and has no entry at all. That off-by-one is
    the format's, not ours, and eos_vox undoes it on the way back in.
    """
    items = sorted(model.v.items())
    max_vox, max_bytes = caps
    assert len(items) <= max_vox, f"{len(items)} voxels exceeds the upload cap of {max_vox}"

    size = _chunk(b"SIZE", struct.pack("<iii", model.sx, model.sy, model.sz))
    xyzi = _chunk(b"XYZI", struct.pack("<i", len(items)) +
                  b"".join(bytes((x, y, z, ci)) for (x, y, z), ci in items))
    pal = bytearray()
    for i in range(1, 257):
        r, g, b = palette.get(i, (0x11, 0x11, 0x11))
        pal += bytes((r, g, b, 255))
    rgba = _chunk(b"RGBA", bytes(pal))

    out = (b"VOX " + struct.pack("<i", 150) +
           _chunk(b"MAIN", b"", size + xyzi + rgba))
    assert len(out) <= max_bytes, f"{len(out)} bytes exceeds the upload cap of {max_bytes}"

    p = pathlib.Path(path)
    p.write_bytes(out)
    return p, len(out), len(items)


def report(path, nbytes, nvox, model):
    over = model.unsupported()
    print(f"{pathlib.Path(path).name}: {nbytes} bytes, {nvox} solid voxels, "
          f"{model.sx}x{model.sy}x{model.sz}, {model.shell_count()} in the shell, "
          f"{len(over)} unsupported")
