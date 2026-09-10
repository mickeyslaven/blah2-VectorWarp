'use strict';

const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

function clockedPage(cpi = .5) {
  let now = 0;
  let sequence = 0;
  const timers = new Map();
  const requests = [];
  const state = {config: {process: {data: {cpi, overlap: .1}}}, frame: null, fail: false};
  const context = vm.createContext({
    URL, URLSearchParams, AbortController,
    Date: class extends Date { static now() { return now; } },
    window: {
      location: {hostname: '127.0.0.1', protocol: 'http:', port: '3000', search: ''},
      setTimeout(fn, delay) { timers.set(++sequence, {at: now + delay, fn}); return sequence; },
      clearTimeout(id) { timers.delete(id); }
    },
    fetch: async url => {
      requests.push(new URL(url, 'http://127.0.0.1:3000').pathname);
      if (state.fail) throw new Error('API restarting');
      const data = url.endsWith('/api/system/status') ? {serverId: 'fixture', radar: 'receiving'} :
        url.endsWith('/api/runtime/config') ? state.config : state.frame;
      const body = JSON.stringify(data);
      return {ok: true, text: async () => body};
    }
  });
  vm.runInContext(fs.readFileSync(require.resolve('../../html/js/common.js'), 'utf8'), context);
  async function settle() { for (let i = 0; i < 40; i++) await Promise.resolve(); }
  async function advance(ms) {
    const end = now + ms;
    await settle();
    let count = 0;
    for (;;) {
      const next = [...timers.entries()].sort((a, b) => a[1].at - b[1].at)[0];
      if (!next || next[1].at > end) break;
      assert.ok(count++ < 10000, 'Timers must remain bounded');
      now = next[1].at;
      timers.delete(next[0]);
      next[1].fn();
      await settle();
    }
    now = end;
    await settle();
  }
  return {context, state, requests, advance, settle, now: () => now};
}

(async () => {
  const page = clockedPage();
  const times = [];
  const loop = page.context.startRadarUpdates(() => { times.push(page.now()); });
  await page.advance(1500);
  assert.deepEqual(times, [0, 500, 1000, 1500]);
  assert.ok(page.requests.every(path => ['/api/runtime/config', '/api/system/status'].includes(path)), 'Never schedule from the saved/editor config');
  page.state.config = {process: {data: {cpi: .1, overlap: .09}}};
  await page.advance(700);
  assert.deepEqual(times.slice(-3), [2000, 2100, 2200], 'An open page follows restarted CPI without reload');
  loop.stop();
  await page.advance(5000);
  assert.equal(times.at(-1), 2200);

  for (const cpi of [null, undefined, 0, -1, Infinity, NaN, '0.5'])
    assert.equal(page.context.radarFrameIntervalMs({process: {data: {cpi}}}), 1000);
  assert.equal(page.context.radarFrameIntervalMs({process: {data: {cpi: .001}}}), 16);
  assert.equal(page.context.radarFrameIntervalMs({process: {data: {cpi: 1e20}}}), 2147483647);

  const slow = clockedPage(.1);
  let calls = 0;
  let release;
  const slowLoop = slow.context.startRadarUpdates(() => {
    calls++;
    return new Promise(resolve => { release = resolve; });
  });
  await slow.advance(3000);
  assert.equal(calls, 1, 'Slow requests cannot overlap');
  release();
  await slow.settle();
  await slow.advance(16);
  assert.equal(calls, 2, 'Resume with newest data, without replaying missed ticks');
  slowLoop.stop();
  release();
  await slow.advance(5000);
  assert.equal(calls, 2);

  const plot = clockedPage(.2);
  const rendered = [];
  let renderFails = true;
  const plotLoop = plot.context.startRadarPlot('http://127.0.0.1:3000/api/map', data => {
    if (renderFails) { renderFails = false; throw new Error('render failed'); }
    rendered.push(data.timestamp);
  });
  await plot.advance(200);
  assert.equal(rendered.length, 0, 'No fake or empty frame is rendered');
  plot.state.frame = {timestamp: 100, data: [[1]]};
  await plot.advance(400);
  assert.deepEqual(rendered, [100], 'Retry the same frame after a render error');
  await plot.advance(400);
  assert.deepEqual(rendered, [100], 'Do not repaint unchanged data');
  plot.state.fail = true;
  await plot.advance(200);
  plot.state.fail = false;
  plot.state.frame = {timestamp: 200, data: [[2]]};
  await plot.advance(200);
  assert.deepEqual(rendered, [100, 200], 'Recover from network errors');
  plotLoop.stop();

  // Very long CPIs must not stop an already open page finding a faster restart.
  const long = clockedPage(600);
  const longTimes = [];
  const longLoop = long.context.startRadarUpdates(() => longTimes.push(long.now()));
  await long.advance(1000);
  long.state.config = {process: {data: {cpi: .2}}};
  await long.advance(1400);
  assert.deepEqual(longTimes, [0, 2000, 2200, 2400]);
  longLoop.stop();
  console.log('Shared frame cadence, restart, slow request and frame retry tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
