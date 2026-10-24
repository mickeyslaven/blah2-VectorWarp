import assert from 'node:assert/strict';
import test from 'node:test';
import {githubIssue, handleRequest, normalizeReport, reportFingerprint} from '../src/index.js';

function sample(overrides = {}) {
  return {
    schema: 1,
    consent: true,
    incident: {
      code: 'SDR_OPEN_FAILED', component: 'receiver', stage: 'open', severity: 'error',
      message: 'Unable to open receiver', firstSeenAt: '2026-10-24T12:00:00Z',
      lastSeenAt: '2026-10-24T12:00:02Z', occurrences: 1, recovered: false
    },
    build: {version: '0.1.30', commit: '0123456789abcdef0123456789abcdef01234567', package: 'deb'},
    system: {os: 'Linux', architecture: 'arm64', kernel: '6.12'},
    receiver: {
      type: 'RspDuo', model: 'RSPduo', driverVersion: '3.15', apiVersion: '3.15',
      sampleRateHz: 2000000, centerFrequencyHz: 100000000, channelCount: 2,
      transport: 'usb', startupStages: {open: true, enumerate: true, init: false},
      health: {streamEstablished: false, callbackFault: false}
    },
    runtime: {
      processorState: 'error', restartState: 'failed', backend: 'cpu',
      configRevision: 'sha256:abc', uptimeSeconds: 12, details: {exitStatus: 1}
    },
    recentEvents: [], comment: 'Failed after reconnecting USB.',
    ...overrides
  };
}

function request(body = sample(), options = {}) {
  return new Request('https://reports.example/v1/reports', {
    method: 'POST', headers: {'Content-Type': 'application/json',
      'X-VectorWarp-Report': '1', ...(options.headers || {})},
    body: typeof body === 'string' ? body : JSON.stringify(body)
  });
}

function environment(extra = {}) {
  return {GITHUB_TOKEN: 'test-token', GITHUB_REPOSITORY: 'owner/repository',
    REPORT_LABELS: 'bug,anonymous-report', ...extra};
}

test('health and preflight do not contact GitHub', async () => {
  const health = await handleRequest(new Request('https://reports.example/health'), {});
  assert.equal(health.status, 200);
  assert.deepEqual(await health.json(), {ok: true, service: 'vectorwarp-report-relay', schema: 1});
  const preflight = await handleRequest(new Request('https://reports.example/v1/reports',
    {method: 'OPTIONS'}), {});
  assert.equal(preflight.status, 204);
  assert.equal(preflight.headers.get('access-control-allow-origin'), '*');
});

test('valid report becomes a bounded GitHub issue', async () => {
  let outbound;
  const response = await handleRequest(request(), environment(), {fetch: async (url, options) => {
    outbound = {url, options};
    return new Response(JSON.stringify({number: 42,
      html_url: 'https://github.com/owner/repository/issues/42'}),
    {status: 201, headers: {'Content-Type': 'application/json'}});
  }});
  assert.equal(response.status, 201);
  assert.deepEqual(await response.json(), {ok: true, reportId: '413ad0afe8390091'});
  assert.equal(outbound.url, 'https://api.github.com/repos/owner/repository/issues');
  assert.equal(outbound.options.headers.Authorization, 'Bearer test-token');
  assert.equal(outbound.options.headers['X-GitHub-Api-Version'], '2026-03-10');
  const issue = JSON.parse(outbound.options.body);
  assert.match(issue.title, /SDR_OPEN_FAILED/);
  assert.deepEqual(issue.labels, ['bug', 'anonymous-report']);
  assert.match(issue.body, /"sampleRateHz": 2000000/);
});

test('free text is sanitized before it reaches GitHub', async () => {
  const dirty = sample();
  dirty.incident.message = 'host=radar.local serial:ABC123 at 192.168.1.4 /Users/alice/config.yml';
  dirty.comment = 'mail alice@example.com or see https://private.example/log';
  const report = normalizeReport(dirty);
  const issue = githubIssue(report, await reportFingerprint(report));
  assert.doesNotMatch(issue.body, /radar\.local|ABC123|192\.168\.1\.4|alice@example\.com|private\.example|Users\/alice/);
  assert.match(issue.body, /\[IP redacted\]/);
  assert.match(issue.body, /\[path redacted\]/);
});

test('schema rejects consent gaps, unknown data and sensitive detail keys', () => {
  assert.throws(() => normalizeReport(sample({consent: false})), /consent/);
  assert.throws(() => normalizeReport(sample({rawConfig: {}})), /unsupported field/);
  const withSerial = sample();
  withSerial.receiver.health.serial = 'ABC';
  assert.throws(() => normalizeReport(withSerial), /sensitive or unsupported/);
  const mention = sample();
  mention.incident.component = '@maintainer';
  assert.throws(() => normalizeReport(mention), /stable identifier/);
});

test('bad requests fail without contacting GitHub', async () => {
  let calls = 0;
  const fetch = async () => { calls++; return new Response(); };
  const malformed = await handleRequest(request('{bad json'), environment(), {fetch});
  assert.equal(malformed.status, 400);
  const wrongType = await handleRequest(new Request('https://reports.example/v1/reports',
    {method: 'POST', headers: {'X-VectorWarp-Report': '1'}, body: '{}'}), environment(), {fetch});
  assert.equal(wrongType.status, 415);
  const tooLarge = await handleRequest(request(sample(), {'headers': {'Content-Length': '70000'}}),
    environment(), {fetch});
  assert.equal(tooLarge.status, 413);
  assert.equal(calls, 0);
});

test('missing application marker fails before parsing or submission', async () => {
  let calls = 0;
  const response = await handleRequest(new Request('https://reports.example/v1/reports', {
    method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(sample())
  }), environment(), {fetch: async () => { calls++; return new Response(); }});
  assert.equal(response.status, 403);
  assert.equal(calls, 0);
});

test('rate limiting happens before parsing or GitHub submission', async () => {
  let githubCalls = 0, keys = [];
  const env = environment({REPORT_RATE_LIMITER: {async limit({key}) {
    keys.push(key); return {success: false};
  }}});
  const response = await handleRequest(request(), env,
    {fetch: async () => { githubCalls++; return new Response(); }});
  assert.equal(response.status, 429);
  assert.equal(response.headers.get('retry-after'), '60');
  assert.equal(githubCalls, 0);
  assert.equal(keys.length, 1);
  assert.doesNotMatch(keys[0], /unknown|192\.168/);
});

test('GitHub failures are generic and never expose upstream details', async () => {
  const response = await handleRequest(request(), environment(), {fetch: async () =>
    new Response('credential ghp_secret was rejected', {status: 401})});
  assert.equal(response.status, 502);
  assert.deepEqual(await response.json(), {ok: false,
    error: 'Report destination is temporarily unavailable'});
});

test('missing deployment configuration fails closed', async () => {
  const response = await handleRequest(request(), {}, {fetch: async () => {
    assert.fail('GitHub must not be contacted');
  }});
  assert.equal(response.status, 503);
});
