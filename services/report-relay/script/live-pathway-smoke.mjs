const endpoint = process.env.REPORT_RELAY_URL;
if (process.env.ALLOW_LIVE_REPORT_TEST !== '1' || !endpoint?.startsWith('https://')) {
  throw new Error('Set ALLOW_LIVE_REPORT_TEST=1 and REPORT_RELAY_URL=https://... to create synthetic private issues.');
}

const pathways = [
  ['SDR_ENUMERATION_FAILED', 'receiver', 'enumerate', 'RspDuo',
    {deviceCount: 0, permissionDenied: true, driverLoaded: true}],
  ['SDR_OPEN_FAILED', 'receiver', 'open', 'Kraken',
    {driverErrorCode: 'DEVICE_BUSY', candidateCount: 1}],
  ['SDR_STREAM_LOST', 'receiver', 'stream', 'Usrp',
    {framesReceived: 2048, lastFrameAgeMs: 8500, droppedFrames: 32}],
  ['RECEIVER_SYNC_FAILED', 'receiver', 'sync', 'Kraken',
    {syncState: 'failed', rollbackAttempted: true, rollbackSucceeded: false,
      reconciliationRequired: true}],
  ['GAIN_MONITOR_FAILED', 'receiver', 'gain-monitor', 'HackRF',
    {probeTimedOut: true, attemptCount: 3, overloadKnown: false}],
  ['PROCESSOR_START_FAILED', 'processor', 'start', 'RspDuo',
    {exitStatus: 1, terminationSignal: 'none', receiverReady: true}],
  ['PROCESSOR_RUNTIME_FAILED', 'processor', 'runtime', 'Replay',
    {framesProcessed: 4096, lastFrameAgeMs: 3000, queueDepth: 8}],
  ['GPU_BACKEND_FAILED', 'processor', 'acceleration', 'RspDuo',
    {driverState: 'failed', fallbackAttempted: true, fallbackSucceeded: false}],
  ['RESTART_FAILED', 'lifecycle', 'restart', 'RspDuo',
    {helperAvailable: true, receiptState: 'failed', timedOut: true, exitStatus: 1}],
  ['CONFIG_PERSISTENCE_FAILED', 'configuration', 'persist', 'RspDuo',
    {writeStage: 'rename', revisionConflict: false, rollbackAttempted: true,
      rollbackSucceeded: true}],
  ['API_UNAVAILABLE', 'api', 'connect', 'Unknown',
    {routeName: 'status', statusCode: 503, timeoutMs: 1800, consecutiveFailures: 4}],
  ['ADSB_SOURCE_FAILED', 'adsb', 'connect', 'RspDuo',
    {sourceCount: 2, healthySourceCount: 0, consecutiveFailures: 5, errorClass: 'timeout'}]
];

function report([code, component, stage, receiverType, details], index) {
  const at = new Date(Date.now() + index).toISOString();
  return {
    schema: 1,
    consent: true,
    incident: {code, component, stage, severity: 'error',
      message: `Synthetic live pathway verification for ${code}`,
      firstSeenAt: at, lastSeenAt: at, occurrences: 1, recovered: true},
    build: {version: '0.1.30-live-test',
      commit: '0123456789abcdef0123456789abcdef01234567', package: 'test'},
    system: {os: 'Linux', architecture: 'arm64', kernel: 'live-test'},
    receiver: {type: receiverType, model: 'synthetic-device',
      driverVersion: 'test', apiVersion: 'test', firmwareVersion: 'test',
      sampleRateHz: 2000000, centerFrequencyHz: 100000000, channelCount: 2,
      transport: 'usb', startupStages: {enumerate: true, open: true, init: true},
      health: {streamEstablished: component !== 'receiver', callbackFault: false}},
    runtime: {processorState: component === 'processor' ? 'error' : 'running',
      restartState: component === 'lifecycle' ? 'failed' : 'idle',
      backend: code === 'GPU_BACKEND_FAILED' ? 'vulkan' : 'cpu',
      configRevision: 'sha256:live-test', uptimeSeconds: 120,
      details: {...details, syntheticLiveTest: true}},
    recentEvents: [],
    comment: 'Automated live pathway verification; no user or hardware data.'
  };
}

for (let index = 0; index < pathways.length; index++) {
  if (index) await new Promise(resolve => setTimeout(resolve, 13000));
  const response = await fetch(`${endpoint.replace(/\/$/, '')}/v1/reports`, {
    method: 'POST',
    headers: {'Content-Type': 'application/json', 'X-VectorWarp-Report': '1'},
    body: JSON.stringify(report(pathways[index], index))
  });
  const result = await response.json().catch(() => ({}));
  console.log(JSON.stringify({code: pathways[index][0], status: response.status,
    reportId: result.reportId || null, error: result.error || null}));
  if (!response.ok) process.exitCode = 1;
}
