// CarMetrics shared helpers: API, live events, header, toast
'use strict';
const CM = (() => {
  const WEB_VERSION = '__CM_VERSION__';   // replaced by tools/build_web.py with the release label
  const $ = (s, el = document) => el.querySelector(s);
  const h = (tag, attrs = {}, ...kids) => {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (k === 'class') el.className = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else if (v === true) el.setAttribute(k, '');
      else if (v !== false && v != null) el.setAttribute(k, v);
    }
    for (const k of kids.flat()) if (k != null) el.append(k.nodeType ? k : String(k));
    return el;
  };

  async function api(path, opts = {}) {
    const init = { method: opts.method || (opts.body !== undefined ? 'POST' : 'GET'), headers: {} };
    if (opts.body !== undefined) {
      init.headers['Content-Type'] = 'application/json';
      init.body = typeof opts.body === 'string' ? opts.body : JSON.stringify(opts.body);
    }
    const r = await fetch(path, init);
    let data = null;
    try { data = await r.json(); } catch (e) { /* not json */ }
    if (r.status === 401) throw new Error('Password required (user "admin")');
    if (!r.ok) throw new Error((data && data.message) || `${r.status} ${r.statusText}`);
    return data;
  }

  let toastTimer;
  function toast(msg, isErr = false, ms = 3500) {
    let t = $('#toast');
    if (!t) { t = h('div', { id: 'toast' }); document.body.append(t); }
    t.textContent = msg;
    t.className = isErr ? 'err' : '';
    t.classList.remove('hidden');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => t.classList.add('hidden'), ms);
  }

  // ---- live state: WebSocket (two-way, v2), then Server-Sent Events, then polling
  const listeners = [];
  let lastState = null, ws = null, es = null, lastLive = 0, lastAny = 0, rev = null, polling = false, mode = '';
  let wsFails = 0, wsSeq = 0;
  const wsWaiting = new Map();
  function live(fn) { listeners.push(fn); if (lastState) fn(lastState); }
  function handleState(s) {
    lastAny = Date.now();
    if (rev !== null && s.rev !== rev) onConfigChanged();
    rev = s.rev;
    lastState = s;
    if (s.clock === false) syncTime();
    listeners.forEach(fn => fn(s));
    updateHeader(s);
  }
  function connectWs() {
    if (!('WebSocket' in window)) return connectSse();
    try { ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`); } catch (e) { return connectSse(); }
    let opened = false;
    ws.onopen = () => { opened = true; wsFails = 0; mode = 'ws'; if (es) { es.close(); es = null; } };
    ws.onmessage = ev => {
      let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
      if (m.sensors) { lastLive = Date.now(); handleState(m); }
      else if (m.type === 'config') onConfigChanged();
      else if (m.type === 'ack' && wsWaiting.has(m.id)) { wsWaiting.get(m.id)(m); wsWaiting.delete(m.id); }
    };
    ws.onclose = () => {
      ws = null;
      if (!opened) wsFails++;
      if (wsFails >= 2) connectSse();               // no WebSocket on this firmware / proxy: use events
      else setTimeout(connectWs, 3000);
    };
  }
  function connectSse() {
    if (es) es.close();
    mode = 'sse';
    es = new EventSource('/events');
    es.addEventListener('state', ev => { lastLive = Date.now(); handleState(JSON.parse(ev.data)); });
    es.addEventListener('config', () => onConfigChanged());
  }
  // Send a command over the WebSocket when open (falls back to HTTP): {cmd:'mute', minutes:5}
  function command(c) {
    if (ws && ws.readyState === 1) {
      return new Promise((res, rej) => {
        const id = ++wsSeq;
        wsWaiting.set(id, m => m.ok ? res(m) : rej(new Error(m.message || 'failed')));
        ws.send(JSON.stringify(Object.assign({ type: 'cmd', id }, c)));
        setTimeout(() => { if (wsWaiting.delete(id)) rej(new Error('No reply from device')); }, 4000);
      });
    }
    return api('/api/v2/command', { body: c });
  }
  async function poll() {
    if (polling) return;
    polling = true;
    try { handleState(await api('/api/state')); } catch (e) { /* offline */ }
    polling = false;
  }
  let configCb = null;
  function onConfig(fn) { configCb = fn; }
  function onConfigChanged() { if (configCb) configCb(); }
  let deadSince = 0;
  setInterval(() => {
    const now = Date.now(), liveOk = now - lastLive < 3500;
    if (!liveOk) {
      poll();
      if (!deadSince) deadSince = now;
      if (now - deadSince > 15000) { deadSince = now; if (mode === 'sse') connectSse(); else if (!ws) connectWs(); }
    } else deadSince = 0;
    const online = now - lastAny < 5000;
    const c = $('#conn');
    if (c) {
      c.textContent = !online ? 'offline' : liveOk ? 'live' : 'polling';
      c.title = mode === 'ws' ? 'WebSocket' : mode === 'sse' ? 'Server-sent events' : '';
      c.className = 'pill ' + (!online ? 'fault' : liveOk ? 'ok' : '');
    }
    document.body.classList.toggle('stale', !online);
  }, 1500);

  // Board profile (valid pins etc.), fetched once per page
  let boardP = null;
  function board() { return boardP || (boardP = api('/api/v2/board').catch(() => null)); }

  // ---- header
  const pages = [['index.html', 'Gauges'], ['charts.html', 'Charts'], ['settings.html', 'Sensors'],
                 ['relays.html', 'Relays'], ['boards.html', 'Boards'], ['weather.html', 'Weather'], ['events.html', 'Events'], ['system.html', 'System']];
  function header() {
    const here = location.pathname.split('/').pop() || 'index.html';
    const hd = h('header', {},
      h('span', { class: 'brand', title: 'Web app ' + WEB_VERSION }, 'CarMetrics', h('small', { id: 'relLabel', class: 'muted', style: 'font-weight:400;font-size:11px;margin-left:6px' }, WEB_VERSION.startsWith('__') ? 'dev' : WEB_VERSION)),
      h('nav', {}, pages.map(([href, name]) => h('a', { href, class: href === here ? 'active' : '' }, name))),
      h('span', { id: 'clock', class: 'pill', title: 'Board clock' }, '--:--'),
      h('span', { id: 'engine', class: 'pill' }, 'engine ?'),
      h('span', { id: 'alert', class: 'pill ok' }, 'OK'),
      h('button', { id: 'mute', class: 'small hidden', onclick: toggleMute }, 'Mute'),
      h('span', { id: 'conn', class: 'pill' }, '...'));
    document.body.prepend(hd);
  }
  function updateHeader(s) {
    const a = $('#alert'), m = $('#mute'), e = $('#engine');
    if (a) {
      a.textContent = { ok: 'OK', warn: 'WARNING', crit: 'CRITICAL', fault: 'SENSOR FAULT' }[s.level] || s.level;
      a.className = 'pill ' + s.level;
    }
    if (m) {
      m.classList.toggle('hidden', s.level === 'ok' && !s.muted);
      m.textContent = s.muted ? `Muted ${Math.ceil((s.muteLeftS || 0) / 60)}m` : 'Mute';
    }
    const c = $('#clock');
    if (c) {
      const t = s.epoch ? new Date(s.epoch * 1000) : null;
      c.textContent = t ? t.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }) : 'clock not set';
      c.title = t ? `Board clock, set from ${s.clockSrc || 'browser'}` : 'The board has no time yet (no internet, GPS or browser)';
      c.className = 'pill ' + (t ? '' : 'warn');
    }
    if (e) { e.textContent = s.engine ? 'engine on' : 'engine off'; e.className = 'pill ' + (s.engine ? 'ok' : ''); }
    let b = $('#safeBanner');
    if (s.safe && !b) {
      b = h('div', { id: 'safeBanner', class: 'banner' }, 'SAFE MODE: the device restarted after repeated crashes. Relays are off and internet features are paused. ',
        h('a', { href: 'system.html#health' }, 'Details'));
      document.querySelector('header').after(b);
    } else if (!s.safe && b) b.remove();
  }
  async function toggleMute() {
    try {
      const muted = lastState && lastState.muted;
      await command({ cmd: 'mute', minutes: muted ? 0 : 5 });
    } catch (e) { toast(e.message, true); }
  }

  // The ESP32 has no battery-backed clock: after every reboot it needs the time
  // again (internet, GPS or this browser). Send it whenever the board says it
  // has none - not just once per tab, which missed reboots and OTA updates.
  let lastTimeSync = 0;
  async function syncTime() {
    if (Date.now() - lastTimeSync < 30000) return;
    lastTimeSync = Date.now();
    try { await api('/api/time', { body: { epochMs: Date.now() } }); } catch (e) { lastTimeSync = 0; }
  }

  function fmt(v, dec) { return v === null || v === undefined || Number.isNaN(v) ? '--' : Number(v).toFixed(dec); }
  const statusText = { ok: '', waiting: 'waiting', unassigned: 'not assigned', not_found: 'not found', fault: 'FAULT', disabled: 'disabled', link_lost: 'LINK LOST' };

  // Firmware and web app are uploaded separately: say so when they don't match
  // (e.g. only littlefs.bin was uploaded and new pages call missing firmware APIs).
  async function checkVersion() {
    if (WEB_VERSION.startsWith('__')) return;   // development copy, not built
    const i = await api('/api/info').catch(() => null);
    if (!i || i.version === WEB_VERSION) return;
    document.querySelector('header').after(h('div', { class: 'banner' },
      `Firmware ${i.version} and web app ${WEB_VERSION} are from different releases. Upload both files of one release on the System page (firmware, then web app), or flash the full image over USB.`));
  }

  document.addEventListener('DOMContentLoaded', () => { header(); connectWs(); checkVersion(); });
  // ---- several boards: Sensors / Relays pages edit another board with ?board=N
  function remoteBoard() { const b = +new URLSearchParams(location.search).get('board'); return b > 0 ? b : 0; }
  async function boardPicker(sel) {   // <select> inside a hidden wrapper; shown when boards are paired
    const d = await api('/api/v2/boards').catch(() => null);
    if (!d || d.role !== 'main' || !d.boards.length) return;
    sel.replaceChildren(h('option', { value: 0 }, 'This board (main)'), ...d.boards.map(b => h('option', { value: b.id }, `${b.name} (board ${b.id})`)));
    sel.value = remoteBoard();
    sel.onchange = () => { location.search = +sel.value ? '?board=' + sel.value : ''; };
    sel.parentElement.classList.remove('hidden');
  }

  return { WEB_VERSION, $, h, api, toast, live, onConfig, fmt, statusText, command, board, remoteBoard, boardPicker, get state() { return lastState; } };
})();
