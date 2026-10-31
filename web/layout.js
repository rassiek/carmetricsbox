'use strict';
// Layout editor: dashboard tiles and OLED page order (one layout model in the config).
(() => {
  const { $, h, api, toast } = CM;
  let sensors = [], tiles = [], oled = [], saved = '';

  const name = id => (sensors.find(s => s.id === id) || { name: id }).name;
  const dirty = () => $('#dirty').classList.toggle('hidden', JSON.stringify({ tiles, oled }) === saved);
  function move(list, i, d) { const [x] = list.splice(i, 1); list.splice(i + d, 0, x); render(); }
  function sel(value, opts, onchange) {
    const s = h('select', {}, opts.map(([v, t]) => h('option', { value: v, selected: String(v) === String(value) }, t)));
    s.onchange = () => { onchange(s.value); render(); };
    return s;
  }
  function arrows(list, i) {
    return [h('button', { class: 'small', disabled: i === 0, onclick: () => move(list, i, -1) }, '↑'),
            h('button', { class: 'small', disabled: i === list.length - 1, onclick: () => move(list, i, 1) }, '↓'),
            h('button', { class: 'small danger', onclick: () => { list.splice(i, 1); render(); } }, '✕')];
  }

  function render() {
    const tl = $('#tList');
    tl.replaceChildren(...(tiles.length ? tiles.map((t, i) => h('div', { class: 'lay-row' },
      h('span', { class: 'name' }, name(t.id)),
      sel(t.size, [[1, 'Normal'], [2, 'Wide']], v => { t.size = +v; }),
      sel(t.widget, [['bar', 'Value + bar'], ['number', 'Value only'], ['spark', 'Value + trend line']], v => { t.widget = v; }),
      ...arrows(tiles, i))) : [h('p', { class: 'muted' }, 'Automatic: every enabled sensor.')]));
    const used = new Set(tiles.map(t => t.id));
    $('#tAdd').replaceChildren(...sensors.filter(s => s.enabled && !used.has(s.id)).map(s => h('option', { value: s.id }, s.name)));

    const ol = $('#oList');
    ol.replaceChildren(...(oled.length ? oled.map((id, i) => h('div', { class: 'lay-row' + (id === '|' ? ' brk' : '') },
      h('span', { class: 'name' }, id === '|' ? '— new page —' : name(id)), ...arrows(oled, i)))
      : [h('p', { class: 'muted' }, 'Automatic: sensors with an OLED style, in Sensors-page order.')]));
    const usedO = new Set(oled);
    $('#oAdd').replaceChildren(...sensors.filter(s => s.enabled && s.oledStyle !== 'off' && !usedO.has(s.id)).map(s => h('option', { value: s.id }, s.name)));
    dirty();
  }

  async function load() {
    const cfg = await api('/api/config');
    sensors = cfg.sensors;
    tiles = (cfg.layout && cfg.layout.tiles) || [];
    oled = (cfg.layout && cfg.layout.oled) || [];
    saved = JSON.stringify({ tiles, oled });
    render();
  }

  document.addEventListener('DOMContentLoaded', () => {
    $('#tAuto').onclick = () => { tiles = []; render(); };
    $('#oAuto').onclick = () => { oled = []; render(); };
    $('#tAddBtn').onclick = () => {
      const id = $('#tAdd').value;
      if (!id) return;
      if (!tiles.length) tiles = sensors.filter(s => s.enabled).map(s => ({ id: s.id, size: 1, widget: 'bar' })).filter(t => t.id !== id);
      tiles.push({ id, size: 1, widget: 'bar' });
      render();
    };
    $('#oAddBtn').onclick = () => { const id = $('#oAdd').value; if (id) { oled.push(id); render(); } };
    $('#oBreak').onclick = () => {
      if (!oled.length) oled = sensors.filter(s => s.enabled && s.oledStyle !== 'off').map(s => s.id);
      oled.push('|'); render();
    };
    $('#save').onclick = async () => {
      try { toast((await api('/api/config/layout', { body: { tiles, oled } })).message); saved = JSON.stringify({ tiles, oled }); dirty(); }
      catch (e) { toast(e.message, true, 6000); }
    };
    load().catch(e => toast(e.message, true));
  });
})();
