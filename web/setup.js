/* setup — the screen a phone lands on after joining the board's own access
 * point, and the only way a board that has never heard of your network gets
 * onto it. The same file runs from Settings on a board that is already up,
 * because pairing a Bluetooth keyboard needs the same radio etiquette and the
 * same six-digit passkey panel as first boot does.
 *
 * It replaces the page instead of adding a fifth tab. A board in SETUP has no
 * network, so Files, Console and Buddy have nothing to show, and a wizard
 * sharing a screen with four tabs is a wizard people get lost in.
 *
 * The non-obvious constraint, and the reason for every retry below: the board
 * has one radio and the access point serving this page is running on it.
 * Scanning retunes that radio, and joining a network takes it away outright.
 * So the request that starts a join is the request most likely to die, and its
 * response can never be the only record of what happened — every outcome is
 * recovered afterwards by polling /api/net/status. A request that fails while
 * the radio is busy is reported as a blink, out loud, and never as a dead
 * board.
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
function show(id, on) { var e = $(id); if (e) e.hidden = !on; }
function delay(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

// NVS and esp_wifi count bytes, not JavaScript characters, and an accented
// letter in an SSID is two of them.
function blen(s) {
  return encodeURIComponent(String(s)).replace(/%[0-9A-F]{2}/gi, 'x').length;
}

// The HTML parser namespaces an <svg> written as markup, so this file needs no
// XML namespace URL of its own. Only ever called with the literals below.
function icon(id) {
  var s = el('span', 'rb-ic');
  s.innerHTML = '<svg viewBox="0 0 16 16"><use href="#' + id + '"></use></svg>';
  return s;
}

function pick(o, keys) {
  if (!o) return null;
  for (var i = 0; i < keys.length; i++) if (o[keys[i]] != null) return o[keys[i]];
  return null;
}
function listOf(d, keys) {
  if (Array.isArray(d)) return d;
  if (!d) return [];
  for (var i = 0; i < keys.length; i++) if (Array.isArray(d[keys[i]])) return d[keys[i]];
  return [];
}
function mkerr(code, msg) { var e = new Error(msg); e.code = code; return e; }

// fetch()'s own wording for a dead socket is "Failed to fetch", which tells a
// person nothing about a board that just moved its radio.
function why(e) {
  if (!e) return 'it failed';
  if (e.code === 'timeout') return 'the board did not answer in time';
  if (e.code === 'network') return 'the connection to the board dropped and did not come back';
  return e.message;
}

// ----------------------------------------------------------------- state

var H = null;            // the seam back into app.js, filled by attach()
var live = false;        // is the setup screen on screen
var bound = false;

var S = {
  manual: false,         // opened from Settings rather than by a board in SETUP
  step: 'board',
  done: {},
  sys: null,             // /api/system, for board identity
  net: null,             // last /api/net/status
  host: null,            // hostname the board reports, for the final address
  apname: null,          // the AP's own SSID, kept because a joined board
                         // stops reporting it and the last screen still needs it
  nets: [], scanned: 0,
  target: null,          // the network being joined
  timer: null, t0: 0,
  devs: [], pairdev: null,
  bonded: null,
  watching: null         // token that cancels an in-flight poll loop
};

// The radio is single. Everything that touches it queues behind this, and the
// controls for the other radio user go dead while it is held rather than
// staying live and silently doing nothing.
var radio = { held: null, what: '' };

function withRadio(what, fn) {
  if (radio.held) {
    return Promise.reject(mkerr('radio_busy',
      'the board is busy with ' + radio.what + ', and it has one radio'));
  }
  radio.what = what;
  radio.held = Promise.resolve().then(fn).then(
    function (v) { radio.held = null; radioUi(); return v; },
    function (e) { radio.held = null; radioUi(); throw e; });
  radioUi();
  return radio.held;
}
function radioUi() {
  var b = !!radio.held;
  ['su-rescan', 'su-blescan', 'su-join', 'su-bleok'].forEach(function (id) {
    var e = $(id);
    if (e) e.disabled = b;
  });
}

// A GET that expects to be interrupted. Anything that is not a definite 4xx is
// retried, and every retry says so on the line the person is watching, because
// a page that goes quiet for eight seconds reads as broken.
function tryGet(path, opt, say) {
  opt = opt || {};
  var max = opt.tries || 5, n = 0;
  function once() {
    n++;
    return H.api(path, opt).catch(function (e) {
      var fatal = e.status >= 400 && e.status < 500 && e.code !== 'busy';
      if (n >= max || fatal) throw e;
      if (say) say('the link dropped while the radio moved — retrying (' + n + ' of ' + (max - 1) + ')');
      return delay(Math.min(4000, 700 * n)).then(once);
    });
  }
  return once();
}

// --------------------------------------------------------------- the shell

var STEPS = ['board', 'wifi', 'ble', 'done'];

function goStep(n) {
  S.step = n;
  STEPS.forEach(function (k) {
    var e = $('st-' + k);
    if (!e) return;
    e.classList.toggle('on', k === n);
    e.classList.toggle('ok', !!S.done[k]);
  });
  var t = $('st-' + n);
  if (!t) return;
  try { t.scrollIntoView({ block: 'start', behavior: 'smooth' }); }
  catch (e) { t.scrollIntoView(); }
}

function summary(step, txt, ok) {
  var e = $('su-' + step + 'sum');
  if (e) e.textContent = txt;
  if (ok != null) S.done[step] = ok;
}

function openScreen(reason, st) {
  if (live) { goStep(S.step); return; }
  live = true;
  S.manual = reason === 'manual';
  if (st) S.net = st;
  H.pause();
  H.quiet(true);
  H.show(true);
  bind();

  show('su-exit', S.manual);
  $('su-sub').textContent = S.manual
    ? 'Changing the network takes the board off the one you are reading this over. Read each step before committing to it.'
    : 'You are on this board’s own access point. It has no way onto your network until you give it one.';
  $('su-finish').textContent = S.manual ? 'Back to the app' : 'Open the board';

  goStep('board');
  loadBoard().then(function () {
    // A board that was already mid-join when the page loaded: pick the watch up
    // where it left off rather than starting a second join over the top of it.
    var j = S.net && S.net.join;
    if (j && String(j.state).toLowerCase() === 'trying') {
      var ssid = j.ssid || (S.net && S.net.ssid) || 'the network';
      S.target = { ssid: ssid, auth: null };
      goStep('wifi');
      trying(ssid, 'A join was already running when this page loaded. Watching it.');
      S.watching = {};
      return watchJoin(ssid, 45000, S.watching);
    }
    // The doc's rule: scan once when setup starts and cache it. Doing it here,
    // while the person is still reading step 1, spends the disruption at the
    // moment it costs least.
    return wifiScan();
  });
}

function closeScreen() {
  if (!live) return;
  live = false;
  S.watching = null;
  stopTick();
  H.show(false);
  H.quiet(false);
  H.resume();
}

// ================================================================== BOARD
//
// Step one answers one question: is this the board in front of me? Everything
// here is identity. Nothing here is a setting.

function loadBoard() {
  return H.api('/api/system', { timeout: 8000 }).then(function (d) {
    S.sys = d;
    drawBoard(d);
    return d;
  }, function () {
    S.sys = null;
    drawBoard(null);
    return null;
  });
}

function row(dl, k, v) {
  if (v == null || v === '') return;
  dl.appendChild(el('dt', null, k));
  dl.appendChild(el('dd', null, String(v)));
}

function drawBoard(d) {
  var dl = $('su-boardinfo');
  dl.textContent = '';

  var ap = (S.net && S.net.ap) || null;
  var apssid = (ap && (ap.ssid || ap.name)) || S.apname;
  if (apssid) {
    S.apname = apssid;
    var p = $('su-apline');
    p.textContent = '';
    p.appendChild(document.createTextNode('You joined '));
    p.appendChild(el('b', null, apssid));
    p.appendChild(document.createTextNode('. The board prints that same name on its panel.'));
    p.hidden = false;
  } else {
    show('su-apline', false);
  }

  if (!d) {
    dl.appendChild(el('dd', 'note',
      'The board did not answer /api/system, so it cannot say what it is. The rest ' +
      'of setup still works.'));
    summary('board', 'not reported', true);
    S.host = (S.net && (S.net.hostname || S.net.host)) || S.host;
    return;
  }

  var b = d.board || {}, c = d.chip || {}, dp = d.display || {};
  var fw = d.fw || {}, nt = d.net || {};
  S.host = nt.hostname || nt.host || S.host;

  row(dl, 'name', S.host || '-');
  row(dl, 'board', b.name || b.id);
  if (b.summary) row(dl, 'panel', b.summary);

  var chip = [];
  if (c.target) chip.push(c.target);
  if (c.cores) chip.push(c.cores + (c.cores === 1 ? ' core' : ' cores'));
  if (c.rev != null) chip.push('rev ' + c.rev);
  if (c.flash_mb) chip.push(c.flash_mb + ' MB flash');
  if (c.psram) chip.push(c.psram.present ? (c.psram.size_mb + ' MB PSRAM') : 'no PSRAM');
  row(dl, 'chip', chip.join(' · '));

  if (dp.controller) {
    row(dl, 'display', dp.controller + ' ' + (dp.w || '?') + 'x' + (dp.h || '?') +
        (dp.clock_hz ? ' @ ' + Math.round(dp.clock_hz / 1000000) + ' MHz' : ''));
  }
  row(dl, 'mac', c.mac);
  row(dl, 'firmware', fw.version ? (fw.version + (fw.idf ? ' · idf ' + fw.idf : '')) : null);
  if (d.heap) row(dl, 'free heap', d.heap.free + ' B');

  var mac = c.mac ? c.mac.replace(/:/g, '').slice(-4) : null;
  summary('board', (b.id || S.host || 'board') + (mac ? ' · ' + mac : ''), true);

  var bn = $('boardname');
  if (bn && S.host) bn.textContent = S.host;
}

// =================================================================== WIFI

// esp_wifi's wifi_auth_mode_t in order, so a board that passes the raw enum
// through is readable here without a firmware change.
var AUTH_NUM = ['open', 'WEP', 'WPA', 'WPA2', 'WPA/WPA2', 'WPA2 enterprise',
                'WPA3', 'WPA2/WPA3', 'WAPI', 'OWE', 'WPA3 enterprise',
                'WPA3 ext', 'WPA3 ext mixed'];

function authName(a) {
  if (a == null) return 'secured';
  if (typeof a === 'number') return AUTH_NUM[a] || ('auth mode ' + a);
  var s = String(a).toLowerCase();
  if (/^\d+$/.test(s)) return AUTH_NUM[+s] || ('auth mode ' + s);
  if (s === 'open' || s === 'none') return 'open';
  return s.replace(/_/g, '/').toUpperCase();
}
function isOpenAuth(a) {
  if (a == null) return false;
  if (typeof a === 'number') return a === 0 || a === 9;
  var s = String(a).toLowerCase();
  return s === 'open' || s === 'none' || s === '0' || s === 'owe' || s === '9';
}
function isEnterprise(a) {
  if (typeof a === 'number') return a === 5 || a === 10;
  return /ent|802\.?1x|eap/i.test(String(a == null ? '' : a));
}
function isWep(a) {
  if (typeof a === 'number') return a === 1;
  return /wep/i.test(String(a == null ? '' : a));
}

function rssiBars(r) {
  if (r == null) return 0;
  if (r >= -55) return 4;
  if (r >= -67) return 3;
  if (r >= -75) return 2;
  return 1;
}
function barsEl(rssi) {
  var n = rssiBars(rssi), w = el('span', 'rb-b'), i;
  for (i = 1; i <= 4; i++) w.appendChild(el('i', i <= n ? 'on' : null));
  return w;
}
function scanmsg(m) { var e = $('su-scanmsg'); if (e) e.textContent = m || ''; }

function normNet(n) {
  var ssid = pick(n, ['ssid', 'name']);
  return {
    ssid: ssid || '',
    rssi: pick(n, ['rssi', 'signal']),
    auth: pick(n, ['auth', 'authmode', 'auth_mode', 'security']),
    channel: pick(n, ['channel', 'ch']),
    hidden: !ssid || !!n.hidden
  };
}

function wifiScan() {
  return withRadio('the WiFi scan', function () {
    scanmsg('scanning…');
    return tryGet('/api/wifi/scan', { timeout: 20000, tries: 5 }, scanmsg)
      .then(function (d) {
        var raw = listOf(d, ['networks', 'aps', 'results', 'scan']);
        var seen = {}, out = [];
        raw.map(normNet).forEach(function (n) {
          // A network on two bands, or with two access points, arrives twice.
          // Keep one row and the loudest reading of it.
          var k = n.ssid ? 's:' + n.ssid : 'h:' + out.length;
          if (seen[k]) {
            if (n.rssi != null && (seen[k].rssi == null || n.rssi > seen[k].rssi)) {
              seen[k].rssi = n.rssi;
            }
            return;
          }
          seen[k] = n;
          out.push(n);
        });
        out.sort(function (a, b) {
          return (b.rssi == null ? -999 : b.rssi) - (a.rssi == null ? -999 : a.rssi);
        });
        S.nets = out;
        S.scanned = Date.now();
        drawNets();
        scanmsg(out.length ? out.length + ' found' : 'nothing in range');
        return out;
      });
  }).catch(function (e) {
    scanmsg('');
    S.nets = [];
    drawNets(e.code === 'radio_busy'
      ? e.message + '. Try again when it finishes.'
      : 'The scan failed: ' + why(e) + '. Try again, or type the name in by hand.');
  });
}

function drawNets(err) {
  var box = $('su-nets');
  box.textContent = '';
  if (err) { box.appendChild(el('p', 'su-bad', err)); return; }
  if (!S.nets.length) {
    box.appendChild(el('p', 'note',
      'No networks yet. Rescan, or use "Other network" if yours does not broadcast ' +
      'its name.'));
    return;
  }
  S.nets.forEach(function (n) { box.appendChild(netRow(n)); });
}

function netRow(n) {
  var b = el('button', 'row-btn');
  b.type = 'button';
  b.appendChild(icon(isOpenAuth(n.auth) ? 'i-open' : 'i-lock'));

  var t = el('span', 'rb-t', n.hidden ? '(hidden network)' : n.ssid);
  var bits = [authName(n.auth)];
  if (n.channel) bits.push('ch ' + n.channel);
  if (n.rssi != null) bits.push(n.rssi + ' dBm');
  var sub = el('em', null, bits.join(' · '));
  t.appendChild(sub);
  b.appendChild(t);
  b.appendChild(barsEl(n.rssi));

  if (isEnterprise(n.auth)) {
    b.disabled = true;
    b.className = 'row-btn off';
    sub.textContent = 'enterprise 802.1X — this board cannot join it';
  } else if (n.hidden) {
    b.disabled = true;
    b.className = 'row-btn off';
    sub.textContent = 'name not broadcast — use "Other network" and type it';
  } else {
    b.onclick = function () { chooseNet(n); };
  }
  return b;
}

function chooseNet(n) {
  S.target = n;
  show('su-pick', false);
  show('su-result', false);
  show('su-trying', false);
  show('su-form', true);

  var manual = !!n.manual;
  show('su-ssidwrap', manual);
  if (manual) $('su-ssid').value = n.ssid || '';

  var t = $('su-target');
  t.textContent = '';
  if (manual) {
    t.appendChild(document.createTextNode('Network by name'));
  } else {
    t.appendChild(document.createTextNode('Joining '));
    t.appendChild(el('b', null, n.ssid));
    var bits = [authName(n.auth)];
    if (n.rssi != null) bits.push(n.rssi + ' dBm');
    t.appendChild(document.createTextNode(' · ' + bits.join(' · ')));
  }

  var open = !manual && isOpenAuth(n.auth);
  show('su-pskwrap', !open);
  $('su-psk').value = '';
  $('su-psk').type = 'password';
  $('su-eye').textContent = 'Show';
  formErr(null);

  setTimeout(function () {
    var f = manual ? $('su-ssid') : (open ? $('su-join') : $('su-psk'));
    try { f.focus(); } catch (e) { /* a captive webview may refuse focus */ }
  }, 60);
}

