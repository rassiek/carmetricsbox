'use strict';
// Digital tiles: every enabled sensor on one screen, no scrolling, any orientation.
(() => {
  const { $, h, api, live, onConfig, fmt } = CM;
  let tiles = {}, sensors = [];

  const STATUS = { waiting: 'WAITING', unassigned: 'NOT SET UP', not_found: 'NOT FOUND', fault: 'FAULT', disabled: 'OFF' };
  const pct = (s, v) => Math.max(0, Math.min(100, (v - s.gaugeMin) / (s.gaugeMax - s.gaugeMin) * 100));

  // Pick the column count that gives the biggest readable tiles for this screen.
  function layout() {
    const box = $('#tiles'), n = sensors.length;
    if (!n) return;
    const hd = document.querySelector('header');
    const gap = 6, pad = 6;
    const W = window.innerWidth - pad * 2, H = window.innerHeight - (hd ? hd.offsetHeight : 0) - pad * 2;
    let best = { score: -1 };
    for (let c = 1; c <= n; c++) {
      const r = Math.ceil(n / c);
      const cw = (W - gap * (c - 1)) / c, ch = (H - gap * (r - 1)) / r;
      if (cw < 70 || ch < 50) continue;
      const score = Math.min(cw / 1.7, ch) - (c * r - n) * 0.5;   // favour ~1.7:1 tiles, few empty cells
      if (score > best.score) best = { score, c, cw, ch };
    }
    if (best.score < 0) best = { c: Math.ceil(Math.sqrt(n)), cw: 120, ch: 70 };   // tiny screen: allow scroll
    box.style.gridTemplateColumns = `repeat(${best.c}, 1fr)`;
    box.style.gridAutoRows = `${Math.floor(best.ch)}px`;
    box.style.padding = pad + 'px';
    box.style.gap = gap + 'px';
    const valueSize = Math.max(18, Math.min(best.ch * 0.42, best.cw * 0.2));
    box.style.setProperty('--v', valueSize + 'px');
    box.style.setProperty('--t', Math.max(11, Math.min(best.ch * 0.14, 18)) + 'px');
  }

  function tick(s, v, cls) {
    return v == null || v <= s.gaugeMin || v >= s.gaugeMax ? null : h('i', { class: 'tick ' + cls, style: `left:${pct(s, v)}%` });
  }

  async function build() {
    const cfg = await api('/api/config');
    sensors = cfg.sensors.filter(s => s.enabled);
    const box = $('#tiles');
    box.innerHTML = '';
    tiles = {};
    $('#empty').classList.toggle('hidden', sensors.length > 0);
    for (const s of sensors) {
      const val = h('span', { class: 'num' }, '--');
      const unit = h('span', { class: 'unit' }, s.unit);
      const state = h('span', { class: 'state' });
      const fill = h('div', { class: 'fill' });
      const bar = h('div', { class: 'bar' }, fill,
        tick(s, s.warnLow, 'w'), tick(s, s.critLow, 'c'), tick(s, s.warnHigh, 'w'), tick(s, s.critHigh, 'c'));
      const tile = h('div', { class: 'tile' },
        h('div', { class: 'top' }, h('span', { class: 'name' }, s.name), state),
        h('div', { class: 'val' }, h('div', { class: 'vin' }, val, unit)), bar);
      box.append(tile);
      tiles[s.id] = { s, tile, val, unit, state, fill };
    }
    layout();
    if (CM.state) update(CM.state);
  }

  function update(st) {
    for (const [id, t] of Object.entries(tiles)) {
      const r = st.sensors[id];
      if (!r) continue;
      const ok = r.st === 'ok' && r.v !== null;
      const v = ok ? parseFloat(r.v) : null;
      t.val.textContent = ok ? fmt(v, t.s.decimals) : (STATUS[r.st] || '--');
      t.unit.style.display = ok ? '' : 'none';
      t.fill.style.width = ok ? pct(t.s, v) + '%' : '0%';
      const lvl = r.lvl !== 'ok' ? r.lvl : (ok ? '' : 'idle');   // fault colour only when it alarms
      t.tile.className = 'tile ' + lvl + (ok ? '' : ' nodata');
      t.state.textContent = r.lvl === 'crit' ? 'CRIT' : r.lvl === 'warn' ? 'WARN' : '';
    }
  }

  function fullscreenButton() {
    const conn = $('#conn');
    if (!conn || !document.documentElement.requestFullscreen) return;
    const b = h('button', { class: 'small', title: 'Full screen' }, '⛶');
    b.onclick = () => document.fullscreenElement ? document.exitFullscreen() : document.documentElement.requestFullscreen().catch(() => {});
    conn.before(b);
    document.addEventListener('fullscreenchange', () => setTimeout(layout, 100));
  }

  let rt;
  window.addEventListener('resize', () => { clearTimeout(rt); rt = setTimeout(layout, 80); });
  onConfig(() => build().catch(e => CM.toast(e.message, true)));
  document.addEventListener('DOMContentLoaded', () => {
    fullscreenButton();
    const hd = document.querySelector('header');   // header height changes (alerts, wrapping) -> re-fit
    if (hd && window.ResizeObserver) new ResizeObserver(() => layout()).observe(hd);
    build().catch(e => CM.toast(e.message, true));
    live(update);
  });
})();
