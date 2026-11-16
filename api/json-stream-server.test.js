'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {EvidenceStatistics} = require('./evidence-statistics');
const {laneConfig, statisticsFilename} = require('./multi-illuminator-evidence');
let stderr = '';

async function reservePorts(count) {
  const listeners = await Promise.all(Array.from({length: count}, () => new Promise((resolve, reject) => {
    const listener = net.createServer();
    listener.once('error', reject);
    listener.listen(0, '127.0.0.1', () => resolve(listener));
  })));
  const ports = listeners.map(listener => listener.address().port);
  await Promise.all(listeners.map(listener => new Promise(resolve => listener.close(resolve))));
  return ports;
}

function request(port, route) {
  return new Promise((resolve, reject) => {
    const req = http.get({host: '127.0.0.1', port, path: route}, response => {
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => { body += chunk; });
      response.on('end', () => resolve({status: response.statusCode,
        headers: response.headers, body}));
    });
    req.on('error', reject);
  });
}

async function waitFor(port, route, expected) {
  for (let attempt = 0; attempt < 80; ++attempt) {
    try {
      const response = await request(port, route);
      if (expected(response)) return response;
    } catch (_) { /* The child may still be binding. */ }
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  throw new Error(`Timed out waiting for ${route}`);
}

function connect(port) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({host: '127.0.0.1', port}, () => resolve(socket));
    socket.once('error', reject);
  });
}

function close(socket) {
  return new Promise(resolve => {
    if (socket.destroyed) return resolve();
    socket.once('close', resolve);
    socket.end();
  });
}

