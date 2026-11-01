'use strict';
(() => {
  const { $, h, api, toast, live, onConfig } = CM;
  let relays = [], saved = '[]', ctl = {};
  const remote = CM.remoteBoard();   // another board's relays: settings kept here, switched over ESP-NOW

  async function control(id, body) {
    try {
      if (remote) {
        const index = JSON.parse(saved).findIndex(r => r.id === id);
        await api('/api/v2/boards/relay', { body: { id: remote, index, on: !!body.on } });
        setTimeout(pollRemote, 600);
      } else await api('/api/relays/control', { body: Object.assign({ id }, body) });
    } catch (e) { toast(e.message, true); }
  }
  async function pollRemote() {   // remote relay states from the Boards API (points: sensors first, then relays)
    const d = await api('/api/v2/boards').catch(() => null);
    const b = d && d.boards.find(x => x.id === remote);
    if (!b) return;
    const st = { relays: {} };
    b.points.filter(p => p.relay).forEach(p => { st.relays[p.id] = { on: p.value === 1, out: p.value === 1, blinkMs: 0 }; });
    liveUpdate(st);
  }

  function renderControls() {
    const box = $('#controls');
    box.innerHTML = '';
    ctl = {};
    const active = JSON.parse(saved).filter(r => r.enabled);
    $('#none').classList.toggle('hidden', active.length > 0);
    for (const r of active) {
      const state = h('span', { class: 'pill' }, '…');
      const btn = h('button', { class: 'primary', style: 'width:100%;padding:14px' }, '…');
      btn.onclick = () => control(r.id, { on: !(ctl[r.id].on) });
      const parts = [h('div', { class: 'row' }, h('b', {}, r.name), h('span', { class: 'spacer' }), state), btn];
      let delay = null;
      if (r.mode === 'blink') {
        delay = h('input', { type: 'number', min: 0, max: 60000, step: 50, value: r.blinkMs });
        delay.addEventListener('change', () => control(r.id, { blinkMs: +delay.value }));
        parts.push(h('label', { style: 'margin-top:8px' }, 'Blink interval (ms, 0 = steady)'), delay);
      }
      box.append(h('div', { class: 'panel', style: 'margin:0' }, parts));
      ctl[r.id] = { state, btn, delay, on: false };
    }
    if (CM.state) liveUpdate(CM.state);
  }

  function liveUpdate(st) {
    for (const [id, c] of Object.entries(ctl)) {
      const r = st.relays[id];
      if (!r) continue;
      c.on = r.on;
      c.btn.textContent = r.on ? 'Turn OFF' : 'Turn ON';
      c.state.textContent = r.on ? (r.out ? 'ON' : 'ON (blink)') : 'OFF';
      c.state.className = 'pill ' + (r.on ? 'ok' : '');
      if (c.delay && document.activeElement !== c.delay) c.delay.value = r.blinkMs;
    }
  }

  function renderConfig() {
    const t = $('#cfg');
    t.innerHTML = '';
    t.append(h('tr', {}, ['Name', 'ID', 'GPIO', 'Mode', 'Blink ms', 'Active low', 'On at boot', 'If link lost', 'Enabled', ''].map(x => h('th', {}, x))));
    relays.forEach((r, i) => {
      const inp = (key, attrs = {}) => {
        const el = h('input', Object.assign({ value: r[key] }, attrs));
        el.addEventListener('change', () => { r[key] = attrs.type === 'number' ? +el.value : el.value; dirty(); });
        return el;
      };
      const chk = key => {
        const el = h('input', { type: 'checkbox', checked: r[key] });
        el.addEventListener('change', () => { r[key] = el.checked; dirty(); });
        return el;
      };
      const mode = h('select', {}, h('option', { value: 'on_off', selected: r.mode === 'on_off' }, 'On/off'),
        h('option', { value: 'blink', selected: r.mode === 'blink' }, 'Blink'));
      mode.addEventListener('change', () => { r.mode = mode.value; dirty(); });
      const safe = h('select', { title: 'Relay board only: what this relay does when the link to the main board is lost' },
        ...[['off', 'Off'], ['on', 'On'], ['hold', 'Keep']].map(([v, l]) => h('option', { value: v, selected: (r.safeState || 'off') === v }, l)));
      safe.addEventListener('change', () => { r.safeState = safe.value; dirty(); });
      t.append(h('tr', {},
        h('td', {}, inp('name', { style: 'min-width:110px' })), h('td', {}, inp('id', { style: 'min-width:80px' })),
        h('td', {}, inp('pin', { type: 'number', style: 'width:64px' })), h('td', {}, mode),
        h('td', {}, inp('blinkMs', { type: 'number', style: 'width:80px' })),
        h('td', {}, chk('activeLow')), h('td', {}, chk('defaultOn')), h('td', {}, safe), h('td', {}, chk('enabled')),
        h('td', {}, h('button', { class: 'small danger', onclick: () => { relays.splice(i, 1); renderConfig(); dirty(); } }, '✕'))));
    });
  }
  function dirty() { $('#dirty').classList.toggle('hidden', JSON.stringify(relays) === saved); }

  async function load() {
    const cfg = remote ? await api('/api/v2/boards/config?id=' + remote) : await api('/api/config');
    relays = cfg.relays;
    saved = JSON.stringify(relays);
    renderConfig(); renderControls(); dirty();
  }

  document.addEventListener('DOMContentLoaded', () => {
    $('#add').onclick = () => {
      let n = relays.length + 1;
      while (relays.some(r => r.id === 'relay' + n)) n++;
      relays.push({ id: 'relay' + n, name: 'Relay ' + n, pin: -1, mode: 'on_off', activeLow: true, defaultOn: false, blinkMs: 500, enabled: true, safeState: 'off' });
      renderConfig(); dirty();
    };
    $('#save').onclick = async () => {
      try {
        const res = remote ? await api('/api/v2/boards/config', { body: { id: remote, relays } }) : await api('/api/config/relays', { body: relays });
        toast(res.message, false, 5000); saved = JSON.stringify(relays); dirty(); setTimeout(load, 800);
      }
      catch (e) { toast(e.message, true, 6000); }
    };
    $('#discard').onclick = () => load().catch(e => toast(e.message, true));
    CM.boardPicker($('#boardSel'));
    load().catch(e => toast(e.message, true));
    if (remote) {
      $('#boardNote').textContent = 'Another board\'s relays: settings are kept here and sent to it; the buttons switch them over ESP-NOW. "If link lost" is what each relay does when that board loses the main board for 5 s.';
      setInterval(pollRemote, 2000); pollRemote();
    } else {
      live(liveUpdate);
      onConfig(() => load().catch(() => {}));
    }
  });
})();
