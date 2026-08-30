/* app — the companion web app for ESP-OS.
 *
 * Everything here is written against the fact that the server is an ESP32 with
 * a few kilobytes of free heap and a handful of sockets. Two things follow.
 *
 * First, requests are queued through a semaphore of two. A browser will happily
 * open six connections to one host; an ESP32 http server has around four
 * workers, and the seventh request does not queue politely, it times out. So
 * the queue lives here, on the client, where it is cheap.
 *
 * Second, nothing on screen is allowed to depend on a request completing. Every
 * fetch has a deadline, every panel renders its own failure inline, and the
 * pollers back off instead of hammering a board that is busy repainting a
 * window. A board that stops answering makes the page go quiet and show a bar,
 * never blank, and never a spinner that spins forever.
 *
 * The non-obvious constraint: settings keys are flat and dotted and none of
 * them exceeds fifteen characters, because they land in NVS and an NVS key is
 * fifteen usable bytes. That limit is invisible from here and unforgiving on
 * the board, so it is enforced at the contract, not discovered at runtime.
 */
(function () {
'use strict';

// ------------------------------------------------------------------ tiny

function $(id) { return document.getElementById(id); }
function el(tag, cls, txt) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (txt != null) e.textContent = txt;
  return e;
}
function svg(id) {
  var s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  var u = document.createElementNS('http://www.w3.org/2000/svg', 'use');
  u.setAttribute('href', '#' + id);
  s.appendChild(u);
  return s;
}
function bytes(n) {
  if (n == null || isNaN(n)) return '-';
  if (n < 1024) return n + ' B';
  var u = ['K', 'M', 'G', 'T'], i = -1;
  do { n /= 1024; i++; } while (n >= 1024 && i < 3);
  return (n < 10 ? n.toFixed(1) : Math.round(n)) + ' ' + u[i];
}
function dur(ms) {
  if (ms == null) return '-';
  var s = Math.floor(ms / 1000);
  var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600);
  var m = Math.floor(s % 3600 / 60);
  if (d) return d + 'd ' + h + 'h';
  if (h) return h + 'h ' + m + 'm';
  if (m) return m + 'm ' + (s % 60) + 's';
  return s + 's';
}
function qp(o) {
  var p = [];
  for (var k in o) if (o[k] != null) p.push(encodeURIComponent(k) + '=' + encodeURIComponent(o[k]));
  return p.length ? '?' + p.join('&') : '';
}
function dirname(p) {
  if (p === '/' || !p) return '/';
  var i = p.replace(/\/+$/, '').lastIndexOf('/');
  return i <= 0 ? '/' : p.slice(0, i);
}
function joinp(dir, leaf) {
  return (dir === '/' ? '' : dir.replace(/\/+$/, '')) + '/' + leaf;
}

function toast(msg, kind) {
  var t = el('div', 'toast' + (kind ? ' ' + kind : ''), msg);
  $('toasts').appendChild(t);
  setTimeout(function () {
    t.style.opacity = '0';
    t.style.transition = 'opacity .25s';
    setTimeout(function () { t.remove(); }, 260);
  }, kind === 'err' ? 5200 : 2600);
}

// One dialog element reused for prompt and confirm, because a board this small
// deserves a page that does not build DOM it does not need.
function dialog(title, msg, def, withInput) {
  var d = $('dlg');
  $('dlg-title').textContent = title;
  $('dlg-msg').textContent = msg || '';
  $('dlg-msg').hidden = !msg;
  var inp = $('dlg-in');
  inp.hidden = !withInput;
  inp.value = def || '';
  return new Promise(function (res) {
    var done = function () {
      d.removeEventListener('close', done);
      res(d.returnValue === 'ok' ? (withInput ? inp.value : true) : null);
    };
    d.addEventListener('close', done);
    // With method=dialog, Enter fires the FIRST submit button, which is Cancel.
    // Nobody expects Enter to mean cancel, so route it to OK by hand.
    inp.onkeydown = function (ev) {
      if (ev.key === 'Enter') { ev.preventDefault(); $('dlg-ok').click(); }
    };
    d.returnValue = '';
    d.showModal();
    if (withInput) { inp.focus(); inp.select(); }
  });
}
function ask(title, msg, def) { return dialog(title, msg, def, true); }
function confirmBox(title, msg) { return dialog(title, msg, '', false); }

// ------------------------------------------------------------------- api

var Q = { active: 0, limit: 2, wait: [] };
function slot() {
  return new Promise(function (res) {
    if (Q.active < Q.limit) { Q.active++; res(); }
    else Q.wait.push(res);
  });
}
function unslot() {
  var n = Q.wait.shift();
  if (n) n(); else Q.active--;
}

var Link = { fails: 0, ok: 0 };

function markUp() {
  Link.fails = 0;
  $('linkdot').className = 'dot up';
  $('offline').hidden = true;
}
function markDown(why) {
  Link.fails++;
  $('linkdot').className = 'dot down';
  if (Link.fails >= 2) {
    $('offlinetext').textContent = why || 'board not answering';
    $('offline').hidden = false;
  }
}

function apiRaw(path, opt) {
  opt = opt || {};
  return slot().then(function () {
    var ctl = new AbortController();
    var timer = setTimeout(function () { ctl.abort(); }, opt.timeout || 9000);
    if (opt.signal) {
      opt.signal.addEventListener('abort', function () { ctl.abort(); });
    }
    var init = {
      method: opt.method || 'GET',
      signal: ctl.signal,
      cache: 'no-store',
      headers: opt.headers || undefined
    };
    if (opt.body != null) init.body = opt.body;
    return fetch(path, init).then(
      function (r) { clearTimeout(timer); return r; },
      function (e) { clearTimeout(timer); throw e; }
    );
  }).then(function (r) { unslot(); return r; },
          function (e) { unslot(); throw e; });
}

function api(path, opt) {
  opt = opt || {};
  return apiRaw(path, opt).then(function (r) {
    var ct = r.headers.get('content-type') || '';
    var isJson = ct.indexOf('json') >= 0;
    return (isJson ? r.json().catch(function () { return null; })
                   : r.text().catch(function () { return ''; }))
      .then(function (body) {
        if (!r.ok) {
          var code = body && body.error ? body.error : ('http_' + r.status);
          var detail = body && body.detail ? body.detail : (typeof body === 'string' ? body : '');
          var e = new Error(detail || code);
          e.code = code;
          e.status = r.status;
          if (r.status >= 500 || r.status === 0) markDown(); else markUp();
          throw e;
        }
        markUp();
        return body;
      });
  }, function (e) {
    if (e.name === 'AbortError') { markDown('request timed out'); e.code = 'timeout'; }
    else if (!e.code) { markDown('cannot reach the board'); e.code = 'network'; }
    throw e;
  });
}

