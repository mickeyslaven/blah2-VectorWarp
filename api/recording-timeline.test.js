'use strict';
const assert = require('assert'); const fs = require('fs'); const os = require('os'); const path = require('path');
const {RecordingTimeline} = require('./recording-timeline');
(async () => { const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vw-timeline-'));
  const file = path.join(directory, 'clip.mchq'), timeline = new RecordingTimeline(); fs.writeFileSync(file, 'x');
  timeline.add({timestamp_ms: 1000, aircraft: {a:{hex:'A'}}, delay_doppler:{a:{delay:1}}}, 1000);
  timeline.add({timestamp_ms: 1500, aircraft: {b:{hex:'B'}}, delay_doppler:{b:{delay:2}}}, 1500);
  await timeline.write(file, 1000, 2000, {sample_rate_hz: 1000});
  assert.equal(timeline.at(file, 600, 1000, 'aircraft').b.hex, 'B');
  timeline.addIlluminator({timestamp_ms: 1700, illuminator: 'fm-a',
    aircraft: {c:{hex:'C'}}, delay_doppler: {c:{delay:3}},
    map: {timestamp:1700}, detections: {timestamp:1700}, tracks: {timestamp:1700},
    first_input_sample: 10, end_input_sample: 19}, 1700);
  await timeline.write(file, 1000, 2000, {sample_rate_hz: 1000});
  assert.equal(timeline.load(file).schema, 'vectorwarp-recording-timeline-v2');
  assert.equal(timeline.at(file, 800, 1000, 'delay_doppler', 'fm-a').c.delay, 3);
  assert.equal(timeline.at(file, 800, 1000, 'tracks', 'fm-a').timestamp, 1700);
  assert.equal(timeline.at(file, 800, 1000, 'aircraft', 'fm-b'), null,
    'replay truth cannot leak across illuminators');
  const manual = path.join(directory, 'manual.blah2iq'); fs.writeFileSync(manual, 'x');
  timeline.start(manual, 2000, {sample_rate_hz: 1000, receiver: 'RspDuo'});
  timeline.add({timestamp_ms: 2200, aircraft: {c:{hex:'C'}}, delay_doppler:{}}, 2200);
  timeline.add({timestamp_ms: 2700, aircraft: {d:{hex:'D'}}, delay_doppler:{}}, 2700);
  for (const illuminator of ['fm-a', 'fm-b', 'fm-c'])
    timeline.addIlluminator({timestamp_ms: 2800, illuminator,
      aircraft: {[illuminator]: {hex: illuminator}}, map: {illuminator}}, 2800);
  await timeline.stop(manual, 3000);
  assert.equal(timeline.at(manual, 750, 1000, 'aircraft').d.hex, 'D');
  assert.equal(timeline.load(manual).receiver, 'RspDuo');
  assert.equal(timeline.load(manual).schema, 'vectorwarp-recording-timeline-v2');
  assert.deepEqual(timeline.load(manual).frames.filter(frame => frame.timestamp_ms === 2800)
    .map(frame => frame.illuminator), ['fm-a', 'fm-b', 'fm-c'],
  'manual timeline retains every illuminator product at a shared CPI timestamp');
  assert.equal(timeline.at(manual, 850, 1000, 'aircraft', 'fm-c')['fm-c'].hex, 'fm-c');
  const profile = {schema:'vectorwarp-replay-profile-v2', configuration_complete:true,
    capture:{channel_count:5}, process:{data:{cpi:0.5}}, location:{},
    multi_illuminator: {enabled:true, illuminators:[{id:'fm-a', frequency_hz:90100000,
      output_sample_rate_hz:300000, transmitter:{latitude:40.1,longitude:-79.9}}],
      timeline_semantics:'per-illuminator-truth-and-products-v2'}};
  await timeline.writeReplayProfile(manual, profile);
  assert.deepEqual(timeline.loadReplayProfile(manual), profile);
  fs.writeFileSync(`${manual}.replay.json`, '{bad');
  assert.equal(timeline.loadReplayProfile(manual), null);
  fs.rmSync(directory, {recursive:true,force:true}); console.log('Recording ADS-B timeline passed.');
})().catch(error => { console.error(error); process.exitCode=1; });
