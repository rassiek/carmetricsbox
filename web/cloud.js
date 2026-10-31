'use strict';
// System page: cloud upload settings/status and altitude calibration.
(() => {
  const { $, h, api, toast, fmt } = CM;
  let nSensors = 10;
  const kb = b => (b / 1024).toFixed(b < 10240 ? 1 : 0) + ' KB';
  const dur = s => (s < 120 ? `${Math.round(s)} s` : s < 7200 ? `${Math.round(s / 60)} min` : s < 172800 ? `${(s / 3600).toFixed(1)} h` : `${(s / 86400).toFixed(1)} days`);
  const form = () => $('#cloud');

  function values() {
    const o = {};
    for (const el of form().elements) {
      if (!el.name) continue;
      if (el.type === 'checkbox') o[el.name] = el.checked;
      else if (el.type === 'number' || el.name === 'intervalMs') o[el.name] = el.value === '' ? null : +el.value;
      else o[el.name] = el.value;
    }
    return o;
  }

  function showProto() {
    const p = form().elements.protocol.value;
    document.querySelectorAll('[data-proto]').forEach(f => f.classList.toggle('hidden', f.dataset.proto !== p));
    const packed = form().elements.zbxPacked.checked, host = form().elements.zbxHost.value || 'carmetrics';
    $('#zbxHelp').innerHTML = packed
      ? `In Zabbix: host <code>${host}</code> with a trapper item <b>carmetrics.json</b> (type Text). Add dependent items with JSONPath preprocessing, e.g. <code>$.coolant_temp[0]</code> (avg), <code>[1]</code> min, <code>[2]</code> max.`
      : `In Zabbix: host <code>${host}</code> with trapper items (Numeric float) named <code>&lt;sensor id&gt;.avg</code>, <code>.min</code>, <code>.max</code> – e.g. <code>coolant_temp.avg</code>.`;
    estimate();
  }

  function estimate() {
    const v = values(), rec = 10 + nSensors * 12, per = v.intervalMs / 1000;
    const recs = (v.bufferKB || 0) * 1024 / rec;
    const wire = v.protocol === 'influx' ? 60 + nSensors * 45 : v.zbxPacked ? 110 + nSensors * 40 : nSensors * 3 * 115;
    const perHour = 3600 / per * wire;
    $('#cEstimate').textContent = `Offline buffer holds about ${dur(recs * per)} of driving at this rate. ` +
      `Upload size ≈ ${kb(perHour)} per hour of driving (${nSensors} sensors).`;
  }

  async function loadStatus() {
    const s = await api('/api/cloud');
    const pill = $('#cStatePill');
    const state = !s.enabled ? ['off', ''] : !s.sta ? ['offline – buffering', 'warn'] : !s.timeSynced ? ['waiting for clock', ''] :
      s.lastError && s.lastOkAgeS < 0 ? ['error', 'crit'] : ['online', 'ok'];
    pill.textContent = state[0]; pill.className = 'pill ' + state[1];
    const rows = [
      ['Recording', s.recording ? 'yes' : !s.enabled ? 'no: upload is off' : 'no: engine not running (untick "Only record while the engine runs" to record on the bench)'],
      ['Waiting to send', `${s.pendingRecords} points · ${kb(s.backlogBytes)} on flash (${s.backlogFiles} files)`],
      ['Sent / dropped', `${s.sentRecords} / ${s.droppedRecords}`],
      ['Last upload', s.lastOkAgeS < 0 ? 'never' : `${dur(s.lastOkAgeS)} ago`],
      ['Clock', s.ntp ? 'internet time' : s.timeSynced ? 'from browser' : 'not set'],
    ];
    if (s.lastError) rows.push(['Last error', s.lastError]);
    const t = $('#cStatus');
    t.innerHTML = '';
    rows.forEach(([k, v]) => t.append(h('tr', {}, h('th', { style: 'width:35%' }, k), h('td', {}, v))));
    $('#otaWarn').textContent = s.backlogBytes > 0 ? `Note: ${kb(s.backlogBytes)} of unsent metrics are on flash – a web app update erases them. Upload them first.` : '';
  }

  async function loadAltitude() {
    const i = await api('/api/info');
    $('#altInfo').textContent = `Sea-level reference ${fmt(i.seaLevelHpa, 1)} hPa (${i.seaLevelSource})` +
      (i.stationHpa != null ? ` · sensor reads ${fmt(i.stationHpa, 1)} hPa` : ' · no BMP pressure reading');
  }

  async function load() {
    const cfg = await api('/api/config');
    nSensors = cfg.sensors.filter(s => s.enabled).length;
    const c = cfg.cloud;
    for (const el of form().elements) {
      if (!el.name || !(el.name in c)) continue;
      if (el.type === 'checkbox') el.checked = !!c[el.name];
      else el.value = c[el.name] ?? '';
    }
    form().elements.zbxPsk.placeholder = c.zbxPskSet ? '•••••• (set)' : '';
    form().elements.influxAuth.placeholder = c.influxAuthSet ? '•••••• (set)' : 'Token xyz…';
    showProto();
  }

  document.addEventListener('DOMContentLoaded', () => {
    load().catch(e => toast(e.message, true));
    loadStatus().catch(() => {});
    loadAltitude().catch(() => {});
    setInterval(() => loadStatus().catch(() => {}), 5000);
    form().addEventListener('change', showProto);
    form().addEventListener('input', estimate);
    $('#cSave').onclick = async () => {
      try { toast((await api('/api/config/cloud', { body: values() })).message); setTimeout(() => { load(); loadStatus(); }, 1500); }
      catch (e) { toast(e.message, true, 7000); }
    };
    $('#cFlush').onclick = async () => { try { toast((await api('/api/cloud/flush', { method: 'POST' })).message); setTimeout(loadStatus, 4000); } catch (e) { toast(e.message, true); } };
    $('#cClear').onclick = async () => {
      if (!confirm('Delete all unsent metrics?')) return;
      try { toast((await api('/api/cloud/clear', { method: 'POST' })).message); setTimeout(loadStatus, 1500); } catch (e) { toast(e.message, true); }
    };
    $('#altCal').onclick = async () => {
      const v = $('#altNow').value;
      if (v === '') { toast('Enter your current altitude', true); return; }
      try { toast((await api('/api/altitude/calibrate', { body: { altitudeM: +v } })).message, false, 6000); setTimeout(loadAltitude, 1500); }
      catch (e) { toast(e.message, true); }
    };
  });
})();