function formErr(m) {
  var e = $('su-formerr');
  e.textContent = m || '';
  e.hidden = !m;
}

// Everything checkable is checked here, before the radio moves: a join that
// fails takes fifteen seconds and drops this page, and finding out afterwards
// that the password was six characters long is a waste of both.
function submitJoin() {
  var t = S.target || {};
  var ssid = t.manual ? $('su-ssid').value : t.ssid;
  var psk = $('su-psk').value;
  var note = '';

  if (t.manual) {
    var trimmed = ssid.replace(/^\s+|\s+$/g, '');
    if (trimmed !== ssid) {
      note = 'A stray space was trimmed off the name. ';
      ssid = trimmed;
    }
    if (!ssid) { formErr('Enter the network name. It is case sensitive.'); return; }
    if (blen(ssid) > 32) {
      formErr('That name is ' + blen(ssid) + ' bytes and the limit is 32.');
      return;
    }
  }

  var open = !t.manual && isOpenAuth(t.auth);
  if (!open) {
    var min = isWep(t.auth) ? 5 : 8;
    if (psk.length && psk.length < min) {
      formErr('A ' + (min === 5 ? 'WEP' : 'WPA') + ' password is at least ' + min +
              ' characters and this one is ' + psk.length + '. Nothing was sent.');
      return;
    }
    if (blen(psk) > 63) {
      formErr('That password is ' + blen(psk) + ' bytes and the limit is 63.');
      return;
    }
    if (!psk.length && !t.manual) {
      formErr('This network is ' + authName(t.auth) + ', so it needs a password.');
      return;
    }
  }

  S.target = { ssid: ssid, auth: t.auth, manual: t.manual,
               rssi: t.rssi, channel: t.channel };
  join(ssid, psk, note);
}

