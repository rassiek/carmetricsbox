// CarMetrics shared helpers: API, live events, header, toast
'use strict';
const CM = (() => {
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

  // ---- live state over Server-Sent Events
  const listeners = [];
  let lastState = null, es = null, lastMsg = 0, rev = null;
  function live(fn) { listeners.push(fn); if (lastState) fn(lastState); }
  function connect() {
    es = new EventSource('/events');
    es.addEventListener('state', ev => {
      lastMsg = Date.now();
      const s = JSON.parse(ev.data);
      if (rev !== null && s.rev !== rev) onConfigChanged();
      rev = s.rev;
      lastState = s;
      listeners.forEach(fn => fn(s));
      updateHeader(s);
    });
    es.addEventListener('config', () => onConfigChanged());
  }
  let configCb = null;
  function onConfig(fn) { configCb = fn; }
  function onConfigChanged() { if (configCb) configCb(); }
  setInterval(() => {
    const stale = Date.now() - lastMsg > 4000;
    const c = $('#conn');
    if (c) { c.textContent = stale ? 'offline' : 'live'; c.className = 'pill ' + (stale ? 'fault' : 'ok'); }
    document.body.classList.toggle('stale', stale);
  }, 1000);

  // ---- header
  const pages = [['index.html', 'Gauges'], ['charts.html', 'Charts'], ['settings.html', 'Sensors'],
                 ['relays.html', 'Relays'], ['system.html', 'System']];
  function header() {
    const here = location.pathname.split('/').pop() || 'index.html';
    const hd = h('header', {},
      h('span', { class: 'brand' }, 'CarMetrics'),
      h('nav', {}, pages.map(([href, name]) => h('a', { href, class: href === here ? 'active' : '' }, name))),
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
    if (e) { e.textContent = s.engine ? 'engine on' : 'engine off'; e.className = 'pill ' + (s.engine ? 'ok' : ''); }
  }
  async function toggleMute() {
    try {
      const muted = lastState && lastState.muted;
      await api('/api/alerts/mute', { body: { minutes: muted ? 0 : 5 } });
    } catch (e) { toast(e.message, true); }
  }

  // Give the ESP32 the time once (it has no RTC) so the OLED clock works.
  async function syncTime() {
    try {
      if (sessionStorage.getItem('cmTime')) return;
      await api('/api/time', { body: { epochMs: Date.now() } });
      sessionStorage.setItem('cmTime', '1');
    } catch (e) { /* ignore */ }
  }

  function fmt(v, dec) { return v === null || v === undefined || Number.isNaN(v) ? '--' : Number(v).toFixed(dec); }
  const statusText = { ok: '', waiting: 'waiting', unassigned: 'not assigned', not_found: 'not found', fault: 'FAULT', disabled: 'disabled' };

  document.addEventListener('DOMContentLoaded', () => { header(); connect(); syncTime(); });
  return { $, h, api, toast, live, onConfig, fmt, statusText, get state() { return lastState; } };
})();
