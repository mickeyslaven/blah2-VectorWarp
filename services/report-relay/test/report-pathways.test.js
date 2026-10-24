import assert from 'node:assert/strict';
import test from 'node:test';
import {githubIssue, normalizeReport, reportFingerprint} from '../src/index.js';

const receiverTypes = ['RspDuo', 'Kraken', 'Usrp', 'HackRF', 'Replay', 'Unknown'];

function baseReport(pathway) {
  return {
    schema: 1,
    consent: true,
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
      configRevision: 'sha256:synthetic-revision',
      uptimeSeconds: 120,
      details: pathway.details
    },
    recentEvents: [{
      code: 'SERVICE_DEGRADED', component: 'status', stage: 'observe',
      severity: 'degraded', message: 'Synthetic preceding event',
      firstSeenAt: '2026-10-24T19:59:58Z', lastSeenAt: '2026-10-24T19:59:59Z',
      occurrences: 1, recovered: true
    }],
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
    const issue = githubIssue(report, await reportFingerprint(report), 'bug,vectorwarp-report');
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
    assert.equal(embedded.runtime.configRevision, 'sha256:synthetic-revision');
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

test('all sensitive text classes are removed while diagnostic scalars survive', async () => {
  const value = baseReport(pathways[2]);
  value.incident.message = 'serial=ABC123 token=ghp_bad password=hunter2 host=radar.local endpoint=https://private.example/report';
  value.comment = '192.168.1.50 2001:db8::1 alice@example.com lat=40.7128 lon=-74.0060 /Users/alice/radar.yml C:\\Users\\alice\\radar.yml';
  value.runtime.details.diagnosticClass = 'stream_callback';
  const report = normalizeReport(value);
  const issue = githubIssue(report, await reportFingerprint(report));

  assert.doesNotMatch(issue.body, /ABC123|ghp_bad|hunter2|radar\.local|private\.example|192\.168\.1\.50|2001:db8|alice@example\.com|40\.7128|-74\.0060|Users[\\/]alice/);
  assert.match(issue.body, /\[IP redacted\]/);
  assert.match(issue.body, /\[email redacted\]/);
  assert.match(issue.body, /\[path redacted\]/);
  assert.match(issue.body, /"diagnosticClass": "stream_callback"/);
});
