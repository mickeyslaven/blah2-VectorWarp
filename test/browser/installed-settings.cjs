'use strict';
// Real Chromium against the installed service. No route mocks or vendor SDK.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {receiverSaveMessage} = require('../../html/js/config_ui');
const replayFile = '/var/lib/vectorwarp/package-test-replay.blah2iq';

function assertReplayIdentity(status, before, accepted) {
  assert.notEqual(status.serverId, before.serverId, 'The installed API must actually restart');
  assert.equal(status.loadedRevision, accepted.revision);
  assert.equal(status.configRevision, accepted.revision);
  assert.equal(status.restart.state, 'command-complete');
  assert.equal(status.processorFresh, true);
  assert.equal(status.processor?.input, 'replay');
  assert.equal(status.processor?.file, replayFile);
}

function assertReceivingReplay(status, appliedAt) {
  // A short non-looping package fixture may reach complete before the first
  // status poll. It is valid only when a fresh frame proves replay ran.
  assert.ok(['playing', 'draining', 'complete'].includes(status.processor?.state), status.processor?.state);
  if (status.processor?.state !== 'complete') assert.equal(status.radar, 'receiving');
  assert.ok(status.lastFrameAt > appliedAt, 'A new frame must follow the browser Apply');
}

async function waitForLoopingReplay(initial, before, accepted, appliedAt, readStatus,
                                    wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds)),
                                    now = Date.now) {
  let status = initial;
  let firstFrameAt = null;
  const deadline = now() + 30000;
  for (;;) {
    assertReplayIdentity(status, before, accepted);
    if (['playing', 'draining'].includes(status.processor?.state) &&
        status.radar === 'receiving' && status.lastFrameAt > appliedAt) {
      assertReceivingReplay(status, appliedAt);
      if (firstFrameAt === null) firstFrameAt = status.lastFrameAt;
      if (status.processor?.state === 'playing' && status.lastFrameAt > firstFrameAt) return status;
    }
    if (now() >= deadline) {
      assertReceivingReplay(status, appliedAt);
      assert.ok(firstFrameAt !== null && status.lastFrameAt > firstFrameAt,
        'Looping replay must produce a second distinct fresh frame within 30 seconds');
      assert.equal(status.processor?.state, 'playing', 'Looping replay must resume playing after draining');
    }
    await wait(200);
    status = await readStatus();
  }
}

async function waitForReplayFrame(initial, before, accepted, appliedAt, readStatus,
                                  wait = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds)),
                                  now = Date.now) {
  let status = initial;
  const deadline = now() + 30000;
  for (;;) {
    assertReplayIdentity(status, before, accepted);
    if (['playing', 'draining', 'complete'].includes(status.processor?.state) &&
        (status.processor?.state === 'complete' || status.radar === 'receiving') &&
        status.lastFrameAt > appliedAt) {
      assertReceivingReplay(status, appliedAt);
      return status;
    }
    if (now() >= deadline) {
      assertReceivingReplay(status, appliedAt);
      throw new Error('Replay produced no fresh frame within 30 seconds');
    }
    await wait(200);
    status = await readStatus();
  }
}

async function runStatusFixtures() {
  const before = {serverId: 'old'};
  const accepted = {revision: 7};
  const appliedAt = 1000;
  const status = (state, frame, overrides = {}) => ({serverId: 'new', loadedRevision: 7,
    configRevision: 7, restart: {state: 'command-complete'}, processorFresh: true,
    processor: {input: 'replay', file: replayFile, state}, radar: 'receiving', lastFrameAt: frame, ...overrides});
  const sequence = [status('draining', 1001), status('playing', 1002)];
  let clock = appliedAt;
  const ready = await waitForLoopingReplay(status('loading', 1000), before, accepted, appliedAt,
    async () => sequence.shift(), async milliseconds => { clock += milliseconds; }, () => clock);
  assert.equal(ready.processor.state, 'playing');
  const completed = await waitForReplayFrame(status('complete', 1001), before, accepted, appliedAt,
    async () => status('complete', 1001));
  assert.equal(completed.processor.state, 'complete');
  const rejects = async (initial, message) => {
    let time = appliedAt;
    await assert.rejects(waitForLoopingReplay(initial, before, accepted, appliedAt,
      async () => initial, async milliseconds => { time += milliseconds; }, () => time), message);
  };
  await rejects(status('loading', 1000), /loading/);
  await rejects(status('playing', 1000), /A new frame/);
  await rejects(status('playing', 1001, {processor: {input: 'hardware', file: replayFile, state: 'playing'}}), /replay/);
  process.stdout.write('installed replay status fixtures passed\n');
}

