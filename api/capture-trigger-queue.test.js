'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {CaptureTriggerQueue, optionsFromEnvironment} = require('./capture-trigger-queue');
const {EvidenceStatistics} = require('./evidence-statistics');

const epoch = 'a'.repeat(64);
const event = (number, observedAt) => ({id: `eligibility:event-${number}`,
  pass_id: `pass-${number}`, state: 'pending', observed_at: observedAt,
  predicted: {delay_km: 10, doppler_hz: 100},
  receiver_epoch: epoch, config_epoch: epoch, site_epoch: epoch});

assert.throws(() => new CaptureTriggerQueue({preSeconds: 6, postSeconds: 5}),
  /maximum clip duration/);
assert.deepEqual(optionsFromEnvironment({}),
  {preSeconds: 2, postSeconds: 3, maxClipSeconds: 10,
    periodicIntervalSeconds: 0, periodicMaxSnapshots: 0});
assert.deepEqual(optionsFromEnvironment({BLAH2_CAPTURE_TRIGGER_PRE_SECONDS: '2',
  BLAH2_CAPTURE_TRIGGER_POST_SECONDS: '8'}),
{preSeconds: 2, postSeconds: 8, maxClipSeconds: 10,
  periodicIntervalSeconds: 0, periodicMaxSnapshots: 0});
assert.deepEqual(optionsFromEnvironment({BLAH2_CAPTURE_TRIGGER_PERIODIC_INTERVAL_SECONDS: '10',
  BLAH2_CAPTURE_TRIGGER_PERIODIC_MAX_SNAPSHOTS: '3'}),
{preSeconds: 2, postSeconds: 3, maxClipSeconds: 10,
  periodicIntervalSeconds: 10, periodicMaxSnapshots: 3});
assert.deepEqual(optionsFromEnvironment({}, {capture: {periodic_snapshots:
  {interval_seconds: 15, max_snapshots: 2}}}),
{preSeconds: 2, postSeconds: 3, maxClipSeconds: 10,
  periodicIntervalSeconds: 15, periodicMaxSnapshots: 2});
assert.throws(() => optionsFromEnvironment({BLAH2_CAPTURE_TRIGGER_PERIODIC_INTERVAL_SECONDS: '9',
  BLAH2_CAPTURE_TRIGGER_PERIODIC_MAX_SNAPSHOTS: '1'}), /at least 10/);
assert.throws(() => optionsFromEnvironment({BLAH2_CAPTURE_TRIGGER_PERIODIC_INTERVAL_SECONDS: '10',
  BLAH2_CAPTURE_TRIGGER_PERIODIC_MAX_SNAPSHOTS: '4'}), /no greater than 3/);
assert.throws(() => optionsFromEnvironment({BLAH2_CAPTURE_TRIGGER_PRE_SECONDS: '2',
  BLAH2_CAPTURE_TRIGGER_POST_SECONDS: '9'}), /ten-second clip limit/);
assert.throws(() => optionsFromEnvironment({BLAH2_CAPTURE_TRIGGER_POST_SECONDS: 'nope'}),
  /non-negative number/);
const queue = new CaptureTriggerQueue({now: () => 5000});
assert.equal(queue.acceptEligibilityEvent({...event(0, 90000), id: 'detection:0'}).accepted, false,
  'Detector-shaped input is not capture admission');
let accepted = queue.acceptEligibilityEvent(event(1, 100000));
assert.equal(accepted.accepted, true);
assert.equal(accepted.clip.clipRevision, 1);
accepted = queue.acceptEligibilityEvent(event(2, 102000));
assert.equal(accepted.coalesced, true, 'Overlapping opportunities share one clip request');
assert.equal(accepted.clip.clipRevision, 2, 'An extended request gets a newer revision');
assert.deepEqual(queue.request().eventIds, ['eligibility:event-1', 'eligibility:event-2']);
assert.deepEqual(queue.request().passIds, ['pass-1', 'pass-2']);
assert.equal(queue.request().preSeconds, 2);
assert.equal(queue.request().postSeconds, 5);
assert.deepEqual(queue.status().recentEvents.map(item => item.clipRevisions), [[2], [2]]);
assert.equal(queue.acceptEligibilityEvent(event(1, 100000)).reason, 'duplicate-event');
assert.equal(queue.request().clipRevision, 2, 'A duplicate event cannot create another revision');

const expanded = new CaptureTriggerQueue({preSeconds: 2, postSeconds: 3});
expanded.acceptEligibilityEvent(event(80, 200000));
expanded.acceptEligibilityEvent(event(81, 198000));
const expandedInitial = expanded.status().recentEvents.find(item => item.eventId === 'eligibility:event-80')
  .captureWindows[0];
