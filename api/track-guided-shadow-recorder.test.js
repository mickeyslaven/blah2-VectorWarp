'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {TrackGuidedShadowRecorder} = require('./track-guided-shadow-recorder');

const waitFor = async predicate => {
  for (let attempt = 0; attempt < 100; ++attempt) {
    if (predicate()) return;
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  throw new Error('Timed out waiting for guided shadow recorder');
};

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-guided-'));
  const filename = path.join(directory, 'evidence.jsonl');
  const config = {capture: {fc: 563000000}, save: {path: `${directory}/`},
    process: {tracker: {track_guided_shadow: {enable: true, max_tracks: 64,
      evidence_path: filename, max_truth_age_s: 3, max_file_mb: 1, max_files: 2}}},
    truth: {adsb: {enabled: true, delay_tolerance: 2, doppler_tolerance: 5}}};
  const source = {get: async kind => kind === 'aircraft' ?
    {aircraft: [{hex: 'abc123', flight: 'TEST', lat: 40, lon: -80}]} :
    {abc123: {timestamp: 100, flight: 'TEST', delay: 20, doppler: -40}}};
  const recorder = new TrackGuidedShadowRecorder(config, source, {now: () => 100500});
  recorder.accept({timestamp: 100000, track_guided_shadow: [{track_id: '001A',
    state: 'TENTATIVE', maturity: 'tentative', delay_bin: 160, delay_km: 20.5,
    doppler_hz: -38}]});
  await waitFor(() => recorder.status().counters.matched === 1 &&
    recorder.status().lastWriteAt !== null);
  const lines = fs.readFileSync(filename, 'utf8').trim().split('\n').map(JSON.parse);
  assert.equal(lines.length, 1);
  assert.equal(lines[0].classification, 'matched');
  assert.equal(lines[0].adsb.hex, 'abc123');
  assert.equal(lines[0].track_id, '001A');
  assert.equal(recorder.status().counters.matched, 1);
  assert.equal(recorder.status().counters.unmatched, 0);

  recorder.accept({timestamp: 100000, track_guided_shadow: [{track_id: '001B',
    state: 'ACTIVE', maturity: 'established', delay_bin: 400, delay_km: 50,
    doppler_hz: 100}]});
  await waitFor(() => recorder.status().counters.unmatched === 1 &&
    recorder.status().recent.length === 2);
  assert.equal(recorder.status().counters.unmatched, 1);
  assert.equal(recorder.status().recent.at(-1).adsb, null);

  const disabled = new TrackGuidedShadowRecorder({save: {path: `${directory}/`},
    process: {tracker: {track_guided_shadow: {enable: true}}}, truth: {adsb: {enabled: false}}},
  {get: async () => assert.fail('disabled recorder queried truth')});
  disabled.accept({timestamp: 1, track_guided_shadow: [{bad: true}]});
  assert.equal(disabled.status().counters.frames, 0);
  fs.rmSync(directory, {recursive: true, force: true});
  console.log('Track-guided shadow ADS-B evidence: matched, unmatched, durable JSONL and disabled isolation passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
