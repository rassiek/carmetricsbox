'use strict';
(() => {
  const { $, h, api, toast, fmt } = CM;
  let names = {}, decs = {}, units = {};

  const when = e => e.epoch ? new Date(e.epoch * 1000).toLocaleString() : `${Math.round(e.uptimeS / 60)} min after start-up (clock not set)`;

  function mini(vals, mark) {
    const ok = vals.filter(v => v !== null);
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('class', 'mini'); svg.setAttribute('viewBox', `0 0 ${vals.length - 1} 100`); svg.setAttribute('preserveAspectRatio', 'none');
    if (ok.length < 2) return svg;
    const lo = Math.min(...ok), hi = Math.max(...ok), span = hi - lo || 1;
    const pts = vals.map((v, i) => v === null ? null : `${i},${(95 - (v - lo) / span * 90).toFixed(1)}`).filter(Boolean);
    svg.innerHTML = `<line x1="${mark}" x2="${mark}" y1="0" y2="100" stroke="#ff4d4f" stroke-width="1" vector-effect="non-scaling-stroke"/>` +
      `<polyline points="${pts.join(' ')}" fill="none" stroke="currentColor" stroke-width="1.5" vector-effect="non-scaling-stroke"/>`;
    return svg;
  }

  async function show(seq, row) {
    document.querySelectorAll('.evt').forEach(r => r.classList.toggle('sel', r === row));
    const e = await api('/api/blackbox/event?seq=' + seq);
    $('#detail').classList.remove('hidden');
    $('#dTitle').textContent = `${e.reason} – ${when(e)}`;
    const rows = [h('tr', {}, h('th', {}, 'Sensor'), h('th', {}, `−${e.before} s … +${e.rows.length - e.before - 1} s`), h('th', {}, 'At alarm'), h('th', {}, 'Min'), h('th', {}, 'Max'))];
    e.ids.forEach((id, k) => {
      const vals = e.rows.map(r => r[k]), ok = vals.filter(v => v !== null), d = decs[id] ?? 1;
      rows.push(h('tr', {}, h('td', {}, names[id] || id), h('td', {}, mini(vals, e.before)),
        h('td', {}, vals[e.before] === null ? '–' : `${fmt(vals[e.before], d)} ${units[id] || ''}`),
        h('td', {}, ok.length ? fmt(Math.min(...ok), d) : '–'), h('td', {}, ok.length ? fmt(Math.max(...ok), d) : '–')));
    });
    $('#dTable').replaceChildren(...rows);
    $('#detail').scrollIntoView({ behavior: 'smooth' });
  }

  async function load() {
    const [cfg, bb] = await Promise.all([api('/api/config'), api('/api/blackbox')]);
    cfg.sensors.forEach(s => { names[s.id] = s.name; decs[s.id] = s.decimals; units[s.id] = s.unit; });
    const t = $('#list');
    if (!bb.events.length) { t.replaceChildren(h('tr', {}, h('td', { class: 'muted' }, bb.enabled ? 'No alarms recorded yet.' : 'Recording is turned off.'))); return; }
    t.replaceChildren(h('tr', {}, h('th', {}, 'What'), h('th', {}, 'When')),
      ...bb.events.map(e => { const r = h('tr', { class: 'evt' }, h('td', {}, e.reason), h('td', {}, when(e))); r.onclick = () => show(e.seq, r).catch(x => toast(x.message, true)); return r; }));
  }

  document.addEventListener('DOMContentLoaded', () => {
    $('#clear').onclick = async () => {
      if (!confirm('Delete all alarm recordings?')) return;
      try { toast((await api('/api/blackbox/clear', { method: 'POST' })).message); $('#detail').classList.add('hidden'); load(); } catch (e) { toast(e.message, true); }
    };
    load().catch(e => toast(e.message, true));
  });
})();
