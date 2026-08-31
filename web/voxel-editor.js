/* voxel-editor — the buddy editor, in a canvas, with no WebGL.
 *
 * This deliberately renders the same way kernel/avatar/eos_buddy.c does: the
 * model is axis aligned so every voxel is at most three visible quads, the
 * three face orientations get three fixed brightness levels, and depth is a
 * painter walk with no depth buffer. That is not a shortcut, it is the point.
 * What you see in this canvas is what the panel can actually draw, so a buddy
 * that looks good here looks the same on a 320x240 ILI9341. A WebGL preview
 * with real lighting would flatter models the board cannot render.
 *
 * The one non-obvious thing: there is no raycast anywhere. The painter walk
 * already visits every visible face in strict far-to-near order, so picking is
 * the same walk with a point-in-parallelogram test that keeps the LAST hit.
 * Last hit means nearest, exactly, with no epsilon and no DDA, and it hands
 * back the face normal for free — which is what "build" needs to know where
 * the new voxel goes.
 *
 * Hard limits come from eos_vox.h and are enforced here rather than discovered
 * on the board: 32 per axis, 4096 voxels, palette index 0 means empty and is
 * never stored. A file this editor writes is a file eos_vox_parse() accepts.
 *
 * Those are the FORMAT's limits, and a board is allowed to be smaller than the
 * format. GET /api/buddy reports the voxel pool and staging buffer it actually
 * has - on esp32c6 that is 1536 voxels and 7264 bytes, not 4096 and 17480 -
 * and setLimits() installs them, so a model too big for the board is refused
 * while it is being built rather than after it has been uploaded. Nothing here
 * ever raises a limit past the format's; a board may only narrow them.
 *
 * The one non-obvious constraint on the writer is SIZE. eos_buddy.c centres
 * and scales the avatar on sx/sy/sz, so the declared box is not metadata, it
 * is the model's proportions on the panel: writing a 22-cube around a 15x13x22
 * penguin draws it small and off-centre, and re-deriving 15x11x22 from the
 * occupied cells makes it taller than the author drew it, because the author
 * left two empty rows in the box on purpose. So the box a file arrived in is
 * remembered and written back, grown when the model outgrows it and cleared
 * only when the model is. A new model, which arrived in no box at all, gets
 * its own bounds. Voxel coordinates never move under any of this.
 */
