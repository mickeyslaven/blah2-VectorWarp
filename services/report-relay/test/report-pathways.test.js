import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import {githubIssue, normalizeReport, reportFingerprint} from '../src/index.js';

const receiverTypes = ['RspDuo', 'Kraken', 'Usrp', 'HackRF', 'Replay', 'Unknown'];

test('live pathway smoke uses the current consent and submission schema', () => {
  const source = fs.readFileSync(new URL('../script/live-pathway-smoke.mjs', import.meta.url), 'utf8');
  assert.match(source, /consent:\s*true/);
  assert.match(source, /submissionId:\s*crypto\.randomUUID\(\)/);
});

function baseReport(pathway) {
  return {
    schema: 1,
    consent: true,
    submissionId: '123e4567-e89b-42d3-a456-426614174000',
    incident: {
      code: pathway.code,
      component: pathway.component,
      stage: pathway.stage,
      severity: pathway.severity || 'error',
      message: `Synthetic ${pathway.code} diagnostic`,
      firstSeenAt: '2026-10-24T20:00:00Z',
      lastSeenAt: '2026-10-24T20:00:02Z',
      occurrences: 2,
      recovered: false
    },
    build: {
      version: '0.1.30-test',
      commit: '0123456789abcdef0123456789abcdef01234567',
      package: 'test'
    },
    system: {os: 'Linux', architecture: 'arm64', kernel: '6.12-test'},
    receiver: {
      type: pathway.receiverType || 'RspDuo',
      model: pathway.model || 'synthetic-device',
      driverVersion: '3.15-test',
      apiVersion: '3.15-test',
      firmwareVersion: 'test',
      sampleRateHz: 2000000,
      centerFrequencyHz: 100000000,
      channelCount: 2,
      transport: 'usb',
      startupStages: pathway.startupStages || {enumerate: true, open: true, init: true},
      health: pathway.health || {streamEstablished: true, callbackFault: false}
    },
    runtime: {
      processorState: pathway.processorState || 'running',
      restartState: pathway.restartState || 'idle',
      backend: pathway.backend || 'cpu',
      configRevision: 'sha256:c0ffee',
      uptimeSeconds: 120,
      details: pathway.details
    },
    recentEvents: [{
      code: 'SERVICE_DEGRADED', component: 'status', stage: 'observe',
      severity: 'degraded', message: 'Synthetic preceding event',
      firstSeenAt: '2026-10-24T19:59:58Z', lastSeenAt: '2026-10-24T19:59:59Z',
      occurrences: 1, recovered: true
    }], features: {recording: {requested: false, paused: false, active: false,
      errorPresent: false, recordedSamples: 0, clipRevision: 1, clipErrorPresent: false,
      clipSamples: 0, timelineCaptureAvailable: true, automaticCleanup: false,
      lowSpacePolicy: 'stop_recording', maximumAgeDays: 30, maximumTotalGb: 100, minimumFreeGb: 5},
    replay: {active: pathway.receiverType === 'Replay', paused: false, seekable: pathway.receiverType === 'Replay',
      positionSamples: 0, totalSamples: 0, sampleRateHz: 2000000, timelineAvailable: false},
    adsb: {enabled: true, online: true, degraded: false, sourceCount: 2, healthySourceCount: 2,
      aircraftCount: 10, directAircraftCount: 9, mlatAircraftCount: 1,
      associationLagCpis: 4, associationLagMs: 2000},
    processing: {acceleration: pathway.backend || 'cpu', clutterAcceleration: pathway.backend || 'cpu',
      configuredCpiMs: 500},
    presentation: {timestampConnections: 1, associationLagCpis: 4, associationLagMs: 2000}},
    comment: 'Automated pathway contract verification; no user data.'
  };
}

