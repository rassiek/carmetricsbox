'use strict';
(() => {
  const { $, h, api, toast, live, fmt, statusText } = CM;
  let sensors = [], saved = '[]', editing = -1, scan = null;

  const TEMPLATES = {
    ds18b20: { unit: '°C', gaugeMin: 0, gaugeMax: 120, decimals: 0, smoothing: 1, oledStyle: 'text', hysteresis: 1 },
    mcp9600: { unit: '°C', gaugeMin: 0, gaugeMax: 800, decimals: 0, smoothing: 0.5, oledStyle: 'bar', mcpAddress: 0x67, hysteresis: 10 },
    max6675: { unit: '°C', gaugeMin: 0, gaugeMax: 800, decimals: 0, smoothing: 0.5, oledStyle: 'bar', sckPin: 5, pin: 17, soPin: 16, hysteresis: 10 },
    adc: { unit: 'psi', pin: 32, divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 100, faultLowV: 0.35, faultHighV: 4.8,
           gaugeMin: 0, gaugeMax: 100, decimals: 0, smoothing: 0.3, oledStyle: 'bar', hysteresis: 1 },
    ads1115: { unit: 'psi', adsAddress: 0x48, adsChannel: 0, divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 100,
               faultLowV: 0.35, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 100, decimals: 0, smoothing: 0.3, oledStyle: 'bar', hysteresis: 1 },
    bmp: { unit: '°C', bmpField: 'temperature', gaugeMin: 0, gaugeMax: 50, decimals: 0, smoothing: 0.3, oledStyle: 'text', hysteresis: 1 },
    rpm: { unit: 'rpm', pin: -1, pulsesPerRev: 10, gaugeMin: 0, gaugeMax: 4500, decimals: 0, smoothing: 0.5, oledStyle: 'text', oledLabel: 'RPM', hysteresis: 50, faultAlarm: false },
    switch: { unit: '', pin: -1, activeLow: true, gaugeMin: 0, gaugeMax: 1, decimals: 0, smoothing: 1, oledStyle: 'text', warnHigh: 0.5, hysteresis: 0 },
    gps: { unit: 'km/h', field: 'speed', gaugeMin: 0, gaugeMax: 140, decimals: 0, smoothing: 1, oledStyle: 'text', hysteresis: 1, faultAlarm: false },
    ina226: { unit: 'A', field: 'current', i2cAddr: 0x40, shuntOhms: 0.001, gaugeMin: -20, gaugeMax: 50, decimals: 1, smoothing: 0.5, oledStyle: 'text', hysteresis: 0.5 },
    imu: { unit: '°', field: 'roll', i2cAddr: 0x68, gaugeMin: -45, gaugeMax: 45, decimals: 0, smoothing: 0.5, oledStyle: 'text', warnHigh: 25, warnLow: -25, critHigh: 35, critLow: -35, hysteresis: 2 },
    computed: { unit: '', formula: 'diff', srcA: '', srcB: '', curve: '', gaugeMin: 0, gaugeMax: 100, decimals: 1, smoothing: 1, oledStyle: 'off', hysteresis: 1, faultAlarm: false },
  };
  const FIELD_UNITS = { speed: 'km/h', altitude: 'm', sats: '', heading: '°', hdop: '', current: 'A', voltage: 'V', power: 'W', roll: '°', pitch: '°' };
  // Analog presets: sensor volts -> value. divider = how much the resistors scale the signal down.
  const PRESETS = {
    '': null,
    'p100': { label: '0.5–4.5 V sender, 0–100 psi (10k/20k divider)', unit: 'psi', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 100, faultLowV: 0.35, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 100 },
    'p30': { label: '0.5–4.5 V sender, 0–30 psi (10k/20k divider)', unit: 'psi', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 30, faultLowV: 0.35, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 30, decimals: 1 },
    'p150': { label: '0.5–4.5 V sender, 0–150 psi (10k/20k divider)', unit: 'psi', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 150, faultLowV: 0.35, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 150 },
    'bar10': { label: '0.5–4.5 V sender, 0–10 bar (10k/20k divider)', unit: 'bar', divider: 1.5, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 10, faultLowV: 0.35, faultHighV: 4.8, gaugeMin: 0, gaugeMax: 10, decimals: 1 },
    'b25': { label: 'Battery via B25 module (×5)', unit: 'V', divider: 5, inMin: 0, inMax: 1, outMin: 0, outMax: 1, faultLowV: null, faultHighV: null, gaugeMin: 8, gaugeMax: 16, decimals: 1 },
    'volts': { label: 'Plain voltage (no scaling)', unit: 'V', divider: 1, inMin: 0, inMax: 1, outMin: 0, outMax: 1, faultLowV: null, faultHighV: null, gaugeMin: 0, gaugeMax: 3.3, decimals: 2 },
  };

  const typeName = { ds18b20: 'DS18B20', mcp9600: 'MCP9600', max6675: 'MAX6675', adc: 'ESP32 analog', ads1115: 'ADS1115', bmp: 'BMP',
    rpm: 'RPM', switch: 'Switch', gps: 'GPS', ina226: 'INA226', imu: 'Tilt', computed: 'Computed' };
  function describe(s) {
    switch (s.type) {
      case 'ds18b20': return s.address ? `1-Wire ${s.address}` : '1-Wire, no address assigned';
      case 'mcp9600': return `MCP9600 I2C 0x${(+s.mcpAddress).toString(16)}`;
      case 'max6675': return `MAX6675 SCK ${s.sckPin} CS ${s.pin} SO ${s.soPin}`;
      case 'adc': return `GPIO ${s.pin}`;
      case 'ads1115': return `ADS1115 0x${(+s.adsAddress).toString(16)} ch ${s.adsChannel}`;
      case 'bmp': return `BMP ${s.bmpField}`;
      case 'rpm': return `GPIO ${s.pin}, ${s.pulsesPerRev} pulses/rev`;
      case 'switch': return `GPIO ${s.pin}${s.activeLow ? ', active low' : ''}`;
      case 'gps': return `GPS ${s.field}`;
      case 'ina226': return `INA226 0x${(+s.i2cAddr).toString(16)} ${s.field}`;
      case 'imu': return `MPU-6050 0x${(+s.i2cAddr).toString(16)} ${s.field}`;
      case 'computed': return `${s.formula}(${s.srcA}${s.srcB && ['diff', 'sum', 'curve_margin'].includes(s.formula) ? ', ' + s.srcB : ''})`;
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
      engineRunningOnly: false, faultAlarm: true, i2cAddr: 0, field: '', pulsesPerRev: 10, activeLow: true, shuntOhms: 0.001,
      formula: '', srcA: '', srcB: '', curve: '' }, TEMPLATES[type]);
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

  let BOARD = null;
  CM.board().then(b => { BOARD = b && b.current; });
  async function calibrateRpm(s) {
    const rpm = +$('#rpmNow').value;
    try { toast((await api('/api/rpm/calibrate', { body: { id: s.id, rpm } })).message, false, 6000); setTimeout(load, 1500); }
    catch (e) { toast(e.message, true, 6000); }
  }
  // Analog pins come from the board profile (ADC1 only: ADC2 stops working with Wi-Fi on)
  let ADC_PINS = [32, 33, 34, 35, 36, 39].map(p => [p, `GPIO ${p}`]);
  CM.board().then(b => { if (b && b.current) { ADC_PINS = b.current.adc1.map(p => [p, `GPIO ${p}`]); TEMPLATES.adc.pin = b.current.adc1[0];
    const d = b.current.defaults; Object.assign(TEMPLATES.max6675, { sckPin: d.egtSck, pin: d.egtCs, soPin: d.egtSo }); } });
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
      hw.push(field('Measurement', 'bmpField', s, { type: 'select', options: [['temperature', 'Temperature'], ['pressure', 'Pressure (hPa)'], ['altitude', 'Altitude (m)'], ['humidity', 'Humidity (%, BME280 only)']] }));
    } else if (s.type === 'adc') {
      hw.push(field('Analog pin (ADC1)', 'pin', s, { type: 'select', options: ADC_PINS, numeric: true }));
    } else if (s.type === 'ads1115') {
      hw.push(field('ADS1115 address', 'adsAddress', s, { type: 'select', numeric: true, options: [[72, '0x48 (ADDR→GND)'], [73, '0x49 (ADDR→VDD)'], [74, '0x4A (ADDR→SDA)'], [75, '0x4B (ADDR→SCL)']] }),
              field('Channel', 'adsChannel', s, { type: 'select', numeric: true, options: [[0, 'A0'], [1, 'A1'], [2, 'A2'], [3, 'A3']] }));
    }
    const pinOpts = () => [[-1, '— choose —'], ...(BOARD ? BOARD.valid : []).map(p => [p, `GPIO ${p}`])];
    const setField = (sel) => { sel.addEventListener('change', () => { if (FIELD_UNITS[s.field] !== undefined) s.unit = FIELD_UNITS[s.field]; edit(editing); renderList(); }); return sel; };
    if (s.type === 'rpm') {
      hw.push(field('Signal GPIO', 'pin', s, { type: 'select', options: pinOpts(), numeric: true }),
        field('Pulses per engine revolution', 'pulsesPerRev', s, { nullable: false, step: '0.01' }),
        h('div', { class: 'wide' }, h('label', {}, 'Calibrate: engine speed right now (e.g. idle on a tachometer)'),
          h('div', { class: 'row' }, h('input', { id: 'rpmNow', type: 'number', placeholder: 'e.g. 800', style: 'max-width:140px' }),
            h('button', { class: 'small', onclick: () => calibrateRpm(s) }, 'Calibrate'))),
        h('p', { class: 'small wide', style: 'margin:0' }, 'Live: ', h('b', { id: 'edLive' }, '—')),
        h('p', { class: 'muted small wide', style: 'margin:0' }, 'Wire the alternator W terminal through an opto-coupler or comparator board: never straight to the ESP32. Save the sensor first, then calibrate with the engine running.'));
    } else if (s.type === 'switch') {
      hw.push(field('Input GPIO', 'pin', s, { type: 'select', options: pinOpts(), numeric: true }),
        field('Active when pulled to ground (internal pull-up)', 'activeLow', s, { type: 'checkbox', wide: true }),
        h('p', { class: 'muted small wide', style: 'margin:0' }, 'Value is 1 when active. Set "Warning high" to 0.5 to alarm when it switches. 12 V switches need a divider or opto-coupler.'));
    } else if (s.type === 'gps') {
      hw.push(setField(field('Value', 'field', s, { type: 'select', options: [['speed', 'Speed (km/h)'], ['altitude', 'Altitude (m)'], ['sats', 'Satellites'], ['heading', 'Heading (°)'], ['hdop', 'Accuracy (HDOP)']] })),
        h('p', { class: 'muted small wide', style: 'margin:0' }, 'Set the GPS UART pins on the System page. Without a module this shows "not found" and never alarms (unless you tick "alarm if missing").'));
    } else if (s.type === 'ina226') {
      hw.push(field('I2C address', 'i2cAddr', s, { type: 'select', numeric: true, options: Array.from({ length: 16 }, (_, k) => [0x40 + k, '0x' + (0x40 + k).toString(16)]) }),
        setField(field('Value', 'field', s, { type: 'select', options: [['current', 'Current (A)'], ['voltage', 'Voltage (V)'], ['power', 'Power (W)']] })),
        field('Shunt resistor (Ω)', 'shuntOhms', s, { nullable: false, step: '0.0001' }));
    } else if (s.type === 'imu') {
      hw.push(field('I2C address', 'i2cAddr', s, { type: 'select', numeric: true, options: [[0x68, '0x68 (AD0→GND)'], [0x69, '0x69 (AD0→VCC)']] }),
        setField(field('Value', 'field', s, { type: 'select', options: [['roll', 'Roll (side tilt)'], ['pitch', 'Pitch (nose up/down)']] })),
        h('p', { class: 'muted small wide', style: 'margin:0' }, 'Mount it flat and use "Offset" to zero it on level ground.'));
    } else if (s.type === 'computed') {
      const srcOpts = [['', '— choose —'], ...sensors.filter(x => x.id !== s.id).map(x => [x.id, x.name])];
      const f = s.formula;
      hw.push(field('Formula', 'formula', s, { type: 'select', rerender: true, options: [['diff', 'A − B (e.g. boost = MAP − baro)'], ['sum', 'A + B'], ['rate', 'Rate of change of A, per minute'], ['time_in_warn', 'Minutes A has been in warning'], ['curve_margin', 'A − curve(B) (e.g. oil pressure above its RPM minimum)']] }),
        field('A', 'srcA', s, { type: 'select', options: srcOpts }));
      if (['diff', 'sum', 'curve_margin'].includes(f)) hw.push(field('B', 'srcB', s, { type: 'select', options: srcOpts }));
      if (f === 'curve_margin') hw.push(field('Curve: B → minimum A', 'curve', s, { type: 'text', wide: true, placeholder: '800:10,2000:25,3500:40' }),
        h('p', { class: 'muted small wide', style: 'margin:0' }, 'Set "Critical low" to 0 to alarm when A drops below the curve.'));
    }
    const analog = s.type === 'adc' || s.type === 'ads1115';
    let calib = null;
    if (analog) {
      const preset = h('select', {}, Object.entries(PRESETS).map(([k, p]) => h('option', { value: k }, p ? p.label : 'Apply a preset…')));
      preset.addEventListener('change', () => { const p = PRESETS[preset.value]; if (p) { const { label, ...rest } = p; Object.assign(s, rest); edit(editing); renderList(); } });
      calib = h('fieldset', {}, h('legend', {}, 'Calibration'),
        h('p', { class: 'muted small', style: 'margin-top:0' },
          'sensor volts = pin volts × divider. The input range (sensor volts) maps linearly onto the output range. ',
          'Fault limits flag a broken wire (e.g. below 0.35 V on a 0.5–4.5 V sender). Fit a 100 kΩ pull-down on each analog input so an unplugged sender reads 0 V and shows FAULT.'),
        h('div', { class: 'fields' },
          h('div', { class: 'wide' }, preset),
          field('Divider (×)', 'divider', s, { nullable: false }),
          field('Input min (V)', 'inMin', s, { nullable: false }), field('Input max (V)', 'inMax', s, { nullable: false }),
          field('Output at min', 'outMin', s, { nullable: false }), field('Output at max', 'outMax', s, { nullable: false }),
          field('Fault below (V)', 'faultLowV', s), field('Fault above (V)', 'faultHighV', s),
          field('Non-linear curve: sensor volts → value (optional, replaces the ranges)', 'curve', s, { type: 'text', wide: true, placeholder: 'fuel sender: 0.4:0,1.2:25,2.0:50,2.7:75,3.2:100' })),
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
        field('Only alarm while engine runs', 'engineRunningOnly', s, { type: 'checkbox', wide: true }),
        field('Alarm if sensor is missing or faulty', 'faultAlarm', s, { type: 'checkbox', wide: true }))));
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
      if (el && r && s.type === 'rpm') el.textContent = `${r.volts != null ? (+r.volts).toFixed(1) : '–'} Hz`;
      else if (el && r) el.textContent = r.volts != null ? `${(+r.volts).toFixed(3)} V at sensor → ${r.v === null ? statusText[r.st] : fmt(+r.v, s.decimals) + ' ' + s.unit}` : statusText[r.st] || '—';
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