function fail(e, what) {
  var m = (what ? what + ': ' : '') + (e && e.message ? e.message : 'failed');
  toast(m, 'err');
  return null;
}

// --------------------------------------------------------------- polling
//
// One backing-off poller. On a good board it ticks at `base`; on a board that
// is busy or gone it doubles out to 30s instead of piling requests onto a
// server that already cannot keep up.

function Poller(base, fn) {
  this.base = base; this.fn = fn;
  this.cur = base; this.timer = null; this.on = false;
}
Poller.prototype.start = function () {
  if (this.on) return;
  this.on = true; this.cur = this.base;
  this._tick();
};
Poller.prototype.stop = function () {
  this.on = false;
  clearTimeout(this.timer);
  this.timer = null;
};
Poller.prototype.kick = function () {
  if (!this.on) return;
  clearTimeout(this.timer);
  this.cur = this.base;
  this._tick();
};
Poller.prototype._tick = function () {
  var self = this;
  if (!this.on) return;
  Promise.resolve().then(this.fn).then(function () {
    self.cur = self.base;
  }, function () {
    self.cur = Math.min(30000, self.cur * 2);
  }).then(function () {
    if (!self.on) return;
    self.timer = setTimeout(function () { self._tick(); }, self.cur);
  });
};

// ----------------------------------------------------------------- state

var S = {
  sys: null,
  settings: {},
  themes: [],
  apps: [],
  models: [],
  limits: { chunk_max: 4096, path_max: 96, name_max: 40, list_max: 128 },
  tab: 'files'
};

// ---------------------------------------------------------------- themes

var ROLES = ['bg', 'surface', 'overlay', 'text', 'muted', 'accent', 'accent_alt',
             'ok', 'warn', 'err', 'border_focused', 'border_unfocused',
             'bar_bg', 'bar_fg', 'tab_active', 'tab_inactive'];

function applyTheme(t) {
  if (!t || !t.colors) return;
  var r = document.documentElement;
  for (var i = 0; i < ROLES.length; i++) {
    var c = t.colors[ROLES[i]];
    if (c) r.style.setProperty('--' + ROLES[i].replace(/_/g, '-'), c);
  }
  if (t.metrics && t.metrics.radius != null) {
    r.style.setProperty('--radius', Math.max(0, Math.min(6, t.metrics.radius)) + 'px');
  }
  var m = document.querySelector('meta[name=theme-color]');
  if (!m) {
    m = document.createElement('meta');
    m.name = 'theme-color';
    document.head.appendChild(m);
  }
  if (t.colors.bar_bg) m.content = t.colors.bar_bg;
}

function loadThemes() {
  return api('/api/themes').then(function (d) {
    S.themes = (d && d.themes) || [];
    var active = d && d.active;
    for (var i = 0; i < S.themes.length; i++) {
      if (S.themes[i].name === active) applyTheme(S.themes[i]);
    }
    return d;
  }, function () { return null; });
}

// ---------------------------------------------------------------- system

function renderBar() {
  var s = S.sys;
  if (!s) return;
  $('boardname').textContent = (s.net && s.net.hostname) || (s.board && s.board.id) || 'esp-os';
  var b = $('barstats');
  b.textContent = '';
  var add = function (label, val) {
    var w = el('span', null);
    w.appendChild(document.createTextNode(label + ' '));
    w.appendChild(el('b', null, val));
    b.appendChild(w);
  };
  if (s.heap) add('heap', bytes(s.heap.free));
  if (s.net && s.net.rssi != null) add('rssi', s.net.rssi + 'dB');
  add('up', dur(s.uptime_ms));
}

function pollSystem() {
  return api('/api/system', { timeout: 6000 }).then(function (d) {
    S.sys = d;
    if (d && d.limits) S.limits = d.limits;
    renderBar();
    if (S.tab === 'settings') renderInfo();
    return d;
  });
}

// ================================================================== FILES

var F = {
  path: '/sd',
  entries: [],
  offset: 0,
  more: false,
  busy: false
};

function crumbs() {
  var c = $('f-crumbs');
  c.textContent = '';
  var parts = F.path.split('/').filter(Boolean);
  var mk = function (label, target) {
    var b = el('button', null, label);
    b.type = 'button';
    b.onclick = function () { go(target); };
    return b;
  };
  c.appendChild(mk('/', '/'));
  var acc = '';
  for (var i = 0; i < parts.length; i++) {
    acc += '/' + parts[i];
    if (i) c.appendChild(el('span', 'sep', '/'));
    c.appendChild(mk(parts[i], acc));
  }
}

function go(p) {
  F.path = p || '/';
  F.offset = 0;
  F.entries = [];
  listFiles();
}

function usage() {
  var mount = '/' + (F.path.split('/').filter(Boolean)[0] || '');
  if (mount === '/') { $('f-usage').hidden = true; return Promise.resolve(); }
  return api('/api/fs/usage' + qp({ point: mount }), { timeout: 12000 })
    .then(function (u) {
      if (!u || !u.total) { $('f-usage').hidden = true; return; }
      var pct = Math.min(100, Math.round(u.used / u.total * 100));
      $('f-usagefill').style.width = pct + '%';
      $('f-usagetxt').textContent = bytes(u.total - u.used) + ' free of ' + bytes(u.total);
      $('f-usage').hidden = false;
    }, function () { $('f-usage').hidden = true; });
}

function listFiles(append) {
  if (F.busy) return Promise.resolve();
  F.busy = true;
  crumbs();
  var note = $('f-note');
  if (!append) {
    note.hidden = false;
    note.textContent = 'reading ' + F.path + ' ...';
    note.className = 'note';
  }
  return api('/api/fs/list' + qp({
    path: F.path, offset: F.offset, count: S.limits.list_max || 128
  }), { timeout: 14000 }).then(function (d) {
    var list = (d && d.entries) || [];
    F.entries = append ? F.entries.concat(list) : list;
    F.offset = F.entries.length;
    F.more = !!(d && d.more);
    drawFiles();
    usage();
  }, function (e) {
    note.hidden = false;
    note.className = 'note';
    note.style.color = 'var(--err)';
    note.textContent = 'cannot read ' + F.path + ' - ' + e.message;
    if (!append) { $('f-list').textContent = ''; $('f-more').hidden = true; }
  }).then(function () { F.busy = false; });
}