assert.deepEqual({revision: expandedInitial.revision, requestedEventTime: expandedInitial.requestedEventTime,
  windowStart: expandedInitial.windowStart, windowEnd: expandedInitial.windowEnd},
{revision: 2, requestedEventTime: 198000, windowStart: 196000, windowEnd: 203000},
  'Coalescing an earlier event rewrites the existing child window to the physical clip bounds');

accepted = queue.acceptEligibilityEvent(event(3, 300000));
assert.equal(accepted.coalesced, false, 'A disjoint opportunity rolls to a queued clip');
assert.equal(accepted.clip.clipRevision, 3);
assert.equal(queue.status().queuedClips, 2);
assert.equal(queue.acknowledge(2, null,
  {file: '/clips/clip-2.mchq', samples: 12000000}), true);
assert.deepEqual(queue.status().recentEvents.slice(0, 2).map(item => item.state),
  ['acknowledged', 'acknowledged']);
assert.deepEqual(queue.eventsForRevision(2).map(item => item.eventId),
  ['eligibility:event-1', 'eligibility:event-2']);
assert.equal(queue.eventsForRevision(2)[0].file, '/clips/clip-2.mchq');
assert.equal(queue.eventsForRevision(2)[0].samples, 12000000);
assert.equal(queue.request().clipRevision, 3, 'Acknowledgement advances to the next clip');
assert.equal(queue.acknowledge(3, 'Disk full\nclip stopped'), true);
assert.equal(queue.request().pending, false);
assert.equal(queue.request().acknowledged, false);
assert.equal(queue.request().failure, 'Disk full clip stopped');
assert.equal(queue.acknowledge(3), false, 'A terminal clip cannot be acknowledged twice');

let retryClock = 5000;
const startup = new CaptureTriggerQueue({now: () => retryClock});
startup.acceptEligibilityEvent(event(4, 4000));
assert.equal(startup.acknowledge(1, 'Kraken clip pre-roll is unavailable'), true);
retryClock = 7100;
const retry = startup.retryPreRollFailure(1);
assert.equal(retry.clipRevision, 2);
assert.equal(retry.requestedEventTime, 7100,
  'A startup retry records a fresh window after the raw ring has warmed');
assert.deepEqual(startup.status().recentEvents[0].clipRevisions, [1, 2]);
assert.equal(startup.retryPreRollFailure(1), null, 'A startup event is retried only once');
assert.equal(startup.acknowledge(2, null,
  {file: '/clips/startup-retry.mchq', samples: 12000000}), true);
assert.equal(startup.status().recentEvents[0].state, 'acknowledged');

let orderedRetryClock = 5000;
const orderedRetry = new CaptureTriggerQueue({preSeconds: 0, postSeconds: 1,
  now: () => orderedRetryClock});
orderedRetry.acceptEligibilityEvent(event(5, 4000));
orderedRetry.acceptEligibilityEvent(event(6, 10000));
orderedRetry.acceptEligibilityEvent(event(7, 20000));
assert.equal(orderedRetry.acknowledge(1, 'Kraken clip pre-roll is unavailable'), true);
orderedRetryClock = 7100;
assert.equal(orderedRetry.retryPreRollFailure(1).clipRevision, 4);
assert.deepEqual(orderedRetry.clips.filter(clip => clip.pending).map(clip => clip.revision),
  [4, 5, 6], 'A startup retry keeps every later recorder request monotonic');
assert.deepEqual(orderedRetry.status().recentEvents.map(item => item.clipRevisions),
  [[1, 4], [5], [6]]);
assert.equal(orderedRetry.acknowledge(4), true);
assert.equal(orderedRetry.request().clipRevision, 5);
assert.equal(orderedRetry.acknowledge(5), true);
assert.equal(orderedRetry.request().clipRevision, 6);
assert.equal(orderedRetry.acknowledge(6), true);

const bounded = new CaptureTriggerQueue({preSeconds: 0, postSeconds: 1, maxClips: 2});
for (let index = 0; index < 2; ++index)
  assert.equal(bounded.acceptEligibilityEvent(event(index + 10, index * 10000)).accepted, true);
