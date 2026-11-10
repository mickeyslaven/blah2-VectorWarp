'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {primaryRadarContext, laneConfig} = require('./multi-illuminator-evidence');
const {limits, associationGates} = require('./evidence-statistics');
const {externalTrack} = require('./tracker-publication');
const {OprsEventRecorder} = require('./oprs-event-recorder');
const {DelayDopplerHistory} = require('./adsb-geometry');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-rate-context-'));
const close = (actual, expected) => assert.ok(Math.abs(actual - expected) < 1e-9,
  `${actual} differs from ${expected}`);

(async () => {
  for (const rate of [2400000, 6144000]) {
    const tx = {latitude: 0, longitude: 1, altitude: 0};
    const lane = {id: 'primary', enabled: true, frequency_hz: 527000000,
      output_sample_rate_hz: rate, transmitter: tx};
    const config = {capture: {fs: rate * 10, fc: 539000000},
      location: {rx: {latitude: 0, longitude: 0, altitude: 0},
        tx: {latitude: 1, longitude: 0, altitude: 100}},
      truth: {adsb: {smoothing_window: 3, max_position_age: 10}},
      process: {data: {cpi: .5}, ambiguity: {delayMin: -10, delayMax: 500,
        dopplerMin: -600, dopplerMax: 600}, detection: {minDelay: 5},
      clutter: {delayMax: 400}, multi_illuminator: {enabled: true, illuminators: [
        {...lane, id: 'disabled', enabled: false, output_sample_rate_hz: 1}, lane]}}};
    const point = timestamp => ({now: timestamp, aircraft: [{hex: 'abc123',
      lat: 0, lon: .5 + (timestamp - 100) / 10, alt_geom: 10000, seen_pos: 0}]});
    const source = {getAt: async (kind, timestamp) => {
      assert.equal(kind, 'aircraft', 'primary truth must be recomputed from raw aircraft');
      return point(timestamp);
    }, get: async () => point(101), status: () => ({enabled: true})};
    const primary = primaryRadarContext(config, source);
    assert.equal(primary.illuminator.id, 'primary');
    assert.equal(primary.config.capture.fs, rate);
    assert.equal(primary.config.capture.fc, lane.frequency_hz);
    assert.equal(config.capture.fs, rate * 10, 'capture metadata remains full-passband');
    assert.equal(config.capture.fc, 539000000);

    const span = limits(primary.config);
    close(span.kmPerBin, 299792.458 / rate);
    close(span.delayMaxKm, 500 * 299792.458 / rate);
    close(span.detectionMinDelayKm, 5 * 299792.458 / rate);
    close(span.clutterMaxKm, 400 * 299792.458 / rate);
    const gates = associationGates({...span, matchDelayKm: 0},
      {timestamp: 101}, 101000, 'tracker');
    close(gates.delayKm, 3 * 299792.458 / rate);
    assert.equal(gates.dopplerHz, 8, 'Doppler resolution depends on CPI, not sample rate');

    const event = {track_id: 'track', timestamp: 101000,
      detections: [{delay: 10, doppler: 20}]};
    const track = externalTrack(event, primary.config.capture.fs);
    close(track.delay, 10 * rate / 299792.458);
    const filename = path.join(directory, `${rate}.ndjson`);
    const recorder = new OprsEventRecorder(primary.config, {filename});
    recorder.acceptTracker({timestamp: 101000, data: [track]});
    recorder.close();
    const rows = fs.readFileSync(filename, 'utf8').trim().split('\n').map(JSON.parse);
    assert.equal(rows[0].sample_rate_hz, rate);
    close(rows.find(row => row.kind === 'track_measurement').delay_km, 10);

    const oracle = new DelayDopplerHistory(laneConfig(config, lane));
    oracle.update(point(100));
    const expected = oracle.update(point(101)).data.abc123;
    await primary.source.getAt('delayDoppler', 100);
    const actual = (await primary.source.getAt('delayDoppler', 101)).abc123;
    close(actual.delay, expected.delay);
    close(actual.doppler, expected.doppler);
    assert.equal(actual.frequency_hz, lane.frequency_hz);
    const wrongCarrier = new DelayDopplerHistory({...primary.config,
      capture: {...primary.config.capture, fc: config.capture.fc}});
    wrongCarrier.update(point(100));
    const wrong = wrongCarrier.update(point(101)).data.abc123;
    close(actual.doppler / wrong.doppler, lane.frequency_hz / config.capture.fc);
    close(actual.delay, wrong.delay);

    const single = {...config, process: {...config.process,
      multi_illuminator: {...config.process.multi_illuminator, enabled: false}}};
    const unchanged = primaryRadarContext(single, source);
    assert.equal(unchanged.config, single);
    assert.equal(unchanged.source, source);
    assert.equal(unchanged.illuminator, null);
  }
  console.log('2.4/6.144 MS/s lane units, primary consumers, truth carrier and capture metadata passed.');
})().catch(error => { console.error(error); process.exitCode = 1; })
  .finally(() => fs.rmSync(directory, {recursive: true, force: true}));
