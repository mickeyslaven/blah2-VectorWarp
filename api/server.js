const express = require('express');
const net = require("net");
const fs = require('fs');
const yaml = require('js-yaml');
const dns = require('dns');
const path = require('path');
const {spawn} = require('child_process');
const {getDeviceProfiles, validateConfig, FIELD_RULES} = require('./config-manager.js');
const {readConfig, writable, saveConfig} = require('./config-store.js');
const {getUpstreamStatus} = require('./upstream-status.js');
const {createAdsbSource} = require('./adsb-source.js');
const {checkNetworkBindings, bindMessage} = require('./network-check.js');
const {status: validateProcessorStatus, fresh: processorStatusFresh} = require('./processor-status.js');

// parse config file
const configFile = path.resolve(process.argv[2] || 'config/config.yml');
const startupDocument = readConfig(configFile);
var config = startupDocument.config;
const adsbSource = createAdsbSource(config, {preview: process.env.BLAH2_PREVIEW === 'true'});
if (startupDocument.setupRequired)
  console.error('Setup required:', startupDocument.readError || startupDocument.validation.errors);
const serverId = `${process.pid}-${Date.now()}`;
const serviceErrors = new Map();
const ownedPorts = new Set();
let restartState = {state: 'idle'};
let lastFrameAt = null;
let timestampConnections = 0;
let upstreamCache = null;
let processorStatus = null;

function replayTruthDisabled() {
  return config.capture?.replay?.state === true ||
    (processorStatusFresh(processorStatus) && processorStatus.value.input === 'replay');
}
function replayTruthStatus() {
  return {enabled: false, online: false, disabledForReplay: true,
    message: 'Live ADS-B truth is not replayed.'};
}

let restartCommand = null;
if (process.env.BLAH2_CONFIG_RESTART_COMMAND) {
  try {
    const parsed = JSON.parse(process.env.BLAH2_CONFIG_RESTART_COMMAND);
    if (!Array.isArray(parsed) || parsed.length === 0 ||
        parsed.some(value => typeof value !== 'string' || value.length === 0))
      throw new Error('expected a JSON array of non-empty strings');
    restartCommand = parsed;
  } catch (error) {
    console.error(`Invalid BLAH2_CONFIG_RESTART_COMMAND: ${error.message}`);
  }
}

var stash_map = require('./stash/maxhold.js');
var stash_detection = require('./stash/detection.js');
var stash_iqdata = require('./stash/iqdata.js');
var stash_timing = require('./stash/timing.js');

// constants
const setupPort = Number(process.env.BLAH2_SETUP_PORT || 3000);
const PORT = process.env.BLAH2_SETUP_PORT ? setupPort : Number.isInteger(config.network?.ports?.api) &&
  config.network.ports.api > 0 && config.network.ports.api <= 65535 ?
  config.network.ports.api : setupPort;
const HOST = net.isIP(config.network?.ip || '') ? config.network.ip : '0.0.0.0';
var map = '';
var detection = '';
var track = '';
var timestamp = '';
var timing = '';
var iqdata = '';
var data_map = '';
var data_detection = '';
var data_tracker = '';
var data_timestamp = '';
var data_timing = '';
var data_iqdata = '';
var capture = false;
var captureStartedAt = null;
let captureRevision = 0;

// api server
const app = express();
app.use(express.json({limit: '256kb', strict: true}));
function configWriteOriginAllowed(req) {
  const origin = req.get('Origin');
  if (!origin) return true;
  try {
    const originHost = new URL(origin).hostname;
    const requestHost = new URL(`http://${req.get('Host')}`).hostname;
    return originHost === requestHost;
  } catch (_) {
    return false;
  }
}
function processorStatusOriginAllowed(req) {
  const origin = req.get('Origin');
  if (!origin) return true; // Native processor telemetry has no browser Origin.
  try { return new URL(origin).host === req.get('Host'); }
  catch (_) { return false; }
}
// header on all requests
app.use(function(req, res, next) {
  const origin = req.get('Origin');
  if (!origin || req.method === 'GET' || configWriteOriginAllowed(req)) {
    res.header("Access-Control-Allow-Origin", origin || "*");
    if (origin) res.header('Vary', 'Origin');
  }
  res.header('Access-Control-Allow-Methods', 'GET, POST, PUT, OPTIONS');
  res.header('Access-Control-Allow-Headers', 'Content-Type, If-Match');
  res.header('Access-Control-Expose-Headers', 'ETag');
  res.header('Cache-Control', 'private, no-cache, no-store, must-revalidate');
  res.header('Expires', '-1');
  res.header('Pragma', 'no-cache');
  next();
});
app.options('*', (req, res) => {
  if (req.path === '/api/config' && !configWriteOriginAllowed(req))
    return res.status(403).json({ok: false,
      errors: ['Configuration changes must come from this VectorWarp host.']});
  res.sendStatus(204);
});

