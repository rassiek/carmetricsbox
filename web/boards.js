'use strict';
(() => {
  const { $, h, api, toast } = CM;
  const dur = s => s < 90 ? s + ' s' : s < 5400 ? Math.round(s / 60) + ' min' : (s / 3600).toFixed(1) + ' h';
  const sig = r => r ? `${r} dBm` : '–';
  const fmt = (v, d) => v === null || v === undefined ? '–' : Number(v).toFixed(d);
  const lvlCls = { ok: 'ok', warn: 'warn', crit: 'crit', fault: 'fault' };
  let names = {};   // found boards: name typed so far (kept across refreshes)

  async function accept(mac) {
    try { toast((await api('/api/v2/boards/accept', { body: { mac, name: names[mac] || '' } })).message, false, 6000); load(); }
    catch (e) { toast(e.message, true, 6000); }
  }
  async function forget(b) {
    if (!confirm(`Forget "${b.name}"? It goes back to unpaired and loses the settings sent to it.`)) return;
    try { toast((await api('/api/v2/boards/forget', { body: { id: b.id } })).message); load(); } catch (e) { toast(e.message, true); }
  }
  async function relay(b, index, on) {
    try { await api('/api/v2/boards/relay', { body: { id: b.id, index, on } }); setTimeout(load, 700); } catch (e) { toast(e.message, true); }
  }

  function renderNode(d) {
    const n = d.node;
    $('#nodePanel').classList.remove('hidden');
    $('#intro').textContent = `This is an extra board (role: ${d.role.replace('_', ' + ')}). It finds the main board by itself over ESP-NOW.`;
    const tiles = [
      ['Paired', n.paired ? `yes, board ${n.id}` : 'not yet', n.paired ? '' : 'press Accept on the main board\'s Boards page'],
      ['Link to main board', n.linkUp ? 'up' : 'down', n.mainInRange ? 'main board in range' : 'searching channels…'],
      ['Radio channel', String(d.link.channel || '–'), 'follows the main board'],
      ['Settings', n.configRev ? 'received' : 'none yet', n.configRev ? 'rev ' + n.configRev.toString(16) : 'set up on the main board'],
      ['Messages', `${d.link.rx} in / ${d.link.tx} out`, d.link.txFail ? `${d.link.txFail} failed` : ''],
    ];
    $('#nodeKv').replaceChildren(...tiles.map(([a, b, c]) => h('div', {}, h('span', {}, a), h('b', {}, b), h('span', {}, c))));
  }

  function renderMain(d) {
    $('#intro').textContent = 'Extra boards (engine bay sensors, relays near the loads) join this one over ESP-NOW. ' +
      'This board stays the only web app: their sensors and relays are set up on the Sensors and Relays pages with the board selector.';
    $('#foundPanel').classList.toggle('hidden', !d.found.length);
    $('#found').replaceChildren(...d.found.map(f => {
      const name = h('input', { value: names[f.mac] ?? f.name, placeholder: 'Name, e.g. Engine bay', style: 'max-width:200px' });
      name.oninput = () => { names[f.mac] = name.value; };
      return h('div', { class: 'row', style: 'gap:10px;flex-wrap:wrap;margin:6px 0' },
        h('b', {}, f.name || f.mac), h('span', { class: 'muted small' }, `${f.role}, ${f.chip}, ${f.firmware}, ${sig(f.rssi)}, ${f.mac}`),
        h('span', { class: 'spacer' }), name, h('button', { class: 'primary', onclick: () => accept(f.mac) }, 'Accept'));
    }));
    $('#noBoards').classList.toggle('hidden', d.boards.length > 0 || d.found.length > 0);
    $('#boards').replaceChildren(...d.boards.map(b => {
      const st = h('span', { class: 'pill ' + (b.online ? 'ok' : 'warn') }, b.online ? 'online' : (b.ageS < 0 ? 'not seen yet' : `offline ${dur(b.ageS)}`));
      const lvl = h('span', { class: 'pill ' + (lvlCls[b.level] || '') }, b.level === 'ok' ? 'OK' : b.level.toUpperCase());
      const c = b.config;
      const cfgText = !c.hasShare ? 'no settings yet' : c.pending ? 'sending settings…' : c.status === 'accepted' ? (c.upToDate ? 'settings up to date' : 'settings accepted') : `settings REFUSED: ${c.message}`;
      const sensors = b.points.filter(p => !p.relay), relays = b.points.filter(p => p.relay);
      const rows = sensors.map(p => h('tr', {}, h('td', {}, p.name), h('td', { style: 'text-align:right' }, p.status === 'ok' ? `${fmt(p.value, p.decimals)} ${p.unit}` : p.status),
        h('td', {}, p.level && p.level !== 'ok' ? h('span', { class: 'pill ' + (lvlCls[p.level] || '') }, p.level) : '')));
      const relayBtns = relays.map((p, i) => {
        const on = p.value === 1;
        return h('button', { class: on ? 'primary' : '', disabled: !b.online, onclick: () => relay(b, i, !on) }, `${p.name}: ${on ? 'ON' : 'OFF'}`);
      });
      return h('div', { class: 'panel' },
        h('div', { class: 'row', style: 'gap:8px;flex-wrap:wrap' }, h('h3', { style: 'margin:0' }, `${b.name} `, h('span', { class: 'muted small' }, `board ${b.id}`)),
          h('span', { class: 'spacer' }), lvl, st),
        h('p', { class: 'muted small' }, `${b.role}, ${b.chip}, firmware ${b.firmware || '?'} · signal ${sig(b.rssi)} here, ${sig(b.rssiAtBoard)} there · ` +
          `up ${dur(b.uptimeS)} · ${b.lost} of ${b.received + b.lost} messages lost` + (b.relaysSafe ? ' · relays in safe state (link was lost)' : '') + (b.safeMode ? ' · SAFE MODE' : '')),
        h('p', { class: 'small', style: c.status === 'refused' ? 'color:var(--crit)' : '' }, cfgText),
        sensors.length ? h('table', { class: 'small' }, rows) : h('p', { class: 'muted small' }, 'No sensors on this board.'),
        relayBtns.length ? h('div', { class: 'row', style: 'gap:8px;flex-wrap:wrap;margin-top:8px' }, relayBtns) : '',
        h('div', { class: 'row', style: 'margin-top:10px;gap:8px' },
          h('a', { href: `settings.html?board=${b.id}` }, h('button', {}, 'Sensors…')), h('a', { href: `relays.html?board=${b.id}` }, h('button', {}, 'Relays…')),
          h('span', { class: 'spacer' }), h('button', { class: 'small danger', onclick: () => forget(b) }, 'Forget')));
    }));
  }

  async function load() {
    const d = await api('/api/v2/boards');
    const l = d.link;
    $('#linkPill').textContent = l.ok ? `${l.type}, channel ${l.channel}` : 'radio off';
    $('#linkPill').className = 'pill ' + (l.ok ? 'ok' : 'warn');
    if (d.role !== 'main') renderNode(d); else renderMain(d);
  }

  document.addEventListener('DOMContentLoaded', () => {
    const tick = () => { if (document.activeElement?.tagName !== 'INPUT') load().catch(e => toast(e.message, true)); };
    tick(); setInterval(tick, 2000);
  });
})();