function trying(ssid, hint) {
  show('su-pick', false);
  show('su-form', false);
  show('su-result', false);
  show('su-trying', true);
  S.t0 = Date.now();
  stopTick();
  var t = function () {
    $('su-tryingtxt').textContent =
      'Joining ' + ssid + '… ' + Math.round((Date.now() - S.t0) / 1000) + ' s';
  };
  t();
  S.timer = setInterval(t, 1000);
  tryhint(hint || 'The access point may drop for a moment while the radio changes ' +
                  'channel. This page keeps asking and will say what happened.');
}
function tryhint(m) { $('su-tryinghint').textContent = m; }
function stopTick() { if (S.timer) { clearInterval(S.timer); S.timer = null; } }

function join(ssid, psk, note) {
  trying(ssid, note ? note + 'Handing it to the board.' : null);
  var mine = {};
  S.watching = mine;

  return withRadio('the WiFi join', function () {
    return H.api('/api/wifi/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ssid: ssid, psk: psk }),
      timeout: 30000
    }).then(function (d) {
      if (S.watching !== mine) return null;
      return settle(d, ssid, mine);
    }, function (e) {
      if (S.watching !== mine) return null;
      // Losing this request is the expected case, not the failure case: the
      // radio moved out from under it. The board still knows the answer.
      if (e.code === 'timeout' || e.code === 'network') {
        tryhint('The board stopped answering, which is exactly what happens when the ' +
                'radio leaves the access point. Waiting for it to come back and say ' +
                'how it went.');
        return watchJoin(ssid, 45000, mine);
      }
      throw e;
    });
  }).catch(function (e) {
    if (S.watching !== mine) return;
    joinFailed(e.code || 'unknown', why(e), ssid);
  });
}

