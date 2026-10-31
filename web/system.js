'use strict';
(() => {
  const { $, h, api, toast } = CM;
  const form = () => $('#sys');

  function dur(s) { const d = Math.floor(s / 86400), hh = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60); return (d ? d + 'd ' : '') + hh + 'h ' + m + 'm'; }
  const kb = b => (b / 1024).toFixed(0) + ' KB';

  async function loadInfo() {
    const i = await api('/api/info');
    const rows = [
      ['Firmware', `${i.version} (${i.build})`],
      ['Uptime', dur(i.uptimeS)],
      ['Access point', `${i.apSsid} – ${i.apIp}`],
      ['Network', i.staSsid && i.ip ? `${i.staSsid} – ${i.ip} (${i.rssi} dBm)` : i.networks ? (i.wifiState || 'not connected') : 'no networks saved'],
      ['Address', `http://${i.hostname}.local`],
      ['BMP sensor', i.bmp], ['OLED', i.oled],
      ['Memory', `${kb(i.heapFree)} free (lowest ${kb(i.heapMin)})`],
      ['Web app storage', `${kb(i.fsUsed)} / ${kb(i.fsTotal)}`],
      ['Firmware space', `${kb(i.sketchSize)} used, ${kb(i.sketchFree)} slot`],
      ['Clock', i.timeSynced ? 'set from browser' : 'not set'],
    ];
    const t = $('#info');
    t.innerHTML = '';
    rows.forEach(([k, v]) => t.append(h('tr', {}, h('th', { style: 'width:40%' }, k), h('td', {}, v))));
  }

  // ---- board health (refreshes every 5 s)
  const pct = v => v === null || v === undefined ? '–' : v + '%';
  async function loadHealth() {
    const m = await api('/api/v2/system');
    const fw = $('#hFw');
    fw.textContent = `${m.firmware.slot}: ${m.firmware.state}`;
    fw.className = 'pill ' + (m.firmware.state === 'confirmed' || m.firmware.state === 'factory' ? 'ok' : 'warn');
    $('#hSafe').classList.toggle('hidden', !m.safeMode);
    $('#hSafe').textContent = m.safeMode ? `Safe mode: ${m.safeReason}. Running the last known-good settings with relays off and internet features paused. Fix the cause, then leave safe mode.` : '';
    $('#hLeave').classList.toggle('hidden', !m.safeMode);
    $('#hNote').classList.toggle('hidden', !m.firmware.note);
    $('#hNote').textContent = m.firmware.note || '';
    const mem = m.memory, k = b => (b / 1024).toFixed(0) + ' KB';
    const tiles = [
      ['Uptime', dur(m.uptime.s), `boot #${m.uptime.boots}, last reset: ${m.uptime.resetReason}`],
      ['CPU core 0', pct(m.cpu[0]), 'Wi-Fi, display, uploads'],
      ['CPU core 1', pct(m.cpu[1]), 'sensors, alarms, web'],
      ['Free memory', k(mem.free), `lowest ${k(mem.min)}, largest block ${k(mem.largest)}`],
      ['Loop time', (m.loop.avgUs / 1000).toFixed(1) + ' ms', `max ${(m.loop.maxUs / 1000).toFixed(1)} ms (10 s)`],
      ['Wi-Fi', m.wifi.connected ? `${m.wifi.rssi} dBm` : 'not joined', m.wifi.connected ? `${m.wifi.ssid}, ch ${m.wifi.channel}` : `${m.wifi.apClients} on CarMetrics AP`],
      ['Flash writes', String(m.flash.configWrites + m.flash.bufferWrites), `settings ${m.flash.configWrites}, buffer ${m.flash.bufferWrites} (${m.flash.bufferKB} KB)`],
      ['Clients', String(m.clients.websocket + m.clients.events), `${m.clients.websocket} WebSocket, ${m.clients.events} events`],
      ['Chip', m.chip.model, `${m.chip.cores} cores, ${m.chip.mhz} MHz, ${m.chip.flashMB} MB flash` + (m.chip.psramKB ? `, ${m.chip.psramKB / 1024} MB PSRAM` : '')],
      ['Temperature', m.chip.tempC === null ? '–' : m.chip.tempC + ' °C', 'chip (rough)'],
      ['Firmware', m.firmware.version, `core ${m.firmware.core}, ${m.firmware.sketchKB} / ${m.firmware.slotKB} KB`],
    ];
    if (m.chip.psramKB) tiles.splice(4, 0, ['PSRAM free', k(mem.psramFree), '']);
    $('#hKv').replaceChildren(...tiles.map(([a, b, c]) => h('div', {}, h('span', {}, a), h('b', {}, b), h('span', {}, c))));
    $('#hTasks').replaceChildren(h('tr', {}, h('th', {}, 'Task'), h('th', {}, 'Stack headroom')),
      ...m.tasks.map(t => h('tr', {}, h('td', {}, t.name), h('td', { style: t.stackFree < 1024 ? 'color:var(--crit)' : '' }, t.stackFree + ' bytes'))));
    $('#hResets').textContent = 'Recent resets (newest first): ' + m.uptime.history.join(', ');
  }

  // ---- board profile
  let boards = null;
  async function loadBoard() {
    boards = await api('/api/v2/board');
    const sel = $('#bSel');
    sel.replaceChildren(h('option', { value: 'auto' }, `Detect automatically (${boards.detected})`),
      ...boards.profiles.map(p => h('option', { value: p.id }, p.name)));
    sel.value = boards.setting;
    showBoard();
  }
  function showBoard() {
    const id = $('#bSel').value, p = boards.profiles.find(x => x.id === (id === 'auto' ? boards.detected : id));
    if (!p) return;
    const list = a => { const r = []; for (let i = 0; i < a.length; i++) { let j = i; while (j + 1 < a.length && a[j + 1] === a[j] + 1) j++; r.push(j > i ? `${a[i]}-${a[j]}` : `${a[i]}`); i = j; } return r.join(', '); };
    $('#bInfo').innerHTML = '';
    $('#bInfo').append(p.notes, h('br'), `Outputs: GPIO ${list(p.output)}. Analog (ADC1): GPIO ${list(p.adc1)}. Use with care: GPIO ${list(p.caution)}.`,
      h('br'), `Default pins: I2C ${p.defaults.i2cSda}/${p.defaults.i2cScl}, 1-Wire ${p.defaults.oneWirePin}, buzzer ${p.defaults.buzzerPin}, LED ${p.defaults.ledPin}.`);
  }
  async function saveBoard() {
    try {
      const r = await api('/api/v2/board', { body: { board: $('#bSel').value, applyPins: $('#bPins').checked } });
      toast(r.message, false, 6000);
      setTimeout(() => { loadBoard().catch(() => {}); loadConfig().catch(() => {}); }, 1500);
    } catch (e) { toast(e.message, true, 8000); }
  }

  // ---- saved Wi-Fi networks
  let nets = [];
  function renderNets() {
    const box = $('#nets');
    box.replaceChildren(...nets.map((n, i) => {
      const ssid = h('input', { value: n.ssid, placeholder: 'Network name' });
      const pass = h('input', { type: 'password', autocomplete: 'new-password', placeholder: n.passSet ? '(saved, blank = keep)' : 'Password (blank = open)' });
      const pr = h('select', {}, [9, 8, 7, 6, 5, 4, 3, 2, 1].map(p => h('option', { value: p, selected: p === n.priority }, `prio ${p}`)));
      ssid.oninput = () => { n.ssid = ssid.value; };
      pass.oninput = () => { n.pass = pass.value; };
      pr.onchange = () => { n.priority = +pr.value; };
      return h('div', { class: 'netrow' }, ssid, pass, pr, h('button', { class: 'small danger', type: 'button', onclick: () => { nets.splice(i, 1); renderNets(); } }, '✕'));
    }));
    if (!nets.length) box.append(h('p', { class: 'muted small' }, 'No networks saved: the board only runs its own access point.'));
  }

  // ---- turbo timer
  async function saveTurbo() {
    const f = $('#turbo'), out = {};
    for (const el of f.elements) {
      if (!el.name) continue;
      out[el.name] = el.type === 'checkbox' ? el.checked : el.type === 'number' ? (el.value === '' ? null : +el.value) : el.value;
    }
    try { toast((await api('/api/config/turbo', { body: out })).message); } catch (e) { toast(e.message, true, 6000); }
  }

  async function loadConfig() {
    const cfg = await api('/api/config');
    nets = (cfg.system.networks || []).map(n => ({ ssid: n.ssid, pass: '', passSet: n.passSet, priority: n.priority }));
    renderNets();
    const tf = $('#turbo'), t = cfg.turbo || {};
    const opts = (first) => [h('option', { value: '' }, first), ...cfg.sensors.map(x => h('option', { value: x.id }, x.name))];
    tf.elements.egtId.replaceChildren(...opts('— choose —')); tf.elements.rpmId.replaceChildren(...opts('— none —')); tf.elements.speedId.replaceChildren(...opts('— none —'));
    for (const el of tf.elements) {
      if (!el.name || !(el.name in t)) continue;
      if (el.type === 'checkbox') el.checked = !!t[el.name]; else el.value = t[el.name] ?? '';
    }
    const f = form(), s = cfg.system;
    const eng = f.elements.engineSensorId;
    eng.innerHTML = '';
    eng.append(h('option', { value: '' }, '— always running —'), ...cfg.sensors.map(x => h('option', { value: x.id }, x.name)));
    for (const el of f.elements) {
      if (!el.name || !(el.name in s)) continue;
      if (el.type === 'checkbox') el.checked = !!s[el.name];
      else el.value = s[el.name] ?? '';
    }
  }

  async function saveSystem() {
    const out = {};
    for (const el of form().elements) {
      if (!el.name) continue;
      if (el.type === 'checkbox') out[el.name] = el.checked;
      else if (el.type === 'number' || el.name === 'oneWireResolution' || el.name === 'i2cKhz' || el.name === 'gpsBaud') out[el.name] = el.value === '' ? null : +el.value;
      else out[el.name] = el.value;
    }
    out.networks = nets.filter(n => n.ssid.trim()).map(n => ({ ssid: n.ssid.trim(), pass: n.pass, priority: n.priority }));
    try {
      const r = await api('/api/config/system', { body: out });
      toast(r.message + '. Network changes reboot the device – reconnect if needed.', false, 6000);
      setTimeout(() => { loadInfo().catch(() => {}); loadConfig().catch(() => {}); }, 1500);
    } catch (e) { toast(e.message, true, 6000); }
  }

  async function ensureAuth() {
    const r = await fetch('/api/auth/check');   // browser shows the login prompt if needed
    if (r.status !== 200) throw new Error('Password required (user "admin")');
  }

  function upload() {
    const file = $('#otaFile').files[0];
    if (!file) { toast('Choose a .bin file first', true); return; }
    const target = $('#otaTarget').value;
    if (target === 'firmware' && /littlefs/i.test(file.name)) { toast('That looks like a web app image – choose "Web app"', true); return; }
    ensureAuth().then(() => {
      const xhr = new XMLHttpRequest(), fd = new FormData();
      fd.append('update', file, file.name);
      xhr.open('POST', '/api/update?target=' + target);
      xhr.upload.onprogress = e => { if (e.lengthComputable) { const p = Math.round(e.loaded / e.total * 100); $('#otaBar').style.width = p + '%'; $('#otaPct').textContent = p + '%'; } };
      xhr.onload = () => {
        let msg = xhr.statusText; try { msg = JSON.parse(xhr.responseText).message; } catch (e) {}
        toast(msg, xhr.status !== 200, 8000);
        if (xhr.status === 200) setTimeout(() => location.reload(), 9000);
      };
      xhr.onerror = () => toast('Upload failed – connection lost', true);
      $('#otaGo').disabled = true;
      xhr.onloadend = () => { $('#otaGo').disabled = false; };
      xhr.send(fd);
    }).catch(e => toast(e.message, true));
  }

  document.addEventListener('DOMContentLoaded', () => {
    loadInfo().catch(e => toast(e.message, true));
    loadConfig().catch(e => toast(e.message, true));
    const hl = () => loadHealth().catch(() => {});
    hl(); setInterval(hl, 5000);
    loadBoard().catch(e => toast(e.message, true));
    $('#bSel').onchange = showBoard;
    $('#bSave').onclick = saveBoard;
    $('#hLeave').onclick = async () => {
      if (!confirm('Leave safe mode and reboot with your normal settings?')) return;
      try { toast((await api('/api/v2/safemode/leave', { method: 'POST' })).message); } catch (e) { toast(e.message, true); }
    };
    const restore = slot => async () => {
      if (!confirm(slot === 'good' ? 'Restore the last known-good settings?' : 'Undo the last settings change?')) return;
      try { toast((await api('/api/v2/config/restore', { body: { slot } })).message); setTimeout(loadConfig, 1500); } catch (e) { toast(e.message, true, 6000); }
    };
    $('#undoCfg').onclick = restore('previous');
    $('#goodCfg').onclick = restore('good');
    $('#sysSave').onclick = saveSystem;
    $('#turboSave').onclick = saveTurbo;
    $('#netAdd').onclick = () => { if (nets.length >= 8) return toast('At most 8 networks', true); nets.push({ ssid: '', pass: '', priority: 5 }); renderNets(); };
    $('#otaGo').onclick = upload;
    $('#reboot').onclick = async () => {
      if (!confirm('Reboot the ESP32?')) return;
      try { toast((await api('/api/reboot', { method: 'POST' })).message); } catch (e) { toast(e.message, true); }
    };
    $('#import').onchange = async ev => {
      const f = ev.target.files[0];
      if (!f) return;
      try {
        const json = JSON.parse(await f.text());
        if (json.system) { delete json.system.apPass; delete json.system.staPass; delete json.system.adminPass; }
        toast((await api('/api/config/import', { body: json })).message);
        setTimeout(loadConfig, 1200);
      } catch (e) { toast(e.message, true, 6000); }
      ev.target.value = '';
    };
    const tz = form().elements.tzOffsetMinutes;
    tz.title = `Your browser: ${-new Date().getTimezoneOffset()}`;
  });
})();
