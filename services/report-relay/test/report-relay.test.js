import assert from 'node:assert/strict';
import test from 'node:test';
import {createIssue, githubIssue, handleRequest, normalizeReport, reportFingerprint,
  ReportDeduplicator, submissionReceiptId} from '../src/index.js';

const SUBMISSION = '123e4567-e89b-42d3-a456-426614174000';
function sample(overrides = {}) {
  return {schema: 1, consent: true, submissionId: SUBMISSION,
    incident: {code: 'SDR_OPEN_FAILED', component: 'receiver', stage: 'open', severity: 'error',
      message: 'Unable to open receiver', firstSeenAt: '2026-10-24T12:00:00Z',
      lastSeenAt: '2026-10-24T12:00:02Z', occurrences: 1, recovered: false},
    build: {version: '0.1.30', commit: '0123456789abcdef0123456789abcdef01234567', package: 'deb'},
    system: {os: 'Linux', architecture: 'arm64', kernel: '6.12'},
    receiver: {type: 'RspDuo', model: 'RSPduo', driverVersion: '3.15', apiVersion: '3.15',
      sampleRateHz: 2000000, centerFrequencyHz: 100000000, channelCount: 2,
      transport: 'usb', startupStages: {open: true, enumerate: true, init: false},
      health: {streamEstablished: false, callbackFault: false}},
    runtime: {processorState: 'error', restartState: 'failed', backend: 'cpu',
      configRevision: 'sha256:abc', uptimeSeconds: 12, details: {exitStatus: 1}},
    recentEvents: [], comment: 'Failed after reconnecting USB.', ...overrides};
}
function request(body = sample(), options = {}) {
  return new Request('https://reports.example/v1/reports', {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-VectorWarp-Report': '1', ...(options.headers || {})},
    body: typeof body === 'string' ? body : JSON.stringify(body)});
}
function environment(extra = {}) {
  return {GITHUB_TOKEN: 'test-token', GITHUB_REPOSITORY: 'owner/repository',
    REPORT_LABELS: 'bug,anonymous-report', REPORT_RATE_LIMITER: {limit: async () => ({success: true})}, ...extra};
}
const direct = fetch => ({fetch, deduplicate: (report, env, fetcher) => createIssue(report, env, fetcher)});

test('health and preflight do not contact GitHub', async () => {
  assert.deepEqual(await (await handleRequest(new Request('https://reports.example/health'), {})).json(),
    {ok: true, service: 'vectorwarp-report-relay', schema: 1});
  assert.equal((await handleRequest(new Request('https://reports.example/v1/reports', {method: 'OPTIONS'}), {})).status, 204);
});

test('valid report becomes a bounded GitHub issue with separate grouping and receipt IDs', async () => {
  let outbound;
  const fetch = async (url, options) => { outbound = {url, options}; return new Response(JSON.stringify({number: 42,
    html_url: 'https://github.com/owner/repository/issues/42'}), {status: 201}); };
  const normalized = normalizeReport(sample());
  const response = await handleRequest(request(), environment(), direct(fetch));
  assert.equal(response.status, 201);
  assert.deepEqual(await response.json(), {ok: true, reportId: await submissionReceiptId(normalized)});
  const issue = JSON.parse(outbound.options.body);
  assert.match(issue.body, new RegExp(`Fingerprint.*${await reportFingerprint(normalized)}`));
  assert.match(issue.body, new RegExp(`Submission.*${await submissionReceiptId(normalized)}`));
  assert.deepEqual(issue.labels, ['bug', 'anonymous-report']);
});

test('adversarial free text and structured secrets are discarded before GitHub', async () => {
  const dirty = sample();
  const probes = [
    'Authorization: Basic dXNlcjpwYXNz', 'Authorization: "Bearer ghp_quoted"',
    'password=\\"correct horse battery staple\\"',
    '{\\"hex\\":\\"a1b2c3\\",\\"flight\\":\\"PRIVATE12\\",\\"icao24\\":\\"abc123\\",\\"flightNumber\\":\\"VW123\\",\\"call_sign\\":\\"SECRET7\\"}',
    '["safe","SECOND_SECRET"]', 'password=“correct horse battery staple”'
  ];
  dirty.incident.message = probes.join(' ');
  dirty.comment = probes.join(' | ');
  dirty.system.browser = probes[1];
  dirty.receiver.model = probes[4];
  const report = normalizeReport(dirty);
  const issue = githubIssue(report, await reportFingerprint(report), await submissionReceiptId(report));
  assert.equal(report.incident.message,
    'VectorWarp detected SDR_OPEN_FAILED during receiver open.');
  assert.equal(report.comment, null); assert.equal(report.system.browser, null);
  assert.equal(report.receiver.model, null);
  assert.doesNotMatch(issue.body,
    /dXNlcjpwYXNz|ghp_quoted|correct horse|battery staple|a1b2c3|PRIVATE12|abc123|VW123|SECRET7|SECOND_SECRET/);
});

test('schema rejects consent gaps, unsafe Markdown release values and sensitive detail-key variants', () => {
  assert.throws(() => normalizeReport(sample({consent: false})), /consent/);
  assert.throws(() => normalizeReport(sample({rawConfig: {}})), /unsupported field/);
  assert.throws(() => normalizeReport(sample({submissionId: 'not-a-uuid'})), /submissionId/);
  const mention = sample(); mention.build.version = '@maintainer ![x](https://beacon.invalid)';
  assert.throws(() => normalizeReport(mention), /release identifier/);
  for (const key of ['lat', 'lon', 'latDeg', 'lonDeg', 'receiverSerialNumber', 'accessTokenValue',
    'adsbHex', 'flight', 'icao', 'icao24', 'flightNumber', 'callsign', 'call_sign', 'aircraftHex']) {
    const value = sample(); value.runtime.details = {[key]: 'secret'};
    assert.throws(() => normalizeReport(value), /unsupported diagnostic/, key);
  }
  const array = sample(); array.runtime.details.routeName = ['status', 'SECOND_SECRET'];
  assert.throws(() => normalizeReport(array), /unsupported diagnostic/);
});

