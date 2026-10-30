'use strict';
(() => {
  const { $, h, api, live, onConfig, fmt, statusText } = CM;
  let cards = {};

  function niceTicks(min, max, n = 5) {
    const raw = (max - min) / n, mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const step = [1, 2, 2.5, 5, 10].map(m => m * mag).find(s => s >= raw) || raw;
    const ticks = [];
    for (let v = Math.ceil(min / step) * step; v <= max + 1e-9; v += step) ticks.push(+v.toFixed(6));
    if (ticks[0] !== min) ticks.unshift(min);
    if (ticks[ticks.length - 1] !== max) ticks.push(max);
    return ticks.map(String);
  }

  function zones(s) {
    const lo = s.gaugeMin, hi = s.gaugeMax, z = [];
    const C = { crit: 'rgba(255,77,79,.85)', warn: 'rgba(244,196,48,.85)', ok: 'rgba(52,199,123,.55)' };
    const cl = s.critLow ?? lo, wl = s.warnLow ?? cl, ch = s.critHigh ?? hi, wh = s.warnHigh ?? ch;
    const add = (a, b, c) => { a = Math.max(lo, a); b = Math.min(hi, b); if (b > a) z.push({ from: a, to: b, color: c }); };
    add(lo, cl, C.crit); add(cl, wl, C.warn); add(wl, wh, C.ok); add(wh, ch, C.warn); add(ch, hi, C.crit);
    return z;
  }

  function makeGauge(s, canvas) {
    const common = {
      renderTo: canvas, units: s.unit, minValue: s.gaugeMin, maxValue: s.gaugeMax,
      majorTicks: niceTicks(s.gaugeMin, s.gaugeMax), minorTicks: 2, highlights: zones(s),
      value: s.gaugeMin, valueBox: false, animationDuration: 400, animationRule: 'linear',
      colorPlate: 'transparent', colorMajorTicks: '#8b9ab0', colorMinorTicks: '#5b6a80',
      colorNumbers: '#c7d2e0', colorUnits: '#8b9ab0', colorTitle: '#8b9ab0',
      colorNeedle: '#3fa7ff', colorNeedleEnd: '#3fa7ff', borders: false, borderShadowWidth: 0,
      fontNumbersSize: 22, fontUnitsSize: 22,
    };
    const isTemp = /°?C$/.test(s.unit) && s.type !== 'bmp';
    if (isTemp) {
      return new LinearGauge(Object.assign(common, {
        width: 100, height: 190, barWidth: 8, colorBar: '#263247', colorBarProgress: '#3fa7ff',
        needleType: 'arrow', needleWidth: 3, tickSide: 'left', numberSide: 'left', needleSide: 'left',
        highlightsWidth: 8, barBeginCircle: false,
      })).draw();
    }
    return new RadialGauge(Object.assign(common, {
      width: 160, height: 160, needleType: 'line', needleWidth: 3, needleCircleSize: 6,
      needleCircleOuter: false, colorNeedleCircleInner: '#3fa7ff', highlightsWidth: 10,
      startAngle: 45, ticksAngle: 270,
    })).draw();
  }

  async function build() {
    const cfg = await api('/api/config');
    const grid = $('#cards');
    grid.innerHTML = '';
    cards = {};
    const sensors = cfg.sensors.filter(s => s.enabled);
    $('#empty').classList.toggle('hidden', sensors.length > 0);
    for (const s of sensors) {
      const canvas = h('canvas');
      const val = h('div', { class: 'value' }, '--');
      const badge = h('span', { class: 'badge pill hidden' });
      const card = h('div', { class: 'card', id: 'card-' + s.id }, h('div', { class: 'title' }, s.name), canvas, val, badge);
      grid.append(card);
      let gauge = null;
      try { gauge = makeGauge(s, canvas); } catch (e) { console.warn('gauge', s.id, e); canvas.remove(); }
      cards[s.id] = { s, card, val, badge, gauge };
    }
    if (CM.state) update(CM.state);
  }

  function update(st) {
    for (const [id, c] of Object.entries(cards)) {
      const r = st.sensors[id];
      if (!r) continue;
      const v = r.v === null ? null : parseFloat(r.v);
      c.val.innerHTML = '';
      c.val.append(fmt(v, c.s.decimals), ' ', h('small', {}, c.s.unit));
      if (c.gauge && v !== null) c.gauge.value = Math.min(Math.max(v, c.s.gaugeMin), c.s.gaugeMax);
      c.card.className = 'card ' + (r.lvl !== 'ok' ? r.lvl : '');
      const txt = statusText[r.st] || '';
      c.badge.textContent = txt;
      c.badge.className = 'badge pill ' + (r.st === 'fault' || r.st === 'not_found' ? 'fault' : '') + (txt ? '' : ' hidden');
    }
  }

  onConfig(() => build().catch(e => CM.toast(e.message, true)));
  document.addEventListener('DOMContentLoaded', () => {
    build().catch(e => CM.toast(e.message, true));
    live(update);
  });
})();
