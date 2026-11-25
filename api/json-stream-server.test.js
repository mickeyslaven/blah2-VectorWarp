'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const zlib = require('zlib');
const {validateConfig} = require('./config-manager');
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

function request(port, route, headers = {}) {
  return new Promise((resolve, reject) => {
    const req = http.get({host: '127.0.0.1', port, path: route, headers}, response => {
      const chunks = [];
      response.on('data', chunk => { chunks.push(chunk); });
      response.on('end', () => {
        const bytes = Buffer.concat(chunks);
        const body = (response.headers['content-encoding'] === 'gzip' ?
          zlib.gunzipSync(bytes) : bytes).toString('utf8');
        resolve({status: response.statusCode, headers: response.headers, body});
      });
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
    const config = require('./config-store').configurationForReceiver('Kraken');
    config.process.performance.acceleration = 'gpu-resident';
    config.process.tracker.gpu_capacity = 128;
    config.process.surveillance_fusion.mode = 'coherent_beam_bank';
    config.process.bearing.enable = true;
    config.process.detection.track_before_detect.enable = true;
    config.process.detection.track_before_detect.max_cpis = 4;
    config.process.tracker.initiate.N = 9;
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
        transmitter_location: {latitude: 40.04, longitude: -80, altitude: 300},
        transmitter: {name: 'FM 90.5', latitude: 40.04, longitude: -80, altitude: 300}}
    ]};
    config.process.evidence = {causal_funnel: {enabled: false,
      api_ingest_enabled: true, epoch_id: 'api-only-causal-stream-test',
      path: path.join(directory, 'causal-funnel'), max_candidates: 32,
      max_queue: 32, max_queue_mb: 16, max_file_mb: 16, max_files: 2}};
    const ports = await reservePorts(Object.keys(config.network.ports).length);
    Object.keys(config.network.ports).forEach((name, index) => { config.network.ports[name] = ports[index]; });
    config.network.ip = '127.0.0.1';
    const validation = validateConfig(config);
    assert.equal(validation.valid, true, validation.errors.join('; '));
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
    const apiEnvironment = {...process.env, BLAH2_RECEIVER_TYPES: 'Kraken',
        VECTORWARP_CYCLE_LEDGER_DIR: path.join(directory, 'cycles'),
        BLAH2_STATISTICS_PATH: statisticsPath,
        BLAH2_PROVISIONAL_STATISTICS_PATH: path.join(directory, 'statistics-provisional.json'),
        BLAH2_OPRS_EVENTS_PATH: path.join(directory, 'oprs.ndjson')};
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), configPath], {
      env: apiEnvironment,
      stdio: ['ignore', 'ignore', 'pipe']
    });
    child.stderr.on('data', chunk => { stderr += chunk; });
    const apiPort = config.network.ports.api;
    await waitFor(apiPort, '/api/system/status', response => response.status === 200);
    await assert.rejects(connect(config.network.ports.causal_funnel),
      /ECONNREFUSED/, 'API-only collection must not expose the native TCP ingest port');
    for (const query of ['', '?timing=provisional']) {
      const response = await request(apiPort, `/api/statistics${query}`);
      const statistics = JSON.parse(response.body);
      assert.ok(Buffer.byteLength(response.body) < 256 * 1024);
      assert.ok(statistics.illuminators.every(lane => lane.eligibility_events === undefined &&
        lane.measured_track_basis === undefined));
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
    const compactAssociationHistory = JSON.parse((await request(apiPort,
      '/api/association/passes?compact=true')).body);
    assert.equal(compactAssociationHistory.passes.length, 1);
    assert.deepEqual(Object.keys(compactAssociationHistory.passes[0]).sort(),
      ['association_eligible_cpis', 'channel', 'decision_grade', 'detected', 'detected_cpis',
        'eligible', 'eligible_cpis', 'evaluation_contract_hash', 'evaluation_policy_hash',
        'finalized_at', 'flight', 'hex', 'outcome', 'pass_id', 'sensitivity_epoch_id',
        'sensitivity_epoch_mixed', 'site_epoch', 'started_at', 'timing_policy',
        'tracked', 'tracked_cpis']);
    const compressedPasses = await request(apiPort,
      '/api/association/passes?max_rows=32&max_cols=32', {'Accept-Encoding': 'gzip'});
    assert.equal(compressedPasses.headers['content-encoding'], 'gzip');
    const {statistics_window: compressedWindow, ...compressedHistory} = JSON.parse(compressedPasses.body);
    const {statistics_window: historyWindow, ...plainHistory} = associationHistory;
    assert.deepEqual(compressedHistory, plainHistory,
      'Generic JSON compression must not apply map presentation controls');
    assert.equal(compressedWindow.duration_ms, historyWindow.duration_ms);
    assert.equal(compressedWindow.complete, historyWindow.complete);
    assert.equal((await request(apiPort, '/api/statistics?illuminator=missing')).status, 404);

    const nativeTrack = {timestamp: 1, data: [{id: 'primary-units', state: 'ACTIVE',
      n: 5, delay: 10, doppler: 20}]};
    await send(config.network.ports.track, [Buffer.from(JSON.stringify(nativeTrack))]);
    await waitFor(apiPort, '/api/tracker', response => {
      try { return JSON.parse(response.body).data?.[0]?.id === 'primary-units'; }
      catch (_) { return false; }
    });
    assert.equal((await request(apiPort, '/api/tracker/native')).body,
      JSON.stringify(nativeTrack), 'native route preserves the admitted payload');
    assert.equal((await request(apiPort, '/api/tracker/publication')).status, 404,
      'the retired alternative publication owner has no endpoint');
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
      const cycleEvidence = {schema: 'vectorwarp-processing-cycles-v1',
        anchorMonoNs: '1000000', anchorUtcNs: '1795500000000000000', fault: '', records: []};
      const evidence = name === 'timing' ? {cycleEvidence} : {};
      const first = JSON.stringify({stream: name, message: `fragmented 🙂 ${name}`, ...evidence});
      const second = JSON.stringify({stream: name, sequence: 2, ...evidence});
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
        const capture = acceptedSamples => ({available: true, epoch: '1', discontinuities: '0',
          requalifications: '0', retiredCpis: '0', retiredSamples: '0',
          lanes: [{acceptedSamples, droppedSamples: '0', discardedSamples: '0'}]});
        cycleEvidence.records.push({sequence: '1', startMonoNs: '2000000', endMonoNs: '2500000',
          drainBatches: '0', outcome: 'warmup', captureBefore: capture('1'), captureAfter: capture('2')});
        liveSocket.write(JSON.stringify({cycleEvidenceOnly: true, stream: name, sequence: 999, cycleEvidence}));
        const deadline = Date.now() + 5000;
        let retained = false;
        while (!retained && Date.now() < deadline) {
          try { retained = JSON.parse(fs.readFileSync(path.join(directory, 'cycles/status.json'))).throughSequence === '1'; }
          catch (_) { /* Checkpoint publication is asynchronous. */ }
          if (!retained) await new Promise(resolve => setTimeout(resolve, 20));
        }
        assert.equal(retained, true, 'Checkpoint proves the evidence-only frame reached the real API');
        assert.equal(JSON.parse((await request(apiPort, route)).body).sequence, 2);
        assert.equal(JSON.parse((await request(apiPort, route)).body).cycleEvidenceOnly, undefined);
      } else assert.equal(response.body, second, `${name} must retain the final coalesced frame`);
      if (liveSocket) await close(liveSocket);
    }
    const bearingFrame = {timestamp: 9, method: 'coherent_beam_bank', data: [{id: '0001',
      state: 'ACTIVE', delay: 80, doppler: 20, bearing_deg_true: 90,
      uncertainty_deg: 10, resolved: true, candidates: [{bearing_deg_true: 90, support: 1}]}]};
    await send(config.network.ports.bearing, [Buffer.from(JSON.stringify(bearingFrame))]);
    const locations = await waitFor(apiPort, '/api/locations', value => {
      try { return JSON.parse(value.body).data?.[0]?.estimate; } catch (_) { return false; }
    });
    const diagnosticLocations = JSON.parse(locations.body);
    assert.equal(diagnosticLocations.data[0].id, '0001');
    assert.equal(diagnosticLocations.operational_localization, false);
    assert.equal(diagnosticLocations.data[0].location, null,
      'Bearing and delay provide a diagnostic estimate without altitude resolution');
    assert.equal(diagnosticLocations.data[0].operational_position, false);
    const bearingSocket = await connect(config.network.ports.bearing);
    const bearingAt = timestamp => JSON.stringify({...bearingFrame, timestamp,
      capture_sample_position_available: true, capture_epoch: 0});
    bearingSocket.write(bearingAt(10));
    await waitFor(apiPort, '/api/locations', response => {
      try { const value = JSON.parse(response.body);
        return value.timestamp === 10 && value.data[0].geometry_consistent_samples === 1;
      } catch (_) { return false; }
    });
    bearingSocket.write(bearingAt(11));
    await waitFor(apiPort, '/api/locations', response => {
      try { const value = JSON.parse(response.body);
        return value.timestamp === 11 && value.data[0].geometry_consistent_samples === 2;
      } catch (_) { return false; }
    });
    await close(bearingSocket);
    await send(config.network.ports.bearing, [Buffer.from(bearingAt(12))]);
    await waitFor(apiPort, '/api/locations', response => {
      try { const value = JSON.parse(response.body);
        return value.timestamp === 12 && value.data[0].geometry_consistent_samples === 1;
      } catch (_) { return false; }
    });

    const illuminatorFrame = {schema: 'vectorwarp-multi-illuminator-frame-v1',
      id: 'fm-90.5', frequency_hz: 90500000, sample_rate_hz: 300000,
      first_input_sample: 0, end_input_sample: 1199999, timestamp: 10,
      trackerDiagnostics: {schema: 'vectorwarp-tracker-frame-diagnostics-v2',
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
      detections: {stream: 'fm-detection', timestamp: 10,
        delay: [5], doppler: [20], snr: [8]},
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
    const causalStatus = JSON.parse((await waitFor(apiPort,
      '/api/evidence/causal-funnel/status', value => {
        try { return JSON.parse(value.body).written > 0; } catch (_) { return false; }
      })).body);
    assert.ok(causalStatus.written > 0, JSON.stringify(causalStatus));
    assert.equal(causalStatus.api_ingest_enabled, true);
    assert.equal(causalStatus.native_enabled, false);
    assert.equal(causalStatus.native_received, 0);
    assert.equal(causalStatus.producer.wired, false);
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
      map: {stream: 'fm-map-next', timestamp: 11,
        delay: Array.from({length: 40}, (_, index) => index),
        doppler: Array.from({length: 40}, (_, index) => index),
        data: Array.from({length: 40}, (_, row) =>
          Array.from({length: 40}, (_, column) => row * 40 + column))},
      detections: {stream: 'fm-detection-next', timestamp: 11},
      tracks: {stream: 'fm-track-next', timestamp: 11,
        data: [{id: '9007199254740992'}, {id: '9007199254740993'}]}};
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
    // Freeze shared route selection before consolidating its implementation.
    // Exact retained frames, current primary aliases and failures must agree
    // without coercing track identities or borrowing an adjacent CPI.
    for (const [route, field] of [['map', 'map'], ['detection', 'detections'], ['tracker', 'tracks']]) {
      for (const suffix of ['', '?illuminator=fm-90.5',
        '?illuminator=fm-90.5&frame_timestamp=11&first_input_sample=1200000&end_input_sample=2399999',
        '?presentation=aligned&frame_timestamp=11']) {
        const response = await request(apiPort, `/api/${route}${suffix}`);
        assert.equal(response.status, 200);
        assert.match(response.headers['content-type'], /^application\/json\b/);
        assert.equal(response.body, JSON.stringify(nextIlluminatorFrame[field]));
      }
      for (const suffix of ['illuminator=unknown', 'illuminator=fm-90.5&frame_timestamp=9',
        'illuminator=fm-90.5&first_input_sample=9007199254740993',
        'illuminator=fm-90.5&end_input_sample=-1']) {
        const response = await request(apiPort, `/api/${route}?${suffix}`);
        assert.equal(response.status, 404);
        assert.deepEqual(JSON.parse(response.body), {available: false,
          reason: 'Unknown or not-yet-live illuminator.'});
      }
      const missing = await request(apiPort, `/api/${route}?presentation=aligned&frame_timestamp=9`);
      assert.equal(missing.status, 200);
      assert.equal(missing.body, '', 'Absent exact layers stay empty');
    }
    const bundleRoute = '/api/radar/frame?illuminator=fm-90.5&max_rows=32&max_cols=32';
    const plainBundle = await request(apiPort, bundleRoute);
    assert.equal(plainBundle.status, 200);
    assert.equal(plainBundle.headers['content-encoding'], undefined);
    const bundle = JSON.parse(plainBundle.body);
    assert.deepEqual(Object.keys(bundle), ['schema', 'timestamp', 'first_input_sample',
      'end_input_sample', 'map_timestamp', 'map', 'detections', 'tracks']);
    assert.equal(bundle.schema, 'vectorwarp-radar-display-frame-v1');
    assert.equal(bundle.map.data.length, 20);
    const blockPeak = 41;
    const blockMean = blockPeak + 5 * Math.log10([0, 1, 40, 41].reduce((sum, value) =>
      sum + 10 ** ((value - blockPeak) / 5), 0) / 4);
    assert.equal(bundle.map.data[0][0], Math.round(blockMean * 4) / 4,
      'Map reduction preserves RMS map energy without max-noise bias');
    assert.deepEqual(bundle.tracks, nextIlluminatorFrame.tracks);
    assert.deepEqual(bundle.detections, nextIlluminatorFrame.detections);
    for (const timestamp of [12, 13]) await send(config.network.ports.illuminator,
      [Buffer.from(JSON.stringify({...nextIlluminatorFrame, timestamp,
        first_input_sample: timestamp * 1200000, end_input_sample: timestamp * 1200000 + 1199999,
        map: {timestamp, data_omitted: true},
        detections: {timestamp, delay: [timestamp]}, tracks: {timestamp, data: []}}))]);
    const retainedBundle = JSON.parse((await request(apiPort, bundleRoute)).body);
    assert.deepEqual([retainedBundle.timestamp, retainedBundle.first_input_sample,
      retainedBundle.end_input_sample, retainedBundle.map_timestamp],
    [11, 1200000, 2399999, 11], 'retained heatmap owns the complete display identity');
    assert.deepEqual(retainedBundle.detections, nextIlluminatorFrame.detections);
    assert.deepEqual(retainedBundle.tracks, nextIlluminatorFrame.tracks,
      'metadata-only CPIs cannot repaint stale maps with current overlays');
    const mapRoute = '/api/map?illuminator=fm-90.5&max_rows=32&max_cols=32';
    const plainMap = await request(apiPort, mapRoute);
    assert.deepEqual(JSON.parse(plainMap.body), bundle.map);
    const stashRoute = '/stash/map?max_rows=32&max_cols=32';
    const plainStash = await request(apiPort, stashRoute);
    for (const [route, plain] of [[bundleRoute, plainBundle], [mapRoute, plainMap],
      [stashRoute, plainStash]]) {
      const encoded = await request(apiPort, route, {'Accept-Encoding': 'gzip'});
      assert.equal(encoded.status, plain.status);
      assert.equal(encoded.headers['content-encoding'], 'gzip');
      assert.match(encoded.headers.vary, /Accept-Encoding/);
      assert.equal(encoded.body, plain.body, 'Compression preserves exact public payload bytes');
    }
    assert.equal((await request(apiPort, '/api/radar/frame')).status, 400);
    assert.equal((await request(apiPort, '/api/radar/frame?illuminator=unknown')).status, 404);
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
    const alignedStatus = JSON.parse((await request(apiPort, '/api/presentation/status')).body);
    assert.equal(alignedStatus.aligned_available, true);
    await new Promise(resolve => { child.once('close', resolve); child.kill('SIGTERM'); });
    // Capture policy independently rejects malformed CPI during startup.
    // Inject invalid input only at the presentation boundary to exercise its
    // HTTP refusal without weakening that earlier production guard.
    const preload = path.join(directory, 'invalid-presentation.cjs');
    fs.writeFileSync(preload, `
      const history = require(${JSON.stringify(path.join(__dirname, 'presentation-history.js'))});
      const configured = history.configuredPresentationDelay;
      history.configuredPresentationDelay = config => configured({...config,
        process: {...config.process, data: {...config.process.data, cpi: NaN}}});
    `);
    child = spawn(process.execPath, ['--require', preload, path.join(__dirname, 'server.js'), configPath], {
      env: apiEnvironment, stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', chunk => { stderr += chunk; });
    await waitFor(apiPort, '/api/presentation/status', response => response.status === 200);
    const unavailable = JSON.parse((await request(apiPort, '/api/presentation/status')).body);
    assert.equal(unavailable.aligned_available, false);
    assert.equal(unavailable.presentation_delay_ms, null);
    assert.equal(unavailable.selected_frame_timestamp, null);
    for (const route of ['/api/map', '/api/tracker', '/api/bearing', '/api/locations',
      '/api/timing', '/api/adsb/delay-doppler']) {
      const response = await request(apiPort, `${route}?presentation=aligned&frame_timestamp=12`);
      assert.equal(response.status, 503, `${route} must refuse alignment with invalid CPI`);
      assert.equal(JSON.parse(response.body).available, false);
    }
    assert.equal((await request(apiPort, '/api/map')).status, 200,
      'Unaligned presentation remains available while settings need repair');
    await new Promise(resolve => { child.once('close', resolve); child.kill('SIGTERM'); });
    config.process.multi_illuminator.enabled = false;
    fs.writeFileSync(configPath, yaml.dump(config));
    child = spawn(process.execPath, [path.join(__dirname, 'server.js'), configPath], {
      env: apiEnvironment, stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', chunk => { stderr += chunk; });
    const primaryCatalog = JSON.parse((await waitFor(apiPort, '/api/illuminators/live',
      response => response.status === 200)).body);
    assert.equal(primaryCatalog.enabled, true);
    assert.equal(primaryCatalog.primary, 'primary');
    assert.equal(primaryCatalog.illuminators.length, 1);
    assert.equal(primaryCatalog.illuminators[0].api_illuminator, false);
    assert.equal(primaryCatalog.illuminators[0].frequency_hz, config.capture.fc);
    assert.equal(primaryCatalog.illuminators[0].available, false);
    const primaryFrame = {timestamp: Date.now(), n: 0, nTentative: 0, nAssociated: 0,
      nActive: 0, nConfirmed: 0, nCoasting: 0, data: []};
    await send(config.network.ports.track, [Buffer.from(JSON.stringify(primaryFrame))]);
    const livePrimary = JSON.parse((await waitFor(apiPort, '/api/illuminators/live', response => {
      try { return JSON.parse(response.body).illuminators[0].available === true; }
      catch (_) { return false; }
    })).body);
    assert.equal(livePrimary.illuminators[0].timestamp, primaryFrame.timestamp);
    assert.equal(livePrimary.illuminators[0].output_sample_rate_hz, config.capture.fs);
    await new Promise(resolve => { child.once('close', resolve); child.kill('SIGTERM'); });
    // A coordinator may retain its former radar configuration, but cannot
    // resume local evidence ownership or read its obsolete state files.
    const coordinatorEvidence = path.join(directory, 'coordinator-calibration');
    const coordinatorRegistry = path.join(directory, 'coordinator-nodes.json');
    fs.writeFileSync(coordinatorRegistry, JSON.stringify({nodes: []}), {mode: 0o600});
    fs.mkdirSync(coordinatorEvidence);
    const staleStatistics = path.join(coordinatorEvidence, 'statistics.json');
    const staleBytes = '{"obsolete_local_radar_state":true}\n';
    fs.writeFileSync(staleStatistics, staleBytes);
    const evidenceGuard = path.join(directory, 'coordinator-evidence-guard.cjs');
    fs.writeFileSync(evidenceGuard, `
      const fs = require('fs');
      const root = ${JSON.stringify(coordinatorEvidence)};
      for (const method of ['statSync', 'readFileSync', 'writeFileSync', 'mkdirSync', 'renameSync']) {
        const original = fs[method];
        fs[method] = function (file, ...args) {
          if (String(file).startsWith(root)) {
            process.stderr.write('COORDINATOR_LOCAL_EVIDENCE_ACCESS\\n');
            throw Object.assign(new Error('Coordinator cannot access radar evidence'), {code: 'EACCES'});
          }
          return original.call(this, file, ...args);
        };
      }
    `);
    child = spawn(process.execPath, ['--require', evidenceGuard,
      path.join(__dirname, 'server.js'), configPath], {
      env: {...apiEnvironment, VECTORWARP_NODE_ROLE: 'coordinator',
        VECTORWARP_NODE_REGISTRY: coordinatorRegistry,
        BLAH2_STATISTICS_PATH: staleStatistics,
        BLAH2_PROVISIONAL_STATISTICS_PATH: path.join(coordinatorEvidence, 'provisional.json'),
        BLAH2_OPRS_EVENTS_PATH: path.join(coordinatorEvidence, 'oprs.ndjson')},
      stdio: ['ignore', 'ignore', 'pipe']});
    child.stderr.on('data', chunk => { stderr += chunk; });
    await waitFor(apiPort, '/api/statistics', response => response.status === 200);
    const coordinatorStatus = JSON.parse((await request(apiPort,
      '/api/system/status?summary=1')).body);
    assert.equal(coordinatorStatus.node_role, 'coordinator');
    assert.equal(coordinatorStatus.evidenceHealth.status, 'healthy');
    const network = JSON.parse((await request(apiPort, '/api/statistics')).body);
    assert.equal(network.schema, 'vectorwarp-network-statistics-v1');
    assert.equal(network.health.nodes_total, 0);
    assert.equal(network.health.status, 'no-radar-nodes');
    assert.deepEqual(JSON.parse((await request(apiPort,
      '/api/federation/statistics-summary')).body), network,
    'Coordinator statistics must use the canonical federation cache');
    for (const route of ['/api/association/passes', '/api/live-scorecard',
      '/api/live-scorecard/provisional', '/api/live-scorecard/association-basis',
      '/api/live-aircraft-associations', '/api/suspicious-track-ledger', '/api/radar/coverage'])
      assert.equal((await request(apiPort, route)).status, 409,
        `${route} cannot expose obsolete coordinator-local radar evidence`);
    for (const name of ['map', 'detection', 'track', 'timing', 'illuminator'])
      await assert.rejects(connect(config.network.ports[name]), /ECONNREFUSED/,
        'A coordinator cannot ingest local radar evidence');
    await new Promise(resolve => { child.once('close', resolve); child.kill('SIGTERM'); });
    assert.doesNotMatch(stderr, /COORDINATOR_LOCAL_EVIDENCE_ACCESS/);
    assert.equal(fs.readFileSync(staleStatistics, 'utf8'), staleBytes);
    assert.deepEqual(fs.readdirSync(coordinatorEvidence), ['statistics.json']);
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
