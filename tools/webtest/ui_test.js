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
  await p.screenshot({ path: `${OUT}/system.png`, fullPage: true });

  check(errors.length === 0, 'no browser errors', errors.join(' | '));
  await browser.close();
  console.log(failed ? `FAILED: ${failed}` : 'ALL PASSED');
  process.exit(failed ? 1 : 0);
})();
