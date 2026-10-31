// Headless UI test of the CarMetrics web app against tools/webtest/mock_server.py.
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
  p.on('console', m => { if (m.type() === 'error') errors.push(`${p.url()} ${m.text()}`); });
  const toast = async () => (await p.locator('#toast').textContent().catch(() => '')) || '';

  // Dashboard tiles must fit one screen in every orientation
  for (const [name, w, h] of [['phone-portrait', 390, 844], ['phone-landscape', 844, 390], ['desktop', 1366, 768]]) {
    await p.setViewportSize({ width: w, height: h });
    await p.goto(BASE + 'index.html'); await p.waitForTimeout(2500);
    const [sh, ih] = await p.evaluate(() => [document.documentElement.scrollHeight, innerHeight]);
    check(sh <= ih + 1, `tiles fit ${name}`, `${sh}/${ih}`);
    check((await p.locator('.tile').count()) > 0, `tiles rendered ${name}`);
    await p.screenshot({ path: `${OUT}/tiles-${name}.png` });
  }
  check(['live', 'polling'].includes((await p.locator('#conn').textContent()).trim()), 'live data connection');
  check(await p.locator('#turbo').isVisible(), 'turbo cool-down banner', await p.locator('#turbo').textContent());

  // Layout editor: make EGT a wide trend tile, then check the dashboard
  await p.goto(BASE + 'layout.html'); await p.waitForTimeout(1500);
  await p.selectOption('#tAdd', 'egt'); await p.click('#tAddBtn'); await p.waitForTimeout(200);
  const rows = p.locator('#tList .lay-row');
  const n = await rows.count();
  await rows.nth(n - 1).locator('select').nth(0).selectOption('2');
  await rows.nth(n - 1).locator('select').nth(1).selectOption('spark');
  await p.click('#oBreak'); await p.click('#save'); await p.waitForTimeout(600);
  check((await toast()).includes('Layout saved'), 'layout saved', await toast());
  await p.screenshot({ path: `${OUT}/layout.png`, fullPage: true });
  await p.goto(BASE + 'index.html'); await p.waitForTimeout(3500);
  check((await p.locator('.tile.wide svg.spark polyline').count()) === 1, 'wide sparkline tile on dashboard');
  await p.screenshot({ path: `${OUT}/tiles-layout.png` });

  // Events (alarm black box)
  await p.goto(BASE + 'events.html'); await p.waitForTimeout(1500);
  check((await p.locator('#list .evt').count()) === 2, 'events listed');
  await p.locator('#list .evt').first().click(); await p.waitForTimeout(800);
  check((await p.locator('#dTable svg.mini').count()) >= 8, 'event trends drawn');
  await p.screenshot({ path: `${OUT}/events.png`, fullPage: true });

  // Charts grouped
  await p.setViewportSize({ width: 1366, height: 900 });
  await p.goto(BASE + 'charts.html'); await p.waitForTimeout(2500);
  const groups = await p.locator('.chart .head').allTextContents();
  check(groups.includes('Temperatures (1-Wire)') && groups.includes('Pressures'), 'charts grouped', JSON.stringify(groups));
  await p.screenshot({ path: `${OUT}/charts.png`, fullPage: true });

  // Sensors: add an MCP9600, save
  await p.setViewportSize({ width: 412, height: 915 });
  await p.goto(BASE + 'settings.html'); await p.waitForTimeout(2000);
  const before = await p.locator('#list .list-item').count();
  await p.selectOption('#newType', 'mcp9600'); await p.click('#add'); await p.waitForTimeout(300);
  check((await p.locator('#list .list-item').count()) === before + 1, 'sensor added');
  await p.selectOption('#newType', 'computed'); await p.click('#add'); await p.waitForTimeout(300);
  await p.locator('#editor select').first().selectOption('curve_margin'); await p.waitForTimeout(200);
  check((await p.locator('#editor input[placeholder^="800:10"]').count()) === 1, 'computed sensor editor (curve)');
  await p.selectOption('#newType', 'rpm'); await p.click('#add'); await p.waitForTimeout(300);
  check((await p.locator('#editor button', { hasText: 'Calibrate' }).count()) === 1, 'RPM sensor editor');
  await p.click('#save'); await p.waitForTimeout(700);
  check((await toast()).includes('saved'), 'sensors saved', await toast());
  await p.screenshot({ path: `${OUT}/sensors.png`, fullPage: true });

  // Relays
  await p.goto(BASE + 'relays.html'); await p.waitForTimeout(2000);
  await p.locator('#controls button').first().click(); await p.waitForTimeout(1600);
  check((await p.locator('#controls .pill').first().textContent()).startsWith('ON'), 'relay switched on');

  // Weather
  await p.goto(BASE + 'weather.html'); await p.waitForTimeout(2000);
  check((await p.locator('#wcards .panel').count()) > 0, 'weather cards');
  check((await p.locator('#wTowns input').count()) >= 40, 'town list');
  await p.click('#wSave'); await p.waitForTimeout(600);
  check((await toast()).includes('saved'), 'weather settings saved');
  await p.screenshot({ path: `${OUT}/weather.png`, fullPage: true });

  // System: cloud + altitude
  await p.goto(BASE + 'system.html'); await p.waitForTimeout(2000);
  await p.selectOption('select[name=protocol]', 'influx');
  check(await p.locator('fieldset[data-proto=influx]').isVisible(), 'influx fields shown');
  await p.fill('input[name=influxUrl]', 'http://vm.local:8428/write'); await p.check('input[name=enabled]');
  await p.click('#cSave'); await p.waitForTimeout(700);
  check((await toast()).includes('saved'), 'cloud settings saved', await toast());
  await p.fill('#altNow', '1661'); await p.click('#altCal'); await p.waitForTimeout(600);
  check((await toast()).includes('Sea-level'), 'altitude calibration');
  check((await p.locator('#hKv > div').count()) >= 10, 'board health tiles');
  check((await p.locator('#wLog tr').count()) === 4, 'Wi-Fi join log rows');
  check((await p.locator('#wLog').innerText()).includes('4-way handshake timeout'), 'Wi-Fi join log shows the disconnect reason');
  check((await p.locator('#wState').innerText()).includes('HouseLAN'), 'Wi-Fi state pill');
  check((await p.locator('#info').innerText()).includes('set from browser'), 'board clock set from the browser after a fresh boot');
  check(/\d\d:\d\d/.test(await p.locator('#clock').innerText()), 'board clock shown in the header', await p.locator('#clock').innerText());
  check((await p.locator('#hTasks tr').count()) === 4, 'task stack table');
  check((await p.locator('#bSel option').count()) === 3, 'board profiles listed');
  await p.selectOption('#bSel', 'esp32_wrover'); await p.click('#bSave'); await p.waitForTimeout(700);
  check((await toast()).includes('Board saved'), 'board saved', await toast());
  check((await p.locator('#safeBanner').count()) === 0, 'no safe-mode banner');
  check((await p.locator('#nets .netrow').count()) === 1, 'saved Wi-Fi networks listed');
  await p.click('#netAdd'); const nr = p.locator('#nets .netrow').nth(1);
  await nr.locator('input').nth(0).fill('Home'); await nr.locator('input').nth(1).fill('homepass1');
  await p.click('#sysSave'); await p.waitForTimeout(700);
  check((await toast()).includes('saved'), 'Wi-Fi networks saved', await toast());
  await p.click('#turboSave'); await p.waitForTimeout(600);
  check((await toast()).includes('Turbo timer saved'), 'turbo timer saved', await toast());
  check((await p.getAttribute('#conn', 'title')) === 'WebSocket' && (await p.textContent('#conn')) === 'live', 'live data over WebSocket', await p.getAttribute('#conn', 'title'));
  await p.screenshot({ path: `${OUT}/system.png`, fullPage: true });

  check(errors.length === 0, 'no browser errors', errors.join(' | '));
  await browser.close();
  console.log(failed ? `FAILED: ${failed}` : 'ALL PASSED');
  process.exit(failed ? 1 : 0);
})();