const dropped = bounded.acceptEligibilityEvent(event(12, 20000));
assert.equal(dropped.accepted, true); assert.equal(dropped.dropped, true);
assert.equal(dropped.reason, 'queue-capacity');
assert.equal(bounded.clips.length, 2);
assert.equal(bounded.status().droppedEvents, 1);
assert.deepEqual(bounded.clips.map(clip => clip.revision), [1, 2]);
assert.deepEqual({...bounded.status().recentEvents.at(-1), captureWindows: undefined},
  {eventId: 'eligibility:event-12', passId: 'pass-12', requestedEventTime: 20000,
    state: 'dropped', clipRevisions: [], windowStart: null, windowEnd: null,
    failure: 'queue-capacity', captureWindows: undefined});

const capped = new CaptureTriggerQueue();
capped.acceptEligibilityEvent(event(30, 100000));
capped.acceptEligibilityEvent(event(31, 104000));
const rolled = capped.acceptEligibilityEvent(event(32, 108000));
assert.equal(rolled.coalesced, false,
  'An overlap that would exceed ten seconds rolls into a new complete window');
assert.equal(capped.clips.length, 2);
for (const clip of capped.clips)
  assert.ok(clip.windowEnd - clip.windowStart <= 10000, 'Physical clip duration is hard-capped');
assert.deepEqual(capped.status().recentEvents.at(-1).clipRevisions, [rolled.clip.clipRevision]);

const monotonic = new CaptureTriggerQueue({preSeconds: 0, postSeconds: 1});
monotonic.acceptEligibilityEvent(event(20, 1000));
monotonic.acceptEligibilityEvent(event(21, 10000));
assert.equal(monotonic.acceptEligibilityEvent(event(22, 1500)).clip.clipRevision, 3);
assert.deepEqual(monotonic.clips.map(clip => clip.revision), [3, 4],
  'Extending an earlier clip must not expose a lower revision after roll');
assert.equal(monotonic.acknowledge(3), true);
assert.equal(monotonic.request().clipRevision, 4);

const periodic = new CaptureTriggerQueue({preSeconds: 2, postSeconds: 8,
  periodicIntervalSeconds: 10, periodicMaxSnapshots: 2, now: () => 0});
const parent = event(40, 100000);
assert.equal(periodic.acceptEligibilityEvent(parent).accepted, true);
assert.equal(periodic.acceptPeriodicSnapshot(parent, 109999).reason, 'periodic-interval');
const snapshot1 = periodic.acceptPeriodicSnapshot(parent, 110000);
assert.equal(snapshot1.accepted, true);
assert.equal(snapshot1.clip.clipRevision, 2);
const snapshot2 = periodic.acceptPeriodicSnapshot(parent, 120000);
assert.equal(snapshot2.accepted, true);
assert.equal(periodic.acceptPeriodicSnapshot(parent, 130000).reason, 'periodic-cap');
const periodicRecord = periodic.status().recentEvents[0];
assert.equal(periodicRecord.captureWindows.length, 3,
  'One parent eligibility event owns initial plus bounded child snapshots');
assert.deepEqual(periodicRecord.captureWindows.map(item => item.kind), ['initial', 'periodic', 'periodic']);
assert.equal(new Set(periodicRecord.captureWindows.map(item => item.captureId)).size, 3,
  'Every capture window has a unique child capture ID');
assert.equal(periodic.acknowledge(1, null, {file: '/clips/initial.mchq', samples: 1}), true);
assert.equal(periodic.acknowledge(2, null, {file: '/clips/periodic-1.mchq', samples: 2}), true);
assert.equal(periodic.acknowledge(3, 'Disk full'), true);
assert.deepEqual(periodic.status().recentEvents[0].captureWindows.map(item => item.state),
  ['acknowledged', 'acknowledged', 'failed']);

const sharedPeriodic = new CaptureTriggerQueue({preSeconds: 2, postSeconds: 8,
  periodicIntervalSeconds: 10, periodicMaxSnapshots: 2});
const periodicParentA = event(50, 0), periodicParentB = event(51, 1000);
sharedPeriodic.acceptEligibilityEvent(periodicParentA);
sharedPeriodic.acceptEligibilityEvent(periodicParentB);
const periodicA = sharedPeriodic.acceptPeriodicSnapshot(periodicParentA, 11000);
const periodicB = sharedPeriodic.acceptPeriodicSnapshot(periodicParentB, 11000);
assert.equal(periodicB.coalesced, true,
  'Overlapping periodic requests for distinct parents reuse one physical clip');
const sharedRecords = sharedPeriodic.status().recentEvents;
const sharedWindows = sharedRecords.map(record => record.captureWindows.find(item => item.kind === 'periodic'));
assert.equal(sharedWindows[0].revision, sharedWindows[1].revision);
assert.equal(sharedWindows[0].revision, periodicB.clip.clipRevision,
  'Both parents reference the queue revision after the coalesced request is renumbered');
