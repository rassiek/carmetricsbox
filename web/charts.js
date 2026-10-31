'use strict';
// Grouped canvas line charts: sensors with the same kind of reading share one axis.
(() => {
  const { $, h, api, live, onConfig, fmt } = CM;
  const COLORS = ['#3fa7ff', '#f4c430', '#34c77b', '#ff7a45', '#b37feb', '#ff4d9a', '#36cfc9', '#d3f261'];
  let groups = [], interval = 5, lastAppend = 0, maxLen = 360;

  // 1-Wire temps together, pressures together, EGT on its own, everything else by unit.
  function groupOf(s) {
    if (s.type === 'ds18b20') return ['temps', 'Temperatures (1-Wire)'];
    if (s.type === 'max6675' || s.type === 'mcp9600') return ['egt', 'Exhaust gas temperature'];
    if (/^(psi|bar|kpa)$/i.test(s.unit)) return ['pressure', 'Pressures'];
    if (/^v$/i.test(s.unit)) return ['volts', 'Voltages'];
    return ['unit:' + s.unit + ':' + s.type, null];
  }

  function draw(g) {
    const cv = g.canvas, dpr = window.devicePixelRatio || 1;
    const W = cv.clientWidth, H = cv.clientHeight;
    if (!W) return;
    if (cv.width !== Math.round(W * dpr)) { cv.width = Math.round(W * dpr); cv.height = Math.round(H * dpr); }
    const c = cv.getContext('2d');
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.clearRect(0, 0, W, H);
    const shown = g.series.filter(x => !x.hidden);
    const single = g.series.length === 1 ? g.series[0].s : null;

    // auto-zoom to visible data (+ limits when the chart has a single sensor)
    let vals = [];
    shown.forEach(x => x.data.forEach(v => { if (v !== null) vals.push(v); }));
    let lo = Math.min(...vals), hi = Math.max(...vals);
    if (single) [single.warnLow, single.critLow, single.warnHigh, single.critHigh]
      .filter(t => t != null && t >= single.gaugeMin && t <= single.gaugeMax).forEach(t => { lo = Math.min(lo, t); hi = Math.max(hi, t); });
    const ref = shown[0] ? shown[0].s : g.series[0].s;
    if (!isFinite(lo)) { lo = ref.gaugeMin; hi = ref.gaugeMax; }
    const minSpan = Math.max((ref.gaugeMax - ref.gaugeMin) * 0.05, 1e-3);
    if (hi - lo < minSpan) { const m = (hi + lo) / 2; lo = m - minSpan / 2; hi = m + minSpan / 2; }
    const pad = (hi - lo) * 0.08; lo -= pad; hi += pad;

    const L = 40, R = 8, T = 6, B = 16, pw = W - L - R, ph = H - T - B;
    const y = v => T + ph - (v - lo) / (hi - lo) * ph;
    c.font = '11px system-ui, sans-serif';
    c.fillStyle = '#8b9ab0'; c.strokeStyle = '#263247'; c.lineWidth = 1;
    for (let k = 0; k <= 4; k++) {
      const v = lo + (hi - lo) * k / 4, yy = Math.round(y(v)) + .5;
      c.beginPath(); c.moveTo(L, yy); c.lineTo(W - R, yy); c.stroke();
      c.fillText(v.toFixed(hi - lo < 10 ? 1 : 0), 2, yy + 4);
    }
    c.fillText(`-${Math.round(maxLen * interval / 60)}m`, L, H - 3);
    c.fillText('now', W - R - 22, H - 3);

    if (single) {
      c.setLineDash([4, 4]);
      for (const [v, col] of [[single.warnHigh, '#f4c430'], [single.critHigh, '#ff4d4f'], [single.warnLow, '#f4c430'], [single.critLow, '#ff4d4f']]) {
        if (v == null || v < lo || v > hi) continue;
        c.strokeStyle = col; c.beginPath(); c.moveTo(L, y(v)); c.lineTo(W - R, y(v)); c.stroke();
      }
      c.setLineDash([]);
    }
    for (const x of shown) {
      const n = x.data.length, xx = i => L + (i + (maxLen - n)) / (maxLen - 1) * pw;
      c.strokeStyle = x.color; c.lineWidth = 1.8; c.beginPath();
      let pen = false;
      x.data.forEach((v, i) => {
        if (v === null) { pen = false; return; }
        pen ? c.lineTo(xx(i), y(v)) : c.moveTo(xx(i), y(v));
        pen = true;
      });
      c.stroke();
    }
    // legend values
    for (const x of g.series) {
      const d = x.data.filter(v => v !== null), last = d.length ? d[d.length - 1] : null;
      x.val.textContent = last === null ? '--' : `${fmt(last, x.s.decimals)} ${x.s.unit}`;
      x.range.textContent = d.length ? `${fmt(Math.min(...d), x.s.decimals)}–${fmt(Math.max(...d), x.s.decimals)}` : 'no data';
      x.chip.classList.toggle('off', !!x.hidden);
    }
  }

  async function build() {
    const [cfg, hist] = await Promise.all([api('/api/config'), api('/api/history')]);
    interval = hist.interval || 5;
    maxLen = hist.len || 360;
    $('#span').textContent = `Last ${Math.round(maxLen * interval / 60)} minutes, one point every ${interval}s. Tap a legend entry to hide or show a line.`;
    const box = $('#charts');
    box.innerHTML = '';
    const byKey = new Map();
    for (const s of cfg.sensors.filter(s => s.enabled)) {
      const [key, title] = groupOf(s);
      if (!byKey.has(key)) byKey.set(key, { title, series: [] });
      byKey.get(key).series.push(s);
    }
    groups = [];
    for (const { title, series } of byKey.values()) {
      const canvas = h('canvas'), legend = h('div', { class: 'legend' });
      const g = { canvas, series: [] };
      series.forEach((s, i) => {
        const val = h('b'), range = h('span', { class: 'muted' });
        const chip = h('button', { class: 'chip' }, h('i', { style: `background:${COLORS[i % COLORS.length]}` }), s.name, ' ', val, ' ', range);
        const x = { s, color: COLORS[i % COLORS.length], data: (hist.series[s.id] || []).map(v => (v === null ? null : +v)), val, range, chip, hidden: false };
        chip.onclick = () => { x.hidden = !x.hidden; draw(g); };
        if (series.length === 1) chip.disabled = true;
        legend.append(chip);
        g.series.push(x);
      });
      box.append(h('div', { class: 'chart' }, h('div', { class: 'head' }, h('span', {}, title || series[0].name)), canvas, legend));
      groups.push(g);
    }
    lastAppend = Date.now() - (hist.ageMs || 0);
    groups.forEach(draw);
  }

  live(st => {
    if (Date.now() - lastAppend < interval * 1000) return;
    lastAppend = Date.now();
    for (const g of groups) {
      for (const x of g.series) {
        const r = st.sensors[x.s.id];
        x.data.push(r && r.v !== null && r.st === 'ok' ? +r.v : null);
        if (x.data.length > maxLen) x.data.shift();
      }
      draw(g);
    }
  });
  let rt;
  window.addEventListener('resize', () => { clearTimeout(rt); rt = setTimeout(() => groups.forEach(draw), 100); });
  onConfig(() => build().catch(e => CM.toast(e.message, true)));
  document.addEventListener('DOMContentLoaded', () => build().catch(e => CM.toast(e.message, true)));
})();
