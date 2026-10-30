'use strict';
// Lightweight canvas line charts (replaces the 280 KB Highcharts bundle).
(() => {
  const { $, h, api, live, onConfig, fmt } = CM;
  let charts = {}, interval = 5, lastAppend = 0, maxLen = 360;

  function draw(c) {
    const cv = c.canvas, dpr = window.devicePixelRatio || 1;
    const W = cv.clientWidth, H = cv.clientHeight;
    if (!W) return;
    if (cv.width !== W * dpr) { cv.width = W * dpr; cv.height = H * dpr; }
    const g = cv.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);
    const s = c.s, data = c.data;
    const vals = data.filter(v => v !== null);
    let lo = Math.min(...vals, ...[s.warnLow, s.critLow].filter(x => x != null && x >= s.gaugeMin));
    let hi = Math.max(...vals, ...[s.warnHigh, s.critHigh].filter(x => x != null && x <= s.gaugeMax));
    if (!vals.length) { lo = s.gaugeMin; hi = s.gaugeMax; }
    const minSpan = Math.max((s.gaugeMax - s.gaugeMin) * 0.05, 1e-3);
    if (hi - lo < minSpan) { const mid = (hi + lo) / 2; hi = mid + minSpan / 2; lo = mid - minSpan / 2; }
    const pad = (hi - lo) * 0.1; lo -= pad; hi += pad;
    const L = 38, R = 6, T = 6, B = 16, pw = W - L - R, ph = H - T - B;
    const y = v => T + ph - (v - lo) / (hi - lo) * ph;
    const x = i => L + (maxLen <= 1 ? pw : (i + (maxLen - data.length)) / (maxLen - 1) * pw);

    g.font = '11px system-ui, sans-serif';
    g.fillStyle = '#8b9ab0'; g.strokeStyle = '#263247'; g.lineWidth = 1;
    for (let k = 0; k <= 3; k++) {
      const v = lo + (hi - lo) * k / 3, yy = Math.round(y(v)) + .5;
      g.beginPath(); g.moveTo(L, yy); g.lineTo(W - R, yy); g.stroke();
      g.fillText(v.toFixed(Math.abs(hi - lo) < 10 ? 1 : 0), 2, yy + 4);
    }
    const mins = Math.round(maxLen * interval / 60);
    g.fillText(`-${mins}m`, L, H - 3);
    g.fillText('now', W - R - 22, H - 3);

    const th = [[s.warnHigh, '#f4c430'], [s.critHigh, '#ff4d4f'], [s.warnLow, '#f4c430'], [s.critLow, '#ff4d4f']];
    g.setLineDash([4, 4]);
    for (const [v, col] of th) {
      if (v == null || v < lo || v > hi) continue;
      g.strokeStyle = col; g.beginPath(); g.moveTo(L, y(v)); g.lineTo(W - R, y(v)); g.stroke();
    }
    g.setLineDash([]);
    g.strokeStyle = '#3fa7ff'; g.lineWidth = 1.6; g.beginPath();
    let pen = false;
    data.forEach((v, i) => {
      if (v === null) { pen = false; return; }
      pen ? g.lineTo(x(i), y(v)) : g.moveTo(x(i), y(v));
      pen = true;
    });
    g.stroke();

    const last = [...data].reverse().find(v => v !== null);
    c.cur.textContent = `${fmt(last, s.decimals)} ${s.unit}`;
    c.range.textContent = vals.length ? `min ${fmt(Math.min(...vals), s.decimals)} · max ${fmt(Math.max(...vals), s.decimals)}` : 'no data';
  }

  async function build() {
    const [cfg, hist] = await Promise.all([api('/api/config'), api('/api/history')]);
    interval = hist.interval || 5;
    $('#span').textContent = `Last ${Math.round(maxLen * interval / 60)} minutes (one point every ${interval}s), kept on the ESP32 — cleared on reboot or when sensors change.`;
    const box = $('#charts');
    box.innerHTML = '';
    charts = {};
    for (const s of cfg.sensors.filter(s => s.enabled)) {
      const canvas = h('canvas'), cur = h('b'), range = h('span', { class: 'muted small' });
      box.append(h('div', { class: 'chart' }, h('div', { class: 'head' }, h('span', {}, s.name), cur), canvas, range));
      const data = (hist.series[s.id] || []).map(v => (v === null ? null : +v));
      charts[s.id] = { s, canvas, cur, range, data };
    }
    lastAppend = Date.now() - (hist.ageMs || 0);
    Object.values(charts).forEach(draw);
  }

  live(st => {
    if (Date.now() - lastAppend < interval * 1000) return;
    lastAppend = Date.now();
    for (const [id, c] of Object.entries(charts)) {
      const r = st.sensors[id];
      c.data.push(r && r.v !== null ? +r.v : null);
      if (c.data.length > maxLen) c.data.shift();
      draw(c);
    }
  });
  window.addEventListener('resize', () => Object.values(charts).forEach(draw));
  onConfig(() => build().catch(e => CM.toast(e.message, true)));
  document.addEventListener('DOMContentLoaded', () => build().catch(e => CM.toast(e.message, true)));
})();