function configFileWritable() {
  return writable(configFile);
}

function restartCommandAvailable() {
  if (!restartCommand) return false;
  const command = restartCommand[0];
  const candidates = command.includes(path.sep) ? [command] :
    (process.env.PATH || '').split(path.delimiter)
      .filter(Boolean).map(folder => path.join(folder, command));
  return candidates.some(candidate => {
    try { fs.accessSync(candidate, fs.constants.X_OK); return true; }
    catch (_) { return false; }
  });
}

function launchRestart() {
  const [command, ...args] = restartCommand;
  restartState = {...restartState, state: 'running', startedAt: Date.now(),
    timestampConnections};
  const child = spawn(command, args, {detached: true, stdio: 'ignore'});
  const timeout = setTimeout(() => {
    restartState = {...restartState, state: 'failed',
      message: 'The restart command did not finish within 30 seconds. Check the service manager before retrying.'};
  }, 30000);
  timeout.unref();
  child.on('error', error => {
    clearTimeout(timeout);
    restartState = {...restartState, state: 'failed', message: `Restart could not start: ${error.message}`};
  });
  child.on('exit', (code, signal) => {
    clearTimeout(timeout);
    restartState = {...restartState, state: code === 0 ? 'command-complete' : 'failed',
      message: code === 0 ? 'Restart command finished. Waiting for a new radar connection.' :
        `Restart command failed (${signal || `exit ${code}`}). Check the service manager.`};
  });
  child.unref();
}
app.get('/', (req, res, next) => {
  if (fs.existsSync(path.join(__dirname, '..', 'html', 'index.html'))) return next();
  res.json({service: 'VectorWarp API', settings: '/api/config', status: '/api/system/status'});
});
app.get('/api/map', (req, res) => {
  res.send(map);
});
app.get('/api/detection', (req, res) => {
  res.send(detection);
});
app.get('/api/tracker', (req, res) => {
  res.send(track);
});
app.get('/api/timestamp', (req, res) => {
  res.send(timestamp);
});
app.get('/api/timing', (req, res) => {
  res.send(timing);
});
app.get('/api/iqdata', (req, res) => {
  res.send(iqdata);
});
app.get('/api/config', (req, res) => {
  const document = readConfig(configFile);
  res.set('ETag', `"${document.revision}"`).json(document.config);
});
app.get('/api/runtime/config', (req, res) => {
  // API startup snapshot, not hardware acknowledgement. Do not relabel frames
  // with newly saved tuning/site settings until the services have restarted.
  res.json(config);
});
app.get('/api/config/capabilities', (req, res) => {
  const document = readConfig(configFile);
  const compiledLiveTypes = process.env.BLAH2_RECEIVER_TYPES?.split(',')
    .map(item => item.trim()).filter(Boolean);
  res.json({
    editable: configFileWritable(),
    restartAvailable: restartCommandAvailable(),
    maximumConfigBytes: 262144,
    changesRequireRestart: true,
    // Replay validates all file/profile combinations without loading receiver
    // SDKs. Keep every profile editable, while accurately exposing live support.
    deviceProfiles: getDeviceProfiles().map(item => ({...item,
      liveAvailable: !compiledLiveTypes || compiledLiveTypes.includes(item.type)})),
    compiledLiveTypes: compiledLiveTypes || getDeviceProfiles().map(item => item.type),
    fieldRules: FIELD_RULES,
    setupRequired: document.setupRequired,
    setupMessage: document.readError,
    suppliedDefaults: document.suppliedDefaults,
    configRevision: document.revision,
    validation: document.validation,
    configPath: configFile
  });
});
app.get('/api/system/status', (req, res) => {
  const document = readConfig(configFile);
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  res.json({serverId, configRevision: document.revision,
    acceleration: (() => { try { return JSON.parse(timing)?.acceleration || null; } catch (_) { return null; } })(),
    loadedRevision: startupDocument.revision, setupRequired: document.setupRequired,
    restart: restartState, lastFrameAt, timestampConnections,
    adsbEnabled: config.truth?.adsb?.enabled === true,
    radar: lastFrameAt === null ? 'no-data' : Date.now() - lastFrameAt <
      Math.max(10000, (config.process?.data?.cpi || 1) * 3000) ? 'receiving' : 'stale',
    processor: processor && {...processor, receivedAt: processorStatus.receivedAt},
    processorFresh: processor !== null,
    errors: [...serviceErrors.values()],
    message: lastFrameAt === null ? 'No radar frames received. Check the receiver, processing service and its logs. Settings remain available.' : null});
});
// The processor runs without a browser Origin header. If a browser supplies one,
// retain the same-host restriction used by configuration writes. This telemetry is
// display state only: it never validates radio hardware or permits configuration.
app.post('/api/processor/status', (req, res) => {
  if (!processorStatusOriginAllowed(req))
    return res.status(403).json({ok: false, errors: ['Processor telemetry must come from this VectorWarp host.']});
  const checked = validateProcessorStatus(req.body);
  if (!checked.valid) return res.status(422).json({ok: false, errors: checked.errors});
  processorStatus = {value: checked.value, receivedAt: Date.now()};
  if (checked.value.input === 'replay') adsbSource.clear();
  if (checked.value.input === 'live' && checked.value.recordingError &&
      checked.value.recordingRequestId === captureRevision) {
    capture = false;
    captureStartedAt = null;
  }
  res.status(204).end();
});
app.get('/api/upstream/status', async (req, res) => {
  if (process.env.BLAH2_PREVIEW === 'true') return res.json({supported: true,
    available: false, matched: false, host: 'not connected', controlPort: '—',
    message: 'UI preview only. No receiver connection is opened and no live data is generated.'});
  const document = readConfig(configFile);
  if (!upstreamCache || upstreamCache.revision !== document.revision ||
      Date.now() - upstreamCache.at > 4000) {
    upstreamCache = {at: Date.now(), revision: document.revision,
      promise: getUpstreamStatus(document.config)};
  }
  const status = await upstreamCache.promise;
  res.status(status.available === false ? 503 : 200).json(status);
});
app.post('/api/config/validate', async (req, res) => {
  if (!configWriteOriginAllowed(req))
    return res.status(403).json({valid: false,
      errors: ['Configuration checks must come from this VectorWarp host.']});
  const validation = validateConfig(req.body, readConfig(configFile).config);
  if (validation.valid) {
    validation.errors.push(...await checkNetworkBindings(req.body, config, ownedPorts));
    validation.valid = validation.errors.length === 0;
  }
  res.status(validation.valid ? 200 : 422).json(validation);
});
app.put('/api/config', async (req, res) => {
  if (!configWriteOriginAllowed(req))
    return res.status(403).json({ok: false,
      errors: ['Configuration changes must come from this VectorWarp host.']});
  if (!configFileWritable())
    return res.status(503).json({ok: false,
      errors: ['The active configuration file is read-only.']});
  const wantsRestart = req.query.restart === 'true';
  if (wantsRestart && !restartCommandAvailable())
    return res.status(503).json({ok: false,
      errors: ['A safe restart command is not configured on this server.']});
  if (!req.get('If-Match'))
    return res.status(428).json({ok: false, errors: ['Reload settings before saving (configuration revision required).']});
  if (restartState.state === 'running' || restartState.state === 'scheduled')
    return res.status(409).json({ok: false, errors: ['A restart is already in progress. Wait for its result.']});
  const allowedTypes = process.env.BLAH2_RECEIVER_TYPES?.split(',')
    .map(item => item.trim()).filter(Boolean);
  const replayRequested = req.body?.capture?.replay?.state === true;
  if (allowedTypes && !replayRequested && !allowedTypes.includes(req.body?.capture?.device?.type))
    return res.status(422).json({ok: false, errors: [
      'This receiver backend is unavailable for live capture in this installation. Enable replay to save this profile.'
    ]});
  const validation = validateConfig(req.body, readConfig(configFile).config);
  if (!validation.valid)
    return res.status(422).json({ok: false, errors: validation.errors});
  try {
    const networkErrors = await checkNetworkBindings(req.body, config, ownedPorts);
    if (networkErrors.length) return res.status(422).json({ok: false, errors: networkErrors});
    // Another request can finish while the asynchronous bind checks run.
    if (restartState.state === 'running' || restartState.state === 'scheduled')
      return res.status(409).json({ok: false, errors: ['A restart is already in progress. Wait for its result.']});
    const saved = saveConfig(configFile, req.body, req.get('If-Match').replace(/^"|"$/g, ''));
    res.json({ok: true, restarting: wantsRestart, revision: saved.revision,
      message: wantsRestart ? 'Configuration saved. Restart requested.' :
        'Configuration saved. Restart VectorWarp processing and its API to apply it.'});
    if (wantsRestart) {
      restartState = {state: 'scheduled', requestedAt: Date.now(), revision: saved.revision};
      setTimeout(launchRestart, 150).unref();
    }
  } catch (error) {
    console.error(`Unable to save configuration: ${error.message}`);
    res.status(error.status || 500).json({ok: false,
      errors: [error.status ? error.message : 'Unable to save safely. Check file ownership, directory permissions and free disk space.']});
  }
});
app.get('/api/adsb2dd', (req, res) => {
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  if (!config.truth?.adsb?.enabled) return res.status(400).end();
  // Compatibility link now stays on this API; no converter URL or query is accepted.
  res.json({url: '/api/adsb/delay-doppler'});
});
for (const [route, kind] of [['/api/adsb', 'aircraft'], ['/api/adsb/delay-doppler', 'delayDoppler']]) {
  app.get(route, async (req, res) => {
    if (replayTruthDisabled()) return res.json(replayTruthStatus());
    try { res.json(await adsbSource.get(kind)); }
    catch (error) { res.status(503).json({error: error.message}); }
  });
}
app.get('/api/adsb/status', async (req, res) => {
  if (replayTruthDisabled()) return res.json(replayTruthStatus());
  res.json(await adsbSource.status());
});