// The board may answer the POST with the outcome, with "still trying", or not
// at all. All three end in the same two places.
function settle(d, ssid, mine) {
  if (!d || typeof d !== 'object') return watchJoin(ssid, 45000, mine);

  var st = String(pick(d, ['state', 'status', 'result']) || '').toLowerCase();

  if (d.ok === false || st === 'failed' || st === 'error') {
    joinFailed(pick(d, ['reason', 'error', 'code']), pick(d, ['detail', 'message']), ssid);
    return null;
  }
  if (st === 'trying' || st === 'connecting' || st === 'pending' || d.pending === true) {
    return watchJoin(ssid, 45000, mine);
  }
  if (d.ok === true || st === 'ok' || st === 'joined' || st === 'connected' || d.ip) {
    // Prefer a real status read over the POST body, so the address on screen is
    // the address the board is actually holding.
    return tryGet('/api/net/status', { timeout: 6000, tries: 3 })
      .then(function (n) { joinOk(n || d); }, function () { joinOk(d); });
  }
  return watchJoin(ssid, 45000, mine);
}

// Poll until the board says one way or the other, or until it has been silent
// long enough that the honest answer is that we do not know.
function watchJoin(ssid, budget, mine) {
  var t0 = Date.now(), miss = 0;

  function turn() {
    if (S.watching !== mine || !live) return null;
    if (Date.now() - t0 > budget) {
      if (miss >= 3) joinLost(ssid); else joinFailed('timeout', null, ssid);
      return null;
    }
    return H.api('/api/net/status', { timeout: 5000 }).then(function (n) {
      if (S.watching !== mine) return null;
      miss = 0;
      S.net = n || S.net;

      var j = (n && n.join) || null;
      var js = j ? String(j.state || '').toLowerCase() : '';
      if (js === 'ok' || js === 'joined' || js === 'connected') { joinOk(n); return null; }
      if (js === 'failed' || js === 'error') {
        joinFailed(j.reason, j.detail, ssid);
        return null;
      }
      // No join block in the reply: fall back to the observable facts. An
      // address, a matching SSID and a mode that is no longer SETUP is a join,
      // whatever the firmware chose to call it.
      if (!j && n && n.ip && n.ip !== '0.0.0.0' &&
          (!n.ssid || n.ssid === ssid) && !isSetupMode(n.mode)) {
        joinOk(n);
        return null;
      }
      tryhint('The board is still working on it. ' +
              (n && n.ssid ? 'It reports ' + n.ssid + '. ' : '') +
              'A join gets fifteen seconds on the board, plus a retry.');
      return delay(1200).then(turn);
    }, function () {
      if (S.watching !== mine) return null;
      miss++;
      tryhint('No answer for ' + miss + ' check' + (miss === 1 ? '' : 's') + '. That ' +
              'usually means the radio has left the access point, which is what ' +
              'joining looks like from here.');
      return delay(1400).then(turn);
    });
  }
  return turn();
}

