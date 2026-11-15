'use strict';
// Dashboard tab: Gauges (dials), Charts, Events; weather card; tap a dial to change its limits.
(() => {
  const { $, h, api, get, patch, toast, act, err, S, fmt, conv, page, ui } = CM;
  let view = 'gauges', el = null;
  const spark = {};   // address -> recent values (for "spark" tiles)

  const sensorsCfg = () => (S.settings && S.settings.sensors) || [];
  const addrOf = s => s.type === 'remote' ? `b${s.board}.${s.remoteId}` : `local.${s.id}`;
  function tiles() {
    const lay = (S.settings.layout && S.settings.layout.tiles) || [];
    const byId = Object.fromEntries(sensorsCfg().map(s => [s.id, s]));
    const list = lay.length ? lay.filter(t => byId[t.id]) : sensorsCfg().filter(s => s.enabled).map(s => ({ id: s.id, size: 1, widget: 'bar' }));
    return list.map(t => ({ ...t, s: byId[t.id], a: addrOf(byId[t.id]) }));
  }

  // ---- dial: 240° arc with the warning / critical zones, needle and value
  const A0 = 150, SWEEP = 240;
  const pol = (cx, cy, r, deg) => [cx + r * Math.cos(deg * Math.PI / 180), cy + r * Math.sin(deg * Math.PI / 180)];
  function arc(r, f0, f1, cls) {
    const [x0, y0] = pol(60, 60, r, A0 + SWEEP * f0), [x1, y1] = pol(60, 60, r, A0 + SWEEP * f1);
    return `<path class="${cls}" d="M${x0.toFixed(1)} ${y0.toFixed(1)} A${r} ${r} 0 ${SWEEP * (f1 - f0) > 180 ? 1 : 0} 1 ${x1.toFixed(1)} ${y1.toFixed(1)}"/>`;
  }
  function dialSvg(s) {
    const lo = s.gaugeMin ?? 0, hi = s.gaugeMax ?? 100, f = v => Math.max(0, Math.min(1, (v - lo) / ((hi - lo) || 1)));
    let z = arc(48, 0, 1, 'track');
    if (s.warnHigh != null) z += arc(48, f(s.warnHigh), s.critHigh != null ? f(s.critHigh) : 1, 'zw');
    if (s.critHigh != null) z += arc(48, f(s.critHigh), 1, 'zc');
    if (s.warnLow != null) z += arc(48, s.critLow != null ? f(s.critLow) : 0, f(s.warnLow), 'zw');
    if (s.critLow != null) z += arc(48, 0, f(s.critLow), 'zc');
    return `<svg viewBox="0 0 120 100" class="dial">${z}<line class="needle" x1="60" y1="60" x2="60" y2="18"/><circle cx="60" cy="60" r="4" class="hub"/></svg>`;
  }
  function setDial(tile, s, v) {
    const lo = s.gaugeMin ?? 0, hi = s.gaugeMax ?? 100;
    const fr = v.v == null ? 0 : Math.max(0, Math.min(1, (v.v - lo) / ((hi - lo) || 1)));
    const n = tile.querySelector('.needle');
    if (n) n.setAttribute('transform', `rotate(${(A0 + SWEEP * fr + 90).toFixed(1)} 60 60)`);
  }

  function renderGauges(box) {
    const grid = h('div', { class: 'tiles' });
    for (const t of tiles()) {
      const tile = h('div', { class: `tile size${t.size || 1} w-${t.widget}`, 'data-a': t.a, onclick: () => editLimits(t.s) }, [
        h('div', { class: 'tname' }, t.s.name), t.widget === 'bar' ? h('div', { html: dialSvg(t.s) }) : null,
        t.widget === 'spark' ? h('canvas', { class: 'spark', width: 300, height: 60 }) : null,
        h('div', { class: 'tval' }, [h('span', { class: 'num' }, '--'), ' ', h('span', { class: 'unit' }, '')])]);
      grid.append(tile);
    }
    if (!grid.children.length) grid.append(h('p', { class: 'muted' }, 'No sensors to show yet: add some on the Sensors tab.'));
    box.append(grid, h('div', { class: 'row small' }, [h('a', { href: '#dashboard/layout' }, 'Change tiles and OLED pages')]));
    updateGauges();
  }
  function updateGauges() {
    if (!el) return;
    for (const tile of el.querySelectorAll('.tile')) {
      const a = tile.dataset.a, s = sensorsCfg().find(x => addrOf(x) === a);
      if (!s) continue;
      const v = S.values[a] || { v: null, st: 'waiting', lvl: 'ok' };
      const [txt, u] = fmt(v.v, s.unit, s.decimals);
      tile.querySelector('.num').textContent = v.st === 'ok' || v.st === undefined ? txt : v.st.replace('_', ' ');
      tile.querySelector('.unit').textContent = u;
      tile.className = tile.className.replace(/ lvl-\w+/g, '') + ' lvl-' + (v.lvl || 'ok');
      setDial(tile, s, v);
      const c = tile.querySelector('canvas.spark');
      if (c) drawSpark(c, spark[a] || []);
    }
    const tb = $('#turbo'); if (tb) { const t = S.turbo; tb.classList.toggle('hidden', !t); if (t) tb.textContent = t.phase === 'cooling' ? `Turbo cooling: ${t.leftS} s before switching off` : 'Turbo cool: OK to switch off'; }
  }
  function drawSpark(c, vals) {
    const g = c.getContext('2d'); g.clearRect(0, 0, c.width, c.height);
    const v = vals.filter(x => x != null); if (v.length < 2) return;
    const lo = Math.min(...v), hi = Math.max(...v), r = hi - lo || 1;
    g.strokeStyle = '#3fa7ff'; g.lineWidth = 2; g.beginPath();
    vals.forEach((x, i) => { if (x == null) return; const X = i / (vals.length - 1) * c.width, Y = c.height - 4 - (x - lo) / r * (c.height - 8); i ? g.lineTo(X, Y) : g.moveTo(X, Y); });
    g.stroke();
  }

  // Tap a dial: change its limits (scope "control" is enough for this).
  function editLimits(s) {
    if (s.type === 'remote') return toast('Limits of another board\'s sensor: change them on its Sensors list (choose the board)');
    const o = { warnLow: s.warnLow, warnHigh: s.warnHigh, critLow: s.critLow, critHigh: s.critHigh };
    const f = CM.form(o, [['warnLow', 'Warn below'], ['warnHigh', 'Warn above'], ['critLow', 'Critical below'], ['critHigh', 'Critical above']].map(([k, label]) => ({ k, label, type: 'num' })));
    const dlg = h('div', { class: 'modal' }, [h('div', { class: 'panel' }, [h('h3', {}, `${s.name}: alarm limits (${s.unit})`), f, h('p', { class: 'muted small' }, 'Empty = no limit.'),
      h('div', { class: 'row' }, [h('button', { class: 'primary', onclick: async () => {
        if (await act(() => patch(`/sensors/${s.id}`, { limits: { wl: o.warnLow, wh: o.warnHigh, cl: o.critLow, ch: o.critHigh } }), 'Limits saved')) dlg.remove();
      } }, 'Save'), h('button', { onclick: () => dlg.remove() }, 'Cancel')])])]);
    document.body.append(dlg);
  }

  // ---- charts from the board's history
  // ---- charts: values of the same kind share one chart (same unit; thermocouples apart),
  // a sensor's "Chart" setting puts it on its own or in a named group.
  const PAL = ['#3fa7ff', '#f4a340', '#34c77b', '#e05ad6', '#ffd84d', '#5fd3e6', '#ff6b6b', '#a0a8ff'];
  const SYS = { sys_heap_free_kb: ['Free memory', 'KB', 'mem'], sys_heap_min_kb: ['Lowest free memory', 'KB', 'mem'], sys_cpu0: ['CPU 0', '%', 'cpu'],
                sys_cpu1: ['CPU 1', '%', 'cpu'], sys_loop_max_ms: ['Slowest loop', 'ms', 'loop'], sys_rssi: ['Wi-Fi signal', 'dBm', 'rssi'] };
  const TITLES = { tc: 'Exhaust / thermocouples', '°C': 'Temperatures', '°F': 'Temperatures', psi: 'Pressures', bar: 'Pressures', kPa: 'Pressures',
                   V: 'Voltages', A: 'Currents', m: 'Altitude', 'km/h': 'Speed', mph: 'Speed', kn: 'Speed', rpm: 'Engine speed', RPM: 'Engine speed' };
  // id -> {name, unit (as shown), dec, key, title, s (sensor settings, for limits), kind}
  function seriesInfo(id) {
    const s = sensorsCfg().find(x => x.id === id);
    if (s) {
      if (!s.enabled) return null;
      const unit = fmt(null, s.unit)[1];
      const tc = ['max6675', 'mcp9600'].includes(s.type) || (s.type === 'remote' && /°/.test(s.unit) && (s.gaugeMax || 0) >= 400);
      const g = s.chartGroup || '';
      const key = g === 'own' || (!g && !unit) ? 'own:' + id : g ? 'g:' + g : tc ? 'tc' : 'u:' + unit;
      const title = g === 'own' || key.startsWith('own') ? s.name : g ? g : tc ? TITLES.tc : TITLES[unit] || null;
      return { name: s.name, unit, dec: s.decimals, key, title, s, kind: 'sensor' };
    }
    const a = ((S.settings && S.settings.relays) || []).find(x => x.id === id);
    if (a) {
      if (!a.enabled) return null;
      const unit = a.type === 'relay' ? 'on/off' : a.type === 'servo' ? '°' : '%';
      return { name: a.name, unit, dec: 0, key: 'act:' + unit, title: a.type === 'relay' ? 'Switches' : a.type === 'servo' ? 'Servos' : 'Outputs', kind: 'actuator' };
    }
    const y = SYS[id];
    if (y) return { name: y[0], unit: y[1], dec: 1, key: 'sys:' + y[2], title: `Board: ${y[2] === 'mem' ? 'memory' : y[2] === 'cpu' ? 'CPU' : y[0].toLowerCase()}`, kind: 'system' };
    return null;
  }
  function groupSeries(ids) {
    const groups = new Map();
    for (const id of ids) {
      const i = seriesInfo(id); if (!i) continue;
      if (!groups.has(i.key)) groups.set(i.key, { key: i.key, title: i.title, unit: i.unit, kind: i.kind, items: [] });
      groups.get(i.key).items.push({ id, ...i });
    }
    for (const g of groups.values()) if (!g.title) g.title = g.items.map(x => x.name).join(', ');
    return [...groups.values()];
  }
  function chartPanel(g, valsOf, step, height = 170) {
    const c = h('canvas', { class: 'chart', width: 900, height });
    const legend = g.items.length > 1 ? h('div', { class: 'chips' }, g.items.map((x, k) => h('span', { class: 'chip' }, [h('i', { style: `background:${PAL[k % PAL.length]}` }), x.name]))) : null;
    const p = h('div', { class: 'panel' }, [h('div', { class: 'row' }, [h('b', {}, g.title), h('span', { class: 'muted small' }, g.unit)]), legend, c]);
    drawChart(c, g.items.map((x, k) => ({ vals: valsOf(x), color: PAL[k % PAL.length], s: x.s, dec: x.dec })), step);
    return p;
  }
  async function renderCharts(box) {
    let d;
    try { d = await get('/history'); } catch (e) { return err(e); }
    box.append(h('p', { class: 'muted small' }, `Last ${Math.round(d.count * d.interval / 60)} min, one point every ${d.interval} s. Values of the same kind share a chart; a sensor's "Chart" setting can put it on its own.`));
    const valsOf = x => d.series[x.id].map(v => v == null ? null : x.kind === 'sensor' ? conv(v, x.s.unit)[0] : v);
    const groups = groupSeries(Object.keys(d.series || {}));
    for (const g of groups.filter(g => g.kind !== 'system')) box.append(chartPanel(g, valsOf, d.interval));
    const sys = groups.filter(g => g.kind === 'system');
    if (sys.length) box.append(h('details', {}, [h('summary', {}, 'Board health'), ...sys.map(g => chartPanel(g, valsOf, d.interval, 130))]));
  }
  // series: [{vals, color, s (limits, single series only), dec}]
  function drawChart(c, series, step) {
    const g = c.getContext('2d'), W = c.width, H = c.height, L = 48;
    const all = series.flatMap(x => x.vals.filter(v => v != null));
    const n = Math.max(...series.map(x => x.vals.length));
    if (all.length < 2) { g.fillStyle = '#8b9ab0'; g.font = '16px sans-serif'; g.fillText('No data yet', L, H / 2); return; }
    let lo = Math.min(...all), hi = Math.max(...all); if (hi - lo < 1) { lo -= 0.5; hi += 0.5; }
    const dec = Math.max(0, Math.min(2, series[0].dec ?? 1));
    const Y = x => H - 18 - (x - lo) / (hi - lo) * (H - 28), X = i => L + i / Math.max(1, n - 1) * (W - L - 4);
    g.font = '13px sans-serif'; g.fillStyle = '#8b9ab0'; g.fillText(hi.toFixed(dec), 2, 14); g.fillText(lo.toFixed(dec), 2, H - 18);
    g.fillText(`-${Math.round(n * step / 60)} min`, L, H - 2); g.fillText('now', W - 30, H - 2);
    const s = series.length === 1 && series[0].s;
    if (s) for (const [lim, col] of [[s.warnHigh, '#f4c430'], [s.critHigh, '#ff4d4f'], [s.warnLow, '#f4c430'], [s.critLow, '#ff4d4f']]) {
      if (lim == null) continue; const y = Y(conv(lim, s.unit)[0]); if (y < 0 || y > H) continue;
      g.strokeStyle = col; g.setLineDash([4, 4]); g.beginPath(); g.moveTo(L, y); g.lineTo(W, y); g.stroke(); g.setLineDash([]);
    }
    for (const x of series) {
      g.strokeStyle = x.color; g.lineWidth = 2; g.beginPath(); let pen = false;
      x.vals.forEach((v, i) => { if (v == null) { pen = false; return; } pen ? g.lineTo(X(i), Y(v)) : g.moveTo(X(i), Y(v)); pen = true; });
      g.stroke();
    }
  }

  // ---- events (black box)
  async function renderEvents(box) {
    let d; try { d = await get('/events'); } catch (e) { return err(e); }
    box.append(h('p', { class: 'muted small' }, `Each warning or critical alarm records 30 s before and after it (${d.enabled ? `keeps the last ${d.keep}` : 'recording is off'}).`));
    if (!d.items.length) box.append(h('p', { class: 'muted' }, 'No events recorded.'));
    for (const e of d.items) {
      const when = e.epoch > 1e9 ? new Date(e.epoch * 1000).toLocaleString() : `${Math.round(e.uptimeS / 60)} min after start`;
      const body = h('div');
      box.append(h('div', { class: 'panel' }, [h('div', { class: 'row' }, [h('b', {}, e.reason || 'event'), h('span', { class: 'muted small' }, when), h('span', { class: 'spacer' }),
        h('button', { class: 'small', onclick: async () => {
          if (body.children.length) { body.innerHTML = ''; return; }
          try {
            const ev = await get(`/events/${e.seq}`);
            for (const g of groupSeries(ev.ids)) body.append(chartPanel(g, x => ev.rows.map(r => { const v = r[ev.ids.indexOf(x.id)]; return v == null ? null : x.kind === 'sensor' ? conv(v, x.s.unit)[0] : v; }), ev.interval, 130));
          } catch (x) { err(x); }
        } }, 'Show')]), body]));
    }
    if (d.items.length) box.append(h('button', { class: 'danger small', onclick: () => confirm('Delete all recordings?') && act(() => CM.del('/events')).then(() => show(el)) }, 'Delete all'));
  }

  // ---- weather card
  async function weatherCard(box) {
    if (!S.settings.weather || !S.settings.weather.enabled) return;
    try {
      const w = await get('/weather');
      const code = c => c == null ? '' : c === 0 ? 'clear' : c <= 3 ? 'partly cloudy' : c <= 48 ? 'fog' : c <= 67 ? 'rain' : c <= 77 ? 'snow' : c <= 82 ? 'showers' : 'thunderstorm';
      const card = h('div', { class: 'panel weather' });
      for (const t of (w.towns || []).slice(0, 4)) {
        if (t.temp == null) continue;
        card.append(h('div', { class: 'wrow' }, [h('b', {}, t.name), h('span', { class: 'wtemp' }, fmt(t.temp, '°C', 0).join(' ')), h('span', { class: 'muted small' }, code(t.code)),
          t.wind != null ? h('span', { class: 'muted small' }, `wind ${fmt(t.wind, 'km/h', 0).join(' ')}`) : null, t.rh != null ? h('span', { class: 'muted small' }, `${Math.round(t.rh)} %`) : null]));
      }
      if (card.children.length) box.prepend(card);
    } catch {}
  }

  function show(main, sub) {
    el = main;
    if (sub === 'layout') return CM.layoutEditor(main);
    if (['gauges', 'charts', 'events'].includes(sub)) view = sub;
    main.innerHTML = '';
    main.append(h('div', { id: 'turbo', class: 'turbo hidden' }));
    const views = h('div', { class: 'seg' }, [['gauges', 'Gauges'], ['charts', 'Charts'], ['events', 'Events']].map(([k, t]) =>
      h('a', { href: `#dashboard/${k}`, class: view === k ? 'active' : '' }, t)));
    const box = h('div');
    main.append(views, box);
    if (view === 'gauges') { renderGauges(box); weatherCard(box); }
    else if (view === 'charts') renderCharts(box);
    else renderEvents(box);
  }

  CM.live(() => {
    for (const [a, v] of Object.entries(S.values)) { const l = spark[a] ||= []; l.push(v.v); if (l.length > 120) l.shift(); }
    if (view === 'gauges') updateGauges();
  });
  page('dashboard', { show, leave: () => { el = null; }, onSettings: () => { if (el && view === 'gauges' && !document.querySelector('.modal')) show(el); } });
})();
