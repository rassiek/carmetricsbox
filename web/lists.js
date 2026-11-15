'use strict';
// Sensors and Actuators tabs: the list of one board (this one or a paired board),
// add / edit / remove, live values, controls, discovery.
(() => {
  const { $, h, api, get, patch, del, toast, act, err, S, fmt, page, form, loadPins } = CM;
  let board = 'local';

  async function boardPicker(onpick) {
    const sel = h('select', { style: 'width:auto', onchange: e => { board = e.target.value; onpick(); } }, [h('option', { value: 'local' }, 'This board')]);
    try {
      const b = await get('/boards');
      for (const x of b.items || []) if (x.role !== 'display') sel.append(h('option', { value: 'b' + x.id, selected: board === 'b' + x.id }, `${x.name} (b${x.id})${x.online ? '' : ' · offline'}`));
    } catch {}
    if (![...sel.options].some(o => o.value === board)) board = 'local';
    sel.value = board;
    return h('div', { class: 'row' }, [h('label', { style: 'margin:0' }, 'Board'), sel]);
  }

  // For another board the pins are its own: plain numbers instead of this board's pin list.
  const fieldsFor = list => board === 'local' ? list : list.map(f => f.type === 'pin' ? { ...f, type: 'num' } : f);

  function editor(kind, item, isNew, done) {
    const o = JSON.parse(JSON.stringify(item));
    const schema = fieldsFor(kind === 'sensors' ? SCHEMA.sensor : SCHEMA.actuator);
    const f = form(o, schema, () => presets.classList.toggle('hidden', !['adc', 'ads1115'].includes(o.type)));
    const presets = h('div', { class: 'row small' + (['adc', 'ads1115'].includes(o.type) ? '' : ' hidden') }, [h('span', { class: 'muted' }, 'Start from:')].concat(
      SCHEMA.analogPresets.map(([n, v]) => h('button', { class: 'small', onclick: () => { Object.assign(o, v); f.redraw(); } }, n))));
    const q = `?board=${board}`;
    const save = async () => {
      const body = { ...o }; delete body.state;
      const r = await act(() => isNew ? api(`/${kind === 'sensors' ? 'sensors' : 'actuators'}${q}`, { body })
                                      : patch(`/${kind === 'sensors' ? 'sensors' : 'actuators'}/${item.id}${q}`, body));
      if (r) done();
    };
    return h('div', { class: 'panel edit' }, [
      h('div', { class: 'row' }, [h('h3', {}, isNew ? 'New' : `Edit ${item.name}`), h('span', { class: 'spacer' }), isNew ? null : h('span', { class: 'muted small' }, `id: ${item.id}`)]),
      presets, f,
      h('div', { class: 'row', style: 'margin-top:10px' }, [
        h('button', { class: 'primary', onclick: save }, 'Save'), h('button', { onclick: done }, 'Cancel'), h('span', { class: 'spacer' }),
        isNew ? null : h('button', { class: 'danger', onclick: async () => { if (confirm(`Remove ${item.name}?`) && await act(() => del(`/${kind === 'sensors' ? 'sensors' : 'actuators'}/${item.id}${q}`))) done(); } }, 'Remove')]),
    ]);
  }

  // ---- sensors
  function valueOf(id) {
    const v = S.values[(board === 'local' ? 'local.' : board + '.') + id];
    return v || { v: null, st: 'waiting', lvl: 'ok' };
  }
  async function showSensors(main) {
    if (board === 'local') await loadPins();
    const list = h('div'), top = await boardPicker(() => showSensors(main));
    let items = [];
    const draw = () => {
      list.innerHTML = '';
      for (const s of items) {
        const v = valueOf(s.id), [txt, u] = fmt(v.v, s.unit, s.decimals);
        list.append(h('div', { class: 'list-item ' + (v.lvl || '') }, [
          h('div', { class: 'grow' }, [h('b', {}, s.name), ' ', h('span', { class: 'muted small' }, `${(SCHEMA.sensorTypes.find(t => t[0] === s.type) || [, s.type])[1]}${s.enabled ? '' : ' · disabled'}`)]),
          h('span', { class: 'val', 'data-id': s.id }, `${txt} ${u}`), h('span', { class: 'pill small' }, v.st),
          h('button', { class: 'small', onclick: () => { list.replaceWith(editor('sensors', s, false, () => showSensors(main))); } }, 'Edit')]));
      }
      if (!items.length) list.append(h('p', { class: 'muted' }, 'No sensors yet.'));
    };
    try { items = (await get(`/sensors?board=${board}`)).items; } catch (e) { err(e); }
    draw();
    main.innerHTML = '';
    main.append(h('div', { class: 'row' }, [top, h('span', { class: 'spacer' }),
      h('button', { class: 'primary', onclick: () => list.replaceWith(editor('sensors', SCHEMA.newSensor(), true, () => showSensors(main))) }, 'Add sensor')]), list);
    if (board === 'local') main.append(discoverPanel(s => list.replaceWith(editor('sensors', s, true, () => showSensors(main)))));
    page.sensorsTick = () => { for (const s of items) { const e = $(`.val[data-id="${s.id}"]`); if (e) { const v = valueOf(s.id), [t, u] = fmt(v.v, s.unit, s.decimals); e.textContent = `${t} ${u}`; } } };
  }

  function discoverPanel(open) {
    const out = h('div', { class: 'small' });
    const run = async () => {
      if (!await act(() => api('/sensors/discover', { body: {} }), 'Searching…')) return;
      for (let i = 0; i < 20; i++) {
        await new Promise(r => setTimeout(r, 800));
        const d = await get('/sensors/discover').catch(() => null);
        if (d && !d.running && d.ageMs >= 0) return draw(d);
      }
    };
    const use = preset => { open(Object.assign(SCHEMA.newSensor(), preset)); window.scrollTo(0, 0); };
    const draw = d => {
      out.innerHTML = '';
      out.append(h('h3', {}, 'I2C'), d.i2c.length ? h('div', {}, d.i2c.map(x => h('div', { class: 'row' }, [`${x.addr} ${x.driver || 'unknown device'}`]))) : h('p', { class: 'muted' }, 'Nothing on the I2C bus.'));
      const ow = (d.onewire && d.onewire.devices) || [];
      out.append(h('h3', {}, '1-Wire'), ow.length ? h('div', {}, ow.map(x => h('div', { class: 'row' }, [`${x.address} ${x.tempC ?? '--'} °C `,
        x.assignedTo ? h('span', { class: 'muted' }, `→ ${x.assignedTo}`) : h('button', { class: 'small', onclick: () => use({ type: 'ds18b20', address: x.address, name: 'Temperature', unit: '°C' }) }, 'Add as sensor')]))) : h('p', { class: 'muted' }, 'No 1-Wire sensors found.'));
      out.append(h('h3', {}, 'Analog inputs (volts now)'), h('div', { class: 'grid' }, d.analog.map(x => h('div', { class: 'mini' }, [h('b', {}, x.source), h('div', {}, x.volts === null ? '--' : `${x.volts} V`),
        x.assignedTo ? h('div', { class: 'muted' }, `→ ${x.assignedTo}`) : h('button', { class: 'small', onclick: () => {
          const m = x.source.match(/ads1115@0x(..)\/(\d)/);
          use(m ? { type: 'ads1115', adsAddress: parseInt(m[1], 16), adsChannel: +m[2], name: 'Analog ' + m[2], unit: 'V' } : { type: 'adc', pin: +x.source.slice(4), name: 'Analog', unit: 'V' });
        } }, 'Use')]))));
    };
    return h('div', { class: 'panel' }, [h('div', { class: 'row' }, [h('h3', { style: 'margin:0' }, 'Find connected sensors'), h('span', { class: 'spacer' }), h('button', { onclick: run }, 'Discover')]),
      h('p', { class: 'muted small' }, 'Lists I2C devices, 1-Wire sensors and the voltage on every analog input, so you can see which wire is which.'), out]);
  }

  // ---- actuators
  // Controls are big: they get used on a phone or a touch screen in the car.
  function control(a, q, reload) {
    const send = async body => { await act(() => api(`/actuators/${a.id}/command${q}`, { body })); reload(); };
    const st = a.state || {};
    const auto = a.rule !== 'manual' ? h('button', { class: 'big' + (st.by === 'rule' ? ' on' : ''), onclick: () => send({ auto: true }) }, 'Auto') : null;
    const known = st.value !== undefined;
    if (a.type === 'relay') return h('div', { class: 'row ctl' }, [
      h('button', { class: 'big' + (st.on ? ' on' : ''), onclick: () => send({ on: !st.on }) }, !known ? 'Switch' : st.on ? 'On' : 'Off'), auto]);
    if (a.type === 'servo') {
      const r = h('input', { type: 'range', min: a.servoMinDeg, max: a.servoMaxDeg, value: st.value ?? a.servoMinDeg, onchange: e => send({ angle: +e.target.value }) });
      return h('div', { class: 'row ctl' }, [r, auto]);
    }
    if (a.type === 'hbridge') {
      const sp = h('input', { type: 'range', min: 0, max: 100, value: Math.abs(st.value || 0) || a.maxDuty });
      return h('div', { class: 'row ctl' }, [h('button', { class: 'big', onclick: () => send({ direction: 'reverse', speed: +sp.value }) }, '◀'),
        h('button', { class: 'big', onclick: () => send({ direction: 'stop' }) }, 'Stop'), h('button', { class: 'big', onclick: () => send({ direction: 'forward', speed: +sp.value }) }, '▶'), sp, auto]);
    }
    const r = h('input', { type: 'range', min: 0, max: 100, value: st.value || 0, onchange: e => send({ duty: +e.target.value }) });
    return h('div', { class: 'row ctl' }, [h('button', { class: 'big' + (st.value > 0 ? ' on' : ''), onclick: () => send({ on: !(st.value > 0) }) }, st.value > 0 ? 'On' : 'Off'), r, auto]);
  }
  async function showActuators(main) {
    if (board === 'local') await loadPins();
    const top = await boardPicker(() => showActuators(main));
    const list = h('div');
    let items = [], shown = '', warned = false;
    const q = `?board=${board}`;
    const draw = () => {
      const snap = JSON.stringify(items);
      if (snap === shown) return;   // nothing changed: don't rebuild under the finger
      shown = snap;
      list.innerHTML = '';
      for (const a of items) {
        const st = a.state || {};
        const val = st.value === undefined ? '' : a.type === 'relay' ? (st.on ? 'on' : 'off') : a.type === 'servo' ? `${st.value}°` : `${st.value} %`;
        list.append(h('div', { class: 'list-item act' }, [
          h('div', { class: 'grow' }, [h('b', {}, a.name), ' ', h('span', { class: 'muted small' },
            `${(SCHEMA.actTypes.find(t => t[0] === a.type) || [, a.type])[1]} · ${a.rule}${st.by ? ' · now: ' + st.by : ''} · GPIO ${a.pin}`)]),
          h('span', { class: 'val' }, val),
          a.enabled ? control(a, q, load) : h('button', { class: 'big', title: 'Switched off in its settings', onclick: async () => {
            if (await act(() => patch(`/actuators/${a.id}${q}`, { enabled: true }), `${a.name} enabled`)) load();
          } }, 'Disabled: enable'),
          h('button', { class: 'small', onclick: () => list.replaceWith(editor('actuators', a, false, () => showActuators(main))) }, 'Edit')]));
      }
      if (!items.length) list.append(h('p', { class: 'muted' }, 'Nothing yet: relays, fans (PWM), motors (H-bridge) and servos go here.'));
    };
    async function load() {
      try {
        const r = await get(`/actuators${q}`);
        if (board !== 'local') {   // another board: its live values come with the Boards list
          const b = ((await get('/boards')).items || []).find(x => 'b' + x.id === board);
          for (const a of r.items) {
            const p = b && b.online && (b.points || []).find(x => x.relay && x.id === a.id);
            if (p && p.value != null) a.state = { value: p.value, on: p.value > 0 };
          }
          const cfg = b && b.config;
          if (cfg && cfg.status === 'refused' && !warned && (warned = true)) toast(`${b.name} refused its settings: ${cfg.message}`, true, 8000);
        }
        items = r.items; draw();
      } catch (e) { err(e); }
    }
    await load();
    main.innerHTML = '';
    main.append(h('div', { class: 'row' }, [top, h('span', { class: 'spacer' }),
      h('button', { class: 'primary', onclick: () => list.replaceWith(editor('actuators', SCHEMA.newActuator(), true, () => showActuators(main))) }, 'Add actuator')]), list);
    clearInterval(page.actT);
    page.actT = setInterval(() => { if (!document.querySelector('.edit') && document.activeElement?.type !== 'range') load(); }, 3000);
  }

  page('sensors', { show: main => { board = sessionStorage.getItem('cmBoard') || 'local'; sessionStorage.removeItem('cmBoard'); showSensors(main); }, leave: () => { page.sensorsTick = null; } });
  page('actuators', { show: main => { board = sessionStorage.getItem('cmBoard') || 'local'; sessionStorage.removeItem('cmBoard'); showActuators(main); }, leave: () => clearInterval(page.actT) });
  CM.live(() => page.sensorsTick && page.sensorsTick());
})();