function drawFiles() {
  var tb = $('f-list'), note = $('f-note');
  tb.textContent = '';
  note.style.color = '';
  if (!F.entries.length) {
    note.hidden = false;
    note.className = 'note';
    note.textContent = F.path === '/' ? 'no mounts' : 'empty';
  } else {
    note.hidden = true;
  }
  var sorted = F.entries.slice().sort(function (a, b) {
    if (!!a.is_dir !== !!b.is_dir) return a.is_dir ? -1 : 1;
    return String(a.name).localeCompare(String(b.name));
  });
  for (var i = 0; i < sorted.length; i++) tb.appendChild(fileRow(sorted[i]));
  $('f-more').hidden = !F.more;
}

function fileRow(e) {
  var full = joinp(F.path, e.name);
  var tr = el('tr', e.is_dir ? 'dir' : null);

  var ic = el('td', 'ic');
  ic.appendChild(svg(F.path === '/' ? 'i-disk' : (e.is_dir ? 'i-dir' : 'i-file')));
  tr.appendChild(ic);

  var nm = el('td', 'nm');
  var nb = el('button', null, e.name);
  nb.type = 'button';
  nb.onclick = function () {
    if (e.is_dir) go(full);
    else downloadFile(full);
  };
  nm.appendChild(nb);
  tr.appendChild(nm);

  tr.appendChild(el('td', 'sz', e.is_dir ? '' : bytes(e.size)));

  var ac = el('td', 'ac');
  if (!e.is_dir) ac.appendChild(iconBtn('i-dl', 'download', function () { downloadFile(full); }));
  ac.appendChild(iconBtn('i-ren', 'rename', function () { renameEntry(full, e.name); }));
  ac.appendChild(iconBtn('i-del', 'delete', function () { removeEntry(full, e); }));
  tr.appendChild(ac);
  return tr;
}

function iconBtn(icon, title, fn) {
  var b = el('button');
  b.type = 'button';
  b.title = title;
  b.setAttribute('aria-label', title);
  b.appendChild(svg(icon));
  b.onclick = fn;
  return b;
}

function downloadFile(path) {
  var a = document.createElement('a');
  a.href = '/api/fs/read' + qp({ path: path });
  a.download = path.split('/').pop();
  document.body.appendChild(a);
  a.click();
  a.remove();
}

function renameEntry(full, name) {
  ask('Rename', full, name).then(function (v) {
    if (!v || v === name) return;
    if (v.indexOf('/') >= 0) return toast('name cannot contain /', 'err');
    return api('/api/fs/rename' + qp({ from: full, to: joinp(F.path, v) }),
               { method: 'POST' })
      .then(function () { toast('renamed', 'ok'); go(F.path); },
            function (e) { fail(e, 'rename'); });
  });
}

function removeEntry(full, e) {
  confirmBox('Delete', (e.is_dir ? 'Directory ' : 'File ') + full +
             (e.is_dir ? ' - must already be empty.' : '')).then(function (ok) {
    if (!ok) return;
    return api('/api/fs/remove' + qp({ path: full }), { method: 'POST' })
      .then(function () { toast('deleted', 'ok'); go(F.path); },
            function (err) { fail(err, 'delete'); });
  });
}

function mkdirHere() {
  ask('New folder', 'inside ' + F.path, '').then(function (v) {
    if (!v) return;
    if (v.indexOf('/') >= 0) return toast('name cannot contain /', 'err');
    return api('/api/fs/mkdir' + qp({ path: joinp(F.path, v) }), { method: 'POST' })
      .then(function () { toast('created', 'ok'); go(F.path); },
            function (e) { fail(e, 'mkdir'); });
  });
}

// ------------------------------------------------------------- uploading
//
// Chunked, sequential, one file at a time. The board declares how much it can
// swallow in one request via limits.chunk_max and this never sends more than
// that, because a request bigger than the receive buffer is not a slow upload,
// it is a failed allocation.

function uploadRow(name) {
  var d = el('div', 'up');
  d.appendChild(el('span', 'nm', name));
  var pb = el('span', 'pb');
  var i = el('i');
  pb.appendChild(i);
  d.appendChild(pb);
  var pct = el('span', null, '0%');
  d.appendChild(pct);
  $('f-queue').appendChild(d);
  return { row: d, bar: i, pct: pct };
}

function putChunks(path, blob, ui) {
  var chunk = Math.max(512, Math.min(S.limits.chunk_max || 4096, 16384));
  var total = blob.size;
  var offset = 0;

  var step = function () {
    if (offset >= total) return Promise.resolve();
    var end = Math.min(total, offset + chunk);
    var part = blob.slice(offset, end);
    var last = end >= total;
    var tries = 0;

    var send = function () {
      return apiRaw('/api/fs/write' + qp({
        path: path, offset: offset, final: last ? 1 : 0
      }), {
        method: 'POST',
        body: part,
        headers: { 'Content-Type': 'application/octet-stream' },
        timeout: 20000
      }).then(function (r) {
        if (!r.ok) {
          return r.text().catch(function () { return ''; }).then(function (t) {
            var e = new Error(t || ('http ' + r.status));
            e.status = r.status;
            throw e;
          });
        }
        markUp();
        return r.json().catch(function () { return null; });
      });
    };

    var attempt = function () {
      return send().catch(function (e) {
        // A 4xx is the board telling us the request was wrong; retrying it
        // just wastes the board's time. Only transport trouble is retried.
        if (e.status >= 400 && e.status < 500) throw e;
        if (++tries >= 3) throw e;
        return new Promise(function (r) { setTimeout(r, 350 * tries); }).then(attempt);
      });
    };

    return attempt().then(function () {
      offset = end;
      var p = Math.round(offset / total * 100);
      ui.bar.style.width = p + '%';
      ui.pct.textContent = p + '%';
      return step();
    });
  };

  return total === 0
    ? apiRaw('/api/fs/write' + qp({ path: path, offset: 0, final: 1 }),
             { method: 'POST', body: new Blob([]), timeout: 12000 })
    : step();
}