if (process.argv[2] === '--status-fixtures') {
  runStatusFixtures().catch(error => { console.error(error); process.exitCode = 1; });
} else {
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
    // The packaged page must expose frequency-dependent RSPduo choices even
    // before a vendor SDK is installed. No receiver is opened or started here.
    const lna = page.locator('[data-path="capture.device.lnaState"] select');
    await page.locator('[aria-controls="settings-panel-capture"]').click();
    for (const group of await lna.locator('xpath=ancestor::details').all()) {
      if (await group.getAttribute('open') === null) await group.locator(':scope > summary').click();
    }
    await page.getByRole('spinbutton', {name: 'Center frequency MHz', exact: true}).fill('30');
    await lna.selectOption('0');
    assert.deepEqual(await lna.locator('option').evaluateAll(options => options.map(option => option.value)),
      ['0','1','2','3','4','5','6']);
    await page.getByRole('spinbutton', {name: 'Center frequency MHz', exact: true}).fill('1500');
    assert.deepEqual(await lna.locator('option').evaluateAll(options => options.map(option => option.value)),
      ['0','1','2','3','4','5','6','7','8']);
    // Discard the unsaved form edits rather than triggering its navigation guard.
    await page.getByRole('button', {name: 'Discard changes', exact: true}).click();
    await page.reload({waitUntil: 'domcontentloaded'});
    await page.waitForSelector('#config-fields input', {state: 'attached'});
    await page.locator('#receiver-startup').waitFor({state: 'attached'});
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
    await page.getByRole('button', {name: 'Scan for receivers', exact: true}).click();
    // Both SDK detection and adapter guidance can show this official link.
    await page.locator('#receiver-setup a[href="https://sdrplay.com/hardware-api/"]').first().waitFor();
    // Real browser Apply -> installed API -> Unix broker -> systemd restart ->
    // non-root processor -> API frame/status telemetry. The IQ is generated by
    // the package test; this deliberately does not open receiver hardware.
    const beforeResponse = await page.request.get(`${base}/api/system/status`);
    assert.equal(beforeResponse.status(), 200);
    const before = await beforeResponse.json();
    await page.goto(`${base}/display/configuration?replayFile=${encodeURIComponent(replayFile)}`,
      {waitUntil: 'domcontentloaded'});
    await page.waitForSelector('#config-save', {state: 'attached'});
    await page.locator('#config-save').filter({hasText: 'Start replay'}).waitFor();
    assert.equal(await page.locator('#config-save').textContent(), 'Start replay');
    await revealCpi();
    await cpi.fill('0.02');
    const applyResponse = page.waitForResponse(r => r.url().includes('/api/config?restart=true') &&
      r.request().method() === 'PUT');
    const appliedAt = Number(before.lastFrameAt) || 0;
    await page.locator('#config-save').click();
    const applied = await applyResponse;
    assert.equal(applied.status(), 200, `Browser Apply failed: ${await applied.text()}`);
    const accepted = await applied.json();
    assert.equal(accepted.restarting, true);
    assert.equal(accepted.receiverSync?.acceptance?.mode, 'replay');
    assert.equal(accepted.receiverSync?.acceptance?.physicalReceiverVerified, false);
    // Use the reviewed copy from this source revision, then independently
    // verify the installed services restarted and produced a new replay frame.
    await page.locator('#config-message.success').filter({hasText: receiverSaveMessage(accepted, true)})
      .waitFor({timeout: 145000});
    const afterResponse = await page.request.get(`${base}/api/system/status`);
    assert.equal(afterResponse.status(), 200);
    let after = await afterResponse.json();
    after = await waitForReplayFrame(after, before, accepted, appliedAt, async () => {
      const progressResponse = await page.request.get(`${base}/api/system/status`);
      assert.equal(progressResponse.status(), 200);
      return progressResponse.json();
    }, milliseconds => page.waitForTimeout(milliseconds));
    await page.screenshot({path: path.join(output, 'installed-settings.png'), fullPage: true});
    assert.deepEqual(errors, [], 'No browser JavaScript errors');
    fs.writeFileSync(path.join(output, 'browser.json'), JSON.stringify({passed: true,
      tabs: 6, savedAndReloaded: true, sdkDownloadLink: true,
      browserAppliedReplay: true, rotatedApi: true, freshFrame: true,
      replayFile, revision: accepted.revision, javascriptErrors: errors,
      input: 'synthetic BLAH2IQ replay', hardwareTested: false, sdkInstalled: false}, null, 2));
  } finally {
    await browser.close();
  }
  })().catch(error => { console.error(error); process.exitCode = 1; });
}