(function (global) {
'use strict';

var MAX_DIM = 32;        // EOS_VOX_MAX_DIM
var MAX_VOX = 4096;      // EOS_VOX_MAX_VOXELS
var YAW_STEPS = 32;      // EOS_BUDDY_YAW_STEPS
var CUSTOM_BASE = 209;   // palette slots 209..255 are the colour picker's
var SHADE = [1.0, 0.80, 0.62];  // z faces, y faces, x faces - eos_buddy shade[3]

// ------------------------------------------------------------------ limits

// Every file this editor writes has the same shape, so its length is a
// function of the voxel count alone: 8 signature + 12 MAIN + (12+12) SIZE +
// (12+4+4n) XYZI + (12+1024) RGBA. That is the arithmetic the comment over
// EOS_APPS_VOX_BYTES does, and having it here lets the editor quote the exact
// size of a model it has not written yet.
var VOX_FIXED = 8 + 12 + (12 + 12) + (12 + 4) + (12 + 1024);   // 1096
function voxBytes(n) { return VOX_FIXED + 4 * n; }

function defaultLimits() {
  return { voxels: MAX_VOX, bytes: voxBytes(MAX_VOX), dim: MAX_DIM };
}

function numIn(v, lo, hi, dflt) {
  v = Math.floor(Number(v));
  if (!isFinite(v) || v < lo || v > hi) return dflt;
  return v;
}

// numIn() rejects an out-of-range number; this one folds it into range. The
// remembered SIZE wants folding, because a box that no longer fits the grid is
// still a statement about proportion worth honouring as far as it can be.
function clampIn(v, lo, hi) {
  v = Math.floor(Number(v));
  if (!isFinite(v)) return lo;
  return v < lo ? lo : (v > hi ? hi : v);
}

// Takes the `limits` object out of GET /api/buddy and turns it into something
// safe to enforce. A board can only ever narrow the format's ceilings, so
// every field is clamped into the format's range and a missing or nonsense
// one falls back to the format. The voxel and byte caps are then reconciled
// into the tighter of the two, because two numbers that disagree is two
// numbers the owner has to reason about while trying to draw a penguin.
function sanitizeLimits(l) {
  var d = defaultLimits(), out;
  if (!l || typeof l !== 'object') return d;
  out = {
    voxels: numIn(l.voxels, 1, MAX_VOX, d.voxels),
    bytes:  numIn(l.bytes, voxBytes(1), d.bytes, d.bytes),
    dim:    numIn(l.dim, 2, MAX_DIM, d.dim)
  };
  var byBytes = Math.floor((out.bytes - VOX_FIXED) / 4);
  if (byBytes < out.voxels) out.voxels = Math.max(1, byBytes);
  // out.bytes stays the board's own number. It is what gets quoted back to
  // the owner, and quoting them a figure they will not find in eos_apps.h
  // helps nobody.
  return out;
}

// ------------------------------------------------------------------ colour

function hsl2rgb(h, s, l) {
  h = ((h % 360) + 360) % 360 / 360;
  var r, g, b;
  if (s === 0) { r = g = b = l; }
  else {
    var q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    var p = 2 * l - q;
    var f = function (t) {
      if (t < 0) t += 1;
      if (t > 1) t -= 1;
      if (t < 1 / 6) return p + (q - p) * 6 * t;
      if (t < 1 / 2) return q;
      if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
      return p;
    };
    r = f(h + 1 / 3); g = f(h); b = f(h - 1 / 3);
  }
  return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
}

function hex2rgb(s) {
  s = String(s || '').replace('#', '');
  if (s.length === 3) s = s[0] + s[0] + s[1] + s[1] + s[2] + s[2];
  if (!/^[0-9a-fA-F]{6}$/.test(s)) return [0, 0, 0];
  return [parseInt(s.slice(0, 2), 16), parseInt(s.slice(2, 4), 16), parseInt(s.slice(4, 6), 16)];
}

function rgb2hex(r, g, b) {
  var h = function (v) { return ('0' + (v & 255).toString(16)).slice(-2); };
  return '#' + h(r) + h(g) + h(b);
}

// 13 columns of 16: one grey ramp then twelve hues, dark-saturated up to
// light-pastel. 208 entries, then 47 free slots the colour picker fills in.
// Laid out so the swatch grid reads as a colour wheel rather than a soup.
function buildPalette() {
  var p = new Uint8Array(256 * 3);
  var i, col, row, t, rgb;
  for (row = 0; row < 16; row++) {
    t = row / 15;
    var g = Math.round((0.06 + 0.94 * t) * 255);
    i = (1 + 0 * 16 + row) * 3;
    p[i] = g; p[i + 1] = g; p[i + 2] = g;
  }
  for (col = 1; col <= 12; col++) {
    for (row = 0; row < 16; row++) {
      t = row / 15;
      rgb = hsl2rgb((col - 1) * 30, 0.95 - 0.55 * t * t, 0.12 + 0.80 * t);
      i = (1 + col * 16 + row) * 3;
      p[i] = rgb[0]; p[i + 1] = rgb[1]; p[i + 2] = rgb[2];
    }
  }
  for (i = CUSTOM_BASE; i < 256; i++) {
    p[i * 3] = 40; p[i * 3 + 1] = 39; p[i * 3 + 2] = 37;
  }
  return p;
}

// ------------------------------------------------------------------- grid

function Grid(dim) {
  this.dim = dim;
  this.d = new Uint8Array(dim * dim * dim);
  this.n = 0;
}
Grid.prototype.at = function (x, y, z) { return (z * this.dim + y) * this.dim + x; };
Grid.prototype.inb = function (x, y, z) {
  var d = this.dim;
  return x >= 0 && y >= 0 && z >= 0 && x < d && y < d && z < d;
};
Grid.prototype.get = function (x, y, z) {
  return this.inb(x, y, z) ? this.d[this.at(x, y, z)] : 0;
};
Grid.prototype.set = function (x, y, z, ci) {
  if (!this.inb(x, y, z)) return false;
  var k = this.at(x, y, z), old = this.d[k];
  if (old === ci) return false;
  if (old === 0 && ci !== 0) this.n++;
  if (old !== 0 && ci === 0) this.n--;
  this.d[k] = ci;
  return true;
};
Grid.prototype.clear = function () { this.d.fill(0); this.n = 0; };

// Copies the overlapping corner. Anything outside the new grid is dropped,
// which is the honest thing to do and is warned about in the UI.
Grid.prototype.resized = function (nd) {
  var g = new Grid(nd), m = Math.min(nd, this.dim), x, y, z, c;
  for (z = 0; z < m; z++) for (y = 0; y < m; y++) for (x = 0; x < m; x++) {
    c = this.d[this.at(x, y, z)];
    if (c) g.set(x, y, z, c);
  }
  return g;
};

// ---------------------------------------------------------- .vox encoding
//
// MagicaVoxel chunk stream, exactly the subset eos_vox_parse() reads:
// "VOX " + version, one MAIN whose children are SIZE, XYZI and RGBA.
// Version 150 sits inside the 100..250 window the parser accepts.

function w32(a, o, v) {
  a[o] = v & 255; a[o + 1] = (v >>> 8) & 255;
  a[o + 2] = (v >>> 16) & 255; a[o + 3] = (v >>> 24) & 255;
}
function r32(a, o) {
  return (a[o] | (a[o + 1] << 8) | (a[o + 2] << 16) | (a[o + 3] << 24)) >>> 0;
}
function tag(a, o, s) { for (var i = 0; i < 4; i++) a[o + i] = s.charCodeAt(i); }
function istag(a, o, s) {
  for (var i = 0; i < 4; i++) if (a[o + i] !== s.charCodeAt(i)) return false;
  return true;
}

function writeVox(grid, pal, limits, prefSize) {
  var lim = sanitizeLimits(limits);
  var d = grid.dim, list = [], x, y, z, c;
  var mx = 0, my = 0, mz = 0;
  // (z,y,x) ascending, which is the order eos_vox_finish() sorts into anyway.
  for (z = 0; z < d; z++) for (y = 0; y < d; y++) for (x = 0; x < d; x++) {
    c = grid.d[grid.at(x, y, z)];
    if (!c) continue;
    list.push(x, y, z, c);
    if (x >= mx) mx = x + 1;
    if (y >= my) my = y + 1;
    if (z >= mz) mz = z + 1;
  }
  var n = list.length / 4;
  if (n > lim.voxels)
    throw new Error('this board holds ' + lim.voxels + ' voxels and the model has ' + n);
  var nb = voxBytes(n);
  if (nb > lim.bytes)
    throw new Error('this board stages ' + lim.bytes + ' bytes of .vox and this is ' + nb);
  // The editing grid is deliberately NOT checked against lim.dim. What the
  // board has to accept is the SIZE this writes, and a small model sitting in
  // a large grid declares a small SIZE. Refusing on the grid would refuse a
  // file the board would have taken.

  // The box the model arrived in, where there was one, grown to hold whatever
  // has been added since and never allowed past the grid or the board's dim.
  // Math.max with the occupied bound is what keeps every voxel inside SIZE,
  // which is the one thing eos_vox_parse() will not forgive.
  if (prefSize && prefSize.length === 3) {
    mx = Math.max(mx, clampIn(prefSize[0], 1, d));
    my = Math.max(my, clampIn(prefSize[1], 1, d));
    mz = Math.max(mz, clampIn(prefSize[2], 1, d));
  }
  // eos_vox_parse() calls a zero dimension EOS_VOX_ERR_DIM, so an empty model
  // is written as a legal 1x1x1 with no voxels in it rather than as a file the
  // board will refuse to read back.
  if (!mx) mx = 1;
  if (!my) my = 1;
  if (!mz) mz = 1;
  if (mx > lim.dim || my > lim.dim || mz > lim.dim)
    throw new Error('model is ' + mx + 'x' + my + 'x' + mz +
                    ', larger than ' + lim.dim + ' on an axis');

  var szSize = 12, szXyzi = 4 + 4 * n, szRgba = 1024;
  var children = (12 + szSize) + (12 + szXyzi) + (12 + szRgba);
  var buf = new Uint8Array(8 + 12 + children), o = 0, i;

  tag(buf, o, 'VOX '); w32(buf, o + 4, 150); o += 8;
  tag(buf, o, 'MAIN'); w32(buf, o + 4, 0); w32(buf, o + 8, children); o += 12;

  tag(buf, o, 'SIZE'); w32(buf, o + 4, szSize); w32(buf, o + 8, 0); o += 12;
  w32(buf, o, mx); w32(buf, o + 4, my); w32(buf, o + 8, mz); o += 12;

  tag(buf, o, 'XYZI'); w32(buf, o + 4, szXyzi); w32(buf, o + 8, 0); o += 12;
  w32(buf, o, n); o += 4;
  for (i = 0; i < list.length; i++) buf[o++] = list[i];

  // File entry j is palette index j+1; entry 255 has no index and is padding.
  tag(buf, o, 'RGBA'); w32(buf, o + 4, szRgba); w32(buf, o + 8, 0); o += 12;
  for (i = 0; i < 255; i++) {
    buf[o++] = pal[(i + 1) * 3];
    buf[o++] = pal[(i + 1) * 3 + 1];
    buf[o++] = pal[(i + 1) * 3 + 2];
    buf[o++] = 255;
  }
  buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 255;
  return buf;
}

// What eos_vox_finish() will leave once it has culled. A voxel with all six
// neighbours occupied draws no face and is dropped on the board, which is why
// GET /api/buddy reports 572 voxels for a penguin whose file holds 1280. The
// editor shows both numbers rather than letting the owner conclude that half
// the model failed to upload. Out of bounds counts as empty here exactly as it
// does in eos_vox_occupied(), so the shell of the model always survives.
function countDrawn(grid) {
  var d = grid.dim, g = grid.d, n = 0, x, y, z;
  var solid = function (X, Y, Z) {
    if (X < 0 || Y < 0 || Z < 0 || X >= d || Y >= d || Z >= d) return false;
    return g[(Z * d + Y) * d + X] !== 0;
  };
  for (z = 0; z < d; z++) for (y = 0; y < d; y++) for (x = 0; x < d; x++) {
    if (!g[(z * d + y) * d + x]) continue;
    if (solid(x + 1, y, z) && solid(x - 1, y, z) &&
        solid(x, y + 1, z) && solid(x, y - 1, z) &&
        solid(x, y, z + 1) && solid(x, y, z - 1)) continue;
    n++;
  }
  return n;
}

// Mirrors eos_vox_parse()'s rules so a file this editor opens is a file the
// board opens. Throws with the same reasons the C returns. `limits` is
// optional and narrows the format's ceilings to the board's, so a file that
// parses but will not fit is refused here with the board's own numbers.
function readVox(u8, limits) {
  var lim = sanitizeLimits(limits);
  if (!u8 || u8.length < 8) throw new Error('truncated');
  if (!istag(u8, 0, 'VOX ')) throw new Error('not a .vox file');
  var ver = r32(u8, 4);
  if (ver < 100 || ver > 250) throw new Error('unsupported .vox version ' + ver);
  if (u8.length < 20 || !istag(u8, 8, 'MAIN')) throw new Error('no MAIN chunk');

  var body = 20;
  var mc = r32(u8, 12), mk = r32(u8, 16);
  if (mc > u8.length - body) throw new Error('bad MAIN');
  if (mk > u8.length - body - mc) throw new Error('bad MAIN');

  var pos = body + mc, end = pos + mk;
  var grid = null, pal = null, sx = 0, sy = 0, sz = 0, haveSize = false, haveXyzi = false;
  var nRead = 0;

  while (pos + 12 <= end) {
    var clen = r32(u8, pos + 4), klen = r32(u8, pos + 8), cb = pos + 12;
    if (clen > end - cb) throw new Error('chunk overruns file');
    if (klen > end - cb - clen) throw new Error('chunk overruns file');

    if (!haveXyzi && istag(u8, pos, 'SIZE')) {
      if (clen < 12) throw new Error('truncated SIZE');
      sx = r32(u8, cb); sy = r32(u8, cb + 4); sz = r32(u8, cb + 8);
      if (!sx || !sy || !sz) throw new Error('zero dimension');
      if (sx > lim.dim || sy > lim.dim || sz > lim.dim)
        throw new Error('model is ' + sx + 'x' + sy + 'x' + sz +
                        ', larger than ' + lim.dim + ' on an axis');
      haveSize = true;

    } else if (!haveXyzi && istag(u8, pos, 'XYZI')) {
      if (!haveSize) throw new Error('XYZI before SIZE');
      if (clen < 4) throw new Error('truncated XYZI');
      var n = r32(u8, cb);
      if (n > lim.voxels)
        throw new Error('model has ' + n + ' voxels and the limit is ' + lim.voxels);
      if (n > (clen - 4) / 4) throw new Error('truncated XYZI');
      // One cubic grid holds a non-cubic model; the editor keeps it square.
      grid = new Grid(Math.max(sx, sy, sz));
      for (var i = 0; i < n; i++) {
        var q = cb + 4 + i * 4;
        if (u8[q] >= sx || u8[q + 1] >= sy || u8[q + 2] >= sz)
          throw new Error('voxel outside the declared size');
        if (u8[q + 3] === 0) continue;
        grid.set(u8[q], u8[q + 1], u8[q + 2], u8[q + 3]);
      }
      nRead = n;
      haveXyzi = true;

    } else if (!pal && istag(u8, pos, 'RGBA')) {
      if (clen < 1024) throw new Error('truncated RGBA');
      pal = new Uint8Array(256 * 3);
      for (var j = 0; j < 255; j++) {
        pal[(j + 1) * 3] = u8[cb + j * 4];
        pal[(j + 1) * 3 + 1] = u8[cb + j * 4 + 1];
        pal[(j + 1) * 3 + 2] = u8[cb + j * 4 + 2];
      }
    }
    pos = cb + clen + klen;
  }
  if (!haveSize || !haveXyzi) throw new Error('no SIZE and XYZI pair in the file');
  // count is what the file declared; grid.n is what survived, which differs
  // only if the file repeated a cell. Both are reported because a silent drop
  // is the sort of thing that gets blamed on the upload.
  return { grid: grid, pal: pal, size: [sx, sy, sz], count: nRead, kept: grid.n };
}

// ----------------------------------------------------------------- camera

// Orthonormal screen basis. u is screen right, v is screen down, w points at
// the viewer, so a larger p.w is nearer and a painter walk wants ascending w.
function basis(yaw, pitch) {
  var sa = Math.sin(yaw), ca = Math.cos(yaw);
  var sp = Math.sin(pitch), cp = Math.cos(pitch);
  return {
    u: [ca, -sa, 0],
    v: [sa * sp, ca * sp, -cp],
    w: [sa * cp, ca * cp, sp]
  };
}
function dot3(a, x, y, z) { return a[0] * x + a[1] * y + a[2] * z; }

// The four world corners of one cube face. axis 0/1/2 = x/y/z, pos picks the
// far side of the cell. Winding is consistent; the picker does not care which.
function faceQuad(x, y, z, axis, pos, out) {
  var k = pos ? 1 : 0;
  if (axis === 0) {
    var X = x + k;
    out[0] = X; out[1] = y; out[2] = z;
    out[3] = X; out[4] = y + 1; out[5] = z;
    out[6] = X; out[7] = y + 1; out[8] = z + 1;
    out[9] = X; out[10] = y; out[11] = z + 1;
  } else if (axis === 1) {
    var Y = y + k;
    out[0] = x; out[1] = Y; out[2] = z;
    out[3] = x + 1; out[4] = Y; out[5] = z;
    out[6] = x + 1; out[7] = Y; out[8] = z + 1;
    out[9] = x; out[10] = Y; out[11] = z + 1;
  } else {
    var Z = z + k;
    out[0] = x; out[1] = y; out[2] = Z;
    out[3] = x + 1; out[4] = y; out[5] = Z;
    out[6] = x + 1; out[7] = y + 1; out[8] = Z;
    out[9] = x; out[10] = y + 1; out[11] = Z;
  }
}

// Convex quad containment by consistent edge cross-product sign.
function inQuad(q, px, py) {
  var pos = 0, neg = 0;
  for (var i = 0; i < 4; i++) {
    var ax = q[i * 2], ay = q[i * 2 + 1];
    var j = (i + 1) & 3;
    var cr = (q[j * 2] - ax) * (py - ay) - (q[j * 2 + 1] - ay) * (px - ax);
    if (cr > 0) pos++; else if (cr < 0) neg++;
  }
  return pos === 0 || neg === 0;
}

// ----------------------------------------------------------------- editor

function Editor(opt) {
  this.canvas = opt.canvas;
  this.ctx = this.canvas.getContext('2d');
  this.preview = opt.preview || null;
  this.pctx = this.preview ? this.preview.getContext('2d') : null;
  this.onchange = opt.onchange || function () {};

  this.grid = new Grid(16);
  this.pal = buildPalette();
  this.customUsed = new Uint8Array(256);
  // The format's ceilings until a board tells us its own. Nothing is allowed
  // to be built that these forbid, so an editor that never reaches a board
  // still writes a file every board can read.
  this.limits = sanitizeLimits(opt.limits);
  // The SIZE the current model arrived in, or null for a model that was drawn
  // here. See the note at the top of the file about why this is kept.
  this.size = null;

  this.color = 1 + 5 * 16 + 9;    // a mid green, an inoffensive default
  this.tool = 'build';
  this.zLayer = 0;
  this.slice = false;
  this.orbitMode = false;
  this.mirrorX = false;

  this.eyeIndex = 0;
  this.blinkIndex = 0;

  this.yaw = Math.PI * 0.22;
  this.pitch = 0.52;
  this.scale = 14;
  this.panX = 0;
  this.panY = 0;

  this.undoStack = [];
  this.redoStack = [];
  this.stroke = null;
  this.touched = null;

  this.dirty = true;
  this.running = false;
  this.previewYaw = 0;
  this.previewT = 0;
  this.lastFrame = 0;

  this._quad = new Float64Array(12);
  this._scr = new Float64Array(8);

  this._bindInput();
  this.recentre();
}

Editor.MAX_DIM = MAX_DIM;
Editor.MAX_VOX = MAX_VOX;
Editor.CUSTOM_BASE = CUSTOM_BASE;

// Installs the caps GET /api/buddy reported. Called after a load, so it has to
// cope with a model that is already over them: it does NOT delete voxels -
// throwing away a penguin because a number arrived late would be indefensible
// - it reports the overage and lets the UI say so, and the save path refuses.
// Returns the number of voxels over the new cap, zero when the model fits.
Editor.prototype.setLimits = function (l) {
  this.limits = sanitizeLimits(l);
  this._changed('limits');
  return Math.max(0, this.grid.n - this.limits.voxels);
};

// The exact byte length toVox() would produce right now.
Editor.prototype.byteSize = function () { return voxBytes(this.grid.n); };

// What the board will actually draw, after eos_vox_finish() culls the buried.
Editor.prototype.drawnCount = function () { return countDrawn(this.grid); };

// ------------------------------------------------------------ model state

Editor.prototype.count = function () { return this.grid.n; };
Editor.prototype.dim = function () { return this.grid.dim; };

Editor.prototype.setDim = function (nd) {
  nd = Math.max(2, Math.min(this.limits.dim, nd | 0));
  if (nd === this.grid.dim) return 0;
  var before = this.grid.n;
  this.grid = this.grid.resized(nd);
  this.undoStack.length = 0;
  this.redoStack.length = 0;
  if (this.zLayer >= nd) this.zLayer = nd - 1;
  this.recentre();
  this._changed('dim');
  return before - this.grid.n;   // voxels dropped
};

Editor.prototype.colorHex = function (i) {
  return rgb2hex(this.pal[i * 3], this.pal[i * 3 + 1], this.pal[i * 3 + 2]);
};

Editor.prototype.setColor = function (i) {
  i = i | 0;
  if (i < 1 || i > 255) return;
  this.color = i;
  this._changed('color');
};

// Finds an exact match anywhere in the palette, else takes the next free
// custom slot, else recycles a custom slot no voxel is using. Returns the
// index, or 0 when every custom slot is spoken for by a live voxel.
Editor.prototype.addCustom = function (hex) {
  var rgb = hex2rgb(hex), i;
  for (i = 1; i < 256; i++) {
    if (this.pal[i * 3] === rgb[0] && this.pal[i * 3 + 1] === rgb[1] &&
        this.pal[i * 3 + 2] === rgb[2] && (i < CUSTOM_BASE || this.customUsed[i])) {
      return i;
    }
  }
  for (i = CUSTOM_BASE; i < 256; i++) if (!this.customUsed[i]) return this._setCustom(i, rgb);
  var used = this.usedIndices();
  for (i = CUSTOM_BASE; i < 256; i++) if (!used[i]) return this._setCustom(i, rgb);
  return 0;
};

Editor.prototype._setCustom = function (i, rgb) {
  this.pal[i * 3] = rgb[0]; this.pal[i * 3 + 1] = rgb[1]; this.pal[i * 3 + 2] = rgb[2];
  this.customUsed[i] = 1;
  this._changed('palette');
  return i;
};

Editor.prototype.usedIndices = function () {
  var u = new Uint8Array(256), d = this.grid.d;
  for (var i = 0; i < d.length; i++) if (d[i]) u[d[i]] = 1;
  return u;
};

// ------------------------------------------------------------ undo / redo

Editor.prototype.beginStroke = function () {
  this.stroke = [];
  this.touched = Object.create(null);
};

Editor.prototype.endStroke = function () {
  if (this.stroke && this.stroke.length) {
    this.undoStack.push(this.stroke);
    if (this.undoStack.length > 60) this.undoStack.shift();
    this.redoStack.length = 0;
  }
  this.stroke = null;
  this.touched = null;
};

Editor.prototype._put = function (x, y, z, ci) {
  if (!this.grid.inb(x, y, z)) return;
  var k = this.grid.at(x, y, z);
  if (this.touched) {
    if (this.touched[k]) return;
    this.touched[k] = 1;
  }
  var old = this.grid.d[k];
  if (old === ci) return;
  if (ci !== 0 && old === 0 && this.grid.n >= this.limits.voxels) return;
  this.grid.set(x, y, z, ci);
  if (this.stroke) this.stroke.push(k, old, ci);
};

Editor.prototype._apply = function (rec, undo) {
  for (var i = 0; i < rec.length; i += 3) {
    var k = rec[i], v = undo ? rec[i + 1] : rec[i + 2];
    var d = this.grid.dim, x = k % d, y = ((k / d) | 0) % d, z = (k / (d * d)) | 0;
    this.grid.set(x, y, z, v);
  }
  this.dirty = true;
  this._changed('edit');
};

Editor.prototype.undo = function () {
  var r = this.undoStack.pop();
  if (!r) return false;
  this.redoStack.push(r);
  this._apply(r, true);
  return true;
};

Editor.prototype.redo = function () {
  var r = this.redoStack.pop();
  if (!r) return false;
  this.undoStack.push(r);
  this._apply(r, false);
  return true;
};

Editor.prototype.clear = function () {
  if (!this.grid.n) return;
  // The box belonged to the model that is going. Keeping it would draw the
  // next, smaller model shrunk into a corner of a penguin-sized volume.
  this.size = null;
  this.beginStroke();
  var d = this.grid.dim, x, y, z;
  for (z = 0; z < d; z++) for (y = 0; y < d; y++) for (x = 0; x < d; x++) {
    if (this.grid.d[this.grid.at(x, y, z)]) this._put(x, y, z, 0);
  }
  this.endStroke();
  this.dirty = true;
  this._changed('edit');
};

Editor.prototype._changed = function (kind) {
  this.dirty = true;
  this.onchange(kind);
};

// -------------------------------------------------------------- painting

Editor.prototype.setTool = function (t) { this.tool = t; this._changed('tool'); };
Editor.prototype.setZ = function (z) {
  this.zLayer = Math.max(0, Math.min(this.grid.dim - 1, z | 0));
  this.dirty = true;
  this._changed('z');
};
Editor.prototype.setSlice = function (b) { this.slice = !!b; this._changed('slice'); };
Editor.prototype.setOrbit = function (b) { this.orbitMode = !!b; this._changed('orbit'); };
Editor.prototype.setMirror = function (b) { this.mirrorX = !!b; this._changed('mirror'); };

Editor.prototype.recentre = function () {
  // A canvas inside a hidden panel measures zero. Recentring off that would
  // pin the scale to the floor, so fall back to a sane square instead.
  var w = this.canvas.clientWidth || 320, h = this.canvas.clientHeight || 320;
  this.scale = Math.max(3, Math.min(w, h) / (this.grid.dim * 1.55));
  this.panX = 0;
  this.panY = 0;
  this.dirty = true;
};

// Applies the current tool at a picked cell. `hit` comes from _pick().
Editor.prototype._act = function (hit) {
  if (!hit) return;
  var t = this.tool;
  if (t === 'eye') {
    if (hit.ci) { this.eyeIndex = hit.ci; this._changed('eye'); }
    return;
  }
  var x = hit.x, y = hit.y, z = hit.z;
  if (t === 'build') {
    x = hit.px; y = hit.py; z = hit.pz;
    // In slice mode every placement belongs to the active layer. Clicking the
    // top face of a voxel at the ceiling would otherwise drop a voxel into the
    // hidden region above it, which looks exactly like nothing happening.
    if (this.slice) z = this.zLayer;
  }
  if (t === 'erase') this._put(x, y, z, 0);
  else this._put(x, y, z, this.color);

  if (this.mirrorX) {
    var mx = this.grid.dim - 1 - x;
    if (mx !== x) {
      if (t === 'erase') this._put(mx, y, z, 0);
      else this._put(mx, y, z, this.color);
    }
  }
  this.dirty = true;
  this._changed('edit');
};

// ---------------------------------------------------------------- walking

// Visits every drawable face in far-to-near order. For axis-aligned unit
// cubes under an orthographic projection, iterating each axis in the
// direction that makes its depth contribution increase is an exact painter
// order - no sort, no depth buffer, no cyclic overlap.
Editor.prototype._walk = function (b, cb) {
  var g = this.grid, d = g.dim;
  var wx = b.w[0], wy = b.w[1], wz = b.w[2];
  var x0 = wx > 0 ? 0 : d - 1, xs = wx > 0 ? 1 : -1;
  var y0 = wy > 0 ? 0 : d - 1, ys = wy > 0 ? 1 : -1;
  var z0 = wz > 0 ? 0 : d - 1, zs = wz > 0 ? 1 : -1;
  var zTop = this.slice ? this.zLayer : d - 1;

  var nx = wx > 0 ? 1 : -1, ny = wy > 0 ? 1 : -1, nz = wz > 0 ? 1 : -1;
  var doX = Math.abs(wx) > 1e-6, doY = Math.abs(wy) > 1e-6, doZ = Math.abs(wz) > 1e-6;

  for (var zi = 0; zi < d; zi++) {
    var z = z0 + zs * zi;
    if (z > zTop) continue;
    for (var yi = 0; yi < d; yi++) {
      var y = y0 + ys * yi;
      for (var xi = 0; xi < d; xi++) {
        var x = x0 + xs * xi;
        var ci = g.d[(z * d + y) * d + x];
        if (!ci) continue;
        // A voxel hidden under the slice ceiling has no visible top face.
        if (doX && !this._solid(x + nx, y, z, zTop)) cb(x, y, z, ci, 0, nx > 0);
        if (doY && !this._solid(x, y + ny, z, zTop)) cb(x, y, z, ci, 1, ny > 0);
        if (doZ && !this._solid(x, y, z + nz, zTop)) cb(x, y, z, ci, 2, nz > 0);
      }
    }
  }
};

Editor.prototype._solid = function (x, y, z, zTop) {
  if (z > zTop) return false;
  return this.grid.get(x, y, z) !== 0;
};

// ---------------------------------------------------------------- picking

// The painter walk again, keeping the last face that contains the point.
// Last drawn is nearest, so this is exact.
Editor.prototype._pick = function (sx, sy) {
  var b = basis(this.yaw, this.pitch);
  var c = this._centre(), s = this.scale;
  var ox = this.canvas.clientWidth / 2 + this.panX;
  var oy = this.canvas.clientHeight / 2 + this.panY;
  var q = this._quad, scr = this._scr;
  var best = null;

  this._walk(b, function (x, y, z, ci, axis, pos) {
    faceQuad(x, y, z, axis, pos, q);
    for (var i = 0; i < 4; i++) {
      var px = q[i * 3] - c[0], py = q[i * 3 + 1] - c[1], pz = q[i * 3 + 2] - c[2];
      scr[i * 2] = ox + s * dot3(b.u, px, py, pz);
      scr[i * 2 + 1] = oy + s * dot3(b.v, px, py, pz);
    }
    if (!inQuad(scr, sx, sy)) return;
    var d = [0, 0, 0];
    d[axis] = pos ? 1 : -1;
    best = {
      x: x, y: y, z: z, ci: ci,
      px: x + d[0], py: y + d[1], pz: z + d[2]
    };
  });

  if (best) return best;
  return this._pickPlane(sx, sy, b, c, ox, oy, s);
};

// No voxel under the pointer: drop onto the active layer's floor plane, which
// is z = zLayer in slice mode and the ground otherwise. This is what makes
// the Z slider an actual layer-by-layer workflow rather than a view filter.
Editor.prototype._pickPlane = function (sx, sy, b, c, ox, oy, s) {
  var target = this.slice ? this.zLayer : 0;
  if (Math.abs(b.w[2]) < 1e-4) return null;   // edge on, no solution
  var a = (sx - ox) / s, e = (sy - oy) / s;
  // P = C + a*u + e*v + t*w ; solve P.z == target. u[2] is always 0.
  var t = (target - c[2] - e * b.v[2]) / b.w[2];
  var wx = c[0] + a * b.u[0] + e * b.v[0] + t * b.w[0];
  var wy = c[1] + a * b.u[1] + e * b.v[1] + t * b.w[1];
  var x = Math.floor(wx), y = Math.floor(wy), z = target;
  if (!this.grid.inb(x, y, z)) return null;
  return { x: x, y: y, z: z, ci: this.grid.get(x, y, z), px: x, py: y, pz: z };
};

Editor.prototype._centre = function () {
  var h = this.grid.dim / 2;
  return [h, h, h];
};

// -------------------------------------------------------------- rendering

Editor.prototype._fit = function () {
  var cv = this.canvas;
  var dpr = Math.min(2, global.devicePixelRatio || 1);
  var w = Math.max(1, Math.round(cv.clientWidth * dpr));
  var h = Math.max(1, Math.round(cv.clientHeight * dpr));
  if (cv.width !== w || cv.height !== h) {
    cv.width = w; cv.height = h;
    this.dirty = true;
  }
  return dpr;
};

Editor.prototype.render = function () {
  var dpr = this._fit();
  var ctx = this.ctx, cv = this.canvas;
  var W = cv.clientWidth, H = cv.clientHeight;

  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  var b = basis(this.yaw, this.pitch);
  var c = this._centre(), s = this.scale;
  var ox = W / 2 + this.panX, oy = H / 2 + this.panY;
  var q = this._quad, pal = this.pal;
  var self = this;

  this._drawFloor(ctx, b, c, s, ox, oy);

  ctx.lineJoin = 'round';
  this._walk(b, function (x, y, z, ci, axis, pos) {
    faceQuad(x, y, z, axis, pos, q);
    var f = SHADE[2 - axis];   // axis 2 (z) -> SHADE[0], axis 0 (x) -> SHADE[2]
    var r = pal[ci * 3] * f | 0, g = pal[ci * 3 + 1] * f | 0, bl = pal[ci * 3 + 2] * f | 0;
    var col = 'rgb(' + r + ',' + g + ',' + bl + ')';

    ctx.beginPath();
    for (var i = 0; i < 4; i++) {
      var px = q[i * 3] - c[0], py = q[i * 3 + 1] - c[1], pz = q[i * 3 + 2] - c[2];
      var X = ox + s * dot3(b.u, px, py, pz);
      var Y = oy + s * dot3(b.v, px, py, pz);
      if (i === 0) ctx.moveTo(X, Y); else ctx.lineTo(X, Y);
    }
    ctx.closePath();
    ctx.fillStyle = col;
    ctx.fill();
    // Stroking with the fill colour closes the hairline seams that appear
    // between adjacent quads at fractional scales.
    ctx.strokeStyle = col;
    ctx.lineWidth = 1;
    ctx.stroke();
    if (self.eyeIndex && ci === self.eyeIndex && axis === 1) {
      ctx.strokeStyle = 'rgba(255,255,255,.55)';
      ctx.stroke();
    }
  });

  this.dirty = false;
};

// Grid floor at the active plane, plus the model's footprint box. Gives the
// eye somewhere to stand when the model is empty.
Editor.prototype._drawFloor = function (ctx, b, c, s, ox, oy) {
  var d = this.grid.dim, z = this.slice ? this.zLayer : 0;
  var P = function (x, y, zz) {
    var px = x - c[0], py = y - c[1], pz = zz - c[2];
    return [ox + s * dot3(b.u, px, py, pz), oy + s * dot3(b.v, px, py, pz)];
  };
  ctx.lineWidth = 1;
  ctx.strokeStyle = this.slice ? 'rgba(216,142,86,.42)' : 'rgba(138,135,129,.22)';
  ctx.beginPath();
  for (var i = 0; i <= d; i++) {
    var a1 = P(i, 0, z), a2 = P(i, d, z);
    ctx.moveTo(a1[0], a1[1]); ctx.lineTo(a2[0], a2[1]);
    var b1 = P(0, i, z), b2 = P(d, i, z);
    ctx.moveTo(b1[0], b1[1]); ctx.lineTo(b2[0], b2[1]);
  }
  ctx.stroke();

  if (this.mirrorX) {
    var m = d / 2;
    ctx.strokeStyle = 'rgba(240,192,138,.45)';
    ctx.beginPath();
    var m1 = P(m, 0, z), m2 = P(m, d, z);
    ctx.moveTo(m1[0], m1[1]); ctx.lineTo(m2[0], m2[1]);
    ctx.stroke();
  }
};

// The small canvas: the same renderer, but yaw quantised to the 32 steps the
// board actually uses and a bob on the same curve, so it previews the panel
// rather than the editor.
Editor.prototype.renderPreview = function (dt) {
  if (!this.pctx) return;
  var ctx = this.pctx, cv = this.preview;
  var W = cv.width, H = cv.height;
  ctx.clearRect(0, 0, W, H);
  if (!this.grid.n) return;

  this.previewT += dt;
  this.previewYaw += dt * 0.00035;
  var step = Math.floor(this.previewYaw * YAW_STEPS / (Math.PI * 2)) % YAW_STEPS;
  var yaw = step * (Math.PI * 2 / YAW_STEPS);
  var bob = Math.sin(this.previewT * 0.0022) * 0.35;

  var b = basis(yaw, 0.36);
  var bx = this._bounds();
  var c = [(bx[0] + bx[3] + 1) / 2, (bx[1] + bx[4] + 1) / 2, (bx[2] + bx[5] + 1) / 2 - bob];
  var span = Math.max(bx[3] - bx[0] + 1, bx[4] - bx[1] + 1, bx[5] - bx[2] + 1);
  var s = Math.min(W, H) / (span * 1.6);
  var ox = W / 2, oy = H / 2;
  var q = this._quad, pal = this.pal;

  var saveSlice = this.slice;
  this.slice = false;
  this._walk(b, function (x, y, z, ci, axis, pos) {
    faceQuad(x, y, z, axis, pos, q);
    var f = SHADE[2 - axis];
    var col = 'rgb(' + (pal[ci * 3] * f | 0) + ',' + (pal[ci * 3 + 1] * f | 0) + ',' +
              (pal[ci * 3 + 2] * f | 0) + ')';
    ctx.beginPath();
    for (var i = 0; i < 4; i++) {
      var px = q[i * 3] - c[0], py = q[i * 3 + 1] - c[1], pz = q[i * 3 + 2] - c[2];
      var X = ox + s * dot3(b.u, px, py, pz);
      var Y = oy + s * dot3(b.v, px, py, pz);
      if (i === 0) ctx.moveTo(X, Y); else ctx.lineTo(X, Y);
    }
    ctx.closePath();
    ctx.fillStyle = col; ctx.fill();
    ctx.strokeStyle = col; ctx.lineWidth = 1; ctx.stroke();
  });
  this.slice = saveSlice;
};

Editor.prototype._bounds = function () {
  var d = this.grid.dim, g = this.grid.d;
  var x0 = d, y0 = d, z0 = d, x1 = -1, y1 = -1, z1 = -1;
  for (var z = 0; z < d; z++) for (var y = 0; y < d; y++) for (var x = 0; x < d; x++) {
    if (!g[(z * d + y) * d + x]) continue;
    if (x < x0) x0 = x; if (x > x1) x1 = x;
    if (y < y0) y0 = y; if (y > y1) y1 = y;
    if (z < z0) z0 = z; if (z > z1) z1 = z;
  }
  if (x1 < 0) return [0, 0, 0, d - 1, d - 1, d - 1];
  return [x0, y0, z0, x1, y1, z1];
};

// ------------------------------------------------------------- animation

Editor.prototype.start = function () {
  if (this.running) return;
  this.running = true;
  this.lastFrame = 0;
  var self = this;
  var loop = function (t) {
    if (!self.running) return;
    var dt = self.lastFrame ? Math.min(100, t - self.lastFrame) : 16;
    self.lastFrame = t;
    // _fit() flags dirty when the element changed size, so window resizes and
    // panel show/hide repaint without anyone listening for a resize event.
    self._fit();
    if (self.dirty) self.render();
    self.previewAcc = (self.previewAcc || 0) + dt;
    if (self.previewAcc >= 66) {          // ~15fps, which is panel speed
      self.renderPreview(self.previewAcc);
      self.previewAcc = 0;
    }
    global.requestAnimationFrame(loop);
  };
  global.requestAnimationFrame(loop);
};

Editor.prototype.stop = function () { this.running = false; };

// ----------------------------------------------------------------- input

Editor.prototype._pt = function (e) {
  var r = this.canvas.getBoundingClientRect();
  return [e.clientX - r.left, e.clientY - r.top];
};

Editor.prototype._orbitBy = function (dx, dy) {
  this.yaw -= dx * 0.011;
  this.pitch += dy * 0.009;
  var lim = Math.PI / 2 - 0.06;
  this.pitch = Math.max(-0.35, Math.min(lim, this.pitch));
  this.dirty = true;
};

Editor.prototype.zoomBy = function (f) {
  this.scale = Math.max(2, Math.min(140, this.scale * f));
  this.dirty = true;
};

Editor.prototype._bindInput = function () {
  var self = this, cv = this.canvas;
  var pts = Object.create(null), nPts = 0;
  var mode = null;          // 'tool' | 'orbit' | 'gesture'
  var last = null, pinch = 0, mid = null;

  var isOrbitBtn = function (e) {
    return e.button === 2 || e.button === 1 || e.shiftKey;
  };

  cv.addEventListener('contextmenu', function (e) { e.preventDefault(); });

  cv.addEventListener('pointerdown', function (e) {
    // Capture keeps a drag alive when the finger leaves the canvas. It is a
    // convenience, not a requirement, and some browsers throw on a pointer
    // they do not consider active - losing the whole gesture over that would
    // be a poor trade.
    try { cv.setPointerCapture(e.pointerId); } catch (err) { /* not fatal */ }
    pts[e.pointerId] = self._pt(e);
    nPts++;

    if (nPts === 2) {
      // A second finger cancels whatever the first was doing and becomes a
      // pinch-and-pan. Anything the first finger painted stays painted.
      if (mode === 'tool') self.endStroke();
      mode = 'gesture';
      var k = Object.keys(pts), a = pts[k[0]], b = pts[k[1]];
      pinch = Math.hypot(a[0] - b[0], a[1] - b[1]);
      mid = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
      return;
    }
    if (nPts > 2) return;

    if (e.pointerType === 'mouse' ? isOrbitBtn(e) : self.orbitMode) {
      mode = 'orbit';
      last = self._pt(e);
    } else {
      mode = 'tool';
      last = self._pt(e);
      self.beginStroke();
      self._act(self._pick(last[0], last[1]));
    }
    e.preventDefault();
  });

  cv.addEventListener('pointermove', function (e) {
    if (!(e.pointerId in pts)) return;
    var p = self._pt(e);
    pts[e.pointerId] = p;

    if (mode === 'gesture') {
      var k = Object.keys(pts);
      if (k.length < 2) return;
      var a = pts[k[0]], b = pts[k[1]];
      var d = Math.hypot(a[0] - b[0], a[1] - b[1]);
      var m = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
      if (pinch > 4 && d > 4) self.zoomBy(d / pinch);
      self.panX += m[0] - mid[0];
      self.panY += m[1] - mid[1];
      pinch = d; mid = m;
      self.dirty = true;
      e.preventDefault();
      return;
    }
    if (mode === 'orbit') {
      self._orbitBy(p[0] - last[0], p[1] - last[1]);
      last = p;
      e.preventDefault();
      return;
    }
    if (mode === 'tool') {
      // Drag to keep painting. _put() dedupes per stroke, so dragging over a
      // cell twice does not stack undo records.
      self._act(self._pick(p[0], p[1]));
      last = p;
      e.preventDefault();
    }
  });

  var up = function (e) {
    if (!(e.pointerId in pts)) return;
    delete pts[e.pointerId];
    nPts = Math.max(0, nPts - 1);
    if (nPts === 0) {
      if (mode === 'tool') self.endStroke();
      mode = null;
    } else if (mode === 'gesture' && nPts === 1) {
      var k = Object.keys(pts);
      last = pts[k[0]];
      mode = 'orbit';       // lifting one finger leaves you orbiting, not painting
    }
  };
  cv.addEventListener('pointerup', up);
  cv.addEventListener('pointercancel', up);

  cv.addEventListener('wheel', function (e) {
    e.preventDefault();
    self.zoomBy(e.deltaY < 0 ? 1.12 : 1 / 1.12);
  }, { passive: false });
};

// ------------------------------------------------------------------ files

Editor.prototype.toVox = function () {
  return writeVox(this.grid, this.pal, this.limits, this.size);
};

// The box the file will declare, which is what eos_buddy.c draws the avatar
// inside. Shown in the UI so the owner is never surprised by it.
Editor.prototype.declaredSize = function () {
  var b = this._bounds(), d = this.grid.dim, i;
  var out = this.grid.n ? [b[3] + 1, b[4] + 1, b[5] + 1] : [1, 1, 1];
  if (this.size) for (i = 0; i < 3; i++) {
    out[i] = Math.max(out[i], clampIn(this.size[i], 1, d));
  }
  return out;
};

Editor.prototype.fromVox = function (u8) {
  var r = readVox(u8, this.limits);
  this.grid = r.grid;
  this.size = r.size.slice();
  if (r.pal) {
    // The file's palette is MERGED over a fresh wheel, not adopted whole. A
    // .vox carries all 256 entries whether the model references them or not,
    // and the penguin this OS ships with has 0x111111 in the 251 it does not:
    // adopting that wholesale turns the colour picker into a grid of identical
    // dark squares the instant the buddy loads, which is the blank-canvas
    // failure again in a different costume. Entries a voxel actually paints
    // with are taken byte for byte, so the model is exact and round-trips
    // exact; the rest are the editor's own wheel, so there is something to
    // paint WITH. Nothing references the free entries, so writing them back
    // differently changes no pixel on the panel.
    var used = this.usedIndices(), i;
    this.pal = buildPalette();
    for (i = 1; i < 256; i++) {
      if (!used[i]) continue;
      this.pal[i * 3]     = r.pal[i * 3];
      this.pal[i * 3 + 1] = r.pal[i * 3 + 1];
      this.pal[i * 3 + 2] = r.pal[i * 3 + 2];
    }
    // Anything at or past the custom base that a voxel uses is a real colour
    // now, not a free slot.
    for (i = CUSTOM_BASE; i < 256; i++) this.customUsed[i] = used[i];
  }
  this.undoStack.length = 0;
  this.redoStack.length = 0;
  if (this.zLayer >= this.grid.dim) this.zLayer = this.grid.dim - 1;
  this.recentre();
  this._changed('load');
  return r;
};

global.EOSVox = {
  Editor: Editor,
  Grid: Grid,
  writeVox: writeVox,
  readVox: readVox,
  countDrawn: countDrawn,
  voxBytes: voxBytes,
  defaultLimits: defaultLimits,
  sanitizeLimits: sanitizeLimits,
  buildPalette: buildPalette,
  hex2rgb: hex2rgb,
  rgb2hex: rgb2hex,
  MAX_DIM: MAX_DIM,
  MAX_VOX: MAX_VOX,
  CUSTOM_BASE: CUSTOM_BASE
};

})(window);