function uploadOne(file) {
  var path = joinp(F.path, file.name);
  var ui = uploadRow(file.name);
  return putChunks(path, file, ui).then(function () {
    ui.row.classList.add('done');
    ui.pct.textContent = 'done';
    setTimeout(function () { ui.row.remove(); }, 2500);
  }, function (e) {
    ui.row.classList.add('fail');
    ui.pct.textContent = 'failed';
    ui.row.title = e.message;
    // Tell the board to let go of the half-written handle so the next upload
    // is not refused as busy.
    api('/api/fs/upload/abort' + qp({ path: path }), { method: 'POST' })
      .catch(function () {});
    throw e;
  });
}

function uploadAll(files) {
  var list = Array.prototype.slice.call(files);
  if (!list.length) return;
  var i = 0, bad = 0;
  var next = function () {
    if (i >= list.length) {
      if (bad) toast(bad + ' of ' + list.length + ' failed', 'err');
      else toast('uploaded ' + list.length, 'ok');
      go(F.path);
      return;
    }
    var f = list[i++];
    uploadOne(f).catch(function () { bad++; }).then(next);
  };
  next();
}

function bindFiles() {
  $('f-up').onclick = function () { go(dirname(F.path)); };
  $('f-refresh').onclick = function () { go(F.path); };
  $('f-mkdir').onclick = mkdirHere;
  $('f-more').onclick = function () { listFiles(true); };
  $('f-pick').onclick = function () { $('f-file').click(); };
  $('f-file').onchange = function (e) {
    uploadAll(e.target.files);
    e.target.value = '';
  };

  var drop = $('f-drop');
  ['dragenter', 'dragover'].forEach(function (n) {
    drop.addEventListener(n, function (e) {
      e.preventDefault();
      drop.classList.add('over');
    });
  });
  ['dragleave', 'drop'].forEach(function (n) {
    drop.addEventListener(n, function (e) {
      e.preventDefault();
      drop.classList.remove('over');
    });
  });
  drop.addEventListener('drop', function (e) {
    if (e.dataTransfer && e.dataTransfer.files) uploadAll(e.dataTransfer.files);
  });
}

// =============================================================== SETTINGS

// Flat dotted keys, every one at most fifteen characters so it fits an NVS
// key. `reboot` marks the ones the firmware cannot apply live.
var GROUPS = [
  { title: 'Network', fields: [
    { key: 'wifi.ssid', label: 'WiFi SSID', type: 'text', max: 32, reboot: true },
    { key: 'wifi.psk', label: 'WiFi password', type: 'password', max: 63, reboot: true,
      write: true, hint: 'Write only. The board never sends it back.' },
    { key: 'net.host', label: 'Board name', type: 'text', max: 24, reboot: true,
      hint: 'Also the mDNS name: <name>.local' }
  ]},
  { title: 'Megabrain', fields: [
    { key: 'brain.host', label: 'Host or IP', type: 'text', max: 47,
      hint: 'Host only. The board builds /ask itself.' },
    { key: 'brain.port', label: 'Port', type: 'number', min: 1, max: 65535 },
    { key: 'brain.model', label: 'Model', type: 'select', options: 'models', free: true, max: 31 },
    { key: 'brain.max', label: 'Max tokens', type: 'number', min: 16, max: 2048 },
    { key: 'brain.system', label: 'System prompt', type: 'area', max: 223 }
  ]},
  { title: 'Appearance', fields: [
    { key: 'ui.theme', label: 'Theme', type: 'select', options: 'themes' },
    { key: 'ui.bright', label: 'Brightness', type: 'range', min: 0, max: 255, live: true }
  ]},
  { title: 'System', fields: [
    { key: 'sys.tz', label: 'Timezone', type: 'text', max: 47,
      hint: 'POSIX TZ, e.g. PST8PDT,M3.2.0,M11.1.0' },
    { key: 'sys.autostart', label: 'Autostart app', type: 'select', options: 'apps' }
  ]}
];

var Inputs = {};

function optionsFor(src) {
  if (src === 'themes') {
    return S.themes.map(function (t) { return { v: t.name, l: t.name }; });
  }
  if (src === 'apps') {
    return [{ v: '', l: 'none' }].concat(S.apps.map(function (a) {
      return { v: a.id, l: a.name || a.id };
    }));
  }
  if (src === 'models') {
    return S.models.map(function (m) { return { v: m, l: m }; });
  }
  return [];
}

function buildSettings() {
  var host = $('s-groups');
  host.textContent = '';
  Inputs = {};

  GROUPS.forEach(function (g) {
    var card = el('div', 'card');
    var h = el('h2', null, g.title);
    var dirty = el('span', 'right dirty');
    dirty.hidden = true;
    dirty.textContent = 'unsaved';
    h.appendChild(dirty);
    card.appendChild(h);

    g.fields.forEach(function (f) {
      var lab = el('label', f.type === 'range' ? 'inline' : null);
      lab.appendChild(document.createTextNode(f.label + ' '));
      if (f.reboot) lab.appendChild(el('span', 'rb', '(reboot)'));

      var inp;
      if (f.type === 'select') {
        inp = el('select');
      } else if (f.type === 'area') {
        inp = el('textarea');
        inp.rows = 3;
      } else {
        inp = el('input');
        inp.type = f.type === 'password' ? 'password' : (f.type === 'number' ? 'number' : f.type);
        if (f.type === 'password') inp.autocomplete = 'new-password';
      }
      if (f.max != null && (f.type === 'text' || f.type === 'password' || f.type === 'area')) {
        inp.maxLength = f.max;
      }
      if (f.min != null) inp.min = f.min;
      if (f.max != null && (f.type === 'number' || f.type === 'range')) inp.max = f.max;

      var val = null;
      if (f.type === 'range') {
        val = el('span', 'val', '-');
        lab.appendChild(inp);
        lab.appendChild(val);
      } else {
        lab.appendChild(inp);
      }
      card.appendChild(lab);
      if (f.hint) card.appendChild(el('p', 'hint', f.hint));

      Inputs[f.key] = { f: f, inp: inp, val: val, dirty: dirty };

      var touched = function () {
        if (f.type === 'range' && val) val.textContent = inp.value;
        dirty.hidden = !groupDirty(g);
      };
      inp.addEventListener('input', touched);
      inp.addEventListener('change', function () {
        touched();
        if (f.live) saveKeys([f.key]);
      });
    });

    var bar = el('div', 'toolbar end');
    var save = el('button', 'primary', 'Save');
    save.type = 'button';
    save.onclick = function () {
      saveKeys(g.fields.map(function (f) { return f.key; })).then(function () {
        dirty.hidden = !groupDirty(g);
      });
    };
    var undo = el('button', null, 'Revert');
    undo.type = 'button';
    undo.onclick = function () { fillSettings(); dirty.hidden = true; };
    bar.appendChild(undo);
    bar.appendChild(save);
    card.appendChild(bar);
    host.appendChild(card);
  });
}

