'use strict';
// Dashboard tab: Gauges (dials), Charts, Events; weather card; tap a dial to change its limits.
(() => {
  const { $, h, api, get, patch, toast, act, err, S, fmt, conv, page, ui, has } = CM;
  let view = 'gauges', el = null;
  // Gauges of all boards, or one board only: 'all' | 'local' | 'b<id>' (kept for this browser tab)
  let only = (() => { try { return sessionStorage.getItem('cmGauges') || 'all'; } catch { return 'all'; } })();
  const spark = {};   // address -> recent values (for "spark" tiles)

  const sensorsCfg = () => (S.settings && S.settings.sensors) || [];
  const addrOf = s => s.type === 'remote' ? `b${s.board}.${s.remoteId}` : `local.${s.id}`;
  function tiles() {
    const lay = (S.settings.layout && S.settings.layout.tiles) || [];
    const byId = Object.fromEntries(sensorsCfg().map(s => [s.id, s]));
    const boardOf = s => s.type === 'remote' ? 'b' + s.board : 'local';
    const keep = s => only === 'all' || boardOf(s) === only;
    const list = (lay.length ? lay : CM.autoTiles(sensorsCfg())).filter(t => byId[t.id] && (t.widget === 'bars'
      ? [t.id].concat((t.with || '').split(',')).some(id => byId[id] && keep(byId[id])) : keep(byId[t.id])));
    return list.map(t => ({ ...t, s: byId[t.id], a: addrOf(byId[t.id]),
      group: t.widget === 'bars' ? [t.id].concat((t.with || '').split(',').filter(x => byId[x])).map(id => byId[id]).filter(keep) : null }));
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

  // Thermometer group: several sensors as vertical bars on one tile (gearbox, transfer case...)
  function barsTile(t) {
    const tile = h('div', { class: `tile size${t.size || 1} w-bars`, 'data-group': t.group.map(addrOf).join(',') }, [
      h('div', { class: 'tname' }, t.group.length > 3 ? 'Temperatures' : t.group.map(s => s.oledLabel || s.name).join(' · ')),
      h('div', { class: 'thermos' }, t.group.map(s => {
        const lo = s.gaugeMin ?? 0, hi = s.gaugeMax ?? 100, f = v => `${Math.max(0, Math.min(100, (v - lo) / ((hi - lo) || 1) * 100)).toFixed(1)}%`;
        // Each sensor's own limits: coloured zones on the tube, a mark and the value at each limit
        const pct = v => Math.max(0, Math.min(100, (v - lo) / ((hi - lo) || 1) * 100));
        const zone = (a, b, c) => h('i', { class: 'zone ' + c, style: `bottom:${pct(a)}%;height:${Math.max(0, pct(b) - pct(a))}%` });
        const ticks = [];
        if (s.warnHigh != null) ticks.push(zone(s.warnHigh, s.critHigh ?? hi, 'w'));
        if (s.critHigh != null) ticks.push(zone(s.critHigh, hi, 'c'));
        if (s.warnLow != null) ticks.push(zone(s.critLow ?? lo, s.warnLow, 'w'));
        if (s.critLow != null) ticks.push(zone(lo, s.critLow, 'c'));
        for (const [v, c] of [[s.warnHigh, 'w'], [s.critHigh, 'c'], [s.warnLow, 'w'], [s.critLow, 'c']].filter(x => x[0] != null))
          ticks.push(h('i', { class: 'tk ' + c, style: `bottom:${f(v)}` }), h('span', { class: 'tkl ' + c, style: `bottom:${f(v)}` }, fmt(v, s.unit, 0)[0]));
        return h('div', { class: 'thermo', 'data-a': addrOf(s), onclick: e => { e.stopPropagation(); if (!document.body.classList.contains('fs')) editLimits(s); } }, [
          h('div', { class: 'tv' }, '--'), h('div', { class: 'tube' }, [h('div', { class: 'fill' }), ...ticks]), h('div', { class: 'tl' }, s.oledLabel || s.name)]);
      })), h('div', { class: 'why' })]);
    return tile;
  }
  function renderGauges(box) {
    const grid = h('div', { class: 'tiles' });
    for (const t of tiles()) {
      if (t.widget === 'bars') { grid.append(barsTile(t)); continue; }
      const tile = h('div', { class: `tile size${t.size || 1} w-${t.widget}`, 'data-a': t.a, onclick: () => { if (!document.body.classList.contains('fs')) editLimits(t.s); } }, [
        h('div', { class: 'tname' }, t.s.name), t.widget === 'bar' ? h('div', { html: dialSvg(t.s) }) : null,
        t.widget === 'spark' ? h('canvas', { class: 'spark', width: 300, height: 60 }) : null,
        h('div', { class: 'tval' }, [h('span', { class: 'num' }, '--'), ' ', h('span', { class: 'unit' }, '')]), h('div', { class: 'why' })]);
      grid.append(tile);
    }
    if (!grid.children.length) grid.append(h('p', { class: 'muted' }, 'No sensors to show yet: add some on the Sensors tab.'));
    box.append(grid);
    updateGauges();
    fitTiles();
  }
  // A value, or "--" with a short reason (the tile stays readable when nothing is connected)
  const SHORT = { waiting: 'waiting', not_found: 'no sensor', unassigned: 'not set up', fault: 'wiring fault', disabled: 'off', offline: 'board offline', stale: 'no data' };
  const okSt = v => v.st === 'ok' || v.st === undefined;
  function valueText(s, v) { const [txt, u] = fmt(v.v, s.unit, s.decimals); return okSt(v) ? [txt, u] : ['--', '']; }
  const whyNot = v => okSt(v) ? '' : (SHORT[v.st] || String(v.st).replace(/_/g, ' '));
  function updateGauges() {
    if (!el) return;
    const tbWas = $('#turbo') && $('#turbo').classList.contains('hidden');
    for (const tile of el.querySelectorAll('.tile[data-a]')) {
      const a = tile.dataset.a, s = sensorsCfg().find(x => addrOf(x) === a);
      if (!s) continue;
      const v = S.values[a] || { v: null, st: 'waiting', lvl: 'ok' };
      const [txt, u] = valueText(s, v);
      tile.querySelector('.num').textContent = txt;
      tile.querySelector('.unit').textContent = u;
      const why = tile.querySelector('.why'); if (why) why.textContent = whyNot(v);
      tile.className = tile.className.replace(/ lvl-\w+/g, '') + ' lvl-' + (v.lvl || 'ok');
      setDial(tile, s, v);
      const c = tile.querySelector('canvas.spark');
      if (c) drawSpark(c, spark[a] || []);
    }
    const order = { ok: 0, warn: 1, fault: 2, crit: 3 };
    for (const tile of el.querySelectorAll('.tile.w-bars')) {
      let worst = 'ok';
      for (const th of tile.querySelectorAll('.thermo')) {
        const a = th.dataset.a, s = sensorsCfg().find(x => addrOf(x) === a); if (!s) continue;
        const v = S.values[a] || { v: null, st: 'waiting', lvl: 'ok' };
        const [txt, u] = valueText(s, v);
        th.querySelector('.tv').textContent = okSt(v) ? `${txt}${u.startsWith('°') ? '°' : ' ' + u}` : '--';
        th.title = whyNot(v);
        const lo = s.gaugeMin ?? 0, hi = s.gaugeMax ?? 100;
        th.querySelector('.fill').style.height = v.v == null ? '0' : `${Math.max(0, Math.min(100, (v.v - lo) / ((hi - lo) || 1) * 100))}%`;
        th.className = 'thermo lvl-' + (v.lvl || 'ok') + (okSt(v) ? '' : ' none');
        if (order[v.lvl] > order[worst]) worst = v.lvl;
      }
      tile.className = tile.className.replace(/ lvl-\w+/g, '') + ' lvl-' + worst;
      const why = tile.querySelector('.why');   // one short line for the tubes without a reading
      if (why) { const none = [...tile.querySelectorAll('.thermo.none')]; why.textContent = !none.length ? ''
        : none.length === 1 ? `${none[0].querySelector('.tl').textContent}: ${none[0].title}` : `${none.length} without a reading`; }
    }
    const tb = $('#turbo'); if (tb) { const t = S.turbo; tb.classList.toggle('hidden', !t); if (t) tb.textContent = t.phase === 'cooling' ? `Turbo cooling: ${t.leftS} s before switching off` : 'Turbo cool: OK to switch off'; if (tbWas !== tb.classList.contains('hidden')) fitTiles(); }
  }

  // ---- full screen: only the tiles, sized to fill the screen (2 columns upright, 3 sideways)
  let wake = null;
  function fitTiles() {
    const grid = el && el.querySelector('.tiles'); if (!grid) return;
    if (!document.body.classList.contains('fs')) { grid.style.cssText = ''; return; }
    const W = innerWidth, cols = W > innerHeight ? 3 : 2;
    const H = innerHeight - Math.max(0, grid.getBoundingClientRect().top + scrollY);   // below the turbo banner, if shown
    const spans = [...grid.children].reduce((n, t) => n + (t.classList.contains('size2') ? 2 : 1), 0);
    const rows = Math.max(1, Math.ceil(spans / cols));
    grid.style.cssText = `grid-template-columns:repeat(${cols},1fr);grid-auto-rows:${Math.max(60, Math.floor((H - 6 - 6 * (rows - 1)) / rows))}px;gap:6px`;
  }
  async function fullScreen(on) {
    document.body.classList.toggle('fs', on);
    try {
      if (on && document.documentElement.requestFullscreen && !document.fullscreenElement) await document.documentElement.requestFullscreen();
      if (!on && document.fullscreenElement) await document.exitFullscreen();
    } catch {}
    try { if (on && navigator.wakeLock) wake = await navigator.wakeLock.request('screen'); else if (!on && wake) { wake.release(); wake = null; } } catch {}
    fitTiles();
  }
  document.addEventListener('fullscreenchange', () => { if (!document.fullscreenElement && document.body.classList.contains('fs')) fullScreen(false); });
  window.addEventListener('resize', fitTiles);
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

  // Board filter: shown when gauges come from more than one board (main board with workers)
  function boardFilter() {
    const ids = [...new Set(sensorsCfg().filter(s => s.type === 'remote').map(s => 'b' + s.board))].sort();
    if (!ids.length) { only = 'all'; return null; }
    const sel = h('select', { class: 'small', style: 'width:auto', title: 'Show the gauges of', onchange: e => {
      only = e.target.value; try { sessionStorage.setItem('cmGauges', only); } catch {} show(el); } },
      [['all', 'All boards'], ['local', 'This board']].concat(ids.map(b => [b, b])).map(([v, t]) => h('option', { value: v, selected: v === only }, t)));
    if (![...sel.options].some(o => o.value === only)) { only = 'all'; sel.value = 'all'; }
    get('/boards').then(r => { for (const x of r.items || []) { const o = [...sel.options].find(o => o.value === 'b' + x.id); if (o) o.textContent = x.name; } }).catch(() => {});
    return sel;
  }

  function show(main, sub) {
    el = main;
    if (sub === 'layout') return CM.layoutEditor(main);
    const VIEWS = [['gauges', 'Gauges'], ['charts', 'Charts', 'history'], ['events', 'Events', 'events']].filter(x => !x[2] || has(x[2]));
    if (VIEWS.some(x => x[0] === sub)) view = sub;
    if (!VIEWS.some(x => x[0] === view)) view = 'gauges';
    main.innerHTML = '';
    main.append(h('div', { id: 'turbo', class: 'turbo hidden' }));
    const views = VIEWS.length > 1 ? h('div', { class: 'seg' }, VIEWS.map(([k, t]) =>
      h('a', { href: `#dashboard/${k}`, class: view === k ? 'active' : '' }, t))) : null;
    const box = h('div');
    const tools = view === 'gauges' ? h('div', { class: 'row gtools' }, [boardFilter(), h('button', { class: 'small', onclick: () => fullScreen(true) }, '⛶ Full screen'),
      h('a', { class: 'btn small', href: '#dashboard/layout' }, 'Edit layout')]) : null;
    main.append(h('div', { class: 'row' }, [views, h('span', { class: 'spacer' }), tools]), box,
      h('button', { class: 'fsexit', onclick: () => fullScreen(false), title: 'Leave full screen' }, '✕'));
    if (view === 'gauges') { renderGauges(box); if (has('weather')) weatherCard(box); }
    else if (view === 'charts') renderCharts(box);
    else renderEvents(box);
  }

  CM.live(() => {
    for (const [a, v] of Object.entries(S.values)) { const l = spark[a] ||= []; l.push(v.v); if (l.length > 120) l.shift(); }
    if (view === 'gauges') updateGauges();
  });
  page('dashboard', { show, leave: () => { if (document.body.classList.contains('fs')) fullScreen(false); el = null; }, onSettings: () => { if (el && view === 'gauges' && !document.querySelector('.modal')) show(el); } });
})();
