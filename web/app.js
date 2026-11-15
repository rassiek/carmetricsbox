'use strict';
// Web app core: API v1 client, live data (WebSocket, polling fallback), names and
// units from the settings, the 5 tabs, small DOM helpers. Pages register with CM.page().
const CM = (() => {
  const A = '/api/v1';
  const $ = (s, el = document) => el.querySelector(s);
  const $$ = (s, el = document) => [...el.querySelectorAll(s)];
  function h(tag, attrs = {}, ...kids) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) {
      if (v === undefined || v === null || v === false) continue;
      if (k.startsWith('on')) e[k] = v;
      else if (k === 'class') e.className = v;
      else if (k === 'html') e.innerHTML = v;
      else if (k === 'value') e.value = v;
      else if (k === 'checked' || k === 'selected' || k === 'disabled') e[k] = !!v;
      else e.setAttribute(k, v === true ? '' : v);
    }
    for (const c of kids.flat(3)) if (c !== null && c !== undefined && c !== false) e.append(c.nodeType ? c : document.createTextNode(c));
    return e;
  }

  // ---- API: errors come back as {error:{code,message,field}}
  async function api(path, { method, body, raw } = {}) {
    const opt = { method: method || (body !== undefined ? 'POST' : 'GET'), headers: {} };
    if (raw) { opt.body = raw; opt.headers['Content-Type'] = 'application/octet-stream'; }
    else if (body !== undefined) { opt.body = JSON.stringify(body); opt.headers['Content-Type'] = 'application/json'; }
    const r = await fetch(A + path, opt);
    const t = await r.text();
    let j = null; try { j = t ? JSON.parse(t) : {}; } catch { j = { text: t }; }
    if (!r.ok) {
      const e = new Error((j && j.error && j.error.message) || `${r.status} ${r.statusText}`);
      e.field = j && j.error && j.error.field; e.status = r.status; throw e;
    }
    return j;
  }
  const get = p => api(p), patch = (p, b) => api(p, { method: 'PATCH', body: b }), del = p => api(p, { method: 'DELETE' });

  let toastT;
  function toast(msg, bad = false, ms = 3500) {
    const t = $('#toast'); if (!t || !msg) return;
    t.textContent = msg; t.className = bad ? 'show bad' : 'show';
    clearTimeout(toastT); toastT = setTimeout(() => t.className = '', ms);
  }
  // Run an API call from a button: shows the reply or the error.
  async function act(fn, okMsg) {
    try { const r = await fn(); toast(okMsg || (r && r.message) || 'Done'); return r || true; }
    catch (e) { toast(e.message, true, 6000); return null; }
  }

  // ---- state shared by the pages
  const S = { info: {}, settings: null, meta: {}, values: {}, level: 'ok', muted: false, rev: null, conn: 'connecting' };
  const ui = () => (S.settings && S.settings.ui) || {};

  // Units: values come in the sensor's unit; the person's preference converts the common ones.
  function conv(v, unit) {
    const u = ui();
    if (v === null || v === undefined || isNaN(v)) return [v, unit];
    const un = u.units || {};
    if (unit === '°C' && un.temperature === 'F') return [v * 9 / 5 + 32, '°F'];
    if (un.pressure && ['bar', 'psi', 'kPa'].includes(unit) && unit !== un.pressure) {
      const kpa = unit === 'bar' ? v * 100 : unit === 'psi' ? v * 6.89476 : v;
      return [un.pressure === 'bar' ? kpa / 100 : un.pressure === 'psi' ? kpa / 6.89476 : kpa, un.pressure];
    }
    if (un.speed && unit === 'km/h' && un.speed !== 'km/h') return [un.speed === 'mph' ? v / 1.609344 : v / 1.852, un.speed];
    return [v, unit];
  }
  function fmt(v, unit, dec = 1) {
    const [x, u] = conv(v, unit);
    if (x === null || x === undefined || isNaN(x)) return ['--', u || ''];
    return [Number(x).toFixed(u === '°F' ? 0 : dec), u || ''];
  }

  // ---- live data
  const listeners = [];
  function live(cb) { listeners.push(cb); if (Object.keys(S.values).length) cb(legacy()); }
  // {sensors:{id:{v,st,lvl}}} for the layout editor; sensor id = mirror id for other boards
  function legacy() {
    const s = {};
    for (const [a, x] of Object.entries(S.values)) {
      const [b, id] = a.split(/\.(.*)/);
      s[b === 'local' ? id : `${b}_${id}`] = x;
    }
    return { sensors: s, level: S.level, muted: S.muted };
  }
  function emit() { for (const cb of listeners) try { cb(legacy()); } catch (e) { console.error(e); } header(); }
  function takePoints(m) {
    if (m.items) for (const x of m.items) S.values[x.a] = { v: x.v, st: x.st, lvl: x.lvl };
    if (m.v) for (const [a, x] of Object.entries(m.v)) S.values[a] = { v: x[0], st: x[1], lvl: x[2] };
    if (m.level) S.level = m.level;
    if (m.muted !== undefined) S.muted = m.muted;
    if (m.running !== undefined) S.running = m.running;
    if (m.type === 'points' || m.items) S.turbo = m.turbo || null;
    if (m.rev !== undefined && S.rev !== null && String(m.rev) !== String(S.rev)) reloadSettings();
    if (m.rev !== undefined) S.rev = m.rev;
    emit();
  }
  let ws, pollT, wsTries = 0;
  function connect() {
    try { ws = new WebSocket(`ws://${location.host}${A}/ws`); } catch { return poll(); }
    ws.onmessage = e => {
      let m; try { m = JSON.parse(e.data); } catch { return; }
      if (m.type === 'points') { S.conn = 'live'; takePoints(m); }
      else if (m.type === 'alerts') { S.alerts = m; S.level = m.level; S.muted = m.muted; emit(); }
      else if (m.type === 'config') reloadSettings();
    };
    ws.onopen = () => { wsTries = 0; clearInterval(pollT); pollT = null; };
    ws.onclose = () => { S.conn = 'polling'; header(); poll(); setTimeout(connect, Math.min(30000, 2000 * ++wsTries)); };
  }
  function poll() {
    if (pollT) return;
    const once = () => get('/points').then(p => { if (S.conn !== 'live') S.conn = 'polling'; takePoints(p); }).catch(() => { S.conn = 'offline'; header(); });
    once(); pollT = setInterval(once, 2000);
  }
  let settingsWaiters = [];
  async function reloadSettings() {
    try {
      const [st, pm] = await Promise.all([get('/settings'), get('/points?meta=1')]);
      S.settings = st; S.rev = st.rev;
      S.meta = {}; for (const x of pm.items) S.meta[x.a] = x;
      takePoints(pm);
      applyNames();
      for (const f of settingsWaiters) f(); settingsWaiters = [];
      if (current && pages[current].onSettings) pages[current].onSettings();
    } catch (e) { toast(e.message, true); }
  }
  const ready = () => S.settings ? Promise.resolve() : new Promise(r => settingsWaiters.push(r));

  // ---- tabs
  const TABS = [['dashboard', 'Dashboard'], ['sensors', 'Sensors'], ['actuators', 'Actuators'], ['boards', 'Boards'], ['system', 'System']];
  const pages = {};
  let current = null;
  function page(name, def) { pages[name] = def; }
  function applyNames() {
    const u = ui();
    document.title = (u.machineName ? u.machineName + ' · ' : '') + (u.appName || 'CarMetrics');
    $('#brand').textContent = u.appName || 'CarMetrics';
    $('#machine').textContent = u.machineName || '';
    for (const [id, def] of TABS) { const a = $(`#tab-${id}`); if (a) a.textContent = (u.pages && u.pages[id]) || def; }
  }
  function show() {
    const [name, sub] = (location.hash.slice(1) || 'dashboard').split('/');
    const n = pages[name] ? name : 'dashboard';
    for (const [id] of TABS) $(`#tab-${id}`).classList.toggle('active', id === n);
    if (current && pages[current].leave) pages[current].leave();
    current = n;
    const main = $('#main'); main.innerHTML = '';
    ready().then(() => { if (current === n) pages[n].show(main, sub); });
  }
  function header() {
    const p = $('#level');
    const lv = S.level || 'ok';
    p.className = 'pill ' + lv;
    p.textContent = { ok: 'OK', warn: 'Warning', crit: 'CRITICAL', fault: 'Sensor fault' }[lv] || lv;
    $('#mute').textContent = S.muted ? 'Unmute' : 'Mute';
    $('#mute').classList.toggle('hidden', lv === 'ok' && !S.muted);
    $('#conn').textContent = S.conn;
    $('#conn').className = 'pill ' + (S.conn === 'live' ? 'ok' : S.conn === 'offline' ? 'crit' : '');
  }

  document.addEventListener('DOMContentLoaded', async () => {
    $('#mute').onclick = () => act(() => api('/alerts/mute', { body: { minutes: S.muted ? 0 : 15 } }));
    window.addEventListener('hashchange', show);
    try { S.info = await get('/info'); if (S.info.safeMode) $('#safe').classList.remove('hidden'); } catch {}
    // The board has no clock of its own until it reaches the internet: lend it ours.
    if (!S.info.epoch) api('/system/time', { body: { epoch: Math.floor(Date.now() / 1000) } }).catch(() => {});
    await reloadSettings();
    show();
    connect();
  });

  // ---- small form helpers shared by the pages
  // Field schema: {k, label, type:'text'|'num'|'bool'|'select'|'pin'|'sensor'|'pass', opts:[[v,t]], show:o=>bool, help, step, wide}
  function form(obj, fields, onchange) {
    const box = h('div', { class: 'fields' });
    const redraw = () => {
      box.innerHTML = '';
      for (const f of fields) {
        if (f.show && !f.show(obj)) continue;
        const v = getPath(obj, f.k);
        let inp;
        const set = x => { setPath(obj, f.k, x); if (f.redraw) redraw(); if (onchange) onchange(obj, f.k); };
        if (f.type === 'bool') {
          inp = h('input', { type: 'checkbox', checked: !!v, onchange: e => set(e.target.checked) });
          box.append(h('label', { class: 'check' + (f.wide ? ' wide' : ''), title: f.help || '' }, [inp, f.label]));
          continue;
        }
        if (f.type === 'select' || f.type === 'sensor' || f.type === 'pin') {
          let opts = f.opts || [];
          if (typeof opts === 'function') opts = opts(obj);
          if (f.type === 'sensor') opts = [['', '(none)']].concat(sensorChoices());
          if (f.type === 'pin') opts = pinChoices(f.input);
          inp = h('select', { onchange: e => set(f.type === 'pin' ? +e.target.value : (f.num ? +e.target.value : e.target.value)) },
                  opts.map(([ov, ot]) => h('option', { value: ov, selected: String(ov) === String(v ?? '') }, ot)));
        } else {
          inp = h('input', { type: f.type === 'num' ? 'number' : f.type === 'pass' ? 'password' : 'text', value: v ?? '', step: f.step || 'any',
                             placeholder: f.placeholder || '', autocomplete: 'off' });
          inp.onchange = e => set(f.type === 'num' ? (e.target.value === '' ? null : +e.target.value) : e.target.value);
        }
        inp.name = f.k;
        box.append(h('div', { class: f.wide ? 'wide' : '' }, [h('label', { title: f.help || '' }, f.label), inp,
                    f.help && f.helpShow ? h('div', { class: 'muted small' }, f.help) : null]));
      }
    };
    redraw();
    box.redraw = redraw;
    return box;
  }
  function getPath(o, k) { return k.split('.').reduce((x, p) => x == null ? x : x[p], o); }
  function setPath(o, k, v) { const ps = k.split('.'); const last = ps.pop(); let x = o; for (const p of ps) x = x[p] ??= {}; x[last] = v; }
  function sensorChoices() {
    return Object.values(S.meta).filter(m => m.kind === 'sensor' && m.board === 'local').map(m => [m.a.slice(6), m.name]);
  }
  let pinInfo = null;
  function pinChoices(input) {
    const out = [[-1, '(not used)']];
    for (const p of (pinInfo && pinInfo.pins) || []) {
      if (!input && !p.abilities.includes('output')) continue;
      out.push([p.gpio, `GPIO ${p.gpio}` + (p.fixed ? ` · ${p.fixed}` : '') + (p.usedBy ? ` · ${p.usedBy}` : '') +
                (p.abilities.includes('analog') ? ' · analog' : '') + (p.abilities.includes('boot_pin') ? ' · boot pin' : '')]);
    }
    return out;
  }
  async function loadPins() { try { pinInfo = await get('/pins'); } catch { pinInfo = { pins: [] }; } return pinInfo; }
  const err = e => toast(e.message, true, 6000);

  // The tiles when none are chosen: the most important first (six fit a phone),
  // the other plain temperatures together as one thermometer group, then the rest.
  function autoTiles(list) {
    const en = list.filter(s => s.enabled);
    const tc = s => ['max6675', 'mcp9600'].includes(s.type) || (s.type === 'remote' && /°/.test(s.unit) && (s.gaugeMax || 0) >= 400);
    const used = new Set(), out = [];
    const take = pred => { const s = en.find(x => !used.has(x.id) && pred(x)); if (s) { used.add(s.id); out.push({ id: s.id, size: 1, widget: 'bar' }); } };
    take(s => /coolant|water_temp|engine_temp/.test(s.id));
    take(s => /oil/.test(s.id) && /psi|bar|kPa/.test(s.unit));
    take(tc);
    take(s => /boost|map_press/.test(s.id));
    take(s => s.unit === 'V');
    const temps = en.filter(s => !used.has(s.id) && /°/.test(s.unit) && !tc(s) && !/cabin|outside|ambient|air_temp/.test(s.id));
    if (temps.length >= 2) {
      const g = temps.slice(0, 4); g.forEach(s => used.add(s.id));
      out.push({ id: g[0].id, size: 1, widget: 'bars', with: g.slice(1).map(s => s.id).join(',') });
    }
    for (const s of en) if (!used.has(s.id)) out.push({ id: s.id, size: 1, widget: 'bar' });
    return out;
  }

  return { autoTiles, $, $$, h, api, get, patch, del, toast, act, err, live, S, ui, fmt, conv, page, form, reloadSettings, loadPins,
           pins: () => pinInfo, sensorChoices, go: hash => { location.hash = hash; } };
})();