const pathways = [
  {
    code: 'SDR_ENUMERATION_FAILED', component: 'receiver', stage: 'enumerate',
    receiverType: 'RspDuo', startupStages: {enumerate: false, open: false, init: false},
    health: {streamEstablished: false, callbackFault: false},
    details: {deviceCount: 0, permissionDenied: true, driverLoaded: true}
  },
  {
    code: 'SDR_OPEN_FAILED', component: 'receiver', stage: 'open', receiverType: 'Kraken',
    startupStages: {enumerate: true, open: false, init: false},
    health: {streamEstablished: false, callbackFault: false},
    details: {driverErrorCode: 'DEVICE_BUSY', candidateCount: 1}
  },
  {
    code: 'SDR_STREAM_LOST', component: 'receiver', stage: 'stream', receiverType: 'Usrp',
    health: {streamEstablished: false, callbackFault: true},
    details: {framesReceived: 2048, lastFrameAgeMs: 8500, droppedFrames: 32}
  },
  {
    code: 'RECEIVER_SYNC_FAILED', component: 'receiver', stage: 'sync', receiverType: 'Kraken',
    health: {streamEstablished: false, callbackFault: false},
    details: {syncState: 'failed', rollbackAttempted: true, rollbackSucceeded: false,
      reconciliationRequired: true}
  },
  {
    code: 'GAIN_MONITOR_FAILED', component: 'receiver', stage: 'gain-monitor',
    receiverType: 'HackRF', severity: 'degraded',
    details: {probeTimedOut: true, attemptCount: 3, overloadKnown: false}
  },
  {
    code: 'PROCESSOR_START_FAILED', component: 'processor', stage: 'start',
    receiverType: 'RspDuo', processorState: 'error', restartState: 'failed',
    details: {exitStatus: 1, terminationSignal: 'none', receiverReady: true}
  },
  {
    code: 'PROCESSOR_RUNTIME_FAILED', component: 'processor', stage: 'runtime',
    receiverType: 'Replay', processorState: 'error',
    details: {framesProcessed: 4096, lastFrameAgeMs: 3000, queueDepth: 8}
  },
  {
    code: 'GPU_BACKEND_FAILED', component: 'processor', stage: 'acceleration',
    receiverType: 'RspDuo', processorState: 'degraded', backend: 'vulkan',
    details: {driverState: 'failed', fallbackAttempted: true, fallbackSucceeded: false}
  },
  {
    code: 'RESTART_FAILED', component: 'lifecycle', stage: 'restart',
    receiverType: 'RspDuo', processorState: 'stopped', restartState: 'failed',
    details: {helperAvailable: true, receiptState: 'failed', timedOut: true, exitStatus: 1}
  },
  {
    code: 'CONFIG_PERSISTENCE_FAILED', component: 'configuration', stage: 'persist',
    receiverType: 'RspDuo', processorState: 'running', restartState: 'not-requested',
    details: {writeStage: 'rename', revisionConflict: false, rollbackAttempted: true,
      rollbackSucceeded: true}
  },
  {
    code: 'API_UNAVAILABLE', component: 'api', stage: 'connect', receiverType: 'Unknown',
    severity: 'degraded', processorState: 'unknown',
    details: {routeName: 'status', statusCode: 503, timeoutMs: 1800, consecutiveFailures: 4}
  },
  {
    code: 'ADSB_SOURCE_FAILED', component: 'adsb', stage: 'connect', receiverType: 'RspDuo',
    severity: 'degraded',
    details: {sourceCount: 2, healthySourceCount: 0, consecutiveFailures: 5,
      errorClass: 'timeout'}
  }
];

test('every classified pathway preserves its bug-fixing evidence in the issue', async t => {
  for (const pathway of pathways) await t.test(pathway.code, async () => {
    const report = normalizeReport(baseReport(pathway));
    const issue = githubIssue(report, await reportFingerprint(report), 'test-report-id', 'bug,vectorwarp-report');
    const embedded = JSON.parse(issue.body.match(/```json\n([\s\S]+)\n```/)[1]);

    assert.equal(embedded.incident.code, pathway.code);
    assert.equal(embedded.incident.component, pathway.component);
    assert.equal(embedded.incident.stage, pathway.stage);
    assert.equal(embedded.build.version, '0.1.30-test');
    assert.equal(embedded.build.commit, '0123456789abcdef0123456789abcdef01234567');
    assert.equal(embedded.system.os, 'Linux');
    assert.equal(embedded.system.architecture, 'arm64');
    assert.equal(embedded.receiver.type, pathway.receiverType);
    assert.equal(embedded.receiver.sampleRateHz, 2000000);
    assert.equal(embedded.receiver.centerFrequencyHz, 100000000);
    assert.equal(embedded.runtime.configRevision, 'sha256:c0ffee');
    assert.equal(embedded.recentEvents.length, 1);
    assert.deepEqual(embedded.runtime.details, pathway.details);
    assert.deepEqual(issue.labels, ['bug', 'vectorwarp-report']);
  });
});

test('every supported receiver family can be represented without a hardware identifier', () => {
  for (const receiverType of receiverTypes) {
    const report = normalizeReport(baseReport({...pathways[1], receiverType}));
    assert.equal(report.receiver.type, receiverType);
    assert.equal(Object.hasOwn(report.receiver, 'serial'), false);
  }
});

test('user comments are sanitized while allowlisted diagnostic scalars survive', async () => {
  const value = baseReport(pathways[2]);
  value.incident.message = 'Authorization: Basic dXNlcjpwYXNz password=“correct horse battery staple”';
  value.comment = '["safe","SECOND_SECRET"] {\\"icao24\\":\\"abc123\\",\\"call_sign\\":\\"SECRET7\\"}';
  const report = normalizeReport(value);
  const issue = githubIssue(report, await reportFingerprint(report), 'test-report-id');

  assert.ok(report.comment);
  assert.equal(report.incident.message,
    'VectorWarp detected SDR_STREAM_LOST during receiver stream.');
  assert.doesNotMatch(issue.body,
    /dXNlcjpwYXNz|correct horse|battery staple|SECOND_SECRET|abc123|SECRET7/);
  assert.match(issue.body, /"framesReceived": 2048/);
});
