'use strict';
// Real Chromium against the installed service. No route mocks or vendor SDK.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {chromium} = require('playwright');
const base = process.argv[2];
const output = path.resolve(process.argv[3] || 'service-evidence');
assert.match(base || '', /^http:\/\/(127\.0\.0\.1|10\.[0-9.]+):[0-9]+$/,
  'Only the disposable local package service may be tested');
fs.mkdirSync(output, {recursive: true});

(async () => {
  const browser = await chromium.launch({headless: true});
  const page = await browser.newPage({viewport: {width: 1440, height: 1000}});
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  try {
    await page.goto(`${base}/display/configuration/`, {waitUntil: 'domcontentloaded'});
    await page.waitForSelector('#config-fields input', {state: 'attached'});
    assert.match(await page.title(), /VectorWarp.*Settings/);
    const tabs = page.getByRole('tab');
    assert.equal(await tabs.count(), 6);
    for (let i = 0; i < 6; i++) {
      await tabs.nth(i).click();
      assert.equal(await tabs.nth(i).getAttribute('aria-selected'), 'true');
      assert.equal(await page.locator('[role=tabpanel]:visible').count(), 1);
    }
    const cpi = page.locator('[data-path="process.data.cpi"] input');
    const panel = await cpi.evaluate(el => el.closest('[role=tabpanel]').id);
    const revealCpi = async () => {
      await page.locator(`[aria-controls="${panel}"]`).click();
      for (const group of await cpi.locator('xpath=ancestor::details').all()) {
        if (await group.getAttribute('open') === null) await group.locator(':scope > summary').click();
      }
    };
    await revealCpi();
    const original = await cpi.inputValue();
    const changed = String(Number(original) + .1);
    await cpi.fill(changed);
    const saved = page.waitForResponse(r => r.url().includes('/api/config?') && r.request().method() === 'PUT');
    await page.locator('#config-save-later').click();
    assert.equal((await saved).status(), 200, 'Actual Save for later must persist through service permissions');
    await page.reload({waitUntil: 'domcontentloaded'});
    await page.waitForSelector('#config-fields input', {state: 'attached'});
    assert.equal(await page.locator('[data-path="process.data.cpi"] input').inputValue(), changed);
    await revealCpi();
    await page.locator('[data-path="process.data.cpi"] input').fill(original);
    const restored = page.waitForResponse(r => r.url().includes('/api/config?') && r.request().method() === 'PUT');
    await page.locator('#config-save-later').click();
    assert.equal((await restored).status(), 200);
    await page.getByRole('tab').first().click();
    await page.getByRole('button', {name: 'Check receiver software', exact: true}).click();
    // Both SDK detection and adapter guidance can show this official link.
    await page.locator('#receiver-setup a[href="https://sdrplay.com/hardware-api/"]').first().waitFor();
    await page.screenshot({path: path.join(output, 'installed-settings.png'), fullPage: true});
    assert.deepEqual(errors, [], 'No browser JavaScript errors');
    fs.writeFileSync(path.join(output, 'browser.json'), JSON.stringify({passed: true,
      tabs: 6, savedAndReloaded: true, sdkDownloadLink: true, javascriptErrors: errors,
      hardwareTested: false}, null, 2));
  } finally {
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