function joinOk(n) {
  stopTick();
  S.watching = null;
  S.net = n || S.net;
  var ssid = (n && n.ssid) || (S.target && S.target.ssid) || 'your network';
  S.host = (n && (n.hostname || n.host)) || S.host;

  summary('wifi', ssid, true);
  fillDone(n, false);
  result('ok', 'Joined ' + ssid,
    addrLine(n) + ' The credentials are stored now and only now — the board never ' +
    'saves a password it has not already proved works.',
    [{ label: 'Pair a keyboard', cls: 'primary big', fn: function () { goStep('ble'); } },
     { label: 'Finish', fn: function () { goStep('done'); } }]);
}

function addrLine(n) {
  var ip = n && n.ip && n.ip !== '0.0.0.0' ? n.ip : null;
  var md = mdnsOf(n);
  if (ip && md) return 'It answers at ' + md + ' and at ' + ip + '.';
  if (ip) return 'It answers at ' + ip + '.';
  if (md) return 'It answers at ' + md + '.';
  return 'It has not reported an address yet.';
}
function mdnsOf(n) {
  if (n && n.mdns) return n.mdns;
  var h = (n && (n.hostname || n.host)) || S.host;
  return h ? h + '.local' : null;
}

// A join fails for reasons that need different things from the person, so
// "failed" on its own is useless. These are the ones esp_wifi actually
// reports, by name and by wifi_err_reason_t number.
var NUMR = {
  1: 'unknown', 2: 'bad_auth', 3: 'bad_auth', 4: 'assoc_fail', 5: 'ap_full',
  6: 'assoc_fail', 7: 'assoc_fail', 8: 'assoc_fail', 15: 'bad_auth',
  23: 'enterprise', 200: 'weak', 201: 'no_ap', 202: 'bad_auth',
  203: 'assoc_fail', 204: 'bad_auth', 205: 'assoc_fail'
};

function reasonKey(r) {
  if (r == null) return 'unknown';
  if (typeof r === 'number') return NUMR[r] || 'unknown';
  var s = String(r).toLowerCase();
  if (/^\d+$/.test(s)) return NUMR[+s] || 'unknown';
  if (/wrong.?pass|bad.?auth|auth.?fail|handshake|psk|4way/.test(s)) return 'bad_auth';
  if (/no.?ap|not.?found|no.?such|missing/.test(s)) return 'no_ap';
  if (/full|too.?many/.test(s)) return 'ap_full';
  if (/assoc/.test(s)) return 'assoc_fail';
  if (/beacon|weak|range|rssi/.test(s)) return 'weak';
  if (/ent|802\.?1x|eap/.test(s)) return 'enterprise';
  if (/dhcp|no.?ip|ip.?fail/.test(s)) return 'ip_fail';
  if (/timeout|timed/.test(s)) return 'timeout';
  if (/busy|radio/.test(s)) return 'busy';
  return 'unknown';
}

function joinFailed(reason, detail, ssid) {
  stopTick();
  S.watching = null;

  var k = reasonKey(reason);
  var title = 'Could not join ' + ssid;
  var text;
  var again  = { label: 'Try again', cls: 'primary big',
                 fn: function () { chooseNet(S.target); } };
  var list   = { label: 'Pick another network', fn: backToList };
  var rescan = { label: 'Rescan', fn: function () { backToList(); wifiScan(); } };
  var acts;

  switch (k) {
    case 'bad_auth':
      title = 'Wrong password';
      text = ssid + ' was found and answered, but it refused the password. It is case ' +
             'sensitive, and a WPA password is 8 to 63 characters. Watch for a phone ' +
             'capitalising the first letter for you.';
      acts = [again, list];
      break;
    case 'no_ap':
      text = 'Nothing called ' + ssid + ' answered. Either the name is spelled ' +
             'differently, or the board is out of range of it — the board hears ' +
             'the network from where it is sitting, not from where your phone is.';
      acts = [rescan, again];
      break;
    case 'assoc_fail':
      text = ssid + ' was heard but would not take the board. That is usually distance, ' +
             'a MAC filter on the router, or a 5 GHz-only network: this board is 2.4 GHz.';
      acts = [again, rescan];
      break;
    case 'ap_full':
      text = ssid + ' refused the board because it already has as many devices as it ' +
             'will take. Free a slot on the router and try again.';
      acts = [again, list];
      break;
    case 'weak':
      text = ssid + ' was in range during the scan but its signal went away during the ' +
             'join. Move the board closer to the router, or the router closer to it.';
      acts = [again, rescan];
      break;
    case 'enterprise':
      text = ssid + ' uses 802.1X enterprise login, which needs a username as well as a ' +
             'password. This board cannot join that. Use a personal WPA2 network or a ' +
             'phone hotspot instead.';
      acts = [list];
      break;
    case 'ip_fail':
      text = 'The board joined ' + ssid + ' but the router never handed it an address. ' +
             'That is DHCP on the router, not the password.';
      acts = [again, list];
      break;
    case 'timeout':
      text = 'The join did not finish in the time the board allows. If ' + ssid + ' is far ' +
             'away, this is what that looks like: move the board closer and try again.';
      acts = [again, rescan];
      break;
    case 'busy':
      text = (detail || 'The radio was already busy.') + ' Wait a moment and try again.';
      acts = [again];
      break;
    default:
      text = 'The board reported ' + (detail || reason || 'no reason') + '. Nothing was ' +
             'saved, so the board is still on its own access point and you can try again.';
      acts = [again, rescan];
  }
  if (detail && k !== 'busy' && k !== 'unknown') text += ' (the board said: ' + detail + ')';

  summary('wifi', 'not joined', false);
  result('bad', title, text, acts);
}