async function send(port, chunks) {
  const socket = await connect(port);
  for (const chunk of chunks) socket.write(chunk);
  await close(socket);
}

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-json-stream-'));
  const configPath = path.join(directory, 'config.yml');
  let child;
  try {
    const source = path.join(__dirname, '..', 'config', 'config-kraken.yml');
    const config = yaml.load(fs.readFileSync(source, 'utf8'));
    config.process.performance.acceleration = 'gpu-resident';
    config.process.tracker.gpu_capacity = 128;
    config.process.surveillance_fusion.mode = 'coherent_beam_bank';
    config.process.bearing.enable = true;
    config.capture.device.array_geometry = {
      shape: 'regular_polygon', units: 'm', coordinate_frame: 'array_xy_right_handed',
      mapping_confirmed: true, geometry_confirmed: true, orientation_confirmed: true,
      x_axis_bearing_deg_true: 80, bearing_channels: [0, 1, 2, 3, 4],
      elements: Array.from({length: 5}, (_, daq_channel) => ({id: `element-${daq_channel}`,
        daq_channel, receiver_serial: String(1000 + daq_channel),
        position: [.2 * Math.cos(2 * Math.PI * daq_channel / 5),
          .2 * Math.sin(2 * Math.PI * daq_channel / 5), 0]}))
    };
    config.location.rx = {name: 'Receiver', latitude: 40,
      longitude: -80, altitude: 0};
    config.location.tx = {name: 'Illuminator', latitude: 40.05,
      longitude: -80, altitude: 0};
    config.capture.fc = 90900000;
    config.process.multi_illuminator = {enabled: true, illuminators: [
      {id: 'fm-90.5', enabled: true, standard: 'FM', frequency_hz: 90500000,
        occupied_bandwidth_hz: 200000, output_sample_rate_hz: 300000,
        filter_taps_per_phase: 24,
        transmitter: {name: 'FM 90.5', latitude: 40.04, longitude: -80, altitude: 300}}
    ]};
    const ports = await reservePorts(Object.keys(config.network.ports).length);
    Object.keys(config.network.ports).forEach((name, index) => { config.network.ports[name] = ports[index]; });
    config.network.ip = '127.0.0.1';
    fs.writeFileSync(configPath, yaml.dump(config));
    const statisticsPath = path.join(directory, 'statistics.json');
    const illuminator = config.process.multi_illuminator.illuminators[0];
    const seededAt = Date.now() - 5000;
    const operationalEvidence = new EvidenceStatistics(laneConfig(config, illuminator), {}, {
      filename: statisticsFilename(statisticsPath, illuminator.id, 'provisional'),
      timingPolicy: 'provisional', now: () => seededAt,
      setIntervalFn() { return {unref() {}}; }, clearIntervalFn() {}});
    for (let index = 0; index < 3; index += 1) {
      const timestamp = seededAt + index * 500;
      operationalEvidence.observe('abc123', {flight: 'OPERATIONAL',
        delay: operationalEvidence.limits.detectionMinDelayKm + 10 * operationalEvidence.limits.kmPerBin,
        doppler: 100, timestamp: timestamp / 1000,
        provenance: {observation_time_offset_s: 0, projection_model: 'identity'}},
      timestamp, null, null);
    }
    operationalEvidence.finish('abc123');
    assert.equal(operationalEvidence.associationPasses().length, 1);
    operationalEvidence.close();
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), configPath], {
      env: {...process.env, BLAH2_RECEIVER_TYPES: 'Kraken',
        BLAH2_STATISTICS_PATH: statisticsPath,
        BLAH2_PROVISIONAL_STATISTICS_PATH: path.join(directory, 'statistics-provisional.json'),
        BLAH2_OPRS_EVENTS_PATH: path.join(directory, 'oprs.ndjson')},
      stdio: ['ignore', 'ignore', 'pipe']
    });
    child.stderr.on('data', chunk => { stderr += chunk; });
    const apiPort = config.network.ports.api;
    await waitFor(apiPort, '/api/system/status', response => response.status === 200);
    for (const query of ['', '?timing=provisional']) {
      const statistics = JSON.parse((await request(apiPort, `/api/statistics${query}`)).body);
      assert.equal(statistics.timing_policy, 'provisional',
        'Ordinary SDR statistics must use the same operational lane as association history');
      assert.equal(statistics.overall_association_statistics.eligible_adsb_passes, 1);
      assert.equal(statistics.overall_association_statistics.unique_aircraft_passes.eligible_adsb_passes, 1);
    }
    const exactStatistics = JSON.parse((await request(apiPort, '/api/statistics?timing=exact')).body);
    assert.equal(exactStatistics.timing_policy, 'exact');
    assert.equal(exactStatistics.overall_association_statistics.eligible_adsb_passes, 0,
      'Explicit exact evidence must not borrow operational pass counts');
    const selectedStatistics = JSON.parse((await request(apiPort,
      '/api/statistics?illuminator=fm-90.5')).body);
    assert.equal(selectedStatistics.timing_policy, 'provisional');
    assert.equal(selectedStatistics.windows.lifetime.eligible_passes, 1);
    const associationHistory = JSON.parse((await request(apiPort, '/api/association/passes')).body);
    assert.equal(associationHistory.timing_policy, 'provisional');
    assert.equal(associationHistory.passes.length, 1);
    assert.equal((await request(apiPort, '/api/statistics?illuminator=missing')).status, 404);

    const nativeTrack = {timestamp: 1, data: [{id: 'primary-units', state: 'ACTIVE',
      n: 5, delay: 10, doppler: 20}]};
    await send(config.network.ports.track, [Buffer.from(JSON.stringify(nativeTrack))]);
    await waitFor(apiPort, '/api/tracker', response => {
      try { return JSON.parse(response.body).data?.[0]?.id === 'primary-units'; }
      catch (_) { return false; }
    });
    const oprsRows = fs.readFileSync(path.join(directory, 'oprs.ndjson'), 'utf8')
      .trim().split('\n').map(JSON.parse);
    assert.equal(oprsRows[0].sample_rate_hz, 300000,
      'legacy tracker socket carries primary-lane bins, not full-passband capture bins');
    assert.ok(Math.abs(oprsRows.find(row => row.kind === 'track_measurement').delay_km -
      10 * 299792.458 / 300000) < 1e-9);

    const routes = [
      ['map', '/api/map'], ['detection', '/api/detection'], ['track', '/api/tracker'],
      ['bearing', '/api/bearing'], ['timing', '/api/timing'], ['iqdata', '/api/iqdata']
    ];
    for (const [name, route] of routes) {
      const first = JSON.stringify({stream: name, message: `fragmented 🙂 ${name}`});
      const second = JSON.stringify({stream: name, sequence: 2});
      const bytes = Buffer.from(first);
      // Split the emoji across byte chunks and coalesce the final first frame
      // with a complete second frame on the same TCP connection.
      const emoji = bytes.indexOf(Buffer.from('🙂'));
      const chunks = [bytes.subarray(0, emoji + 2),
        Buffer.concat([bytes.subarray(emoji + 2), Buffer.from(second)])];
      // Timing deliberately expires on disconnect, unlike the historical
      // display streams, so observe its complete frame before closing it.
      const liveSocket = name === 'timing' ? await connect(config.network.ports[name]) : null;
      if (liveSocket) chunks.forEach(chunk => liveSocket.write(chunk));
      else await send(config.network.ports[name], chunks);
      const response = await waitFor(apiPort, route, value => {
        if (name !== 'timing') return value.body === second;
        try {
          const parsed = JSON.parse(value.body);
          return parsed.stream === name && parsed.sequence === 2;
        } catch (_) { return false; }
      });
      if (name === 'timing') {
        const parsed = JSON.parse(response.body);
        assert.equal(parsed.stream, name);
        assert.equal(parsed.sequence, 2);
        assert.ok(Object.hasOwn(parsed, 'capture_backlog_ms'),
          'Timing responses include derived capture health fields');
      } else assert.equal(response.body, second, `${name} must retain the final coalesced frame`);
      if (liveSocket) await close(liveSocket);
    }
    const bearingFrame = {timestamp: 9, method: 'coherent_beam_bank', data: [{id: '0001',
      state: 'ACTIVE', delay: 80, doppler: 20, bearing_deg_true: 90,
      uncertainty_deg: 10, resolved: true, candidates: [{bearing_deg_true: 90, support: 1}]}]};
    await send(config.network.ports.bearing, [Buffer.from(JSON.stringify(bearingFrame))]);
    const locations = await waitFor(apiPort, '/api/locations', value => {
      try { return JSON.parse(value.body).data?.[0]?.location; } catch (_) { return false; }
    });
    assert.equal(JSON.parse(locations.body).data[0].id, '0001');

    const illuminatorFrame = {schema: 'vectorwarp-multi-illuminator-frame-v1',
      id: 'fm-90.5', frequency_hz: 90500000, sample_rate_hz: 300000,
      first_input_sample: 0, end_input_sample: 1199999, timestamp: 10,
      trackerDiagnostics: {schema: 'vectorwarp-cpu-tracker-frame-diagnostics-v1',
        truth_blind: true, scope: 'single-illuminator-single-frame',
        pair_semantics: 'exclusive-ordered-candidate-track-pair-rejections',
        no_op: false, detections: 7, tracks_before: 2, tracks_after: 3,
        candidate_track_pairs: 14, pair_already_claimed: 1,
        pair_delay_rejected: 6, pair_doppler_rejected: 4, pair_in_gate: 3,
        associations: 2, misses: 0, promotions: 1, tentative_expired: 0,
        inactivity_deleted: 0, birth_candidates: 5, birth_snr_rejected: 4,
        initiated: 1},
      temporalShadow: {active: true, banks: [{name: 'motion8',
        map: {stream: 'nested-experiment-map'}}]},
      map: {stream: 'fm-map', timestamp: 10},
      detections: {stream: 'fm-detection', timestamp: 10},
      tracks: {stream: 'fm-track', timestamp: 10}};
    await send(config.network.ports.illuminator,
      [Buffer.from(JSON.stringify(illuminatorFrame))]);
    const illuminators = JSON.parse((await waitFor(apiPort, '/api/illuminators/live', value => {
      try { return JSON.parse(value.body).illuminators?.[0]?.available === true; }
      catch (_) { return false; }
    })).body);
    assert.equal(illuminators.primary, 'fm-90.5');
    assert.equal(illuminators.illuminators[0].frequency_hz, 90500000);
    assert.deepEqual(illuminators.illuminators[0].tracker_diagnostics,
      illuminatorFrame.trackerDiagnostics,
      'Native radar-only tracker counters survive the frame stream and status route');
    const laneScorecard = JSON.parse((await request(apiPort,
      '/api/live-scorecard?illuminator=fm-90.5')).body);
    assert.equal(laneScorecard.scope, 'single-illuminator');
    assert.equal(laneScorecard.identity.frequency_hz, 90500000);
    assert.equal(laneScorecard.identity.sample_rate_hz, 300000);
    const aggregateScorecard = JSON.parse((await request(apiPort,
      '/api/live-scorecard')).body);
    assert.equal(aggregateScorecard.scope, 'all-illuminators-non-merged');
    assert.equal(aggregateScorecard.illuminators.length, 1);
    const coverage = JSON.parse((await request(apiPort,
      '/api/radar/coverage?illuminator=fm-90.5')).body);
    assert.equal(coverage.scope, 'single-illuminator');
    assert.equal(coverage.identity.illuminator, 'fm-90.5');
    assert.equal((await request(apiPort,
      '/api/live-scorecard?illuminator=unknown')).status, 404);
    const detectionResponse = await request(apiPort,
      '/api/detection?illuminator=fm-90.5');
    const trackerResponse = await request(apiPort,
      '/api/tracker?illuminator=fm-90.5');
    assert.match(detectionResponse.headers['content-type'], /^application\/json\b/);
    assert.match(trackerResponse.headers['content-type'], /^application\/json\b/);
    assert.equal(JSON.parse(detectionResponse.body).stream, 'fm-detection');
    assert.equal(JSON.parse(trackerResponse.body).stream, 'fm-track');

    // A generic HTTP health probe must not permanently poison the legacy
    // listeners when the primary products are derived from the authoritative
    // multi-illuminator frame instead of separate legacy connections.
    await Promise.all(['map', 'detection', 'track'].map(async name => {
      const socket = await connect(config.network.ports[name]);
      socket.on('error', () => {});
      socket.write('GET / HTTP/1.0\r\n\r\n');
      await new Promise(resolve => socket.once('close', resolve));
    }));
    await waitFor(apiPort, '/api/system/status', response => {
      try { return JSON.parse(response.body).errors.some(error =>
        /(?:map|detection|track) connection/.test(error)); }
      catch (_) { return false; }
    });
    const nextIlluminatorFrame = {...illuminatorFrame, timestamp: 11,
      first_input_sample: 1200000, end_input_sample: 2399999,
      map: {stream: 'fm-map-next', timestamp: 11},
      detections: {stream: 'fm-detection-next', timestamp: 11},
      tracks: {stream: 'fm-track-next', timestamp: 11}};
    await send(config.network.ports.illuminator,
      [Buffer.from(JSON.stringify(nextIlluminatorFrame))]);
    const recoveredFromLegacyProbe = JSON.parse((await waitFor(apiPort,
      '/api/system/status', response => {
        try { return !JSON.parse(response.body).errors.some(error =>
          /(?:map|detection|track) connection/.test(error)); }
        catch (_) { return false; }
      })).body);
    assert.ok(!recoveredFromLegacyProbe.errors.some(error =>
      /(?:map|detection|track) connection/.test(error)));
    assert.equal(JSON.parse((await request(apiPort,
      '/api/map?illuminator=fm-90.5')).body).stream, 'fm-map-next');
    assert.equal(JSON.parse((await request(apiPort,
      '/api/map?illuminator=fm-90.5&frame_timestamp=10')).body).stream,
      'fm-map', 'an exact lane timestamp must not mix adjacent products');
    assert.equal((await request(apiPort,
      '/api/map?illuminator=fm-90.5&frame_timestamp=9')).status, 404);
    const restartedIlluminatorFrame = {...illuminatorFrame, timestamp: 12,
      map: {stream: 'fm-map-restarted', timestamp: 12},
      detections: {stream: 'fm-detection-restarted', timestamp: 12},
      tracks: {stream: 'fm-track-restarted', timestamp: 12}};
    await send(config.network.ports.illuminator,
      [Buffer.from(JSON.stringify(restartedIlluminatorFrame))]);
    assert.equal(JSON.parse((await request(apiPort,
      '/api/map?illuminator=fm-90.5')).body).stream, 'fm-map-restarted');
    assert.equal((await request(apiPort,
      '/api/map?illuminator=fm-90.5&frame_timestamp=11')).status, 404,
    'a regressed process-local sample counter starts a clean producer session');
    assert.equal((await request(apiPort,
      '/api/tracker?illuminator=unknown')).status, 404);

    // A partial frame belongs only to its original socket and cannot contaminate
    // the next processor connection.
    await send(config.network.ports.map, [Buffer.from('{"stale":')]);
    const recovered = JSON.stringify({stream: 'map', reconnect: true});
    await send(config.network.ports.map, [Buffer.from(recovered)]);
    assert.equal((await waitFor(apiPort, '/api/map', value => value.body === recovered)).body, recovered);

    const broken = await connect(config.network.ports.map);
    broken.on('error', () => {});
    broken.write('{"broken":]');
    await new Promise(resolve => broken.once('close', resolve));
    const unhealthy = JSON.parse((await waitFor(apiPort, '/api/system/status', response => {
      try { return JSON.parse(response.body).errors.some(error => error.includes('map connection')); }
      catch (_) { return false; }
    })).body);
    assert.ok(unhealthy.errors.some(error => error.includes('map connection')));

    const oversized = await connect(config.network.ports.map);
    oversized.on('error', () => {});
    oversized.write(Buffer.concat([Buffer.from('{"payload":"'),
      Buffer.alloc(16 * 1024 * 1024, 'x')]));
    await new Promise(resolve => oversized.once('close', resolve));
    const bounded = JSON.parse((await waitFor(apiPort, '/api/system/status', response => {
      try { return JSON.parse(response.body).errors.some(error => error.includes('exceeds limit')); }
      catch (_) { return false; }
    })).body);
    assert.ok(bounded.errors.some(error => error.includes('exceeds limit')),
      'Oversized input must terminate only its own TCP connection');

    const healthy = JSON.stringify({stream: 'map', recoveredAfterError: true});
    await send(config.network.ports.map, [Buffer.from(healthy)]);
    const status = JSON.parse((await waitFor(apiPort, '/api/system/status', response => {
      try { return !JSON.parse(response.body).errors.some(error => error.includes('map connection')); }
      catch (_) { return false; }
    })).body);
    assert.ok(!status.errors.some(error => error.includes('map connection')),
      'A valid reconnect must clear only its prior connection health error');
    assert.equal((await request(apiPort, '/api/map')).body, healthy);
    assert.equal(child.exitCode, null, 'Malformed input must not stop the API process');
    console.log('JSON stream server fragmentation, isolation, error containment and recovery tests passed.');
  } finally {
    if (child && child.exitCode === null) {
      await new Promise(resolve => {
        child.once('close', resolve);
        if (!child.kill('SIGTERM')) resolve();
      });
    }
    fs.rmSync(directory, {recursive: true, force: true, maxRetries: 10,
      retryDelay: 50});
  }
})().catch(error => {
  console.error(error);
  if (stderr) console.error(stderr);
  process.exitCode = 1;
});