function groupDirty(g) {
  for (var i = 0; i < g.fields.length; i++) {
    if (keyDirty(g.fields[i].key)) return true;
  }
  return false;
}
function keyDirty(k) {
  var it = Inputs[k];
  if (!it) return false;
  if (it.f.write) return it.inp.value !== '';
  var cur = S.settings[k];
  return String(cur == null ? '' : cur) !== String(it.inp.value);
}

function fillSettings() {
  Object.keys(Inputs).forEach(function (k) {
    var it = Inputs[k], f = it.f;
    if (f.type === 'select') {
      var opts = optionsFor(f.options);
      var cur = S.settings[k];
      it.inp.textContent = '';
      var seen = false;
      opts.forEach(function (o) {
        var op = el('option', null, o.l);
        op.value = o.v;
        if (String(o.v) === String(cur)) seen = true;
        it.inp.appendChild(op);
      });
      // A model or theme the board is using but no longer offers must still
      // show, or Save would silently change it.
      if (!seen && cur != null && cur !== '') {
        var op2 = el('option', null, cur + ' (not listed)');
        op2.value = cur;
        it.inp.appendChild(op2);
      }
      it.inp.value = cur == null ? '' : cur;
    } else if (f.write) {
      it.inp.value = '';
      it.inp.placeholder = S.settings[k + '_set'] ? 'set - leave blank to keep' : 'not set';
    } else {
      it.inp.value = S.settings[k] == null ? '' : S.settings[k];
      if (f.type === 'range' && it.val) it.val.textContent = it.inp.value;
    }
  });
  GROUPS.forEach(function (g) {
    var d = Inputs[g.fields[0].key];
    if (d) d.dirty.hidden = !groupDirty(g);
  });
}

function saveKeys(keys) {
  var patch = {}, n = 0;
  keys.forEach(function (k) {
    if (!keyDirty(k)) return;
    var it = Inputs[k];
    var v = it.inp.value;
    if (it.f.type === 'number' || it.f.type === 'range') v = Number(v);
    patch[k] = v;
    n++;
  });
  if (!n) { toast('nothing changed'); return Promise.resolve(); }

  return api('/api/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(patch),
    timeout: 12000
  }).then(function (d) {
    if (d && d.settings) S.settings = d.settings;
    else Object.keys(patch).forEach(function (k) { S.settings[k] = patch[k]; });
    fillSettings();
    if (patch['ui.theme']) loadThemes();
    var rb = (d && d.reboot_required) || [];
    if (rb.length) toast('saved - reboot to apply: ' + rb.join(', '), 'ok');
    else toast('saved', 'ok');
  }, function (e) { fail(e, 'save'); });
}

function loadSettings() {
  return api('/api/settings').then(function (d) {
    S.settings = (d && d.settings) || d || {};
    fillSettings();
  }, function (e) {
    toast('settings unavailable: ' + e.message, 'err');
  });
}

function renderInfo() {
  var s = S.sys, dl = $('s-info');
  dl.textContent = '';
  if (!s) {
    dl.appendChild(el('dd', 'note', 'board not answering'));
    return;
  }
  var head = function (t) {
    var h = el('dt', 'head', t);
    dl.appendChild(h);
  };
  var row = function (k, v, bad) {
    dl.appendChild(el('dt', null, k));
    dl.appendChild(el('dd', bad ? 'bad' : null, v == null ? '-' : String(v)));
  };

  if (s.board) {
    head('profile');
    row('id', s.board.id);
    row('name', s.board.name);
  }
  if (s.chip) {
    head('chip');
    row('target', s.chip.target);
    row('variant', s.chip.variant);
    row('cores', s.chip.cores);
    row('flash', s.chip.flash_mb + ' MB');
    row('psram', s.chip.psram && s.chip.psram.present
      ? (s.chip.psram.size_mb + ' MB ' + s.chip.psram.type) : 'none');
    row('mac', s.chip.mac);
  }
  if (s.render) {
    head('render');
    row('tier', s.render.tier);
    row('compositor', s.render.compositor);
    row('lvgl', s.render.lvgl ? 'yes' : 'no');
  }
  if (s.display) {
    head('display');
    row('controller', s.display.controller);
    row('size', s.display.w + ' x ' + s.display.h);
    row('bus', s.display.bus);
    row('clock', s.display.clock_hz ? (s.display.clock_hz / 1e6) + ' MHz' : null);
  }
  if (s.heap) {
    head('memory');
    row('free heap', bytes(s.heap.free), s.heap.free < 12000);
    row('min free', bytes(s.heap.min_free));
    row('largest block', bytes(s.heap.largest_block));
  }
  if (s.net) {
    head('network');
    row('ssid', s.net.ssid);
    row('ip', s.net.ip);
    row('mdns', s.net.mdns);
    row('rssi', s.net.rssi != null ? s.net.rssi + ' dBm' : null);
  }
  if (s.fs && s.fs.length) {
    head('storage');
    s.fs.forEach(function (m) {
      row(m.point, m.mounted
        ? (bytes(m.used) + ' of ' + bytes(m.total) + ' used' + (m.writable ? '' : ' (read only)'))
        : 'not mounted', !m.mounted);
    });
  }
  head('runtime');
  row('uptime', dur(s.uptime_ms));
  if (s.fw) { row('firmware', s.fw.version); row('idf', s.fw.idf); }
  if (s.time) row('clock', s.time.synced ? 'synced' : 'not synced');
}

function bindSettings() {
  $('s-reboot').onclick = function () {
    confirmBox('Reboot', 'The board drops off the network for a few seconds.')
      .then(function (ok) {
        if (!ok) return;
        api('/api/system/reboot', { method: 'POST', timeout: 4000 })
          .catch(function () {})
          .then(function () {
            toast('rebooting');
            markDown('board rebooting');
          });
      });
  };
}

// ================================================================== BUDDY

var B = { ed: null, meta: null, loaded: false, centred: false };

