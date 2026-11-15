// Headless UI test of the CarMetrics web app (5.3, API v1) against tools/webtest/mock_server.py.
// usage: node ui_test.js <outDir> [baseUrl]
const { chromium } = require('playwright');
const OUT = process.argv[2] || 'build/webtest';
const BASE = process.argv[3] || 'http://127.0.0.1:8099/';
const fs = require('fs');
fs.mkdirSync(OUT, { recursive: true });

let failed = 0;
const check = (ok, what, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'} ${what} ${detail}`); if (!ok) failed++; };

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const ctx = await browser.newContext({ deviceScaleFactor: 2, httpCredentials: { username: 'admin', password: 'carmetrics' } });
  const p = await ctx.newPage();
  p.on('pageerror', e => errors.push(`${p.url()} ${e.message}`));
  p.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(`${p.url()} ${m.text()}`); });
  p.on('dialog', d => d.accept(d.type() === 'prompt' ? (d.defaultValue() || 'Test') : undefined));
  const toast = async () => (await p.locator('#toast').textContent().catch(() => '')) || '';
  const api = (path, opt) => p.evaluate(([a, o]) => CM.api(a, o), [path, opt]);
  const go = async (hash, wait = 1200) => { await p.evaluate(h => { location.hash = h; }, hash); await p.waitForTimeout(wait); };

  // ---- Dashboard
  await p.setViewportSize({ width: 390, height: 844 });
  await p.goto(BASE); await p.waitForTimeout(2500);
  check((await p.locator('.tile svg.dial').count()) >= 5, 'gauges drawn as dials', String(await p.locator('.tile svg.dial').count()));
  check((await p.locator('#conn').textContent()).trim() === 'live', 'live data over the WebSocket');
  check(await p.locator('#turbo').isVisible(), 'turbo cool-down banner');
  check((await p.locator('#tab-dashboard').textContent()) === 'Dashboard' && (await p.locator('#machine').textContent()) === 'Defender', 'names from the settings');
  const needle = await p.locator('.tile .needle').first().getAttribute('transform');
  check(/rotate\(/.test(needle || ''), 'needle moves with the value', needle);
  const order = await p.$$eval('.tiles .tile .tname', e => e.slice(0, 6).map(x => x.textContent));
  check(order[0] === 'Coolant' && order[1] === 'Oil pressure' && order[2] === 'EGT', 'most important gauges first', order.join(' | '));
  check((await p.locator('.tile.w-bars .thermo').count()) >= 2, 'other temperatures as one thermometer group', String(await p.locator('.tile.w-bars .thermo').count()));
  check((await p.locator('.tile.w-bars .zone').count()) >= 1 && (await p.locator('.tile.w-bars .tkl').count()) >= 1, 'each thermometer shows its own limits', await p.locator('.tile.w-bars .tkl').allTextContents().then(t => t.join(',')));
  const none = await p.$$eval('.tile.w-bars .thermo.none .tv', e => e.map(x => x.textContent));
  const why = await p.locator('.tile.w-bars .why').textContent();
  check(none.length === 1 && none[0] === '--' && why.length <= 30 && /no sensor/.test(why), 'thermometer without a sensor: "--" and one short reason', `${none} | ${why}`);
  const tilesAll = await p.locator('.tiles .tile').count();
  check(await p.locator('.gtools select').isVisible(), 'gauge board filter shown (sensors from more than one board)');
  await p.selectOption('.gtools select', 'local'); await p.waitForTimeout(400);
  const localNames = await p.$$eval('.tiles .thermo .tl', e => e.map(x => x.textContent));
  check((await p.locator('.tiles .tile').count()) <= tilesAll && !localNames.includes('Engine bay Gearbox'), 'filter "This board": other boards\' gauges hidden', localNames.join(','));
  await p.selectOption('.gtools select', 'b1'); await p.waitForTimeout(400);
  check((await p.locator('.tiles .tile').count()) === 1, 'filter one board: only its gauges', String(await p.locator('.tiles .tile').count()));
  await p.selectOption('.gtools select', 'all'); await p.waitForTimeout(400);
  check((await p.locator('.tiles .tile').count()) === tilesAll, 'filter back to all boards');
  await p.screenshot({ path: `${OUT}/dashboard-phone.png`, fullPage: true });
  await p.click('text=Full screen'); await p.waitForTimeout(800);
  const fs = await p.evaluate(() => ({ fs: document.body.classList.contains('fs'), header: getComputedStyle(document.querySelector('header')).display, sh: document.documentElement.scrollHeight, ih: innerHeight }));
  check(fs.fs && fs.header === 'none' && fs.sh <= fs.ih + 1, 'full screen: header hidden, tiles fit the screen', JSON.stringify(fs));
  await p.screenshot({ path: `${OUT}/dashboard-fullscreen-phone.png` });
  await p.click('.fsexit'); await p.waitForTimeout(300);
  await p.setViewportSize({ width: 844, height: 390 }); await p.waitForTimeout(300);
  await p.click('text=Full screen'); await p.waitForTimeout(800);
  const fs2 = await p.evaluate(() => [document.documentElement.scrollHeight, innerHeight]);
  check(fs2[0] <= fs2[1] + 1, 'full screen sideways fits', fs2.join('/'));
  await p.screenshot({ path: `${OUT}/dashboard-fullscreen-landscape.png` });
  await p.click('.fsexit'); await p.waitForTimeout(300);
  check(!(await p.evaluate(() => document.body.classList.contains('fs'))), 'leave full screen');
  await p.setViewportSize({ width: 390, height: 844 });
  await p.locator('.tile').first().click();
  await p.locator('.modal input[name="warnHigh"]').fill('97'); await p.locator('.modal input[name="warnHigh"]').dispatchEvent('change');
  await p.locator('.modal button.primary').click(); await p.waitForTimeout(500);
  check((await api('/sensors/coolant_temp')).warnHigh === 97, 'limits changed from the dial', await toast());
  await go('#dashboard/charts', 1500);
  check((await p.locator('canvas.chart').count()) >= 5, 'charts view');
  const temps = p.locator('.panel', { has: p.locator('b', { hasText: /^Temperatures$/ }) });
  check((await temps.locator('.chip').count()) >= 4, 'temperatures (not EGT) share one chart', await temps.locator('.chips').textContent().catch(() => ''));
  check(await p.locator('.panel b', { hasText: /^Exhaust \/ thermocouples$/ }).isVisible(), 'EGT on its own axis');
  const press = p.locator('.panel', { has: p.locator('b', { hasText: /^Pressures$/ }) });
  check(/Boost/.test(await press.textContent()) && /Oil pressure/.test(await press.textContent()), 'boost and oil pressure share one chart');
  check(await p.locator('.panel b', { hasText: /^Altitude$/ }).isVisible() && await p.locator('.panel b', { hasText: /^Outputs$/ }).isVisible(), 'altitude and actuator outputs plotted');
  check((await p.locator('details summary', { hasText: 'Board health' }).count()) === 1, 'board health charts');
  await p.screenshot({ path: `${OUT}/charts.png`, fullPage: true });
  await go('#dashboard/events');
  await p.locator('text=Show').first().click(); await p.waitForTimeout(500);
  check((await p.locator('canvas.chart').count()) >= 2, 'event recording opens');
  await p.setViewportSize({ width: 1366, height: 800 });
  await go('#dashboard/gauges', 1500);
  await p.screenshot({ path: `${OUT}/dashboard-desktop.png` });

  // ---- Layout editor (tiles + OLED pages)
  await go('#dashboard/layout', 1500);
  check((await p.locator('#oPreview canvas').count()) >= 1, 'OLED preview drawn');
  await p.selectOption('#tAdd', 'egt'); await p.click('#tAddBtn');
  const rows = p.locator('#tList .lay-row'); const n = await rows.count();
  await rows.nth(n - 1).locator('select').nth(1).selectOption('spark');
  await p.click('#save'); await p.waitForTimeout(600);
  const lay = await api('/settings/layout');
  check(lay.tiles.some(t => t.id === 'egt' && t.widget === 'spark'), 'layout saved', JSON.stringify(lay.tiles.slice(-1)));
  await p.screenshot({ path: `${OUT}/layout.png`, fullPage: true });

  // ---- Sensors
  await go('#sensors', 1500);
  check((await p.locator('.list-item').count()) >= 6, 'sensor list');
  await p.click('text=Add sensor'); await p.waitForTimeout(200);
  await p.locator('.edit input[name="name"]').fill('Fuel pressure'); await p.locator('.edit input[name="name"]').dispatchEvent('change');
  await p.locator('.edit select[name="type"]').selectOption('ads1115'); await p.waitForTimeout(200);
  await p.click('text=0.5–4.5 V sender, 0–10 bar');
  check(await p.locator('.edit input[name="inMax"]').inputValue() === '4.5', 'analog preset fills the conversion');
  await p.locator('.edit button.primary').click(); await p.waitForTimeout(800);
  const fp = await api('/sensors/fuel_pressure').catch(e => ({ error: e.message }));
  check(fp.type === 'ads1115' && fp.outMax === 10 && fp.unit === 'bar', 'sensor added', JSON.stringify(fp).slice(0, 120) + ' ' + await toast());
  await p.click('text=Discover'); await p.waitForTimeout(1500);
  check(await p.locator('text=28FF641E8016034A').isVisible(), 'discovery lists 1-Wire sensors');
  check(await p.locator('text=ads1115@0x48/0').isVisible(), 'discovery shows analog volts');
  await p.screenshot({ path: `${OUT}/sensors.png`, fullPage: true });
  await p.selectOption('main select >> nth=0', 'b1'); await p.waitForTimeout(1200);
  check(await p.locator('text=Gearbox').first().isVisible(), 'another board\'s sensors');
  await go('#actuators', 1200);
  await p.selectOption('main select >> nth=0', 'b1'); await p.waitForTimeout(1500);
  check(await p.locator('.list-item', { hasText: 'Pump' }).locator('button', { hasText: 'Off' }).isVisible(), 'another board\'s switch shows its state');

  // ---- Actuators
  await go('#actuators', 1500);
  await p.selectOption('main select >> nth=0', 'local'); await p.waitForTimeout(1500);
  check((await p.locator('.list-item').count()) === 3, 'actuator list');
  const bh = await p.locator('.list-item', { hasText: 'Spot lights' }).locator('button.big').first().boundingBox();
  check(bh && bh.height >= 44, 'switch buttons are touch-sized', bh && String(bh.height));
  await p.locator('.list-item', { hasText: 'Horn' }).locator('button', { hasText: 'Disabled: enable' }).click(); await p.waitForTimeout(600);
  check((await api('/actuators/horn')).enabled === true && await p.locator('.list-item', { hasText: 'Horn' }).locator('button', { hasText: 'Off' }).isVisible(), 'disabled actuator can be enabled from the list');
  await p.locator('.list-item', { hasText: 'Spot lights' }).locator('button', { hasText: 'Off' }).click(); await p.waitForTimeout(400);
  check((await api('/actuators/lights')).id === 'lights' && (await api('/actuators')).items.find(a => a.id === 'lights').state.on === true, 'relay switched from the list');
  await p.click('text=Add actuator'); await p.waitForTimeout(200);
  await p.locator('.edit select[name="type"]').selectOption('pwm'); await p.waitForTimeout(100);
  await p.locator('.edit button.primary').click(); await p.waitForTimeout(500);
  check(/choose its pin/.test(await toast()), 'actuator without a pin refused', await toast());
  await p.locator('.edit select[name="pin"]').selectOption('14');
  await p.locator('.edit select[name="rule"]').selectOption('curve'); await p.waitForTimeout(100);
  check(await p.locator('.edit input[name="curve"]').isVisible(), 'curve rule shows its fields');
  await p.screenshot({ path: `${OUT}/actuators.png`, fullPage: true });

  // ---- Boards
  await go('#boards', 1500);
  check(await p.locator('text=Engine bay').first().isVisible(), 'paired board card');
  check(await p.locator('text=Relay X8').isVisible(), 'unpaired board found');
  await p.locator('.list-item', { hasText: 'Relay X8' }).locator('button').click(); await p.waitForTimeout(800);
  check((await api('/boards')).items.length === 3, 'board paired');
  await p.screenshot({ path: `${OUT}/boards.png`, fullPage: true });

  // ---- System
  for (const s of ['status', 'network', 'hardware', 'data']) {
    await go('#system/' + s, 1500);
    check((await p.locator('main .panel').count()) >= 2, `system: ${s}`);
    await p.screenshot({ path: `${OUT}/system-${s}.png`, fullPage: true });
  }
  await go('#system/status', 1200);
  await p.click('text=Identify this board'); await p.waitForTimeout(400);
  check(/Flashing/.test(await p.locator('#toast').textContent()), 'identify this board');
  await go('#system/network', 1200);
  check(await p.locator('text=Phone hotspot').isVisible(), 'Wi-Fi scan and join on one panel');
  await go('#system/data', 1500);
  {   // weather towns: add two from the list, save, still there after a reload
    const wp = () => p.locator('.panel', { has: p.locator('h3', { hasText: /^Weather$/ }) });
    const addSel = () => wp().locator('select').last();
    const firstOpts = await addSel().evaluate(s => [...s.options].slice(1, 3).map(o => o.value));
    check(firstOpts.length === 2 && firstOpts.every(Boolean), 'town list has names', firstOpts.join(','));
    await addSel().selectOption('Mombasa'); await p.waitForTimeout(200);
    await addSel().selectOption('Kisumu'); await p.waitForTimeout(200);
    const chips = await wp().locator('.chip').allTextContents();
    check(chips.some(c => c.startsWith('Mombasa')) && chips.some(c => c.startsWith('Kisumu')), 'towns added from the list', chips.join(' | '));
    await wp().locator('button.primary').click(); await p.waitForTimeout(800);
    const names = (await api('/settings/weather')).towns.map(t => t.name);
    check(names.includes('Mombasa') && names.includes('Kisumu'), 'towns saved', names.join(','));
    await go('#system/status', 800); await go('#system/data', 1500);
    check((await wp().locator('.chip').allTextContents()).some(c => c.startsWith('Kisumu')), 'towns still there after leaving the page');
  }
  {   // firmware upload shows its progress
    const fw = Buffer.alloc(600 * 1024, 0xff); fw[0] = 0xE9;
    await p.locator('.panel', { hasText: 'Firmware update' }).locator('input[type=file]').setInputFiles({ name: 'carmetrics-main-s3-firmware.bin', mimeType: 'application/octet-stream', buffer: fw });
    await p.waitForTimeout(2000);
    const up = p.locator('.panel', { hasText: 'Firmware update' }).locator('.upload');
    check(await up.isVisible() && /installed/.test(await up.textContent()) && (await up.locator('.progress > div').getAttribute('style')).includes('100%'),
          'firmware upload: progress bar, then the board\'s answer', await up.textContent());
  }
  await p.locator('input[name="appName"]').fill('BoatMetrics'); await p.locator('input[name="appName"]').dispatchEvent('change');
  await p.locator('.panel', { hasText: 'Names and units' }).locator('button.primary').first().click(); await p.waitForTimeout(2500);
  check((await p.locator('#brand').textContent()) === 'BoatMetrics', 'app name changed everywhere', await p.locator('#brand').textContent());
  await p.selectOption('select[name="preset"]', 'boat');
  await p.click('text=Apply'); await p.waitForTimeout(2500);
  check((await p.locator('#tab-dashboard').textContent()) === 'Helm', 'boat preset renames the tabs', await p.locator('#tab-dashboard').textContent());

  check(errors.length === 0, 'no JavaScript errors', errors.slice(0, 5).join(' | '));
  await browser.close();
  console.log(failed ? `UI TEST FAILED (${failed})` : 'UI TEST PASSED');
  process.exit(failed ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
