'use strict';
const fs = require('fs');
const path = require('path');
const {spawn} = require('child_process');
const {sameReceiverOrigin} = require('./receiver-routes');
const {createReceiverHelperClient} = require('./receiver-helper-client');
const INTENT = 'sdrplay-local-build-v1';
const STATUS = '/run/vectorwarp-sdrplay-build/status.json';
const HASH = /^[a-f0-9]{64}$/;

function valid(value) {
  return value && typeof value === 'object' && !Array.isArray(value) &&
    ['current', 'missing', 'stale', 'unavailable'].includes(value.state) &&
    typeof value.ok === 'boolean' && (!value.kit_id || HASH.test(value.kit_id)) &&
    (!value.cohort || HASH.test(value.cohort)) &&
    (value.reason === undefined || (typeof value.reason === 'string' && value.reason.length <= 1800));
}

// Root-owned service output only: never follow links or block on a FIFO.
function progress(file = STATUS, {io = fs, now = Date.now()} = {}) {
  let fd;
  try {
    const parent = io.lstatSync(path.dirname(file));
    if (!parent.isDirectory() || parent.uid !== 0 || parent.mode & 0o022) return null;
    fd = io.openSync(file, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
    const stat = io.fstatSync(fd);
    if (!stat.isFile() || stat.uid !== 0 || stat.mode & 0o022 || stat.size > 16384) return null;
    const data = Buffer.alloc(16385); const size = io.readSync(fd, data, 0, data.length, 0);
    if (size > 16384) return null;
    const value = JSON.parse(data.subarray(0, size).toString('utf8'));
    if (!value || value.schema !== 1 || !['queued', 'running', 'failed', 'current'].includes(value.state) ||
        !Number.isFinite(value.updatedAt) || value.updatedAt > now ||
        (value.reason !== undefined && (typeof value.reason !== 'string' || value.reason.length > 1800)) ||
        (value.kit_id !== undefined && (typeof value.kit_id !== 'string' || !HASH.test(value.kit_id)))) return null;
    if (['queued', 'running'].includes(value.state) && now - value.updatedAt > 200000)
      return {state: 'failed', updatedAt: value.updatedAt, reason: 'The local adapter build did not finish within its time limit.', ...(value.kit_id ? {kit_id: value.kit_id} : {})};
    return {state: value.state, updatedAt: value.updatedAt, reason: value.reason, ...(value.kit_id ? {kit_id: value.kit_id} : {})};
  } catch (_) { return null; }
  finally { if (fd !== undefined) io.closeSync(fd); }
}

function helperStatus(helper, spawnFn = spawn) {
  return new Promise(resolve => {
    let settled = false; let output = ''; let child;
    const finish = value => { if (!settled) { settled = true; resolve(value); } };
    const fail = reason => finish({ok: false, state: 'unavailable', reason});
    try { child = spawnFn('/usr/bin/python3', ['-I', helper, 'status'], {stdio: ['ignore', 'pipe', 'ignore'], cwd: '/', env: {PATH: '/usr/bin:/bin', LANG: 'C', LC_ALL: 'C'}}); }
    catch (_) { fail('Local adapter status is unavailable.'); return; }
    const timer = setTimeout(() => { child.kill('SIGKILL'); fail('Local adapter status timed out.'); }, 3000);
    child.stdout.on('data', part => {
      if (output.length + part.length > 16384) { child.kill('SIGKILL'); fail('Local adapter status is invalid.'); }
      else output += part;
    });
    child.once('error', () => { clearTimeout(timer); fail('Local adapter status is unavailable.'); });
    child.once('close', code => {
      clearTimeout(timer);
      if (code !== 0) { fail('Local adapter status is unavailable.'); return; }
      try { const value = JSON.parse(output); finish(valid(value) ? value : {ok: false, state: 'unavailable', reason: 'Local adapter status is invalid.'}); }
      catch (_) { fail('Local adapter status is unavailable.'); }
    });
  });
}

async function startBuild(request = createReceiverHelperClient()) {
  const result = await request({verb: 'sdrplay-build'});
  if (!result || result.ok !== true || result.status !== 'accepted') {
    const error = new Error(result?.message || 'The local adapter build service did not accept the request.');
    error.code = result?.code || 'BUILD_REQUEST_FAILED';
    throw error;
  }
}

function installSdrplayBuildRoutes(app, {allowedOrigins, helper = '/opt/vectorwarp/libexec/vectorwarp-build-sdrplay', enabled = process.env.BLAH2_SDRPLAY_LOCAL_BUILD === 'true', preview = process.env.BLAH2_PREVIEW === 'true', status = helperStatus, start = startBuild, statusFile = STATUS} = {}) {
  const available = enabled && !preview;
  let cached = null; let pending = null; let starting = false;
  const observe = async () => {
    if (!available) return {ok: false, state: 'unavailable', reason: preview ? 'Local receiver builds are disabled in preview mode.' : 'This installation has no enrolled local RSPduo source kit.'};
    if (cached && cached.expires > Date.now()) return cached.value;
    if (!pending) pending = Promise.resolve(status(helper)).then(value => {
      const result = valid(value) ? value : {ok: false, state: 'unavailable', reason: 'Local adapter status is invalid.'};
      cached = {value: result, expires: Date.now() + 500}; return result;
    }, () => ({ok: false, state: 'unavailable', reason: 'Local adapter status is unavailable.'})).finally(() => { pending = null; });
    return pending;
  };
  const guard = (req, res, mutation = false) => {
    const good = sameReceiverOrigin(req, allowedOrigins) && (!mutation || (req.get('X-VectorWarp-Intent') === INTENT && /^application\/json(?:\s*;|$)/i.test(req.get('Content-Type') || '')));
    if (!good) { res.status(403).json({ok: false, code: 'RECEIVER_ORIGIN_REQUIRED', errors: ['Open Settings from the trusted VectorWarp address.']}); return false; }
    return true;
  };
  const read = async () => ({buildable: available, ...(await observe()), progress: available ? progress(statusFile) : null});
  app.get('/api/sdrplay-build', async (req, res) => { if (guard(req, res)) res.json(await read()); });
  app.post('/api/sdrplay-build', async (req, res) => {
    if (!guard(req, res, true)) return;
    if (!req.body || typeof req.body !== 'object' || Array.isArray(req.body) || Object.keys(req.body).length) return res.status(422).json({ok: false, errors: ['Local adapter build takes no browser arguments.']});
    if (!available) return res.status(409).json({ok: false, errors: [preview ? 'Local receiver builds are disabled in preview mode.' : 'This installation has no enrolled local RSPduo source kit.']});
    if (starting) return res.status(409).json({ok: false, errors: ['A local RSPduo adapter build is already being requested.']});
    const snapshot = await observe();
    if (starting) return res.status(409).json({ok: false, errors: ['A local RSPduo adapter build is already being requested.']});
    if (!snapshot.ok || snapshot.state === 'current') return res.status(409).json({ok: false, errors: [snapshot.reason || 'The local adapter is not ready to build.']});
    const inFlight = progress(statusFile);
    if (inFlight && ['queued', 'running'].includes(inFlight.state) && (!inFlight.kit_id || inFlight.kit_id === snapshot.kit_id)) return res.status(409).json({ok: false, errors: ['A local RSPduo adapter build is already in progress.']});
    starting = true;
    try { await start(); }
    catch (error) { return res.status(503).json({ok: false,
      errors: [`The local adapter build service did not accept the request. ${error.message}`]}); }
    finally { starting = false; }
    cached = null;
    res.status(202).json({ok: true, state: 'queued', message: 'Local RSPduo adapter build requested. Radar processing was not started.'});
  });
}
module.exports = {installSdrplayBuildRoutes, progress, valid, helperStatus, startBuild, INTENT};