function buddyInit() {
  if (B.ed) return;
  B.ed = new window.EOSVox.Editor({
    canvas: $('vx-canvas'),
    preview: $('vx-preview'),
    onchange: buddySync
  });

  buildPal();
  buddySync();

  var tools = $('vx-tools');
  Array.prototype.forEach.call(tools.children, function (b) {
    b.onclick = function () {
      Array.prototype.forEach.call(tools.children, function (o) { o.classList.remove('on'); });
      b.classList.add('on');
      B.ed.setTool(b.dataset.tool);
    };
  });

  var tog = function (id, fn) {
    $(id).onclick = function () {
      var on = !$(id).classList.contains('on');
      $(id).classList.toggle('on', on);
      fn(on);
    };
  };
  tog('vx-orbit', function (v) { B.ed.setOrbit(v); });
  tog('vx-slice', function (v) { B.ed.setSlice(v); });
  tog('vx-mirror', function (v) { B.ed.setMirror(v); });
  $('vx-reset').onclick = function () { B.ed.recentre(); };

  $('vx-undo').onclick = function () { if (!B.ed.undo()) toast('nothing to undo'); };
  $('vx-redo').onclick = function () { if (!B.ed.redo()) toast('nothing to redo'); };
  $('vx-clear').onclick = function () {
    confirmBox('Clear model', 'Removes every voxel. Undo still works.')
      .then(function (ok) { if (ok) B.ed.clear(); });
  };

  $('vx-zslider').oninput = function (e) { B.ed.setZ(+e.target.value); };

  $('vx-dim').onchange = function (e) {
    var dropped = B.ed.setDim(+e.target.value);
    syncDim();
    if (dropped > 0) toast(dropped + ' voxels fell outside the new grid', 'err');
  };

  $('vx-add').onclick = function () {
    var i = B.ed.addCustom($('vx-custom').value);
    if (!i) return toast('all 47 custom slots are in use by voxels', 'err');
    B.ed.setColor(i);
    buildPal();
  };
  $('vx-custom').onchange = function () { $('vx-add').click(); };

  $('b-yaw').oninput = function (e) {
    $('b-yawtxt').textContent = e.target.value + ' / 32';
  };
  $('b-eyeclear').onclick = function () {
    B.ed.eyeIndex = 0;
    B.ed.blinkIndex = 0;
    B.ed.dirty = true;
    buddySync();
  };
  $('b-eyesw').onclick = function () {
    B.ed.eyeIndex = B.ed.color;
    B.ed.dirty = true;
    buddySync();
  };
  $('b-blinksw').onclick = function () {
    B.ed.blinkIndex = B.ed.color;
    buddySync();
  };

  $('b-load').onclick = buddyLoad;
  $('b-save').onclick = buddySave;
  $('b-export').onclick = buddyExport;
  $('b-import').onclick = function () { $('b-importfile').click(); };
  $('b-importfile').onchange = function (e) {
    var f = e.target.files[0];
    e.target.value = '';
    if (!f) return;
    f.arrayBuffer().then(function (ab) {
      try {
        B.ed.fromVox(new Uint8Array(ab));
        buildPal();
        syncDim();
        toast('imported ' + f.name, 'ok');
      } catch (err) {
        toast('bad .vox: ' + err.message, 'err');
      }
    });
  };

  window.addEventListener('keydown', function (e) {
    if (S.tab !== 'buddy') return;
    var t = e.target.tagName;
    if (t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT') return;
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') {
      e.preventDefault();
      if (e.shiftKey) B.ed.redo(); else B.ed.undo();
    }
  });
}

function buildPal() {
  var host = $('vx-pal'), ed = B.ed;
  host.textContent = '';
  var used = ed.usedIndices();
  var CB = window.EOSVox.CUSTOM_BASE;
  var add = function (i) {
    var s = el('i');
    s.style.background = ed.colorHex(i);
    s.title = 'index ' + i;
    s.dataset.i = i;
    if (i === ed.color) s.className = 'on';
    if (used[i]) s.classList.add('used');
    if (i >= CB && !ed.customUsed[i]) s.classList.add('free');
    s.onclick = function () {
      ed.setColor(i);
      buildPal();
    };
    host.appendChild(s);
  };
  // Row major so the 13-column grid reads as grey plus twelve hue columns.
  for (var row = 0; row < 16; row++) {
    for (var col = 0; col < 13; col++) add(1 + col * 16 + row);
  }
  for (var i = CB; i < 256; i++) add(i);
}

function syncDim() {
  var d = B.ed.dim();
  var sel = $('vx-dim');
  var has = false;
  Array.prototype.forEach.call(sel.options, function (o) {
    if (+o.value === d) has = true;
  });
  if (!has) {
    var o = el('option', null, d + ' cubed');
    o.value = d;
    sel.appendChild(o);
  }
  sel.value = d;
  var z = $('vx-zslider');
  z.max = d - 1;
  if (+z.value > d - 1) z.value = d - 1;
}

function buddySync() {
  var ed = B.ed;
  if (!ed) return;
  var n = ed.count(), max = window.EOSVox.MAX_VOX;
  var c = $('vx-count');
  c.textContent = n + ' / ' + max + ' voxels';
  c.className = n >= max ? 'over' : '';
  $('vx-dims').textContent = ed.dim() + ' x ' + ed.dim() + ' x ' + ed.dim();
  $('vx-cur').style.background = ed.colorHex(ed.color);
  $('vx-curtxt').textContent = 'index ' + ed.color;
  $('vx-zlabel').textContent = 'z ' + ed.zLayer;
  if (+$('vx-zslider').value !== ed.zLayer) $('vx-zslider').value = ed.zLayer;

  $('b-eyetxt').textContent = ed.eyeIndex ? String(ed.eyeIndex) : 'none';
  $('b-eyesw').style.background = ed.eyeIndex ? ed.colorHex(ed.eyeIndex) : 'transparent';
  $('b-blinktxt').textContent = ed.blinkIndex ? String(ed.blinkIndex) : 'none';
  $('b-blinksw').style.background = ed.blinkIndex ? ed.colorHex(ed.blinkIndex) : 'transparent';
}

function metaFromForm() {
  var ed = B.ed;
  return {
    schema_version: 1,
    name: $('b-name').value.trim() || 'buddy',
    personality: $('b-person').value.trim(),
    accent: $('b-accent').value,
    idle: {
      behaviour: $('b-idle').value,
      sleep_ms: +$('b-sleep').value,
      home_yaw: +$('b-yaw').value
    },
    eyes: {
      open_index: ed.eyeIndex | 0,
      shut_index: ed.blinkIndex | 0
    },
    model: {
      file: 'buddy.vox',
      dim: [ed.dim(), ed.dim(), ed.dim()],
      voxels: ed.count()
    }
  };
}

