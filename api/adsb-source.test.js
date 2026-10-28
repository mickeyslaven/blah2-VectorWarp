'use strict';
const assert = require('assert');
const http = require('http');
const {sourceAddress, aircraftUrl, configuredSources, configuredAircraftSource,
  normalizeAircraftSnapshot, readJson,
  createAdsbSource} = require('./adsb-source');
const {DelayDopplerHistory} = require('./adsb-geometry');

(async () => {
  const config = {capture: {fc: 100000000}, truth: {adsb: {enabled: true,
    tar1090: 'receiver.local:8080', smoothing_window: 10, max_position_age: 30}},
  location: {rx: {latitude: 1, longitude: 2, altitude: 3}, tx: {latitude: 4, longitude: 5, altitude: 6}}};
  assert.equal(aircraftUrl(config).href, 'http://receiver.local:8080/data/aircraft.json');
  for (const base of ['tar1090', 'dump1090-fa', 'dump1090']) {
    const compatible = {...config, truth: {adsb: {...config.truth.adsb,
      tar1090: `http://receiver.local/${base}`}}};
    assert.equal(aircraftUrl(compatible).href, `http://receiver.local/${base}/data/aircraft.json`);
  }
  assert.equal(sourceAddress('https://receiver.example/tar1090').href, 'https://receiver.example/tar1090/');
  assert.equal(sourceAddress('[::1]:8080/tar1090').href, 'http://[::1]:8080/tar1090/');
  assert.deepEqual(configuredSources(config), ['receiver.local:8080']);
  assert.deepEqual(configuredSources({...config, truth: {adsb: {...config.truth.adsb,
    sources: ['local:readsb', 'backup.example']}}}), ['local:readsb', 'backup.example']);
  const exchange = configuredAircraftSource(
    'exchange:https://api.adsb.lol/v2/point/40.44/-80.06/60');
  assert.equal(exchange.url.href, 'https://api.adsb.lol/v2/point/40.44/-80.06/60');
  assert.equal(exchange.adapter, 'adsbx-v2');
  assert.deepEqual(normalizeAircraftSnapshot({now: 2000000000123,
    ac: [{hex: 'abc123', seen_pos: .5}]}, 'adsbx-v2'),
  {now: 2000000000.123, aircraft: [{hex: 'abc123', seen_pos: .5}]});
  for (const address of ['file:///etc/passwd', 'ftp://receiver', 'http://name:password@receiver', 'receiver?url=other', 'receiver#bad', 'bad host'])
    assert.throws(() => sourceAddress(address), undefined, address);

  let clock = 100000;
  const reads = [];
  let aircraft = {now: 100, aircraft: [{hex: 'abc123', flight: 'TEST', lat: 1, lon: 2, alt_geom: 1000, seen_pos: 0}]};
  const options = {now: () => clock, fetchJson: async url => {
    reads.push(url.href);
    return aircraft;
  }};
  const source = createAdsbSource(config, options);
  const [positions, convertedData, status] = await Promise.all([
    source.get('aircraft'), source.get('delayDoppler'), source.status()]);
  assert.equal(reads.length, 1, 'Concurrent clients share one raw aircraft request');
  assert.equal(positions.aircraft.length, 1);
  assert.deepEqual(Object.keys(convertedData), [], 'One position is motion warmup, not a Doppler result');
  assert.equal(status.online, true);
  assert.equal((await source.status()).delayDoppler.warming, 1,
    'Completed first-position conversion remains visible as motion warmup');

  const delayedStatusConfig = {...config, process: {data: {cpi: .5}},
    truth: {adsb: {...config.truth.adsb, association_lag_cpis: 4}}};
  const delayedStatus = createAdsbSource(delayedStatusConfig, {now: () => clock,
    fetchJson: async () => aircraft});
  const statusStarted = Date.now();
  assert.equal((await delayedStatus.status()).online, true);
  assert.ok(Date.now() - statusStarted < 250,
    'Health status must not inherit the two-second CPI association hold');
  const scheduledAssociationWaits = [];
  const associationOrder = [];
  const delayedAssociation = createAdsbSource(delayedStatusConfig, {now: () => clock,
    fetchJson: async () => { associationOrder.push('fetch'); return aircraft; },
    setTimeoutFn: (callback, delay) => {
      scheduledAssociationWaits.push(delay); associationOrder.push('wait'); callback();
    }});
  await delayedAssociation.getAt('aircraft', clock);
  assert.deepEqual(scheduledAssociationWaits, [2000],
    'Four 500 ms CPIs must create exactly a two-second scoring wait');
  assert.deepEqual(associationOrder, ['wait', 'fetch'],
    'The catch-up wait must precede acquisition of the newest ADS-B snapshot');

  const displayClock = 102000;
  const displaySource = createAdsbSource(delayedStatusConfig, {now: () => displayClock,
    fetchJson: async () => ({now: 100.5, aircraft: [{hex: 'fed123', flight: 'DISPLAY',
      lat: 1, lon: 2, alt_geom: 1000, gs: 0, track: 0, seen_pos: 0}]})});
  const latestDisplay = await displaySource.getPresentation('aircraft');
  assert.equal(latestDisplay.aircraft.length, 1,
    'Latest display positions accept and project feeder snapshots within max_position_age');
  assert.equal(latestDisplay.aircraft[0].vectorwarp.projected_seconds, 1.5);

  let exactReads = 0;
  const exactProjection = createAdsbSource(config, {now: () => 100000,
    fetchJson: async () => {
      exactReads += 1;
      return {now: 100, aircraft: [{hex: 'fed123', flight: 'EXACT', lat: 1, lon: 2,
        alt_geom: 1000, gs: 360, track: 90, seen_pos: 0}]};
    }});
  const [atFirstCpi, atSecondCpi] = await Promise.all([
    exactProjection.getAt('aircraft', 100000), exactProjection.getAt('aircraft', 100500)]);
  assert.equal(exactReads, 1, 'Distinct CPI projections must share raw ADS-B I/O');
  assert.equal(atFirstCpi.now, 100);
  assert.equal(atSecondCpi.now, 100.5);
  assert.notEqual(atFirstCpi.aircraft[0].lon, atSecondCpi.aircraft[0].lon,
    'Each CPI must receive its own event-time projection, not a cached fused position');
  clock += 1001;
  aircraft = {now: clock / 1000, aircraft: [{hex: 'abc123', flight: 'TEST', lat: 1.1, lon: 2.1, alt_geom: 1100, seen_pos: 0}]};
  const derived = await source.get('delayDoppler');
  assert.ok(Number.isFinite(derived.abc123.delay) && Number.isFinite(derived.abc123.doppler));
  assert.equal(derived.abc123.provenance.source_id, 'receiver.local:8080');
  assert.equal(derived.abc123.provenance.position_age_s, 0);
  const expectedHistory = new DelayDopplerHistory(config);
  expectedHistory.update({now: 100, aircraft: [{hex: 'abc123', flight: 'TEST', lat: 1, lon: 2, alt_geom: 1000, seen_pos: 0}]});
  const expected = expectedHistory.update(aircraft).data.abc123;
  assert.equal(derived.abc123.delay, expected.delay, 'Configured RX/TX/frequency geometry is used in-process');
  const recreated = createAdsbSource(config, options);
  assert.deepEqual(await recreated.get('delayDoppler'), {}, 'A recreated source resets motion history to warmup');
  clock += 31000;
  await assert.rejects(source.get('aircraft'), /stale/);
  assert.equal((await source.status()).online, false);
  clock += 1001;
  aircraft = {now: clock / 1000, aircraft: []};
  assert.equal((await source.status()).online, true, 'Empty aircraft and warmup DD are valid');
  const disabled = createAdsbSource({...config, truth: {adsb: {enabled: false}}}, options);
  const count = reads.length;
  assert.equal((await disabled.status()).enabled, false);
  await assert.rejects(disabled.get('aircraft'), /disabled/);
  const preview = createAdsbSource(config, {...options, preview: true});
  assert.equal((await preview.status()).online, false);
  assert.equal(reads.length, count, 'Disabled/preview must not contact upstream services');

  let discoveries = 0;
  let explicitOnline = false;
  const explicitFailure = createAdsbSource(config, {now: () => clock,
    fetchJson: async () => {
      if (!explicitOnline) throw new Error('configured endpoint offline');
      return {now: clock / 1000, aircraft: []};
    },
    discover: async () => { discoveries += 1; throw new Error('must not discover'); }});
  await assert.rejects(explicitFailure.get('aircraft'), /configured endpoint offline/);
  assert.equal(discoveries, 0, 'An explicit remote endpoint never falls back to local discovery');
  assert.equal((await explicitFailure.status()).source.mode, 'explicit');
  explicitOnline = true;
  clock += 1001;
  const recoveredStatus = await explicitFailure.status();
  assert.equal(recoveredStatus.online, true);
  assert.equal(Object.prototype.hasOwnProperty.call(recoveredStatus.source, 'message'), false,
    'Recovered source status does not retain a stale error');

  let autoReads = 0;
  const autoConfig = {...config, truth: {adsb: {...config.truth.adsb, tar1090: 'auto'}}};
  const automatic = createAdsbSource(autoConfig, {now: () => clock, discoveryCacheMs: 30000,
    discover: async () => {
      discoveries += 1;
      return {source: {mode: 'auto', kind: 'file', value: 'local:readsb',
        label: 'Local readsb', path: '/run/readsb/aircraft.json'},
      read: async () => { autoReads += 1; return {now: clock / 1000, aircraft: []}; }};
    }});
  assert.deepEqual((await automatic.get('aircraft')).aircraft, []);
  const automaticStatus = await automatic.status();
  assert.equal(automaticStatus.source.mode, 'auto');
  assert.equal(automaticStatus.source.value, 'local:readsb');
  assert.equal(discoveries, 1);
  assert.equal(autoReads, 1, 'Raw result cache is shared by automatic-source clients');

  const replay = createAdsbSource({...autoConfig, capture: {...autoConfig.capture,
    replay: {state: true}}}, {now: () => clock,
    discover: async () => { discoveries += 1; throw new Error('must not discover'); }});
  const replayStatus = await replay.status();
  assert.equal(replayStatus.online, false);
  assert.equal(replayStatus.source.state, 'inactive');
  assert.equal(discoveries, 1, 'Replay must not probe live ADS-B sources');

  let localSelections = 0;
  const localConfig = {...config, truth: {adsb: {...config.truth.adsb,
    tar1090: 'local:readsb'}}};
  const local = createAdsbSource(localConfig, {now: () => clock,
    selectLocal: async value => {
      localSelections += 1;
      assert.equal(value, 'local:readsb');
      throw new Error('Selected local readsb is offline');
    }, discover: async () => { discoveries += 1; throw new Error('must not discover'); }});
  await assert.rejects(local.get('aircraft'), /readsb is offline/);
  assert.equal(localSelections, 1);
  assert.equal(discoveries, 1, 'A selected local source never falls back to auto discovery');

  const multiReads = [];
  const started = new Set();
  let releaseConcurrent;
  const bothStarted = new Promise(resolve => { releaseConcurrent = resolve; });
  const multiConfig = {...config, truth: {adsb: {...config.truth.adsb,
    sources: ['primary.example', 'backup.example'], source_timeout: .25}}};
  const multi = createAdsbSource(multiConfig, {now: () => clock,
    fetchJson: async (url, readOptions) => {
      multiReads.push([url.hostname, readOptions.timeoutMs]);
      started.add(url.hostname);
      if (started.size === 2) releaseConcurrent();
      await bothStarted;
      if (url.hostname === 'primary.example') throw new Error('primary unavailable');
      return {now: clock / 1000, aircraft: [{hex: 'def456', lat: 1.2, lon: 2.2,
        alt_geom: 1200, gs: 0, track: 0, seen_pos: .5}]};
    }});
  const multiAircraft = await multi.get('aircraft');
  assert.deepEqual(started, new Set(['primary.example', 'backup.example']),
    'Configured sources start concurrently');
  assert.deepEqual(multiReads.map(item => item[1]), [250, 250],
    'Each HTTP source receives the configured independent timeout');
  assert.equal(multiAircraft.aircraft[0].vectorwarp.source_id, 'backup.example');
  assert.equal(multiAircraft.aircraft[0].vectorwarp.source_endpoint,
    'http://backup.example/data/aircraft.json');
  const multiStatus = await multi.status();
  assert.equal(multiStatus.online, true, 'One healthy source keeps fused truth online');
  assert.equal(multiStatus.degraded, true);
  assert.equal(multiStatus.sources.length, 2);
  assert.equal(multiStatus.sources[0].healthy, false);
  assert.equal(multiStatus.sources[0].consecutive_failures, 1);
  assert.equal(multiStatus.sources[1].healthy, true);
  assert.equal(multiStatus.sources[1].freshness.fresh_positions, 1);
  assert.deepEqual(multiStatus.fusion, {...multiAircraft.vectorwarp_fusion,
    fused_at: clock, healthy_sources: 1, failed_sources: 1});

  const server = http.createServer((req, res) => {
    if (req.url === '/stall') { res.writeHead(200); res.write('{'); return; }
    if (req.url === '/large') return res.end('x'.repeat(200));
    if (req.url === '/redirect') { res.writeHead(302, {Location: 'http://other.invalid'}); return res.end(); }
    if (req.url === '/html') return res.end('<html>Login</html>');
    res.end('{}');
  });
  const sockets = new Set();
  server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const url = route => new URL(`http://127.0.0.1:${server.address().port}${route}`);
  try {
    assert.deepEqual(await readJson(url('/')), {});
    await assert.rejects(readJson(url('/stall'), {timeoutMs: 30}), /timed out/);
    await assert.rejects(readJson(url('/large'), {maxBytes: 64}), /size limit/);
    await assert.rejects(readJson(url('/redirect')), /302/);
    await assert.rejects(readJson(url('/html')), /invalid JSON/);
  } finally { for (const socket of sockets) socket.destroy(); await new Promise(resolve => server.close(resolve)); }
  console.log('ADS-B source addressing, freshness, warmup, cache, timeout and disabled/preview tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
