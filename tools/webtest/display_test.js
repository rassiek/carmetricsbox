// Headless test of the touch display's own web page (layout editor, settings, firmware)
// against tools/webtest/display_mock.py. usage: node display_test.js <outDir> <baseUrl>
const { chromium } = require('playwright');
const fs = require('fs');
const OUT = process.argv[2] || 'build/webtest', BASE = process.argv[3] || 'http://127.0.0.1:8098/';
fs.mkdirSync(OUT, { recursive: true });
let failed = 0;
const check = (ok, what, detail = '') => { console.log(`${ok ? 'PASS' : 'FAIL'} display: ${what} ${detail}`); if (!ok) failed++; };
(async () => {
  const browser = await chromium.launch();
  const ctx = await browser.newContext({ deviceScaleFactor: 2, httpCredentials: { username: 'admin', password: 'carmetrics' } });
  const p = await ctx.newPage();
  const errors = [];
  p.on('pageerror', e => errors.push(e.message));
  p.on('console', m => { if (m.type() === 'error' && !/status of 400/.test(m.text())) errors.push(m.text()); });
  const toast = async () => (await p.locator('#toast').textContent()) || '';
  await p.setViewportSize({ width: 1100, height: 900 });
  await p.goto(BASE); await p.waitForTimeout(1500);
  check((await p.locator('#link').textContent()) === 'ESP-NOW', 'link shown');
  check((await p.locator('#tiles tr').count()) === 5, 'automatic layout loaded: 4 tiles');
  const px = await p.evaluate(() => { const c = document.querySelector('#cv'); const d = c.getContext('2d').getImageData(0, 0, c.width, c.height).data; let n = 0; for (let i = 0; i < d.length; i += 4) if (d[i] + d[i + 1] + d[i + 2] > 300) n++; return n; });
  check(px > 2000, 'preview drawn with values', `${px} bright pixels`);
  // edit: add a relay tile, make the first tile wide, rename page, add a page
  await p.click('#tileAdd');
  const rows = p.locator('#tiles tr');
  await rows.nth(5).locator('select').first().selectOption('r:0:0');
  check(await rows.nth(5).locator('select').nth(1).isDisabled(), 'relay tile: widget fixed to relay');
  await rows.nth(1).locator('input[type=number]').first().fill('2');
  await p.fill('#pgName', 'Engine');
  await p.click('#pgAdd');
  check((await p.locator('#pageTabs button').count()) === 4, 'page added (2 pages + Relays + Setup tabs)');
  await p.click('#save'); await p.waitForTimeout(600);
  check((await toast()).includes('Layout saved'), 'layout saved', await toast());
  await p.locator('#pageTabs button').first().click(); await p.waitForTimeout(200);
  check((await p.locator('#pgName').inputValue()) === 'Engine' && (await p.locator('#tiles tr').count()) === 6, 'saved layout read back');
  await p.screenshot({ path: `${OUT}/display-layout.png`, fullPage: true });
  await p.fill('#pgCols', '9'); await p.waitForTimeout(100);
  check((await p.locator('#pgCols').inputValue()) === '9', 'grid field accepts typing');
  await p.click('#save'); await p.waitForTimeout(500);
  check(!(await toast()).includes('1-4 columns'), 'grid clamped before saving', await toast());
  // settings
  await p.click('nav a[data-t=settings]'); await p.waitForTimeout(700);
  check((await p.locator('#sName').inputValue()) === 'Dash', 'settings loaded');
  await p.fill('#sName', 'Dash 2'); await p.click('#sSave'); await p.waitForTimeout(600);
  check((await toast()).includes('Saved'), 'settings saved', await toast());
  await p.waitForTimeout(500);
  check((await p.locator('#rawTouch').textContent()).includes('1234'), 'raw touch shown for calibration');
  // status
  await p.click('nav a[data-t=status]'); await p.waitForTimeout(400);
  check((await p.locator('#kv').innerText()).includes('board 2'), 'status: paired board id');
  await p.screenshot({ path: `${OUT}/display-status.png`, fullPage: true });
  // firmware: wrong file refused
  await p.click('nav a[data-t=firmware]');
  await p.setInputFiles('#fwFile', { name: 'notes.bin', mimeType: 'application/octet-stream', buffer: Buffer.from('hello') });
  await p.click('#fwSend'); await p.waitForTimeout(600);
  check((await toast()).includes('Not a firmware file'), 'non-firmware file refused', await toast());
  await p.setInputFiles('#fwFile', { name: 'carmetrics-display-jc3248w535-firmware.bin', mimeType: 'application/octet-stream', buffer: Buffer.from([0xE9, 1, 2, 3, 4]) });
  await p.click('#fwSend'); await p.waitForTimeout(600);
  check((await toast()).includes('restarting'), 'firmware uploaded as the raw body', await toast());
  check(errors.length === 0, 'no browser errors', errors.join(' | '));
  await browser.close();
  console.log(failed ? `${failed} FAILED` : 'ALL PASSED');
  process.exit(failed ? 1 : 0);
})();