function metaToForm(m) {
  if (!m) return;
  $('b-name').value = m.name || '';
  $('b-person').value = m.personality || '';
  if (m.accent) $('b-accent').value = m.accent;
  var idle = m.idle || {};
  if (idle.behaviour) $('b-idle').value = idle.behaviour;
  if (idle.sleep_ms != null) $('b-sleep').value = String(idle.sleep_ms);
  if (idle.home_yaw != null) {
    $('b-yaw').value = idle.home_yaw;
    $('b-yawtxt').textContent = idle.home_yaw + ' / 32';
  }
  var eyes = m.eyes || {};
  B.ed.eyeIndex = eyes.open_index | 0;
  B.ed.blinkIndex = eyes.shut_index | 0;
  buddySync();
}

function bstatus(msg, bad) {
  var p = $('b-status');
  p.textContent = msg || '';
  p.style.color = bad ? 'var(--err)' : '';
}

function buddyLoad() {
  bstatus('loading...');
  var meta = api('/api/buddy', { timeout: 10000 }).catch(function () { return null; });
  var vox = apiRaw('/api/fs/read' + qp({ path: '/sd/buddy/buddy.vox' }), { timeout: 20000 })
    .then(function (r) { return r.ok ? r.arrayBuffer() : null; })
    .catch(function () { return null; });

  return Promise.all([meta, vox]).then(function (res) {
    var m = res[0], ab = res[1];
    if (ab) {
      try {
        B.ed.fromVox(new Uint8Array(ab));
        buildPal();
        syncDim();
      } catch (e) {
        bstatus('buddy.vox is not readable: ' + e.message, true);
        return;
      }
    }
    if (m) metaToForm(m.buddy || m);
    B.loaded = true;
    bstatus(ab ? 'loaded from the board' : 'no buddy.vox on the card yet');
  }, function (e) {
    bstatus('load failed: ' + e.message, true);
  });
}

function buddySave() {
  var ed = B.ed;
  if (!ed.count()) return toast('model is empty', 'err');
  var vox;
  try {
    vox = ed.toVox();
  } catch (e) {
    return toast(e.message, 'err');
  }
  var meta = metaFromForm();
  bstatus('saving ' + bytes(vox.length) + ' ...');
  $('b-save').disabled = true;

  // mkdir is allowed to fail: it usually fails because it is already there.
  var rows = [];
  return api('/api/fs/mkdir' + qp({ path: '/sd/buddy' }), { method: 'POST' })
    .catch(function () { return null; })
    .then(function () {
      var ui = uploadRow('buddy.vox');
      rows.push(ui);
      return putChunks('/sd/buddy/buddy.vox', new Blob([vox]), ui);
    })
    .then(function () {
      var ui = uploadRow('buddy.json');
      rows.push(ui);
      var j = new Blob([JSON.stringify(meta, null, 2)], { type: 'application/json' });
      return putChunks('/sd/buddy/buddy.json', j, ui);
    })
    .then(function () {
      return api('/api/buddy/reload', { method: 'POST', timeout: 12000 })
        .catch(function () { return null; });
    })
    .then(function () {
      bstatus('saved to /sd/buddy - the board reloaded it');
      toast('buddy saved', 'ok');
    }, function (e) {
      bstatus('save failed: ' + e.message, true);
      toast('save failed', 'err');
    })
    .then(function () {
      $('b-save').disabled = false;
      rows.forEach(function (r) { r.row.remove(); });
    });
}

function buddyExport() {
  var vox;
  try {
    vox = B.ed.toVox();
  } catch (e) {
    return toast(e.message, 'err');
  }
  var name = ($('b-name').value.trim() || 'buddy') + '.vox';
  var url = URL.createObjectURL(new Blob([vox], { type: 'application/octet-stream' }));
  var a = document.createElement('a');
  a.href = url;
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(function () { URL.revokeObjectURL(url); }, 4000);
}

// ================================================================ CONSOLE

var C = { since: 0, hist: [], hi: 0, follow: true, poller: null };

function conLine(text, cls) {
  var out = $('c-out');
  var span = el('span', cls, text + '\n');
  out.appendChild(span);
  while (out.childNodes.length > 500) out.removeChild(out.firstChild);
  if (C.follow) out.scrollTop = out.scrollHeight;
}

function pollConsole() {
  return api('/api/console/log' + qp({ since: C.since, max: 4096 }), { timeout: 8000 })
    .then(function (d) {
      if (!d) return;
      if (d.dropped) conLine('[' + d.dropped + ' lines dropped]', 'i');
      (d.lines || []).forEach(function (l) {
        if (typeof l === 'string') conLine(l);
        else conLine(l.text, l.level === 'E' ? 'e' : l.level === 'W' ? 'w' : null);
      });
      if (d.next != null) C.since = d.next;
    });
}

function runCmd(cmd) {
  conLine('> ' + cmd, 'cmd');
  C.hist.push(cmd);
  if (C.hist.length > 60) C.hist.shift();
  C.hi = C.hist.length;
  api('/api/console/exec', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ cmd: cmd }),
    timeout: 12000
  }).then(function () {
    if (C.poller) C.poller.kick();
  }, function (e) {
    conLine('! ' + e.message, 'e');
  });
}

function bindConsole() {
  $('c-form').onsubmit = function (e) {
    e.preventDefault();
    var v = $('c-in').value.trim();
    if (!v) return;
    $('c-in').value = '';
    runCmd(v);
  };
  $('c-in').addEventListener('keydown', function (e) {
    if (e.key === 'ArrowUp') {
      if (!C.hist.length) return;
      C.hi = Math.max(0, C.hi - 1);
      $('c-in').value = C.hist[C.hi] || '';
      e.preventDefault();
    } else if (e.key === 'ArrowDown') {
      C.hi = Math.min(C.hist.length, C.hi + 1);
      $('c-in').value = C.hist[C.hi] || '';
      e.preventDefault();
    }
  });
  $('c-clear').onclick = function () { $('c-out').textContent = ''; };
  $('c-follow').onclick = function () {
    C.follow = !C.follow;
    $('c-follow').classList.toggle('on', C.follow);
  };
  C.poller = new Poller(1500, pollConsole);
}

// --------------------------------------------------------------- megabrain

var M = { abort: null, node: null };

function chatMsg(who, cls) {
  var d = el('div', 'msg' + (cls ? ' ' + cls : ''));
  d.appendChild(el('div', 'who', who));
  var t = el('div', 'txt');
  d.appendChild(t);
  $('m-out').appendChild(d);
  $('m-out').scrollTop = $('m-out').scrollHeight;
  return t;
}

