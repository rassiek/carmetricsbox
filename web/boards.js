'use strict';
// Boards tab: other boards (workers, displays): pair, rename, commands, firmware, live values.
(() => {
  const { h, api, get, patch, del, toast, act, err, S, fmt, page } = CM;
  let timer = null, el = null, openFw = null;

  const sig = r => r == null || r === 0 ? '' : r > -60 ? 'strong' : r > -75 ? 'good' : r > -85 ? 'weak' : 'very weak';
  function boardCard(b, d) {
    const q = `/boards/${b.id}`;
    const cmd = (c, m) => act(() => api(`${q}/command`, { body: { cmd: c, minutes: m } }));
    const cfg = b.config || {};
    const state = !b.online ? 'offline' : cfg.pending ? 'sending settings' : cfg.status === 'refused' ? 'settings refused' : cfg.upToDate ? 'up to date' : 'online';
    const fw = b.fw;
    const fwBox = h('div', { class: 'small' + (openFw === b.id || fw ? '' : ' hidden') }, [
      fw ? h('div', { class: 'row' }, [`Firmware ${fw.version}: ${fw.state}`, fw.state === 'sending' ? h('div', { class: 'progress' }, h('div', { style: `width:${fw.percent}%` })) : null,
        fw.state === 'sending' ? `${fw.percent} %` : null, fw.error ? h('span', { class: 'bad' }, fw.error) : null,
        ['offering', 'sending'].includes(fw.state) ? h('button', { class: 'small', onclick: () => act(() => api(`${q}/firmware`, { body: { source: 'cancel' } })) }, 'Cancel') : null]) : null,
      b.sameChip ? h('button', { class: 'small', onclick: () => act(() => api(`${q}/firmware`, { body: { source: 'self' } })) }, `Send this board's firmware (${S.info.firmware})`) : h('span', { class: 'muted' }, `A ${b.chip}: needs its own file. `),
      h('label', {}, 'Or send a firmware file (…-firmware.bin):'),
      h('input', { type: 'file', accept: '.bin', onchange: async e => {
        const f = e.target.files[0]; if (!f) return;
        toast('Uploading…', false, 60000);
        if (await act(() => api('/boards/firmware-file', { method: 'PUT', raw: f }))) act(() => api(`${q}/firmware`, { body: { source: 'file' } }));
      } })]);
    const pts = (b.points || []).map(p => h('span', { class: 'chip' + (p.level && p.level !== 'ok' ? ' ' + p.level : '') },
      `${p.name}: ${p.relay ? (p.value ? 'on' : 'off') : fmt(p.value, p.unit, p.decimals).join(' ')}`));
    return h('div', { class: 'panel board ' + (b.online ? '' : 'off') }, [
      h('div', { class: 'row' }, [h('b', {}, `${b.name}`), h('span', { class: 'muted small' }, `b${b.id} · ${b.role} · ${b.chip} · ${b.firmware}`), h('span', { class: 'spacer' }),
        h('span', { class: 'pill ' + (b.online ? 'ok' : 'crit') }, state)]),
      h('div', { class: 'muted small' }, b.online ? `signal ${b.rssi} dBm (${sig(b.rssi)}) · loss ${b.lossPct} % · up ${Math.round(b.uptimeS / 60)} min${b.relaysSafe ? ' · outputs in safe state' : ''}${b.safeMode ? ' · SAFE MODE' : ''}${b.encrypted ? ' · encrypted' : ''}`
                                              : `last heard ${b.ageS < 0 ? 'never' : b.ageS + ' s ago'}`),
      cfg.status === 'refused' ? h('div', { class: 'bad small' }, `Settings refused: ${cfg.message}`) : null,
      b.display ? h('div', { class: 'small muted' }, `Display: ${b.display.sensors} values, ${b.display.relays} switches${b.display.ip ? ' · ' + b.display.ip : ''}`) : null,
      h('div', { class: 'chips' }, pts),
      h('div', { class: 'row', style: 'margin-top:8px' }, [
        b.role !== 'display' ? h('a', { class: 'btn small', href: '#sensors', onclick: () => sessionStorage.setItem('cmBoard', 'b' + b.id) }, 'Sensors') : null,
        b.role !== 'display' ? h('a', { class: 'btn small', href: '#actuators', onclick: () => sessionStorage.setItem('cmBoard', 'b' + b.id) }, 'Actuators') : null,
        h('button', { class: 'small', onclick: () => cmd('identify') }, 'Identify'),
        h('button', { class: 'small', onclick: () => cmd('resend') }, 'Resend settings'),
        h('button', { class: 'small', onclick: () => confirm(`Restart ${b.name}?`) && cmd('reboot') }, 'Restart'),
        h('button', { class: 'small', onclick: () => { openFw = openFw === b.id ? null : b.id; fwBox.classList.toggle('hidden'); } }, 'Firmware'),
        h('button', { class: 'small', onclick: () => { const n = prompt('New name', b.name); if (n) act(() => patch(q, { name: n })).then(load); } }, 'Rename'),
        h('span', { class: 'spacer' }),
        h('button', { class: 'small danger', onclick: () => confirm(`Forget ${b.name}? It keeps its settings until paired again.`) && act(() => del(q)).then(load) }, 'Forget')]),
      fwBox]);
  }

  async function load() {
    if (!el) return;
    if (el.querySelector('input[type=file]') && document.activeElement && document.activeElement.type === 'file') return;
    let d;
    try { d = await get('/boards'); } catch (e) { return err(e); }
    const box = h('div');
    if (d.node) {   // this is a worker board
      box.append(h('div', { class: 'panel' }, [h('h3', {}, 'This is an extra board'),
        h('p', {}, d.node.paired ? `Paired as b${d.node.id}: link ${d.node.linkUp ? 'up' : 'DOWN'}${d.node.encrypted ? ', encrypted' : ''}. Its sensors and actuators are set up from the main board.` : 'Not paired yet: open the main board\'s Boards tab and press Pair next to this board.')]));
    }
    if (d.found) {
      const found = d.found.filter(f => !(d.items || []).some(b => b.mac === f.mac));
      box.append(h('div', { class: 'panel' }, [h('h3', {}, 'Boards nearby, not paired'),
        found.length ? h('div', {}, found.map(f => h('div', { class: 'list-item' }, [h('div', { class: 'grow' }, [h('b', {}, f.name || f.mac), h('span', { class: 'muted small' }, ` ${f.role} · ${f.chip} · ${f.firmware} · ${f.rssi} dBm`)]),
          h('button', { class: 'primary small', onclick: () => { const n = prompt('Name for this board', f.name || 'Engine bay'); if (n) act(() => api('/boards', { body: { mac: f.mac, name: n } })).then(load); } }, 'Pair')])))
          : h('p', { class: 'muted small' }, 'None heard. A new board shows here a few seconds after it is switched on (role: sensor, relay or display).')]));
      for (const b of d.items || []) box.append(boardCard(b, d));
      if (d.uploaded) box.append(h('p', { class: 'muted small' }, `Firmware file on this board: ${d.uploaded.version} (${d.uploaded.chip}, ${Math.round(d.uploaded.size / 1024)} KB)`));
      box.append(h('p', { class: 'muted small' }, `Radio: channel ${d.link.channel}, sent ${d.link.tx}, received ${d.link.rx}, failed ${d.link.txFail}`));
    }
    el.innerHTML = ''; el.append(box);
  }

  page('boards', {
    show: main => { el = main; load(); clearInterval(timer); timer = setInterval(load, 3000); },
    leave: () => { clearInterval(timer); el = null; },
  });
})();
