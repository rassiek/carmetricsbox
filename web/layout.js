'use strict';
// Layout editor: dashboard tiles and the OLED screen (order, labels, styles,
// thresholds) with a live preview drawn the way the firmware draws it.
(() => {
  const { $, h, api, toast, live } = CM;
  let sensors = [], tiles = [], oled = [], cols = 0, autoOrder = true, oledType = 'ssd1327_128x128', saved = '', savedSensors = '';
  let values = {};   // live values by sensor id

  const sensor = id => sensors.find(s => s.id === id);
  const name = id => (sensor(id) || { name: id }).name;
  const snapshot = () => JSON.stringify({ tiles, oled: autoOrder ? [] : oled, cols });
  const sensorSnap = () => JSON.stringify(sensors.map(s => [s.id, s.oledLabel, s.oledStyle, s.warnLow, s.critLow, s.warnHigh, s.critHigh]));
  const dirty = () => $('#dirty').classList.toggle('hidden', snapshot() === saved && sensorSnap() === savedSensors);
  function move(list, i, d) { const [x] = list.splice(i, 1); list.splice(i + d, 0, x); autoOrder = list !== oled ? autoOrder : false; render(); }
  function sel(value, opts, onchange) {
    const s = h('select', {}, opts.map(([v, t]) => h('option', { value: v, selected: String(v) === String(value) }, t)));
    s.onchange = () => { onchange(s.value); render(); };
    return s;
  }
  function arrows(list, i) {
    return [h('button', { class: 'small', disabled: i === 0, onclick: () => move(list, i, -1) }, '↑'),
            h('button', { class: 'small', disabled: i === list.length - 1, onclick: () => move(list, i, 1) }, '↓'),
            h('button', { class: 'small danger', onclick: () => { list.splice(i, 1); if (list === oled) autoOrder = false; render(); } }, '✕')];
  }
  const autoList = () => sensors.filter(s => s.enabled && s.oledStyle !== 'off').map(s => s.id);
  const defaultLabel = s => (s.oledLabel || s.name.replace(/[^A-Za-z0-9]/g, '').slice(0, 3)).toUpperCase();

  // ---- threshold sliders (one per threshold the sensor uses)
  function thresholdSliders(s) {
    const lo = s.gaugeMin ?? 0, hi = s.gaugeMax ?? 100, step = Math.pow(10, -(s.decimals || 0));
    const box = h('div', { style: 'flex-basis:100%;display:grid;grid-template-columns:auto 1fr auto;gap:2px 8px;align-items:center;padding-left:8px' });
    for (const [key, label, color] of [['warnLow', 'Warn below', 'var(--warn)'], ['critLow', 'Critical below', 'var(--crit)'],
                                        ['warnHigh', 'Warn above', 'var(--warn)'], ['critHigh', 'Critical above', 'var(--crit)']]) {
      if (s[key] === null || s[key] === undefined) continue;
      const out = h('span', { class: 'small', style: `min-width:56px;text-align:right;color:${color}` }, `${s[key]} ${s.unit || ''}`);
      const r = h('input', { type: 'range', min: lo, max: hi, step, value: s[key], style: `accent-color:${color}` });
      r.oninput = () => { s[key] = +r.value; out.textContent = `${s[key]} ${s.unit || ''}`; drawPreview(); dirty(); };
      box.append(h('span', { class: 'small muted' }, label), r, out);
    }
    return box.children.length ? box : null;
  }

  function oledRow(id, i) {
    if (id === '|') return h('div', { class: 'lay-row brk' }, h('span', { class: 'name' }, '— new page —'), ...arrows(oled, i));
    const s = sensor(id);
    if (!s) return h('div', { class: 'lay-row' }, h('span', { class: 'name muted' }, `${id} (removed)`), ...arrows(oled, i));
    const lbl = h('input', { value: defaultLabel(s), maxlength: 3, style: 'width:4.5em;text-transform:uppercase', title: 'OLED label, up to 3 characters' });
    lbl.oninput = () => { s.oledLabel = lbl.value.toUpperCase().slice(0, 3); drawPreview(); dirty(); };
    const style = sel(s.oledStyle === 'off' ? 'text' : s.oledStyle, [['text', 'Text'], ['bar', 'Bar'], ['marker', 'Marker']], v => { s.oledStyle = v; });
    const row = h('div', { class: 'lay-row' }, h('span', { class: 'name' }, s.name), lbl, style, ...arrows(oled, i));
    if (s.oledStyle === 'bar' || s.oledStyle === 'marker') { const t = thresholdSliders(s); if (t) row.append(t); }
    return row;
  }

  function render() {
    const tl = $('#tList');
    tl.replaceChildren(...(tiles.length ? tiles.map((t, i) => h('div', { class: 'lay-row' },
      h('span', { class: 'name' }, name(t.id)),
      sel(t.size, [[1, 'Normal'], [2, 'Wide']], v => { t.size = +v; }),
      sel(t.widget, [['bar', 'Value + bar'], ['number', 'Value only'], ['spark', 'Value + trend line']], v => { t.widget = v; }),
      ...arrows(tiles, i))) : [h('p', { class: 'muted' }, 'Automatic: every enabled sensor.')]));
    const used = new Set(tiles.map(t => t.id));
    $('#tAdd').replaceChildren(...sensors.filter(s => s.enabled && !used.has(s.id)).map(s => h('option', { value: s.id }, s.name)));

    if (autoOrder) oled = autoList();
    $('#oList').replaceChildren(...(oled.length ? oled.map(oledRow) : [h('p', { class: 'muted' }, 'Nothing on the OLED yet: add values below.')]));
    if (autoOrder && oled.length) $('#oList').prepend(h('p', { class: 'muted small' }, 'Automatic order (Sensors page order). Moving a value or adding a page break fixes the order.'));
    const usedO = new Set(oled);
    $('#oAdd').replaceChildren(...sensors.filter(s => s.enabled && !usedO.has(s.id)).map(s => h('option', { value: s.id }, s.name + (s.oledStyle === 'off' ? ' (not shown now)' : ''))));
    $('#oCols').value = String(cols);
    drawPreview();
    dirty();
  }

  // ---- preview: same rules as Display.cpp (text rows of N cells, one row per bar, split into pages)
  function drawPreview() {
    const big = !/64/.test(oledType), W = 128, H = big ? 128 : 64;
    const textH = big ? 14 : 10, barH = big ? 17 : 11, n = cols >= 2 ? cols : (big ? 2 : 3);
    const segs = [[]];
    for (const id of oled) { if (id === '|') { if (segs[segs.length - 1].length) segs.push([]); continue; } const s = sensor(id); if (s && s.enabled) segs[segs.length - 1].push(s); }
    const rows = [], forced = new Set();
    for (const seg of segs) {
      if (rows.length) forced.add(rows.length);
      const t = seg.filter(s => s.oledStyle === 'text' || s.oledStyle === 'off'), b = seg.filter(s => s.oledStyle === 'bar' || s.oledStyle === 'marker');
      for (let i = 0; i < t.length; i += n) rows.push({ bar: false, items: t.slice(i, i + n), h: textH });
      for (const s of b) rows.push({ bar: true, items: [s], h: barH });
    }
    const pages = []; let start = 0, used = 0;
    rows.forEach((r, i) => { if ((forced.has(i) || used + r.h > H) && i > start) { pages.push(rows.slice(start, i)); start = i; used = 0; } used += r.h; });
    pages.push(rows.slice(start));
    const fg = '#7fc6ff', scale = 2;
    $('#oPreview').replaceChildren(...pages.map(pg => {
      const c = h('canvas', { width: W * scale, height: H * scale, style: `width:${W * scale}px;height:${H * scale}px;background:#000;border-radius:4px;image-rendering:pixelated` });
      const g = c.getContext('2d'); g.scale(scale, scale); g.fillStyle = fg; g.strokeStyle = fg; g.lineWidth = 1; g.textBaseline = 'alphabetic';
      let y = 0;
      for (const r of pg) {
        if (r.bar) drawBar(g, y, r.h, W, r.items[0], big);
        else {
          const cw = W / n, fs = n >= 4 ? 7 : (n === 3 && big) ? 10 : big ? 12 : 9;
          g.font = `${fs}px monospace`;
          r.items.forEach((s, k) => {
            const x = k * cw, base = y + r.h - 3, lbl = defaultLabel(s), v = fmtVal(s);
            g.fillText(lbl, x + 1, base);
            let txt = v + unitChar(s); if (g.measureText(lbl).width + 3 + g.measureText(txt).width > cw - 3) txt = v;
            g.fillText(txt, x + cw - 3 - g.measureText(txt).width, base);
          });
        }
        y += r.h;
      }
      return c;
    }));
  }
  const unitChar = s => (s.unit === '°C' || s.unit === 'C') ? 'c' : (s.unit === 'V' || s.unit === 'v') ? 'v' : s.unit === 'm' ? 'm' : '';
  const sample = s => values[s.id] ?? (s.warnHigh ?? s.gaugeMax ?? 100) * 0.6;
  const fmtVal = s => { const v = sample(s); return v === null || isNaN(v) ? '--' : Number(v).toFixed(s.decimals || 0); };
  const frac = (s, v) => Math.max(0, Math.min(1, (v - (s.gaugeMin ?? 0)) / (((s.gaugeMax ?? 100) - (s.gaugeMin ?? 0)) || 1)));
  function drawBar(g, y, hgt, W, s, big) {
    const cw = big ? 7 : 6, labelW = cw * 3 + 3, valueW = cw * 4 + 5, bx = labelW, bw = W - labelW - valueW, bh = hgt - 4, by = y + 2;
    g.font = `${big ? 12 : 9}px monospace`;
    const base = y + hgt - 2 - (hgt > 14 ? 2 : 0);
    g.fillText(defaultLabel(s), 0, base);
    g.strokeRect(bx + 0.5, by + 0.5, bw - 1, bh - 1);
    const ticks = ['warnLow', 'critLow', 'warnHigh', 'critHigh'].filter(k => s[k] !== null && s[k] !== undefined).map(k => frac(s, s[k]));
    const f = frac(s, sample(s));
    if (s.oledStyle === 'marker') {
      for (const t of ticks) g.fillRect(bx + 1 + Math.floor((bw - 3) * t), by + 2, 1, bh - 4);
      const nx = bx + 1 + Math.floor((bw - 5) * f);
      g.fillRect(nx + 1, by + 1, 2, bh - 2); g.fillRect(nx, by + 1, 4, 1); g.fillRect(nx, by + bh - 2, 4, 1);
    } else {
      g.fillRect(bx + 1, by + 1, Math.floor((bw - 2) * f), bh - 2);
      for (const t of ticks) { const tx = bx + Math.floor((bw - 1) * t); g.fillRect(tx, by - 2, 1, 2); g.fillRect(tx, by + bh, 1, 2); }
    }
    const v = fmtVal(s);
    g.fillText(v, W - g.measureText(v).width, base);
  }

  async function load() {
    const cfg = await api('/api/config');
    sensors = cfg.sensors;
    oledType = (cfg.system && cfg.system.oledType) || oledType;
    tiles = (cfg.layout && cfg.layout.tiles) || [];
    const o = (cfg.layout && cfg.layout.oled) || [];
    autoOrder = !o.length; oled = autoOrder ? autoList() : o.slice();
    cols = (cfg.layout && cfg.layout.oledColumns) || 0;
    saved = snapshot(); savedSensors = sensorSnap();
    render();
  }

  async function save() {
    try {
      if (sensorSnap() !== savedSensors) await api('/api/config/sensors', { body: sensors });   // labels, styles, thresholds
      const r = await api('/api/config/layout', { body: { tiles, oled: autoOrder ? [] : oled, oledColumns: cols } });
      toast(r.message); saved = snapshot(); savedSensors = sensorSnap(); dirty();
    } catch (e) { toast(e.message, true, 6000); }
  }

  document.addEventListener('DOMContentLoaded', () => {
    $('#tAuto').onclick = () => { tiles = []; render(); };
    $('#oAuto').onclick = () => { autoOrder = true; render(); };
    $('#oAbbr').onclick = () => {   // the short names used in the car
      const map = { coolant_temp: 'CLT', intake_temp: 'IT', gearbox_temp: 'GBT', transfer_case_temp: 'TCT', cabin_temp: 'CT',
                    egt: 'EGT', boost_pressure: 'BST', oil_pressure: 'OIL', fuel_pressure: 'FP', altitude: 'ALT', rpm: 'RPM' };
      let n = 0;
      for (const s of sensors) if (map[s.id] && s.oledLabel !== map[s.id]) { s.oledLabel = map[s.id]; n++; }
      render(); toast(n ? `${n} labels changed: press Save layout` : 'Labels already match');
    };
    $('#oCols').onchange = () => { cols = +$('#oCols').value; render(); };
    $('#tAddBtn').onclick = () => {
      const id = $('#tAdd').value;
      if (!id) return;
      if (!tiles.length) tiles = sensors.filter(s => s.enabled).map(s => ({ id: s.id, size: 1, widget: 'bar' })).filter(t => t.id !== id);
      tiles.push({ id, size: 1, widget: 'bar' });
      render();
    };
    $('#oAddBtn').onclick = () => {
      const id = $('#oAdd').value;
      if (!id) return;
      const s = sensor(id); if (s && s.oledStyle === 'off') s.oledStyle = 'text';
      autoOrder = false; oled.push(id); render();
    };
    $('#oBreak').onclick = () => { autoOrder = false; oled.push('|'); render(); };
    $('#save').onclick = save;
    load().catch(e => toast(e.message, true));
    live(st => {   // live values for the preview
      if (!st || !st.sensors) return;
      for (const [id, v] of Object.entries(st.sensors)) values[id] = v.v;
      if (document.activeElement?.type !== 'range') drawPreview();
    });
  });
})();