// Neither success nor failure: the board went away and never came back. Calling
// that "failed" would be a lie, and the likeliest truth is that it worked and
// took the access point down with it.
function joinLost(ssid) {
  stopTick();
  S.watching = null;
  var md = mdnsOf(null);
  summary('wifi', 'unconfirmed', true);
  fillDone(null, true);
  result('warn', 'The board stopped answering',
    'It was joining ' + ssid + ' and then went quiet. That most often means it joined ' +
    'and shut this access point down, which is the whole point of the exercise. Rejoin ' +
    'your usual WiFi and look for ' + (md || 'the board') + '. If it is not there, ' +
    'power-cycle the board: it comes straight back to this setup page, and nothing was ' +
    'saved.',
    [{ label: 'What address?', cls: 'primary big', fn: function () { goStep('done'); } },
     { label: 'Check again', fn: function () {
         trying(ssid, 'Asking again, in case the access point came back.');
         S.watching = {};
         watchJoin(ssid, 20000, S.watching);
       } }]);
}

function result(kind, title, text, acts) {
  stopTick();
  show('su-trying', false);
  show('su-pick', false);
  show('su-form', false);
  show('su-result', true);

  var t = $('su-restitle');
  t.textContent = title;
  t.className = 'su-big ' + (kind === 'ok' ? 'good' : kind === 'warn' ? 'warn' : 'bad');
  $('su-restext').textContent = text;

  var box = $('su-resacts');
  box.textContent = '';
  acts.forEach(function (a) {
    var b = el('button', a.cls || null, a.label);
    b.type = 'button';
    b.onclick = a.fn;
    box.appendChild(b);
  });
}

function backToList() {
  S.watching = null;
  stopTick();
  show('su-result', false);
  show('su-form', false);
  show('su-trying', false);
  show('su-pick', true);
}

// ================================================================ KEYBOARD

function blemsg(m) { var e = $('su-blemsg'); if (e) e.textContent = m || ''; }
function bleErr(m) {
  var e = $('su-bleerr');
  e.textContent = m || '';
  e.hidden = !m;
}

function normDev(d) {
  return {
    addr: pick(d, ['addr', 'address', 'mac', 'bda']) || '',
    name: pick(d, ['name', 'label']) || '',
    rssi: pick(d, ['rssi', 'signal']),
    bonded: !!pick(d, ['bonded', 'paired'])
  };
}

function bleScan() {
  bleErr(null);
  return withRadio('the Bluetooth scan', function () {
    blemsg('scanning — keep the keyboard in pairing mode…');
    var t0 = Date.now();
    function turn() {
      return tryGet('/api/ble/scan', { timeout: 20000, tries: 4 }, blemsg).then(function (d) {
        var list = listOf(d, ['devices', 'peripherals', 'results', 'found']).map(normDev);
        list.sort(function (a, b) {
          return (b.rssi == null ? -999 : b.rssi) - (a.rssi == null ? -999 : a.rssi);
        });
        S.devs = list;
        drawDevs();
        if (d && d.scanning && Date.now() - t0 < 25000) {
          blemsg('scanning… ' + list.length + ' so far');
          return delay(1300).then(turn);
        }
        blemsg(list.length ? list.length + ' found' : 'nothing advertising as a keyboard');
        return list;
      });
    }
    return turn();
  }).catch(function (e) {
    blemsg('');
    bleErr(e.code === 'radio_busy'
      ? e.message + '. Try again when it finishes.'
      : 'The scan failed: ' + why(e) + '.');
  });
}

function drawDevs() {
  var box = $('su-bledevs');
  box.textContent = '';
  if (!S.devs.length) {
    box.appendChild(el('p', 'note',
      'Nothing yet. A keyboard only advertises for a minute or so after you put it into ' +
      'pairing mode, and it stops as soon as it reconnects to whatever it is already ' +
      'bonded to. Put it back into pairing mode and scan again.'));
    return;
  }
  S.devs.forEach(function (d) { box.appendChild(devRow(d)); });
}

function devRow(d) {
  var b = el('button', 'row-btn');
  b.type = 'button';
  b.appendChild(icon('i-kbd'));
  var t = el('span', 'rb-t', d.name || '(no name)');
  var bits = [];
  if (d.addr) bits.push(d.addr);
  if (d.rssi != null) bits.push(d.rssi + ' dBm');
  if (d.bonded) bits.push('already bonded here');
  t.appendChild(el('em', null, bits.join(' · ')));
  b.appendChild(t);
  b.appendChild(barsEl(d.rssi));
  b.onclick = function () { pairAsk(d); };
  return b;
}

// The warning belongs here, on the way in, not in a footnote underneath: a
// keyboard bonds to one host, and pairing it here quietly breaks wherever it
// currently lives.
function pairAsk(d) {
  S.pairdev = d;
  bleErr(null);
  $('su-blename').textContent = d.name || d.addr;
  show('su-blepick', false);
  show('su-passkey', false);
  show('su-blewarn', true);
}

function pairCancel() {
  S.pairdev = null;
  S.watching = null;
  show('su-blewarn', false);
  show('su-passkey', false);
  show('su-blepick', true);
}

function pairGo() {
  var d = S.pairdev;
  if (!d) return;
  var mine = {};
  S.watching = mine;

  show('su-blewarn', false);
  show('su-passkey', true);
  $('su-pkdigits').textContent = '······';
  $('su-pklead').textContent = 'Connecting to ' + (d.name || d.addr) + '…';
  bleErr(null);

  withRadio('the Bluetooth pairing', function () {
    return H.api('/api/ble/pair', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ addr: d.addr }),
      timeout: 25000
    }).then(function () {
      return watchPair(d, mine);
    }, function (e) {
      if (e.code === 'timeout' || e.code === 'network') return watchPair(d, mine);
      throw e;
    });
  }).catch(function (e) {
    if (S.watching !== mine) return;
    pairFailed(e.code, e.message, d);
  });
}