// stash API
app.get('/stash/map', (req, res) => {
  res.send(stash_map.get_data_map());
});
app.get('/stash/detection', (req, res) => {
  res.send(stash_detection.get_data_detection());
});
app.get('/stash/iqdata', (req, res) => {
  res.send(stash_iqdata.get_data_iqdata());
});
app.get('/stash/timing', (req, res) => {
  res.send(stash_timing.get_data_timing());
});

// read state of capture
app.get('/capture', (req, res) => {
  res.send(capture);
});
app.get('/capture/request', (req, res) => res.json({recording: capture, revision: captureRevision}));
app.get('/capture/status', (req, res) => {
  const now = Date.now();
  const receiving = lastFrameAt !== null && now - lastFrameAt <
    Math.max(10000, (config.process.data.cpi || 1) * 3000);
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  const replay = processor?.input === 'replay' || config.capture.replay?.state === true;
  const acknowledgement = processor && processor.input === 'live' ? processor : null;
  const recording = acknowledgement?.recording === true;
  const recordingError = acknowledgement?.recordingError || null;
  const acknowledged = acknowledgement && acknowledgement.recordingRequestId === captureRevision &&
    acknowledgement.recording === capture;
  if (capture && recordingError && acknowledgement?.recordingRequestId === captureRevision) {
    capture = false;
    captureStartedAt = null;
  }
  res.json({
    supported: !replay,
    available: !replay && (capture ? acknowledgement !== null : receiving),
    reason: replay ? 'Recording is unavailable while replay is active.' : recordingError ||
      (!receiving && !capture ? 'Start live radar and wait for frames before recording.' : null),
    requested: capture,
    revision: captureRevision,
    acknowledged: acknowledged === true,
    recording,
    recordingFile: acknowledgement?.recordingFile || '',
    recordingError,
    recordedSamples: acknowledgement?.recordedSamples || 0,
    startedAt: recording ? captureStartedAt : null,
    elapsedSeconds: recording && captureStartedAt ?
      Math.max(0, Math.floor((now - captureStartedAt) / 1000)) : 0
  });
});
// toggle state of capture
app.get('/capture/toggle', (req, res) => {
  if (!configWriteOriginAllowed(req)) return res.status(403).json({error: 'Use this VectorWarp host to control recording.'});
  const processor = processorStatusFresh(processorStatus) ? processorStatus.value : null;
  if (processor?.input === 'replay' || config.capture.replay?.state === true)
    return res.status(409).json({error: 'Recording is unavailable while replay is active.'});
  if (!capture && (lastFrameAt === null || Date.now() - lastFrameAt >
      Math.max(10000, (config.process.data.cpi || 1) * 3000)))
    return res.status(409).json({error: 'No live radar frames. Start the radar before recording.'});
  capture = !capture;
  captureRevision += 1;
  captureStartedAt = capture ? Date.now() : null;
  res.json({requested: capture, revision: captureRevision, recording: processor?.recording === true,
    acknowledged: processor?.input === 'live' && processor.recordingRequestId === captureRevision && processor.recording === capture,
    startedAt: captureStartedAt});
});
app.use((error, req, res, next) => {
  if (error instanceof SyntaxError || error.type === 'entity.too.large')
    return res.status(error.type === 'entity.too.large' ? 413 : 400)
      .json({ok: false, errors: ['The configuration request is not valid JSON.']});
  next(error);
});
// When serving the UI directly, advertise this actual API port. A static web
// server can keep using ?apiPort=PORT or a same-origin reverse proxy.
app.use((req, res, next) => {
  if (req.method !== 'GET' || req.path.startsWith('/api/')) return next();
  const relative = req.path.endsWith('/') ? `${req.path}index.html` :
    path.extname(req.path) ? req.path : `${req.path}/index.html`;
  if (!relative.endsWith('.html')) return next();
  const root = path.resolve(__dirname, '..', 'html');
  const filename = path.resolve(root, `.${relative}`);
  if (!filename.startsWith(`${root}${path.sep}`) || !fs.existsSync(filename)) return next();
  const preview = process.env.BLAH2_PREVIEW === 'true' ?
    '<aside class="preview-notice">UI preview · No live radar · Saves affect only this temporary preview</aside>' : '';
  const html = fs.readFileSync(filename, 'utf8')
    .replace('<head>', `<head><script>window.BLAH2_API_PORT=${JSON.stringify(String(PORT))};</script>`)
    .replace(/<body([^>]*)>/, `<body$1>${preview}`);
  res.type('html').send(html);
});
app.use(express.static(path.join(__dirname, '..', 'html')));
app.use((error, req, res, next) => {
  console.error(`API request failed: ${error.message}`);
  res.status(500).json({ok: false, errors: ['The server could not complete this request. Check the VectorWarp API log.']});
});
app.listen(PORT, HOST, () => {
  ownedPorts.add(PORT);
  console.log(`Running on http://${HOST}:${PORT}`);
}).on('error', error => {
  console.error(`Settings API cannot listen on ${HOST}:${PORT}: ${error.code}. Choose a free API port or BLAH2_SETUP_PORT.`);
});

