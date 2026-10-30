'use strict';
(() => {
  const { $, h, api, toast, live, fmt, statusText } = CM;
  let sensors = [], saved = '[]', editing = -1, scan = null;

  const TEMPLATES = {
    ds18b20: { unit: '°C', gaugeMin: 0, gaugeMax: 120, decimals: 0, smoothing: 1, oledStyle: 'text', hysteresis: 1 },
    mcp9600: { unit: '°C', gaugeMin: 0, gaugeMax: 800, decimals: 0, smoothing: 0.5, oledStyle: 'bar', mcpAddress: 0x67, hysteresis: 10 },
    max6675: { unit: '°C', gaugeMin: 0, gaugeMax: 800, decimals: 0, smoothing: 0.5, oledStyle: 'bar', sckPin: 5, pin: 17, soPin: 16, hysteresis: 10 },
    adc: { unit: 'psi', pin: 32, divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 100, faultLowV: 0.2, faultHighV: 4.8,
           gaugeMin: 0, gaugeMax: 100, decimals: 0, smoothing: 0.3, oledStyle: 'bar', hysteresis: 1 },
    ads1115: { unit: 'psi', adsAddress: 0x48, adsChannel: 0, divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 100,
               faultLowV: 0.2, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 100, decimals: 0, smoothing: 0.3, oledStyle: 'bar', hysteresis: 1 },
    bmp: { unit: '°C', bmpField: 'temperature', gaugeMin: 0, gaugeMax: 50, decimals: 0, smoothing: 0.3, oledStyle: 'text', hysteresis: 1 },
  };
  // Analog presets: sensor volts -> value. divider = how much the resistors scale the signal down.
  const PRESETS = {
    '': null,
    'p100': { label: '0.5–4.5 V sender, 0–100 psi (10k/20k divider)', unit: 'psi', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 100, faultLowV: 0.2, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 100 },
    'p30': { label: '0.5–4.5 V sender, 0–30 psi (10k/20k divider)', unit: 'psi', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 30, faultLowV: 0.2, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 30, decimals: 1 },
    'p150': { label: '0.5–4.5 V sender, 0–150 psi (10k/20k divider)', unit: 'psi', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 150, faultLowV: 0.2, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 150 },
    'bar10': { label: '0.5–4.5 V sender, 0–10 bar (10k/20k divider)', unit: 'bar', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 10, faultLowV: 0.2, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 10, decimals: 1 },
    'b25': { label: 'Battery via B25 module (×5)', unit: 'V', divider: 5, inMin: 0, inMax: 1, outMin: 0, outMax: 1, faultLowV: null, faultHighV: null, gaugeMin: 8, gaugeMax: 16, decimals: 1 },
    'volts': { label: 'Plain voltage (no scaling)', unit: 'V', divider: 1, inMin: 0, inMax: 1, outMin: 0, outMax: 1, faultLowV: null, faultHighV: null, gaugeMin: 0, gaugeMax: 3.3, decimals: 2 },
  };

  const typeName = { ds18b20: 'DS18B20', mcp9600: 'MCP9600', max6675: 'MAX6675', adc: 'ESP32 analog', ads1115: 'ADS1115', bmp: 'BMP' };
  function describe(s) {
    switch (s.type) {
      case 'ds18b20': return s.address ? `1-Wire ${s.address}` : '1-Wire, no address assigned';
      case 'mcp9600': return `MCP9600 I2C 0x${(+s.mcpAddress).toString(16)}`;
      case 'max6675': return `MAX6675 SCK ${s.sckPin} CS ${s.pin} SO ${s.soPin}`;
      case 'adc': return `GPIO ${s.pin}`;
      case 'ads1115': return `ADS1115 0x${(+s.adsAddress).toString(16)} ch ${s.adsChannel}`;
      case 'bmp': return `BMP ${s.bmpField}`;
    }
    return s.type;
  }

  function markDirty() { $('#dirty').classList.toggle('hidden', JSON.stringify(sensors) === saved); }

  function renderList() {
    const list = $('#list');
    list.innerHTML = '';
    sensors.forEach((s, i) => {
      list.append(h('div', { class: 'list-item' },
        h('span', { class: 'dot', id: 'dot-' + s.id }),
        h('div', { style: 'flex:1;min-width:0' },
          h('div', {}, h('b', {}, s.name), ' ', h('span', { class: 'muted small' }, `${s.id} · ${typeName[s.type]}`),
            s.enabled ? '' : h('span', { class: 'pill', style: 'margin-left:6px' }, 'disabled')),
          h('div', { class: 'muted small' }, describe(s), ' · ', h('span', { id: 'val-' + s.id }, ''))),
        h('button', { class: 'small', title: 'Move up', onclick: () => move(i, -1), disabled: i === 0 }, '↑'),
        h('button', { class: 'small', title: 'Move down', onclick: () => move(i, 1), disabled: i === sensors.length - 1 }, '↓'),
        h('button', { class: 'small', onclick: () => edit(i) }, 'Edit'),
        h('button', { class: 'small danger', onclick: () => remove(i) }, '✕')));
    });
    if (!sensors.length) list.append(h('p', { class: 'muted' }, 'No sensors. Add one above.'));
    markDirty();
    if (CM.state) liveUpdate(CM.state);
  }

  function move(i, d) { const [s] = sensors.splice(i, 1); sensors.splice(i + d, 0, s); if (editing === i) editing = i + d; renderList(); }
  function remove(i) {
    if (!confirm(`Remove "${sensors[i].name}"?`)) return;
    sensors.splice(i, 1);
    if (editing === i) closeEditor();
    renderList();
  }

  function uniqueId(base) { let id = base, n = 2; while (sensors.some(s => s.id === id)) id = `${base}_${n++}`; return id; }
  function add() {
    const type = $('#newType').value;
    const s = Object.assign({ id: uniqueId(type), name: 'New ' + typeName[type], type, enabled: true, address: '', pin: -1, sckPin: -1, soPin: -1,
      adsAddress: 0x48, adsChannel: 0, mcpAddress: 0x67, bmpField: 'temperature', divider: 1, inMin: 0, inMax: 1, outMin: 0, outMax: 1,
      faultLowV: null, faultHighV: null, offset: 0, oledLabel: '', warnLow: null, critLow: null, warnHigh: null, critHigh: null,
      engineRunningOnly: false }, TEMPLATES[type]);
    sensors.push(s);
    renderList();
    edit(sensors.length - 1);
  }

  // ---------------------------------------------------------------- editor
  const num = v => (v === null || v === undefined ? '' : v);
  function field(label, key, s, opts = {}) {
    const type = opts.type || 'number';
    let input;
    if (type === 'select') {
      input = h('select', {}, opts.options.map(([v, t]) => h('option', { value: v, selected: String(s[key]) === String(v) }, t)));
    } else if (type === 'checkbox') {
      input = h('input', { type: 'checkbox', checked: !!s[key] });
      return h('label', { class: 'check' + (opts.wide ? ' wide' : '') }, input, ' ', label, wire(input, key, s, opts));
    } else {
      input = h('input', { type, value: type === 'number' ? num(s[key]) : (s[key] ?? ''), step: opts.step || 'any',
                           placeholder: opts.placeholder || '' });
    }
    return h('div', { class: opts.wide ? 'wide' : '' }, h('label', {}, label), input, wire(input, key, s, opts));
  }
  function wire(input, key, s, opts) {
    input.addEventListener('change', () => {
      let v;
      if (input.type === 'checkbox') v = input.checked;
      else if ((opts.type || 'number') === 'number') v = input.value.trim() === '' ? (opts.nullable === false ? 0 : null) : +input.value;
      else if (opts.numeric) v = +input.value;
      else v = input.value;
      s[key] = v;
      if (opts.rerender) edit(editing);
      renderList();
    });
    return null;
  }

  const ADC_PINS = [32, 33, 34, 35, 36, 39].map(p => [p, `GPIO ${p}` + (p === 36 ? ' (VP)' : p === 39 ? ' (VN)' : '')]);
  function edit(i) {
    editing = i;
    const s = sensors[i], ed = $('#editor');
    ed.classList.remove('hidden');
    ed.innerHTML = '';
    const hw = [];
    if (s.type === 'ds18b20') {
      const opts = [['', '— not assigned —']];
      if (s.address) opts.push([s.address, s.address]);
      (scan?.devices || []).forEach(d => { if (d.address !== s.address) opts.push([d.address, `${d.address} (${fmt(d.tempC, 1)} °C${d.assignedTo ? ', ' + d.assignedTo : ''})`]); });
      hw.push(field('1-Wire address', 'address', s, { type: 'select', options: opts, wide: true }));
    } else if (s.type === 'max6675') {
      hw.push(field('SCK pin', 'sckPin', s, { nullable: false }), field('CS pin', 'pin', s, { nullable: false }), field('SO pin', 'soPin', s, { nullable: false }));
    } else if (s.type === 'mcp9600') {
      hw.push(field('I2C address', 'mcpAddress', s, { type: 'select', numeric: true,
        options: [0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67].map(a => [a, '0x' + a.toString(16) + (a === 0x67 ? ' (Adafruit default)' : a === 0x60 ? ' (ADDR→GND)' : '')]) }),
        h('p', { class: 'muted small wide', style: 'margin:0' }, 'K-type. Mount it in the cab box and run K-type extension cable to the probe. The MCP9601 also reports open/short probes. If readings drop out, set I2C speed to 100 kHz on the System page.'));
    } else if (s.type === 'bmp') {
      hw.push(field('Measurement', 'bmpField', s, { type: 'select', options: [['temperature', 'Temperature'], ['pressure', 'Pressure (hPa)'], ['altitude', 'Altitude (m)']] }));
    } else if (s.type === 'adc') {
      hw.push(field('Analog pin (ADC1)', 'pin', s, { type: 'select', options: ADC_PINS, numeric: true }));
    } else if (s.type === 'ads1115') {
      hw.push(field('ADS1115 address', 'adsAddress', s, { type: 'select', numeric: true, options: [[72, '0x48 (ADDR→GND)'], [73, '0x49 (ADDR→VDD)'], [74, '0x4A (ADDR→SDA)'], [75, '0x4B (ADDR→SCL)']] }),
              field('Channel', 'adsChannel', s, { type: 'select', numeric: true, options: [[0, 'A0'], [1, 'A1'], [2, 'A2'], [3, 'A3']] }));
    }
    const analog = s.type === 'adc' || s.type === 'ads1115';
    let calib = null;
    if (analog) {
      const preset = h('select', {}, Object.entries(PRESETS).map(([k, p]) => h('option', { value: k }, p ? p.label : 'Apply a preset…')));
      preset.addEventListener('change', () => { const p = PRESETS[preset.value]; if (p) { const { label, ...rest } = p; Object.assign(s, rest); edit(editing); renderList(); } });
      calib = h('fieldset', {}, h('legend', {}, 'Calibration'),
        h('p', { class: 'muted small', style: 'margin-top:0' },
          'sensor volts = pin volts × divider. The input range (sensor volts) maps linearly onto the output range. ',
          'Fault limits flag a broken wire (e.g. below 0.2 V on a 0.5–4.5 V sender).'),
        h('div', { class: 'fields' },
          h('div', { class: 'wide' }, preset),
          field('Divider (×)', 'divider', s, { nullable: false }),
          field('Input min (V)', 'inMin', s, { nullable: false }), field('Input max (V)', 'inMax', s, { nullable: false }),
          field('Output at min', 'outMin', s, { nullable: false }), field('Output at max', 'outMax', s, { nullable: false }),
          field('Fault below (V)', 'faultLowV', s), field('Fault above (V)', 'faultHighV', s)),
        h('p', { class: 'small', style: 'margin-bottom:0' }, 'Live: ', h('b', { id: 'edLive' }, '—')));
    }

    ed.append(
      h('div', { class: 'row' }, h('h3', { style: 'margin:0' }, `Edit: ${s.name}`), h('span', { class: 'spacer' }),
        h('button', { onclick: closeEditor }, 'Done')),
      h('fieldset', {}, h('legend', {}, 'General'), h('div', { class: 'fields' },
        field('Name', 'name', s, { type: 'text' }), field('ID (letters, digits, _)', 'id', s, { type: 'text' }),
        field('Unit', 'unit', s, { type: 'text' }), field('Enabled', 'enabled', s, { type: 'checkbox' }))),
      hw.length ? h('fieldset', {}, h('legend', {}, 'Hardware'), h('div', { class: 'fields' }, hw)) : null,
      calib,
      h('fieldset', {}, h('legend', {}, 'Display'), h('div', { class: 'fields' },
        field('Gauge min', 'gaugeMin', s, { nullable: false }), field('Gauge max', 'gaugeMax', s, { nullable: false }),
        field('Decimals', 'decimals', s, { type: 'select', numeric: true, options: [[0, '0'], [1, '1'], [2, '2'], [3, '3']] }),
        field('Offset (added)', 'offset', s, { nullable: false }),
        field('Smoothing (1 = raw)', 'smoothing', s, { nullable: false, step: '0.05' }),
        field('OLED', 'oledStyle', s, { type: 'select', options: [['off', 'Hidden'], ['text', 'Text cell'], ['bar', 'Bar gauge']] }),
        field('OLED label (≤3)', 'oledLabel', s, { type: 'text' }))),
      h('fieldset', {}, h('legend', {}, 'Alerts (leave blank to disable)'), h('div', { class: 'fields' },
        field('Critical low', 'critLow', s), field('Warning low', 'warnLow', s),
        field('Warning high', 'warnHigh', s), field('Critical high', 'critHigh', s),
        field('Hysteresis', 'hysteresis', s, { nullable: false }),
        field('Only alarm while engine runs', 'engineRunningOnly', s, { type: 'checkbox', wide: true }))));
    ed.scrollIntoView({ behavior: 'smooth', block: 'start' });
  }
  function closeEditor() { editing = -1; $('#editor').classList.add('hidden'); }

  // ---------------------------------------------------------------- live values
  function liveUpdate(st) {
    for (const s of sensors) {
      const r = st.sensors[s.id], dot = $('#dot-' + s.id), val = $('#val-' + s.id);
      if (!dot || !val) continue;
      if (!r) { val.textContent = 'not saved yet'; dot.className = 'dot'; continue; }
      dot.className = 'dot ' + (r.st === 'ok' ? r.lvl : r.st === 'fault' || r.st === 'not_found' ? 'fault' : '');
      const v = r.v === null ? null : +r.v;
      val.textContent = r.st === 'ok' ? `${fmt(v, s.decimals)} ${s.unit}` : statusText[r.st];
    }
    if (editing >= 0) {
      const s = sensors[editing], r = st.sensors[s.id], el = $('#edLive');
      if (el && r) el.textContent = r.volts != null ? `${(+r.volts).toFixed(3)} V at sensor → ${r.v === null ? statusText[r.st] : fmt(+r.v, s.decimals) + ' ' + s.unit}` : statusText[r.st] || '—';
    }
  }

  // ---------------------------------------------------------------- 1-Wire
  async function loadScan() {
    scan = await api('/api/onewire');
    const box = $('#ow');
    box.innerHTML = '';
    if (!scan.devices.length) { box.append(h('p', { class: 'muted' }, scan.lastScanAgeMs < 0 ? 'Not scanned yet.' : 'No sensors found. Check the 4.7k pull-up (to 3.3 V) and wiring.')); return; }
    const dsSensors = sensors.filter(s => s.type === 'ds18b20');
    const tbl = h('table', {}, h('tr', {}, h('th', {}, 'Address'), h('th', {}, 'Temp'), h('th', {}, 'Assigned to')));
    for (const d of scan.devices) {
      const local = sensors.find(s => s.type === 'ds18b20' && s.address === d.address);
      const sel = h('select', {}, h('option', { value: '' }, '— none —'),
        dsSensors.map(s => h('option', { value: s.id, selected: local && local.id === s.id }, s.name)));
      sel.addEventListener('change', () => {
        sensors.forEach(s => { if (s.type === 'ds18b20' && s.address === d.address) s.address = ''; });
        const t = sensors.find(s => s.id === sel.value);
        if (t) t.address = d.address;
        renderList();
        if (editing >= 0) edit(editing);
      });
      tbl.append(h('tr', {}, h('td', {}, h('code', {}, d.address)), h('td', {}, `${fmt(d.tempC, 1)} °C`), h('td', {}, sel)));
    }
    box.append(tbl);
  }
  async function doScan() {
    try {
      $('#scan').disabled = true;
      await api('/api/onewire/scan', { method: 'POST' });
      await new Promise(r => setTimeout(r, 1800));
      await loadScan();
    } catch (e) { toast(e.message, true); } finally { $('#scan').disabled = false; }
  }

  // ---------------------------------------------------------------- load / save
  async function load() {
    const cfg = await api('/api/config');
    sensors = cfg.sensors;
    saved = JSON.stringify(sensors);
    closeEditor();
    renderList();
  }
  async function save() {
    try {
      const r = await api('/api/config/sensors', { body: sensors });
      saved = JSON.stringify(sensors);
      markDirty();
      toast(r.message);
      setTimeout(loadScan, 1500);
    } catch (e) { toast(e.message, true, 6000); }
  }

  document.addEventListener('DOMContentLoaded', () => {
    $('#add').onclick = add;
    $('#save').onclick = save;
    $('#discard').onclick = () => load().catch(e => toast(e.message, true));
    $('#scan').onclick = doScan;
    $('#reset').onclick = async () => {
      if (!confirm('Replace ALL sensors and relays with the defaults? Wi-Fi and passwords are kept.')) return;
      try { toast((await api('/api/config/reset', { method: 'POST' })).message); setTimeout(load, 1200); } catch (e) { toast(e.message, true); }
    };
    load().then(loadScan).catch(e => toast(e.message, true));
    live(liveUpdate);
  });
  window.addEventListener('beforeunload', e => { if (JSON.stringify(sensors) !== saved) { e.preventDefault(); e.returnValue = ''; } });
})();