function watchPair(d, mine) {
  var t0 = Date.now(), miss = 0, shown = false;

  function turn() {
    if (S.watching !== mine || !live) return null;
    if (Date.now() - t0 > 120000) { pairFailed('timeout', null, d); return null; }

    return H.api('/api/ble/status', { timeout: 6000 }).then(function (st) {
      if (S.watching !== mine) return null;
      miss = 0;

      var pk = pick(st, ['passkey', 'pin', 'code']);
      var state = String(pick(st, ['state', 'status']) || '').toLowerCase();
      var bond = st && (st.bonded || st.bond);

      if (pk != null && pk !== '' && String(pk) !== '0') {
        shown = true;
        showPasskey(pk);
      }
      if (state === 'failed' || state === 'error' || (st && st.ok === false)) {
        pairFailed(pick(st, ['reason', 'error', 'code']), pick(st, ['detail', 'message']), d);
        return null;
      }
      if (state === 'bonded' || state === 'paired' || state === 'ready' ||
          (bond && typeof bond === 'object' && (bond.addr || bond.name)) ||
          (bond === true && st.connected)) {
        pairOk(st, d);
        return null;
      }
      if (!shown) {
        $('su-pklead').textContent = state === 'connecting'
          ? 'Connecting to ' + (d.name || d.addr) + '…'
          : 'Waiting for the keyboard to ask for a passkey…';
      }
      return delay(800).then(turn);
    }, function () {
      if (S.watching !== mine) return null;
      miss++;
      if (miss > 12) { pairFailed('no_answer', null, d); return null; }
      return delay(1200).then(turn);
    });
  }
  return turn();
}

function showPasskey(pk) {
  var s = String(pk).replace(/\D/g, '');
  while (s.length < 6) s = '0' + s;
  $('su-pklead').textContent = 'Type this on the keyboard, then press Enter';
  $('su-pkdigits').textContent = s;
}

function pairOk(st, d) {
  S.watching = null;
  var bond = (st && (st.bonded || st.bond)) || null;
  var name = (bond && typeof bond === 'object' && bond.name) ||
             (st && st.name) || d.name || d.addr;
  S.bonded = {
    name: name,
    addr: (bond && typeof bond === 'object' && bond.addr) || (st && st.addr) || d.addr,
    battery: st && st.battery
  };
  show('su-passkey', false);
  show('su-blepick', true);
  drawBonded();
  summary('ble', name, true);
  bleErr(null);
  H.toast('keyboard paired', 'ok');
  if (S.done.wifi) goStep('done');
}

var BLER = {
  not_found: 'The board could not find that keyboard when it went to connect. They stop ' +
             'advertising after a minute; put it back into pairing mode and scan again.',
  connect_fail: 'The keyboard would not accept a connection. Put it next to the board and ' +
                'try again.',
  bond_fail: 'Pairing was refused. If the passkey was mistyped, try again. If the keyboard ' +
             'still believes it is bonded somewhere else, clear that on the keyboard first ' +
             '— on most of them that is holding Fn and a channel key until the light ' +
             'flashes.',
  no_hid: 'That device is not a keyboard. It does not offer the HID service the board needs.',
  no_reports: 'That keyboard offers HID but publishes no input reports the board can ' +
              'subscribe to, so it cannot be used as an input device.',
  timeout: 'Pairing timed out. The passkey has to be typed on the keyboard itself and ' +
           'finished with Enter — the keyboard has no screen, so nothing is echoed ' +
           'while you do it.',
  no_answer: 'The board stopped answering during pairing. Reload this page; if the board is ' +
             'gone too, power-cycle it. Nothing is bonded until the board says so.',
  busy: 'The radio was busy with WiFi. Bluetooth and WiFi share one radio here, so they take ' +
        'turns.',
  radio_busy: 'The radio is busy with WiFi. Bluetooth and WiFi share one radio here, so they ' +
              'take turns.'
};

function pairFailed(reason, detail, d) {
  S.watching = null;
  show('su-passkey', false);
  show('su-blepick', true);

  var k = String(reason == null ? '' : reason).toLowerCase();
  var msg = BLER[k];
  if (!msg) {
    if (/auth|reject|refus|passkey|bond|secur/.test(k)) msg = BLER.bond_fail;
    else if (/hid/.test(k)) msg = BLER.no_hid;
    else if (/report|notify/.test(k)) msg = BLER.no_reports;
    else if (/found|scan/.test(k)) msg = BLER.not_found;
    else if (/connect/.test(k)) msg = BLER.connect_fail;
    else if (/time/.test(k)) msg = BLER.timeout;
    else if (/busy/.test(k)) msg = BLER.busy;
    else msg = 'Pairing ' + (d.name || d.addr) + ' failed: ' +
               (detail || reason || 'no reason given') + '.';
  }
  bleErr(msg);
  summary('ble', 'not paired', false);
}

function drawBonded() {
  var box = $('su-bonded');
  box.textContent = '';
  if (!S.bonded) { box.hidden = true; return; }
  box.hidden = false;

  var p = el('p', 'su-big good');
  p.appendChild(document.createTextNode('Paired with '));
  p.appendChild(el('b', null, S.bonded.name));
  box.appendChild(p);

  var bits = ['The bond is stored in NVS, so the keyboard reconnects by itself after sleep.'];
  if (S.bonded.battery != null) bits.push('Battery ' + S.bonded.battery + '%.');
  box.appendChild(el('p', null, bits.join(' ')));

  var tb = el('div', 'toolbar');
  var f = el('button', 'danger', 'Forget this keyboard');
  f.type = 'button';
  f.onclick = bleForget;
  tb.appendChild(f);
  box.appendChild(tb);
}

function bleForget() {
  H.api('/api/ble/forget', { method: 'POST', timeout: 8000 }).then(function () {
    S.bonded = null;
    drawBonded();
    summary('ble', 'optional', false);
    H.toast('bond dropped', 'ok');
  }, function (e) {
    bleErr('Could not drop the bond: ' + e.message);
  });
}

