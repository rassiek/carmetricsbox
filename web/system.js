'use strict';
// System tab: Status, Network, Hardware, Data.
(() => {
  const { h, api, get, patch, del, toast, act, err, S, page, form, loadPins } = CM;
  const SECTIONS = [['status', 'Status'], ['network', 'Network'], ['hardware', 'Hardware'], ['data', 'Data']];
  const copy = o => JSON.parse(JSON.stringify(o || {}));
  const kv = rows => h('table', { class: 'kv' }, rows.filter(Boolean).map(([k, v]) => h('tr', {}, [h('th', {}, k), h('td', {}, v ?? '')])));
  const panel = (title, ...kids) => h('div', { class: 'panel' }, [h('h3', {}, title), ...kids]);
  // A settings form over one section (or part of it), saved with PATCH /settings/<section>
  function sectionForm(section, fields, pick, note) {
    const o = copy(pick ? pick(S.settings[section]) : S.settings[section]);
    return [form(o, fields), note ? h('p', { class: 'muted small' }, note) : null,
            h('button', { class: 'primary', onclick: () => act(() => patch(`/settings/${section}`, o), 'Saved') }, 'Save')];
  }
  const sysPick = keys => s => Object.fromEntries(keys.map(k => [k, s[k]]));
  const num = (k, label, extra) => ({ k, label, type: 'num', ...extra });

  // ================================================================ Status
  async function status(box) {
    let d; try { d = await get('/system'); } catch (e) { return err(e); }
    const m = d.memory || {}, fw = d.firmware || {}, st = d.storage || {};
    box.append(panel('This board', kv([
      ['Firmware', `${fw.version} (${S.info.variant})`], ['Built', fw.build], ['Board', `${d.board?.name || ''}`], ['Chip', `${d.chip?.model || ''} · ${d.chip?.flashMB} MB flash · ${Math.round((d.chip?.psramKB || 0) / 1024)} MB PSRAM`],
      ['Running for', `${Math.floor(d.uptime.s / 3600)} h ${Math.floor(d.uptime.s / 60) % 60} min · last restart: ${d.uptime.resetReason}`],
      ['Memory free', `${Math.round(m.free / 1024)} KB (lowest ${Math.round(m.min / 1024)} KB, largest block ${Math.round(m.largest / 1024)} KB)${m.psramFree ? ` · PSRAM ${Math.round(m.psramFree / 1024)} KB` : ''}`],
      ['CPU', (d.cpu || []).map(c => `${Math.round(c)} %`).join(' / ')], ['Loop', `${d.loop?.avgUs} µs average, ${d.loop?.maxUs} µs max`],
      ['Storage', `${Math.round(st.fsUsed / 1024)} of ${Math.round(st.fsTotal / 1024)} KB used`],
      ['Viewers', `${d.clients?.websocket || 0} live, ${d.clients?.events || 0} on events`],
    ])));
    if (d.safeMode) box.append(panel('Safe mode', h('p', {}, `The board restarted after crashes (${d.safeReason}). Outputs are off, internet features stopped.`),
      h('button', { class: 'primary', onclick: () => act(() => api('/system/safe-mode/leave', { body: {} })) }, 'Leave safe mode')));
    if (d.crash) {
      const c = d.crash;
      box.append(panel('Last crash', kv([['Cause', c.cause], ['Task', c.task], ['Where', c.pc], ['This firmware', c.thisFirmware ? 'yes' : 'no (an older one)']]),
        h('pre', { class: 'small' }, c.backtrace || ''), h('div', { class: 'row' }, [
          h('a', { class: 'btn small', href: '/api/v1/system/crash/dump' }, 'Download core dump'),
          h('button', { class: 'small', onclick: () => act(() => del('/system/crash')).then(() => show(el, 'status')) }, 'Clear')])));
    }
    box.append(panel('Tasks', h('table', {}, [h('tr', {}, [h('th', {}, 'Task'), h('th', {}, 'Stack left')]), ...(d.tasks || []).map(t => h('tr', {}, [h('td', {}, t.name), h('td', {}, `${t.stackFree} B`)]))])));
    box.append(panel('Restart and undo', h('div', { class: 'row' }, [
      h('button', { onclick: () => confirm('Restart the board?') && act(() => api('/system/reboot', { body: {} })) }, 'Restart'),
      h('button', { onclick: () => confirm('Go back to the settings before the last change?') && act(() => api('/settings/restore', { body: { slot: 'previous' } })) }, 'Undo last settings change'),
      h('button', { onclick: () => confirm('Go back to the last settings that ran without problems?') && act(() => api('/settings/restore', { body: { slot: 'good' } })) }, 'Last good settings')])));
  }

  // ================================================================ Network
  async function network(box) {
    let w; try { w = await get('/network'); } catch (e) { return err(e); }
    const saved = (S.settings.system.networks || []).map(n => n.ssid);
    const wifi = h('div');
    const drawScan = () => {
      wifi.innerHTML = '';
      wifi.append(kv([['Status', w.state], w.ssid ? ['Joined', `${w.ssid} · ${w.ip} · ${w.rssi} dBm`] : null, ['Access point', `${S.settings.system.apSsid} · ${w.apClients || 0} connected`]]));
      const list = h('div');
      for (const n of (w.scan || []).slice(0, 15)) {
        const pass = h('input', { type: 'password', placeholder: n.saved ? 'saved password (leave empty)' : 'password', style: 'max-width:200px' });
        list.append(h('div', { class: 'list-item' }, [h('div', { class: 'grow' }, [h('b', {}, n.ssid || '(hidden)'), h('span', { class: 'muted small' }, ` ${n.rssi} dBm · ch ${n.ch}${n.security ? '' : ' · open'}${n.saved ? ' · saved' : ''}${/^CarMetrics/i.test(n.ssid || '') ? ' · another CarMetrics board' : ''}`)]),
          n.security ? pass : null, h('button', { class: 'small primary', onclick: () => act(() => api('/network/join', { body: { ssid: n.ssid, pass: pass.value } })) }, n.ssid === w.ssid ? 'Joined' : 'Join')]));
      }
      wifi.append(h('div', { class: 'row', style: 'margin:8px 0' }, [h('b', {}, 'Networks in range'), h('span', { class: 'muted small' }, w.scanAgeS >= 0 ? `searched ${w.scanAgeS} s ago` : ''),
        h('span', { class: 'spacer' }), h('button', { class: 'small', onclick: async () => {
          if (!await act(() => api('/network/scan', { body: {} }), 'Searching…')) return;
          setTimeout(async () => { try { w = await get('/network'); drawScan(); } catch {} }, 5000);
        } }, 'Search')]), list.children.length ? list : h('p', { class: 'muted small' }, 'Press Search.'));
      const other = { ssid: '', pass: '' };
      wifi.append(h('details', {}, [h('summary', { class: 'small' }, 'Hidden network / type the name'), form(other, [{ k: 'ssid', label: 'Network name' }, { k: 'pass', label: 'Password', type: 'pass' }]),
        h('button', { class: 'small', onclick: () => act(() => api('/network/join', { body: other })) }, 'Join')]));
      wifi.append(h('div', { class: 'row', style: 'margin-top:8px' }, [h('b', {}, 'Saved'), ...saved.map(s => h('span', { class: 'chip' }, [s, ' ', h('a', { href: '#system/network', onclick: e => { e.preventDefault(); confirm(`Forget ${s}?`) && act(() => del('/network/saved/' + encodeURIComponent(s))); } }, '×')]))]));
    };
    drawScan();
    box.append(panel('Wi-Fi', wifi));
    if (!(w.scanAgeS >= 0 && w.scanAgeS < 60)) {   // an old list: search again (the results follow in a few seconds)
      api('/network/scan', { body: {} }).then(() => setTimeout(async () => { try { w = await get('/network'); drawScan(); } catch {} }, 5000)).catch(() => {});
    }
    box.append(panel('Access point and names', ...sectionForm('system', [
      { k: 'apSsid', label: 'Access point name' }, { k: 'apPass', label: 'Access point password (empty = keep)', type: 'pass' },
      { k: 'hostname', label: 'Host name (http://<name>.local)' }, { k: 'scanWhileApInUse', label: 'Search for networks while a phone is connected', type: 'bool' }],
      sysPick(['apSsid', 'apPass', 'hostname', 'scanWhileApInUse']), 'Changing the access point restarts the board.')));
    box.append(panel('Password and access', ...sectionForm('system', [
      { k: 'adminPass', label: 'Admin password (empty = keep)', type: 'pass' }, { k: 'readNeedsAuth', label: 'Reading needs the password or a key too', type: 'bool' }],
      sysPick(['adminPass', 'readNeedsAuth']))));
    box.append(await keysPanel());
    box.append(panel('Board link', ...sectionForm('system', [
      { k: 'role', label: 'Role', type: 'select', opts: [['main', 'Main board'], ['sensor', 'Extra board: sensors'], ['relay', 'Extra board: actuators'], ['sensor_relay', 'Extra board: both']] },
      { k: 'boardEncrypt', label: 'Encrypt the link for boards paired from now on', type: 'bool' },
      { k: 'canEnabled', label: 'CAN link too (wired)', type: 'bool', redraw: true },
      { k: 'canTx', label: 'CAN TX pin', type: 'pin', show: o => o.canEnabled }, { k: 'canRx', label: 'CAN RX pin', type: 'pin', input: true, show: o => o.canEnabled },
      { k: 'canKbps', label: 'CAN speed (kbit/s)', type: 'select', num: true, opts: [[125, '125'], [250, '250'], [500, '500'], [1000, '1000']], show: o => o.canEnabled }],
      sysPick(['role', 'boardEncrypt', 'canEnabled', 'canTx', 'canRx', 'canKbps']), 'Changing the role restarts the board.')));
  }
  async function keysPanel() {
    const box = h('div');
    const draw = async () => {
      box.innerHTML = '';
      let k; try { k = await get('/keys'); } catch (e) { box.append(h('p', { class: 'muted small' }, e.message)); return; }
      for (const x of k.items) box.append(h('div', { class: 'list-item' }, [h('div', { class: 'grow' }, [h('b', {}, x.name), h('span', { class: 'muted small' }, ` ${x.scope} · id ${x.id}`)]),
        h('button', { class: 'small danger', onclick: () => confirm(`Delete key ${x.name}?`) && act(() => del('/keys/' + x.id)).then(draw) }, 'Delete')]));
      const n = { name: '', scope: 'read' };
      box.append(form(n, [{ k: 'name', label: 'New key: name' }, { k: 'scope', label: 'Allows', type: 'select', opts: [['read', 'Reading'], ['control', 'Reading + switching outputs, mute, limits'], ['admin', 'Everything']] }]),
        h('button', { class: 'small', onclick: async () => { const r = await act(() => api('/keys', { body: n })); if (r && r.key) { prompt('Copy the key now (shown once):', r.key); draw(); } } }, 'Make key'));
    };
    draw();
    return panel('API keys (dashboards, Home Assistant, scripts)', h('p', { class: 'muted small' }, 'Send as "Authorization: Bearer <key>". See docs/API.md.'), box);
  }

  // ================================================================ Hardware
  async function hardware(box) {
    const pins = await loadPins();
    let prof; try { prof = await get('/hardware/profiles'); } catch (e) { return err(e); }
    const p = { board: prof.setting, applyPins: false };
    box.append(panel('Board', form(p, [{ k: 'board', label: 'Board type', type: 'select', opts: [['auto', `Detect (${(prof.items.find(x => x.id === prof.detected) || { name: prof.detected }).name})`]].concat(prof.items.map(x => [x.id, x.name])) },
      { k: 'applyPins', label: 'Move the bus and default sensor pins to this board\'s defaults', type: 'bool' }]),
      h('p', { class: 'muted small' }, prof.current.notes), h('button', { class: 'primary', onclick: () => act(() => api('/hardware/profile', { body: p })) }, 'Save')));
    const table = h('table', {}, [h('tr', {}, ['GPIO', 'Can do', 'Used by', ''].map(t => h('th', {}, t)))]);
    for (const x of pins.pins) table.append(h('tr', {}, [h('td', {}, x.gpio), h('td', { class: 'small' }, x.abilities.join(', ') + (x.fixed ? ` · ${x.fixed}` : '')), h('td', { class: 'small' }, x.usedBy || ''),
      h('td', {}, [x.abilities.includes('output') && !x.usedBy ? h('button', { class: 'small', onclick: () => act(() => api(`/pins/${x.gpio}/test`, { body: { mode: 'blink' } }), `GPIO ${x.gpio} blinking for 5 s`) }, 'Blink') : null,
        h('button', { class: 'small', onclick: async () => { const r = await act(() => api(`/pins/${x.gpio}/test`, { body: { mode: 'read' } }), ' '); if (r) toast(`GPIO ${x.gpio}: ${r.level ? 'HIGH' : 'LOW'}${r.volts !== undefined ? `, ${r.volts} V` : ''}`); } }, 'Read')])]));
    box.append(panel('Pins', (pins.warnings || []).map(w => h('p', { class: 'bad small' }, w)), table));
    box.append(panel('Status light', ...sectionForm('system', [
      { k: 'statusLight.builtin', label: 'Built-in RGB LED', type: 'bool', redraw: true },
      { k: 'statusLight.builtinPin', label: 'Its pin', type: 'pin', show: o => o.statusLight.builtin, help: '(not used) = the board\'s own (S3 DevKitC: 48, v1.1: 38)' },
      num('statusLight.builtinBrightness', 'Brightness %', { show: o => o.statusLight.builtin }),
      { k: 'statusLight.external', label: 'External RGB LED', type: 'bool', redraw: true },
      { k: 'statusLight.r', label: 'Red pin', type: 'pin', show: o => o.statusLight.external }, { k: 'statusLight.g', label: 'Green pin', type: 'pin', show: o => o.statusLight.external },
      { k: 'statusLight.b', label: 'Blue pin', type: 'pin', show: o => o.statusLight.external },
      { k: 'statusLight.commonAnode', label: 'Common anode (long leg to 3.3 V)', type: 'bool', show: o => o.statusLight.external },
      num('statusLight.externalBrightness', 'Brightness %', { show: o => o.statusLight.external })], sysPick(['statusLight']),
      'Green OK · amber warning · amber flashing sensor fault · red flashing critical (steady when muted) · blue flashing: extra board without its main board · purple: safe mode.'),
      h('div', { class: 'row' }, [h('button', { class: 'small', onclick: () => act(() => api('/hardware/status-light/test', { body: { which: 'builtin' } })) }, 'Test built-in'),
        h('button', { class: 'small', onclick: () => act(() => api('/hardware/status-light/test', { body: { which: 'external' } })) }, 'Test external')])));
    box.append(panel('Buses, buzzer, LED', ...sectionForm('system', [
      { k: 'i2cSda', label: 'I2C SDA', type: 'pin' }, { k: 'i2cScl', label: 'I2C SCL', type: 'pin' }, num('i2cKhz', 'I2C kHz'),
      { k: 'oneWirePin', label: '1-Wire pin', type: 'pin' }, { k: 'oneWireResolution', label: 'DS18B20 resolution', type: 'select', num: true, opts: [[9, '9 bit'], [10, '10 bit'], [11, '11 bit'], [12, '12 bit']] },
      { k: 'buzzerPin', label: 'Buzzer pin', type: 'pin' }, { k: 'buzzerActiveLow', label: 'Buzzer sounds on LOW', type: 'bool' },
      { k: 'ledPin', label: 'Warning LED pin', type: 'pin' }, { k: 'ledActiveLow', label: 'LED on when LOW', type: 'bool' }],
      sysPick(['i2cSda', 'i2cScl', 'i2cKhz', 'oneWirePin', 'oneWireResolution', 'buzzerPin', 'buzzerActiveLow', 'ledPin', 'ledActiveLow']), 'Bus changes restart the board.')));
    box.append(panel('OLED', ...sectionForm('system', [
      { k: 'oledType', label: 'Screen', type: 'select', opts: [['none', 'None'], ['ssd1306_128x64', '0.96" SSD1306 128×64'], ['sh1106_128x64', '1.3" SH1106 128×64'], ['ssd1327_128x128', '1.5" SSD1327 128×128'], ['ssd1327_ws_128x128', '1.5" SSD1327 (Waveshare)']] },
      { k: 'oledFlip', label: 'Upside down', type: 'bool' }, { k: 'oledClock', label: 'Show the clock', type: 'bool' }, num('oledPageSeconds', 'Seconds per page'), num('oledContrast', 'Contrast (0-255)')],
      sysPick(['oledType', 'oledFlip', 'oledClock', 'oledPageSeconds', 'oledContrast'])), h('a', { class: 'btn small', href: '#dashboard/layout' }, 'OLED pages and dashboard tiles')));
    box.append(panel(`Running detection (${CM.ui().runningWord || 'engine'})`, ...sectionForm('system', [
      { k: 'engineSensorId', label: 'Sensor that shows it runs', type: 'sensor' }, num('engineRunningAbove', 'Running above'), num('engineDelaySeconds', 'After (s)'),
      { k: 'ignitionPin', label: 'Ignition sense pin', type: 'pin', input: true }, { k: 'ignitionActiveLow', label: 'Ignition on when LOW', type: 'bool' },
      num('sleepAfterMinutes', 'Sleep after ignition off (min, 0 = never)'),
      { k: 'gpsRxPin', label: 'GPS RX pin (GPS TX)', type: 'pin', input: true }, { k: 'gpsTxPin', label: 'GPS TX pin', type: 'pin' }, num('gpsBaud', 'GPS baud')],
      sysPick(['engineSensorId', 'engineRunningAbove', 'engineDelaySeconds', 'ignitionPin', 'ignitionActiveLow', 'sleepAfterMinutes', 'gpsRxPin', 'gpsTxPin', 'gpsBaud']))));
    box.append(panel('Turbo timer', ...sectionForm('turbo', [{ k: 'enabled', label: 'On', type: 'bool' }, { k: 'egtId', label: 'EGT sensor', type: 'sensor' }, num('safeEgt', 'Safe to stop below'),
      num('minIdleSeconds', 'Idle at least (s)'), { k: 'rpmId', label: 'RPM sensor', type: 'sensor' }, num('idleRpm', 'Idle below RPM'), { k: 'speedId', label: 'Speed sensor', type: 'sensor' }, { k: 'beep', label: 'Beep when OK', type: 'bool' }])));
    const alt = { altitudeM: null };
    box.append(panel('Altitude', form(alt, [num('altitudeM', 'Where you are now (m)')]), h('button', { class: 'small', onclick: () => act(() => api('/hardware/altitude', { body: alt })) }, 'Calibrate')));
  }

  // ================================================================ Data
  async function data(box) {
    const u = copy(S.settings.ui);
    let pr = { items: [] }; try { pr = await get('/presets'); } catch {}
    const pick = { preset: pr.current || '' };
    box.append(panel('Names and units', form(u, [
      { k: 'appName', label: 'App name' }, { k: 'machineName', label: 'Machine name' }, { k: 'runningWord', label: 'What runs (engine, pump, generator)' },
      ...['dashboard', 'sensors', 'actuators', 'boards', 'system'].map(p => ({ k: `pages.${p}`, label: `Tab: ${p}` })),
      { k: 'units.temperature', label: 'Temperature', type: 'select', opts: [['C', '°C'], ['F', '°F']] },
      { k: 'units.pressure', label: 'Pressure', type: 'select', opts: [['', 'as each sensor'], ['bar', 'bar'], ['psi', 'psi'], ['kPa', 'kPa']] },
      { k: 'units.speed', label: 'Speed', type: 'select', opts: [['', 'as each sensor'], ['km/h', 'km/h'], ['mph', 'mph'], ['kn', 'knots']] }]),
      h('button', { class: 'primary', onclick: () => act(() => patch('/settings/ui', u), 'Saved') }, 'Save'),
      h('div', { class: 'row', style: 'margin-top:10px' }, [form(pick, [{ k: 'preset', label: 'Start from a preset', type: 'select', opts: [['', '(choose)']].concat(pr.items.map(x => [x, x])) }]),
        h('button', { onclick: () => pick.preset && confirm(`Apply the ${pick.preset} preset? Names and units change.`) && act(() => api('/presets/apply', { body: pick })) }, 'Apply')])));
    const towns = copy(S.settings.weather);
    box.append(panel('Weather', form(towns, [{ k: 'enabled', label: 'Show the weather (needs internet)', type: 'bool' }, num('refreshMinutes', 'Refresh every (min)'),
      { k: 'autoSeaLevel', label: 'Correct the altitude with the weather', type: 'bool' }, { k: 'refTown', label: 'Town used for that' }]),
      townsEditor(towns), h('button', { class: 'primary', onclick: () => act(() => patch('/settings/weather', towns), 'Saved') }, 'Save')));
    let cs = {}; try { cs = await get('/cloud'); } catch {}
    box.append(panel('Metrics upload (Zabbix / InfluxDB)', ...sectionForm('cloud', [
      { k: 'enabled', label: 'On', type: 'bool' }, { k: 'protocol', label: 'Send to', type: 'select', opts: [['zabbix', 'Zabbix'], ['influx', 'InfluxDB']], redraw: true },
      num('intervalMs', 'Sample every (ms)'), num('uploadEverySec', 'Upload every (s)'), num('bufferKB', 'Keep offline (KB)'),
      { k: 'onlyEngineRunning', label: 'Only while running', type: 'bool' }, { k: 'sendSystem', label: 'Include board health', type: 'bool' },
      { k: 'zbxServer', label: 'Zabbix server', show: o => o.protocol === 'zabbix' }, num('zbxPort', 'Port', { show: o => o.protocol === 'zabbix' }),
      { k: 'zbxHost', label: 'Host name in Zabbix', show: o => o.protocol === 'zabbix' }, { k: 'zbxPacked', label: 'One packed item', type: 'bool', show: o => o.protocol === 'zabbix' },
      { k: 'zbxPskIdentity', label: 'PSK identity', show: o => o.protocol === 'zabbix' }, { k: 'zbxPsk', label: 'PSK (empty = keep)', type: 'pass', show: o => o.protocol === 'zabbix' },
      { k: 'influxUrl', label: 'InfluxDB write URL', wide: true, show: o => o.protocol === 'influx' }, { k: 'influxAuth', label: 'Authorization (empty = keep)', type: 'pass', show: o => o.protocol === 'influx' },
      { k: 'influxMeasurement', label: 'Measurement', show: o => o.protocol === 'influx' }]),
      h('pre', { class: 'small' }, JSON.stringify(cs, null, 1).slice(0, 600)),
      h('div', { class: 'row' }, [h('button', { class: 'small', onclick: () => act(() => api('/cloud/flush', { body: {} })) }, 'Upload now'),
        h('button', { class: 'small danger', onclick: () => confirm('Delete data waiting to be uploaded?') && act(() => del('/cloud/backlog')) }, 'Delete waiting data')])));
    box.append(panel('Alarm recordings and history', ...sectionForm('blackbox', [{ k: 'enabled', label: 'Record 30 s before and after each alarm', type: 'bool' }, num('keepEvents', 'Keep the last')]),
      ...sectionForm('system', [num('historyIntervalSeconds', 'Chart history: a point every (s)'), num('tzOffsetMinutes', 'Time zone (minutes from UTC)')], sysPick(['historyIntervalSeconds', 'tzOffsetMinutes']))));
    box.append(panel('Backup', h('div', { class: 'row' }, [
      h('a', { class: 'btn', href: '/api/v1/backup' }, 'Download settings'),
      h('label', { class: 'btn' }, ['Restore from file', h('input', { type: 'file', accept: '.json', class: 'hidden', onchange: async e => {
        const f = e.target.files[0]; if (!f) return;
        let j; try { j = JSON.parse(await f.text()); } catch { return toast('Not a settings file', true); }
        if (confirm('Replace all settings with this file?')) act(() => api('/backup', { body: j }));
      } })]),
      h('button', { class: 'danger', onclick: () => confirm('Sensors, actuators and layout back to defaults? Network and passwords are kept.') && act(() => api('/settings/reset', { body: { keep: ['system'] } })) }, 'Reset to defaults')])));
    const fwIn = (target, label) => h('label', { class: 'btn' }, [label, h('input', { type: 'file', accept: '.bin', class: 'hidden', onchange: async e => {
      const f = e.target.files[0]; if (!f) return;
      toast('Uploading ' + f.name + '…', false, 120000);
      await act(() => api('/firmware' + (target ? '?target=' + target : ''), { method: 'PUT', raw: f }));
    } })]);
    box.append(panel('Firmware update', h('p', { class: 'muted small' }, 'First the firmware (…-firmware.bin), then the web app (…-webapp.bin) from the same release. The …-usb.bin file is for USB only.'),
      h('div', { class: 'row' }, [fwIn('', 'Firmware file'), fwIn('webapp', 'Web app file')])));
  }
  function townsEditor(w) {
    const box = h('div');
    const draw = () => {
      box.innerHTML = '';
      (w.towns || []).forEach((t, i) => box.append(h('span', { class: 'chip' }, [t.name, ' ', h('a', { href: '#system/data', onclick: e => { e.preventDefault(); w.towns.splice(i, 1); draw(); } }, '×')])));
      if (typeof KENYA_TOWNS !== 'undefined' && (w.towns || []).length < 4) {
        const sel = h('select', { style: 'width:auto', onchange: e => { const t = KENYA_TOWNS.find(x => x[0] === e.target.value); if (t) { (w.towns ||= []).push({ name: t[0], lat: t[1], lon: t[2] }); draw(); } } },
          [h('option', { value: '' }, 'Add a town…')].concat(KENYA_TOWNS.map(t => h('option', { value: t[0] }, t[0]))));
        box.append(sel);
      }
    };
    draw();
    return box;
  }

  let el = null;
  async function show(main, sub) {
    el = main;
    const s = SECTIONS.some(x => x[0] === sub) ? sub : 'status';
    main.innerHTML = '';
    const box = h('div');
    main.append(h('div', { class: 'seg' }, SECTIONS.map(([k, t]) => h('a', { href: `#system/${k}`, class: k === s ? 'active' : '' }, t))), box);
    if (s === 'hardware' || s === 'network') await loadPins();
    await ({ status, network, hardware, data })[s](box);
  }
  page('system', { show, leave: () => { el = null; } });
})();
