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
      ['Network', i.staSsid ? (i.ip ? `${i.staSsid} – ${i.ip} (${i.rssi} dBm)` : `${i.staSsid} – not connected`) : 'not configured'],
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

  async function loadConfig() {
    const cfg = await api('/api/config');
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
      else if (el.type === 'number' || el.name === 'oneWireResolution') out[el.name] = el.value === '' ? null : +el.value;
      else out[el.name] = el.value;
    }
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
    $('#sysSave').onclick = saveSystem;
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
