// Headless UI test of the web app on a 5.4 worker board (mock_server.py ... worker):
// only its own sensors and actuators, no Boards tab, charts/events/weather/cloud hidden,
// the main board panel on System -> Status.
// usage: node ui_worker_test.js <outDir> [baseUrl]
const { chromium } = require('playwright');
const OUT = process.argv[2] || 'build/webtest';
const BASE = process.argv[3] || 'http://127.0.0.1:8097/';
require('fs').mkdirSync(OUT, { recursive: true });

let failed = 0;
const check = (ok, what, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'} worker: ${what} ${detail}`); if (!ok) failed++; };

(async () => {
  const browser = await chromium.launch();
  const errors = [];
  const ctx = await browser.newContext({ deviceScaleFactor: 2, httpCredentials: { username: 'admin', password: 'carmetrics' } });
  const p = await ctx.newPage();
  p.on('pageerror', e => errors.push(`${p.url()} ${e.message}`));
  p.on('console', m => { if (m.type() === 'error' && !/Failed to load resource/.test(m.text())) errors.push(`${p.url()} ${m.text()}`); });
  p.on('dialog', d => d.accept());
  const go = async (hash, wait = 1200) => { await p.evaluate(h => { location.hash = h; }, hash); await p.waitForTimeout(wait); };

  await p.setViewportSize({ width: 390, height: 844 });
  await p.goto(BASE); await p.waitForTimeout(2500);
  check(!(await p.locator('#tab-boards').isVisible()), 'no Boards tab');
  check((await p.locator('.seg').count()) === 0, 'gauges only (no charts or events without history)');
  check((await p.locator('.gtools select').count()) === 0, 'no board filter (one board)');
  const names = await p.$$eval('.tiles .tile .tname', e => e.map(x => x.textContent));
  check(names.length >= 2 && !names.some(n => /Coolant|Gearbox/.test(n)), 'only its own gauges', names.join(' | '));
  check((await p.locator('.thermo.none .tv').allTextContents()).every(t => t === '--'), 'missing sensor shows "--"');
  await p.screenshot({ path: `${OUT}/worker-dashboard.png`, fullPage: true });
  await go('#boards');
  check(/^#?(dashboard)?$/.test(await p.evaluate(() => location.hash.replace('#', ''))) || await p.locator('.tiles').count() > 0, '#boards falls back to the dashboard');
  await go('#system/status', 1500);
  const mainPanel = p.locator('.panel', { has: p.locator('h3', { hasText: /^Main board$/ }) });
  check(await mainPanel.isVisible() && /b3/.test(await mainPanel.textContent()), 'System -> Status: main board panel (paired as b3)');
  check(await mainPanel.locator('button', { hasText: 'Forget the main board' }).isVisible(), 'forget the main board button');
  await p.screenshot({ path: `${OUT}/worker-system-status.png`, fullPage: true });
  await go('#system/data', 1500);
  const titles = await p.$$eval('.panel h3', e => e.map(x => x.textContent));
  check(!titles.some(t => /Weather|Metrics upload|Alarm recordings/.test(t)), 'no weather, cloud or alarm recordings', titles.join(' | '));
  check(await p.locator('text=Start from a preset').count() === 0, 'no presets');
  await go('#system/hardware', 1500);
  const hw = await p.$$eval('.panel h3', e => e.map(x => x.textContent));
  check(!hw.includes('OLED') && !hw.includes('Altitude'), 'no OLED or altitude panels', hw.join(' | '));
  const cb = p.locator('.panel', { has: p.locator('h3', { hasText: /^Custom board$/ }) });
  check(await cb.isVisible(), 'Hardware: custom board form');
  await cb.locator('summary').click();
  check(/ESP32 DevKit/.test(await cb.textContent()) && (await cb.locator('input[name="valid"]').inputValue()).includes('32-39'), 'form filled in from the current board', await cb.locator('input[name="valid"]').inputValue());
  await cb.locator('input[name="name"]').fill('Pump board v2'); await cb.locator('input[name="name"]').dispatchEvent('change');
  await cb.locator('input[name="valid"]').fill('2, 4, 6, 13-19'); await cb.locator('input[name="valid"]').dispatchEvent('change');
  await cb.locator('button', { hasText: 'Save definition' }).click(); await p.waitForTimeout(600);
  check(/flash/.test(await p.locator('#toast').textContent()), 'flash pin refused with the reason', await p.locator('#toast').textContent());
  await cb.locator('input[name="valid"]').fill('2, 4, 13-19, 21-23, 25-27, 32-39'); await cb.locator('input[name="valid"]').dispatchEvent('change');
  await cb.locator('input[name="relays"]').fill('16, 17'); await cb.locator('input[name="relays"]').dispatchEvent('change');
  await cb.locator('button', { hasText: 'Save definition' }).click(); await p.waitForTimeout(1500);
  const saved = await p.evaluate(() => CM.get('/hardware/custom-board'));
  check(saved.name === 'Pump board v2' && saved.fixed.relays.join() === '16,17' && saved.valid.includes(35), 'custom board saved', JSON.stringify(saved.fixed));
  check(await p.locator('select[name="board"] option[value="custom"]').count() === 1, 'custom board offered as the board type');
  await p.screenshot({ path: `${OUT}/worker-hardware.png`, fullPage: true });
  await go('#actuators', 1500);
  check(await p.locator('text=Spot lights').count() > 0, 'its own actuators listed');
  check(errors.length === 0, 'no script errors', errors.join('\n'));
  await browser.close();
  console.log(failed ? `WORKER UI TEST: ${failed} failed` : 'WORKER UI TEST PASSED');
  process.exit(failed ? 1 : 0);
})();