function listenData(server, name) {
  server.on('error', error => {
    const message = bindMessage(`${name} data port`, error.code);
    serviceErrors.set(name, message);
    console.error(message);
  });
  server.on('connection', socket => socket.on('error', error => {
    serviceErrors.set(name, `${name} connection: ${error.code}`);
  }));
  const port = config.network?.ports?.[name];
  if (!Number.isInteger(port) || port < 1 || port > 65535 || port === PORT) {
    serviceErrors.set(name, `${name} has an invalid or conflicting port. Correct it in Settings.`);
    return;
  }
  server.on('close', () => ownedPorts.delete(port));
  server.listen(port, HOST, () => {
    ownedPorts.add(port);
    serviceErrors.delete(name);
  });
}

// tcp listener map
const server_map = net.createServer((socket)=>{
    socket.on("data",(msg)=>{
        data_map = data_map + msg.toString();
        if (data_map.slice(-1) === "}")
        {
          map = data_map;
          stash_map.update_data(map);
          data_map = '';
        }
    });
    socket.on("close",()=>{
        console.log("Connection closed.");
    })
});
listenData(server_map, 'map');

// tcp listener detection
const server_detection = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
      data_detection = data_detection + msg.toString();
      if (data_detection.slice(-1) === "}")
      {
        detection = data_detection;
        stash_detection.update_data(detection);
        data_detection = '';
      }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_detection, 'detection');

