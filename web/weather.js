'use strict';
(() => {
  const { $, h, api, toast, fmt } = CM;
  let cfg = null;
  const DIRS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];
  const dir = d => (d == null ? '' : DIRS[Math.round(d / 45) % 8]);
  const ago = s => (s < 90 ? `${s}s` : s < 5400 ? `${Math.round(s / 60)} min` : `${Math.round(s / 3600)} h`);

  function renderSettings() {
    const w = cfg.weather, chosen = new Set(w.towns.map(t => t.name));
    $('#wEnabled').checked = w.enabled; $('#wAuto').checked = w.autoSeaLevel; $('#wEvery').value = w.refreshMinutes;
    const box = $('#wTowns');
    box.innerHTML = '';
    for (const t of KENYA_TOWNS) {
      const cb = h('input', { type: 'checkbox', value: t.name, checked: chosen.has(t.name) });
      cb.onchange = refSelect;
      box.append(h('label', { class: 'check' }, cb, ' ', t.name));
    }
    refSelect();
  }
  function selectedTowns() { return [...document.querySelectorAll('#wTowns input:checked')].map(i => KENYA_TOWNS.find(t => t.name === i.value)); }
  function refSelect() {
    const sel = $('#wRef'), cur = sel.value || cfg.weather.refTown;
    sel.innerHTML = '';
    sel.append(h('option', { value: '' }, '— none —'), ...selectedTowns().map(t => h('option', { value: t.name, selected: t.name === cur }, t.name)));
  }

  async function save() {
    const towns = selectedTowns();
    if (towns.length > 12) { toast('Choose at most 12 towns', true); return; }
    try {
      const r = await api('/api/config/weather', { body: { enabled: $('#wEnabled').checked, autoSeaLevel: $('#wAuto').checked,
        refreshMinutes: +$('#wEvery').value, refTown: $('#wRef').value, towns } });
      toast(r.message);
      setTimeout(load, 4000);
    } catch (e) { toast(e.message, true, 6000); }
  }

  async function load() {
    const w = await api('/api/weather');
    const st = $('#wstatus');
    if (!w.enabled) st.textContent = 'Weather is turned off.';
    else if (!w.sta) st.innerHTML = 'Offline – weather needs internet. Set <b>Also join network</b> on the <a href="system.html">System</a> page (e.g. your phone hotspot).';
    else if (w.error) st.textContent = 'Online, but the last fetch failed: ' + w.error;
    else st.textContent = w.ageS >= 0 ? `Online · updated ${ago(w.ageS)} ago` : 'Online · fetching…';
    const sl = w.seaLevel;
    $('#sealevel').textContent = `Altimeter sea-level reference: ${fmt(sl.hpa, 1)} hPa (${sl.source === 'weather' ? 'from ' + sl.town + ' weather' : 'manual – set on the System page'})`;
    const box = $('#wcards');
    box.innerHTML = '';
    for (const t of w.towns) {
      const row = (k, v) => h('div', { class: 'wrow' }, h('span', { class: 'muted' }, k), h('b', {}, v));
      box.append(h('div', { class: 'panel', style: 'margin:0' },
        h('div', { class: 'row' }, h('h3', { style: 'margin:0' }, t.name, cfg && t.name === cfg.weather.refTown ? ' ★' : ''),
          h('span', { class: 'spacer' }), h('span', { class: 'muted small' }, t.time ? t.time.slice(11, 16) : '')),
        h('div', { class: 'wtemp' }, `${fmt(t.temp, 1)}°C`, h('span', { class: 'muted small' }, ` ${WMO[t.code] || ''}`)),
        row('Feels like', `${fmt(t.feels, 1)} °C`), row('Humidity', `${fmt(t.rh, 0)} %`),
        row('Wind', `${fmt(t.wind, 0)} km/h ${dir(t.windDir)} (gusts ${fmt(t.gust, 0)})`),
        row('Rain', `${fmt(t.precip, 1)} mm`), row('Cloud', `${fmt(t.cloud, 0)} %`),
        row('Sea-level pressure', `${fmt(t.msl, 1)} hPa`), row('Surface pressure', `${fmt(t.sfc, 1)} hPa`),
        row('Elevation', `${fmt(t.elevation, 0)} m`)));
    }
    if (w.sta && !w.towns.length && !w.error) box.append(h('p', { class: 'muted' }, 'No data yet.'));
  }

  document.addEventListener('DOMContentLoaded', async () => {
    try { cfg = await api('/api/config'); renderSettings(); } catch (e) { toast(e.message, true); }
    load().catch(e => toast(e.message, true));
    setInterval(() => load().catch(() => {}), 30000);
    $('#refresh').onclick = async () => { await api('/api/weather/refresh', { method: 'POST' }).catch(() => {}); toast('Refreshing…'); setTimeout(load, 5000); };
    $('#wSave').onclick = save;
  });
})();