test('chunked body is cancelled immediately after the streaming limit', async () => {
  let reads = 0, cancelled = false, submitted = false;
  const source = {url: 'https://reports.example/v1/reports', method: 'POST',
    headers: new Headers({'Content-Type': 'application/json', 'X-VectorWarp-Report': '1'}),
    body: {getReader: () => ({read: async () => (++reads <= 20 ?
      {done: false, value: new Uint8Array(8192)} : {done: true}),
    cancel: async () => { cancelled = true; }})}};
  const response = await handleRequest(source, environment(), {deduplicate: async () => { submitted = true; }});
  assert.equal(response.status, 413); assert.equal(cancelled, true); assert.equal(reads, 9); assert.equal(submitted, false);
});

test('same submission is idempotent and creates one GitHub issue', async () => {
  const values = new Map(); let calls = 0;
  const state = {storage: {get: async key => values.get(key), put: async (key, value) => values.set(key, value),
    delete: async key => values.delete(key)}};
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async () => { calls++; return new Response(JSON.stringify({number: 7,
    html_url: 'https://github.com/owner/repository/issues/7'}), {status: 201}); };
  try {
    const object = new ReportDeduplicator(state, environment());
    const normalized = normalizeReport(sample());
    const make = () => new Request('https://internal/', {method: 'POST', body: JSON.stringify(normalized)});
    const first = await object.fetch(make()), second = await object.fetch(make());
    assert.equal(first.status, 201); assert.equal(second.status, 200); assert.equal(calls, 1);
    assert.deepEqual(await first.json(), await second.json());
  } finally { globalThis.fetch = originalFetch; }
});

test('ambiguous GitHub success and final storage failure reconcile without a duplicate POST', async t => {
  const normalized = normalizeReport(sample());
  const reportId = await submissionReceiptId(normalized);
  const marker = `**Submission:** \`${reportId}\``;
  const make = () => new Request('https://internal/', {method: 'POST', body: JSON.stringify(normalized)});
  for (const failure of ['response-lost', 'storage-put']) await t.test(failure, async () => {
    const values = new Map(); let posts = 0, searches = 0, failCompletePut = failure === 'storage-put';
    const state = {storage: {get: async key => values.get(key), delete: async key => values.delete(key),
      put: async (key, value) => {
        if (value.phase === 'complete' && failCompletePut) { failCompletePut = false; throw new Error('storage unavailable'); }
        values.set(key, value);
      }}};
    const originalFetch = globalThis.fetch;
    globalThis.fetch = async (url, options = {}) => {
      if (String(url).includes('/search/issues')) {
        searches++;
        return new Response(JSON.stringify({items: [{body: marker,
          html_url: 'https://github.com/owner/repository/issues/9'}]}), {status: 200});
      }
      posts++;
      if (failure === 'response-lost') throw new TypeError('connection closed after send');
      return new Response(JSON.stringify({number: 9,
        html_url: 'https://github.com/owner/repository/issues/9'}), {status: 201});
    };
    try {
      assert.equal((await new ReportDeduplicator(state, environment()).fetch(make())).status,
        failure === 'response-lost' ? 502 : 500);
      assert.deepEqual(values.get('submission'), {phase: 'pending', reportId});
      const retry = await new ReportDeduplicator(state, environment()).fetch(make());
      assert.equal(retry.status, 200); assert.deepEqual(await retry.json(), {ok: true, reportId});
      assert.equal(posts, 1); assert.equal(searches, 1);
      assert.deepEqual(values.get('submission'), {phase: 'complete', reportId});
    } finally { globalThis.fetch = originalFetch; }
  });
});

test('bad requests and missing mandatory protection bindings fail closed', async () => {
  let calls = 0; const fetch = async () => { calls++; return new Response(); };
  assert.equal((await handleRequest(request('{bad json'), environment(), direct(fetch))).status, 400);
  assert.equal((await handleRequest(new Request('https://reports.example/v1/reports', {method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-VectorWarp-Report': '1'},
    body: new Uint8Array([0xc3, 0x28])}), environment(), direct(fetch))).status, 400);
  assert.equal((await handleRequest(new Request('https://reports.example/v1/reports',
    {method: 'POST', headers: {'X-VectorWarp-Report': '1'}, body: '{}'}), environment(), direct(fetch))).status, 415);
  assert.equal((await handleRequest(request(sample(), {headers: {'Content-Length': '70000'}}), environment(), direct(fetch))).status, 413);
  assert.equal((await handleRequest(request(), {...environment(), REPORT_RATE_LIMITER: undefined}, direct(fetch))).status, 503);
  assert.equal((await handleRequest(request(), environment(), {fetch})).status, 503);
  assert.equal(calls, 0);
});

test('rate limiting happens before parsing or GitHub submission', async () => {
  let calls = 0;
  const response = await handleRequest(request(), environment({REPORT_RATE_LIMITER: {limit: async () => ({success: false})}}),
    direct(async () => { calls++; return new Response(); }));
  assert.equal(response.status, 429); assert.equal(calls, 0);
});

test('GitHub failures are generic and never expose upstream details', async () => {
  const response = await handleRequest(request(), environment(), direct(async () =>
    new Response('credential ghp_secret was rejected', {status: 401})));
  assert.equal(response.status, 502);
  assert.deepEqual(await response.json(), {ok: false, error: 'Report destination is temporarily unavailable'});
});