// tcp listener tracker
const server_tracker = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
      data_tracker = data_tracker + msg.toString();
      if (data_tracker.slice(-1) === "}")
      {
        track = data_tracker;
        data_tracker = '';
      }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_tracker, 'track');

// tcp listener timestamp
const server_timestamp = net.createServer((socket)=>{
  timestampConnections += 1;
  socket.on("data",(msg)=>{
    lastFrameAt = Date.now();
    data_timestamp = data_timestamp + msg.toString();
    timestamp = data_timestamp;
    data_timestamp = '';
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_timestamp, 'timestamp');

// tcp listener timing
const server_timing = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
    data_timing = data_timing + msg.toString();
    if (data_timing.slice(-1) === "}")
    {
      timing = data_timing;
      stash_timing.update_data(timing);
      data_timing = '';
    }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_timing, 'timing');

// tcp listener iqdata metadata
const server_iqdata = net.createServer((socket)=>{
  socket.on("data",(msg)=>{
    data_iqdata = data_iqdata + msg.toString();
    if (data_iqdata.slice(-1) === "}")
    {
      iqdata = data_iqdata;
      stash_iqdata.update_data(iqdata);
      data_iqdata = '';
    }
  });
  socket.on("close",()=>{
      console.log("Connection closed.");
  })
});
listenData(server_iqdata, 'iqdata');

process.on('SIGTERM', () => {
  console.log('SIGTERM signal received.');
  process.exit(0);
});