function brainStatus() {
  return api('/api/brain/status', { timeout: 8000 }).then(function (d) {
    if (!d) return;
    S.models = d.models || [];
    var sel = $('m-model');
    var cur = sel.value || d.model;
    sel.textContent = '';
    var list = S.models.length ? S.models : (d.model ? [d.model] : []);
    list.forEach(function (m) {
      var o = el('option', null, m);
      o.value = m;
      sel.appendChild(o);
    });
    if (cur) sel.value = cur;
    $('m-host').textContent = (d.host || '?') + (d.port && d.port !== 80 ? ':' + d.port : '') +
      (d.reachable === false ? ' (unreachable)' : '');
    if (Inputs['brain.model']) fillSettings();
  }, function () {
    $('m-host').textContent = 'status unavailable';
  });
}

function brainAsk(q) {
  if (M.abort) return toast('a request is already in flight', 'err');
  chatMsg('you', 'you').textContent = q;
  var out = chatMsg('megabrain');
  out.classList.add('run');
  M.node = out;

  var ctl = new AbortController();
  M.abort = ctl;
  $('m-stop').disabled = false;
  $('m-send').disabled = true;

  var body = {
    q: q,
    model: $('m-model').value || undefined,
    max: +$('m-max').value || 256
  };

  // The stream deadline is long because a slow model legitimately takes a
  // while to produce its first token. Everything else on this page uses the
  // short one.
  apiRaw('/api/brain/ask', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    timeout: 120000,
    signal: ctl.signal
  }).then(function (r) {
    if (!r.ok) {
      return r.text().catch(function () { return ''; }).then(function (t) {
        throw new Error(t || ('http ' + r.status));
      });
    }
    markUp();
    if (!r.body || !r.body.getReader) {
      // No streams: take the whole answer at the end. Slower to appear but
      // identical once it lands.
      return r.text().then(function (t) { out.textContent = t; });
    }
    var reader = r.body.getReader();
    var dec = new TextDecoder();
    var pump = function () {
      return reader.read().then(function (res) {
        if (res.done) {
          out.textContent += dec.decode();
          return;
        }
        // eos_brain holds back a partial UTF-8 sequence, so every flush the
        // board sends is whole characters. Streaming decode is still used
        // here in case a proxy re-splits them.
        out.textContent += dec.decode(res.value, { stream: true });
        $('m-out').scrollTop = $('m-out').scrollHeight;
        return pump();
      });
    };
    return pump();
  }).then(function () {
    out.classList.remove('run');
  }, function (e) {
    out.classList.remove('run');
    if (e.name === 'AbortError') {
      if (!out.textContent) out.textContent = '(cancelled)';
    } else {
      out.parentNode.classList.add('err');
      out.textContent = (out.textContent ? out.textContent + '\n' : '') + '! ' + e.message;
    }
  }).then(function () {
    M.abort = null;
    M.node = null;
    $('m-stop').disabled = true;
    $('m-send').disabled = false;
  });
}

function bindBrain() {
  $('m-form').onsubmit = function (e) {
    e.preventDefault();
    var v = $('m-in').value.trim();
    if (!v) return;
    $('m-in').value = '';
    brainAsk(v);
  };
  $('m-in').addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      $('m-form').requestSubmit();
    }
  });
  $('m-stop').onclick = function () {
    if (M.abort) M.abort.abort();
    api('/api/brain/cancel', { method: 'POST', timeout: 5000 }).catch(function () {});
  };
}

// ==================================================================== app

var sysPoller = new Poller(5000, pollSystem);

function showTab(name) {
  S.tab = name;
  ['files', 'settings', 'buddy', 'console'].forEach(function (t) {
    $('p-' + t).hidden = t !== name;
  });
  Array.prototype.forEach.call($('tabs').children, function (b) {
    var on = b.dataset.tab === name;
    b.classList.toggle('on', on);
    b.setAttribute('aria-selected', on ? 'true' : 'false');
  });

  if (name === 'buddy') {
    buddyInit();
    syncDim();
    // The canvas measures zero until its panel is shown, so the constructor's
    // fit was against a fallback. Redo it once, now that it has a real size.
    if (!B.centred) { B.centred = true; B.ed.recentre(); }
    B.ed.start();
    if (!B.loaded) { B.loaded = true; buddyLoad(); }
  } else if (B.ed) {
    B.ed.stop();
  }

  if (name === 'console') {
    C.poller.start();
    brainStatus();
  } else if (C.poller) {
    C.poller.stop();
  }

  if (name === 'settings') {
    renderInfo();
    loadSettings();
  }
  if (name === 'files' && !F.entries.length) listFiles();

  try { location.hash = name; } catch (e) { /* file:// with no hash support */ }
}

function bindTabs() {
  Array.prototype.forEach.call($('tabs').children, function (b) {
    b.onclick = function () { showTab(b.dataset.tab); };
  });
  window.addEventListener('hashchange', function () {
    var h = location.hash.replace('#', '');
    if (h && h !== S.tab && $('p-' + h)) showTab(h);
  });
}

function boot() {
  bindTabs();
  bindFiles();
  bindSettings();
  bindConsole();
  bindBrain();
  buildSettings();

  $('retrynow').onclick = function () {
    Link.fails = 0;
    $('offline').hidden = true;
    sysPoller.kick();
    if (S.tab === 'files') go(F.path);
  };

  $('linkdot').className = 'dot busy';

  // Themes and the app list first: both feed the settings selects, and both
  // are small enough that doing them up front costs nothing.
  loadThemes()
    .then(function () {
      return api('/api/apps').then(function (d) {
        S.apps = (d && d.apps) || [];
      }, function () { S.apps = []; });
    })
    .then(function () {
      // The model list feeds the Settings select as well as the chat box, so
      // it has to be here before Settings is first drawn - otherwise the
      // saved model shows as "not listed" against an empty list.
      return brainStatus().catch(function () { return null; });
    })
    .then(function () {
      sysPoller.start();
      var h = location.hash.replace('#', '');
      showTab(h && $('p-' + h) ? h : 'files');
    });

  // A hidden tab costs the board nothing.
  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      sysPoller.stop();
      if (C.poller) C.poller.stop();
      if (B.ed) B.ed.stop();
    } else {
      sysPoller.start();
      if (S.tab === 'console' && C.poller) C.poller.start();
      if (S.tab === 'buddy' && B.ed) B.ed.start();
    }
  });
}

if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', boot);
} else {
  boot();
}

})();