assert.notEqual(sharedWindows[0].captureId, sharedWindows[1].captureId,
  'Shared physical clips retain separate child capture windows for each parent');

const pressuredPeriodic = new CaptureTriggerQueue({preSeconds: 2, postSeconds: 8, maxClips: 1,
  periodicIntervalSeconds: 10, periodicMaxSnapshots: 2});
const pressuredParent = event(60, 0);
pressuredPeriodic.acceptEligibilityEvent(pressuredParent);
const periodicDrop = pressuredPeriodic.acceptPeriodicSnapshot(pressuredParent, 10000);
assert.equal(periodicDrop.dropped, true);
assert.equal(periodicDrop.window.state, 'dropped');
assert.equal(periodicDrop.window.failure, 'queue-capacity');
assert.equal(pressuredPeriodic.acceptPeriodicSnapshot(pressuredParent, 10001).reason, 'periodic-interval',
  'A capacity drop consumes its interval instead of retrying every sampler tick');
assert.equal(pressuredPeriodic.status().recentEvents[0].captureWindows.filter(item =>
  item.kind === 'periodic').length, 1);

(async () => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-capture-trigger-'));
  let clock = 2_000_000_000_000, sampler;
  const config = {capture: {fs: 2400000, fc: 563000000, device: {type: 'Kraken'}},
    location: {rx: {latitude: 1, longitude: 2}, tx: {latitude: 3, longitude: 4}},
    truth: {adsb: {enabled: true}}, process: {
      ambiguity: {delayMin: -10, delayMax: 500, dopplerMin: -600, dopplerMax: 600},
      clutter: {delayMax: 200}, detection: {minDelay: 5},
      bearing: {manifold_recorder: {max_truth_age_s: 3}}}};
  const triggerQueue = new CaptureTriggerQueue({maxClips: 1});
  let aircraft = {abc123: {delay: 10, doppler: 100,
    timestamp: clock / 1000, flight: 'SILENT', geometry: {bearing_deg_true: 42}}};
  const source = {async get() { return aircraft; }};
  const statistics = new EvidenceStatistics(config, source, {
    filename: path.join(directory, 'statistics.json'), now: () => clock,
    setIntervalFn(callback) { sampler = callback; return {unref() {}}; },
    clearIntervalFn() {},
    onEligibilityEvent: value => triggerQueue.acceptEligibilityEvent(value)});
  sampler();
  while (statistics.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(triggerQueue.request().pending, true,
    'The ADS-B sampler must enqueue capture without any detector frame');
  assert.deepEqual(triggerQueue.request().eventIds,
    [statistics.status().eligibility_events[0].id]);
  assert.equal(statistics.status().eligibility_events[0].capture_state, 'trigger_pending');
  clock += 1000;
  statistics.acceptDetection({timestamp: clock, delay: [10], doppler: [100]});
  while (statistics.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(triggerQueue.request().clipRevision, 1,
    'Repeated opportunities in the same pass must remain once per eligibility event');
  clock += 4000;
  aircraft.abc123.timestamp = clock / 1000;
  statistics.acceptTracker({timestamp: clock,
    data: [{delay: 10 / (299792.458 / 2400000), doppler: 100}]});
  statistics.acceptDetection({timestamp: clock, delay: [10], doppler: [100]});
  while (statistics.running) await new Promise(resolve => setImmediate(resolve));
  clock += 16000;
  aircraft = {def456: {delay: 11, doppler: 110, timestamp: clock / 1000,
    flight: 'DROPPED', geometry: {bearing_deg_true: 84}}};
  sampler();
  while (statistics.running) await new Promise(resolve => setImmediate(resolve));
  const droppedEvent = statistics.status().eligibility_events.at(-1);
  assert.equal(droppedEvent.capture_state, 'dropped');
  assert.equal(droppedEvent.capture_failure, 'queue-capacity');
  assert.equal(triggerQueue.acknowledge(1, null,
    {file: '/clips/eligibility-1.mchq', samples: 12000000}), true);
  statistics.acceptCaptureEvents(triggerQueue.eventsForRevision(1));
  const completedEvent = statistics.status().eligibility_events[0];
  assert.equal(completedEvent.capture_state, 'acknowledged');
  assert.deepEqual(completedEvent.capture_clip_revisions, [1]);
  assert.equal(completedEvent.capture_file, '/clips/eligibility-1.mchq');
  assert.equal(completedEvent.capture_samples, 12000000);
  assert.equal(completedEvent.capture_window_start, 1_999_999_998_000);
  assert.equal(completedEvent.capture_window_end, 2_000_000_003_000);
  assert.equal(completedEvent.first_detection_at, 2_000_000_001_000);
  assert.equal(completedEvent.first_track_at, 2_000_000_005_000);
  assert.equal(completedEvent.capture_detected_frames, 1);
  assert.equal(completedEvent.capture_first_detection_at, 2_000_000_001_000);
  assert.equal(completedEvent.capture_tracked_frames, 0);
  assert.equal(completedEvent.capture_first_track_at, null);
  statistics.close(); fs.rmSync(directory, {recursive: true, force: true});

  const periodicDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-periodic-capture-'));
  let periodicClock = 2_100_000_000_000, periodicSampler;
  const periodicQueue = new CaptureTriggerQueue({periodicIntervalSeconds: 10,
    periodicMaxSnapshots: 2, now: () => periodicClock});
  let periodicAircraft = {abc123: {delay: 10, doppler: 100,
    timestamp: periodicClock / 1000, flight: 'PERIODIC', geometry: {bearing_deg_true: 42}}};
  const periodicStats = new EvidenceStatistics(config, {async get() { return periodicAircraft; }}, {
    filename: path.join(periodicDirectory, 'statistics.json'), now: () => periodicClock,
    setIntervalFn(callback) { periodicSampler = callback; return {unref() {}}; }, clearIntervalFn() {},
    onEligibilityEvent: value => periodicQueue.acceptEligibilityEvent(value),
    onPeriodicCapture: (value, timestamp) => periodicQueue.acceptPeriodicSnapshot(value, timestamp)});
  periodicSampler();
  while (periodicStats.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(periodicQueue.clips.length, 1, 'Initial ADS-B-only capture remains the first window');
  periodicClock += 1000;
  periodicStats.acceptDetection({timestamp: periodicClock, delay: [10], doppler: [100]});
  while (periodicStats.running) await new Promise(resolve => setImmediate(resolve));
  periodicClock += 9000; periodicAircraft.abc123.timestamp = periodicClock / 1000;
  periodicSampler();
  while (periodicStats.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(periodicQueue.clips.length, 2,
    'A physically eligible pass schedules the first periodic child without detector evidence');
  periodicClock += 1000;
  periodicStats.acceptDetection({timestamp: periodicClock, delay: [10], doppler: [100]});
  while (periodicStats.running) await new Promise(resolve => setImmediate(resolve));
  periodicClock += 9000; periodicAircraft.abc123.timestamp = periodicClock / 1000;
  periodicSampler();
  while (periodicStats.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(periodicQueue.clips.length, 3, 'Periodic snapshots respect the configured interval');
  periodicClock += 10000; periodicAircraft.abc123.timestamp = periodicClock / 1000;
  periodicSampler();
  while (periodicStats.running) await new Promise(resolve => setImmediate(resolve));
  assert.equal(periodicQueue.clips.length, 3, 'Periodic snapshots stop at the per-pass cap');
  const parentEvent = periodicStats.status().eligibility_events[0];
  assert.equal(periodicStats.status().eligibility_event_count, 1,
    'Child captures must not inflate eligibility-event denominators');
  assert.equal(parentEvent.capture_windows.length, 3);
  assert.deepEqual(parentEvent.capture_windows.map(item => item.kind), ['initial', 'periodic', 'periodic']);
  for (const clip of [...periodicQueue.clips]) {
    periodicQueue.acknowledge(clip.revision, null,
      {file: `/clips/${clip.revision}.mchq`, samples: clip.revision});
    periodicStats.acceptCaptureEvents(periodicQueue.eventsForRevision(clip.revision));
  }
  const classified = periodicStats.status().eligibility_events[0].capture_windows;
  assert.deepEqual(classified.map(item => item.detected_frames), [1, 1, 0],
    'Later periodic evidence stays on its own capture window rather than the initial clip');
  const legacy = periodicStats.status().eligibility_events[0];
  assert.equal(legacy.capture_state, 'acknowledged');
  assert.equal(legacy.capture_file, '/clips/1.mchq');
  assert.equal(legacy.capture_samples, 1,
    'Legacy capture scalars remain attached to the initial window after periodic acknowledgements');
  periodicStats.close(); fs.rmSync(periodicDirectory, {recursive: true, force: true});
  console.log('Capture trigger queue coalescing, bounds and ADS-B-only admission tests passed.');
})().catch(error => { console.error(error); process.exitCode = 1; });