function bleStatusOnce() {
  return H.api('/api/ble/status', { timeout: 6000 }).then(function (st) {
    var bond = st && (st.bonded || st.bond);
    if (bond && typeof bond === 'object' && (bond.addr || bond.name)) {
      S.bonded = {
        name: bond.name || bond.addr,
        addr: bond.addr,
        battery: st.battery
      };
      drawBonded();
      summary('ble', S.bonded.name, true);
    }
    return st;
  }, function () { return null; });
}

// ==================================================================== DONE

function fillDone(n, uncertain) {
  var box = $('su-addrs');
  box.textContent = '';
  var md = mdnsOf(n);
  var ip = n && n.ip && n.ip !== '0.0.0.0' ? n.ip : null;
  if (md) box.appendChild(addrEl(md));
  if (ip) box.appendChild(addrEl(ip));
  if (!md && !ip) {
    box.appendChild(el('p', 'note',
      'The board never reported an address. Look for it in the router’s client list, ' +
      'or read it off the panel once it is up.'));
  }

  $('su-donehead').textContent = uncertain
    ? 'The board should be on your network.'
    : 'The board is on ' +
      ((n && n.ssid) || (S.target && S.target.ssid) || 'your network') + '.';

  var ul = $('su-donenotes');
  ul.textContent = '';
  var add = function (t) { ul.appendChild(el('li', null, t)); };

  if (!S.manual) {
    var ap = S.apname || 'this access point';
    add('The board shuts ' + ap + ' down once it is on your network. Rejoin your usual ' +
        'WiFi before using the address above.');
    add('If this page opened by itself when you joined, it is a sign-in window and not a ' +
        'full browser. Close it and type the address into your usual browser.');
  }
  if (md) {
    add('Android does not resolve .local names. Use the numeric address there, and give ' +
        'the board a fixed lease on the router so that address does not move.');
  }
  add('To move the board to a different network later, open Settings and use "Forget ' +
      'WiFi". That puts it back on its own access point, and back on this page.');

  summary('done', uncertain ? 'unconfirmed' : 'ready', true);
}

function addrEl(hostpart) {
  // No scheme written into the source: a protocol-relative href inherits the
  // one this page was served over, which is the only one the board speaks.
  var a = el('a', 'addr-a', hostpart);
  a.href = '//' + hostpart + '/';
  a.rel = 'noreferrer';
  return a;
}

// ================================================================== wiring

function bind() {
  if (bound) return;
  bound = true;

  STEPS.forEach(function (k) {
    var h = document.querySelector('#st-' + k + ' .step-h');
    if (h) h.onclick = function () { goStep(k); };
  });

  $('su-boardnext').onclick = function () { goStep('wifi'); };
  $('su-exit').onclick = closeScreen;

  $('su-rescan').onclick = function () { backToList(); wifiScan(); };
  $('su-other').onclick = function () { chooseNet({ ssid: '', auth: null, manual: true }); };
  $('su-back').onclick = backToList;
  $('su-eye').onclick = function () {
    var i = $('su-psk');
    var showing = i.type === 'text';
    i.type = showing ? 'password' : 'text';
    $('su-eye').textContent = showing ? 'Show' : 'Hide';
  };
  $('su-form').onsubmit = function (e) { e.preventDefault(); submitJoin(); };

  $('su-blescan').onclick = bleScan;
  $('su-blecancel').onclick = pairCancel;
  $('su-bleok').onclick = pairGo;
  $('su-pkcancel').onclick = function () {
    S.watching = null;
    pairCancel();
    bleErr('Pairing cancelled here. The board may still be waiting for the passkey until ' +
           'it gives up on its own.');
  };
  $('su-bleskip').onclick = function () { goStep('done'); };

  $('su-finish').onclick = function () {
    if (S.manual) { closeScreen(); return; }
    var a = $('su-addrs').querySelector('a');
    if (a) location.href = a.href;
  };

  bleStatusOnce();
}

// ================================================================== public

function isSetupMode(m) {
  m = String(m == null ? '' : m).toLowerCase();
  return m === 'setup' || m === 'ap' || m === 'apsta' || m === 'portal' ||
         m === 'provision' || m === 'provisioning' || m === 'connecting';
}

function probe() {
  if (location.hash === '#setup') { openScreen('manual'); return Promise.resolve(true); }
  return H.api('/api/net/status', { timeout: 6000 }).then(function (d) {
    S.net = d || null;
    if (d && (d.hostname || d.host)) S.host = d.hostname || d.host;
    if (d && d.ap && (d.ap.ssid || d.ap.name)) S.apname = d.ap.ssid || d.ap.name;
    if (d && isSetupMode(d.mode)) { openScreen('boot', d); return true; }
    return false;
  }, function () {
    // No status endpoint, or no board at all. Either way this is not our screen
    // to take: the app has its own way of saying the board is not answering.
    return false;
  });
}

function forget() {
  return H.confirm('Forget WiFi',
    'The board drops the stored network, restarts into setup and puts its own access ' +
    'point back up. You will have to join that access point from a phone to put it back ' +
    'on a network. Continue?')
  .then(function (yes) {
    if (!yes) return null;
    return H.api('/api/wifi/forget', { method: 'POST', timeout: 8000 }).then(function () {
      H.toast('forgotten — the board is restarting into setup', 'ok');
      return true;
    }, function (e) {
      H.toast('forget failed: ' + e.message, 'err');
      return false;
    });
  });
}

window.EOS_SETUP = {
  attach: function (host) { H = host; },
  probe: probe,
  open: function (reason) { openScreen(reason || 'manual'); },
  close: closeScreen,
  forget: forget,
  isOpen: function () { return live; }
};

})();
