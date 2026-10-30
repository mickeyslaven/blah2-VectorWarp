'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const os = require('os');
const {execFile} = require('child_process');
const {sameReceiverOrigin} = require('./receiver-routes');
const INTENT = 'sdrplay-local-build-v1';
const absoluteFile = (value, exists) => typeof value === 'string' && path.isAbsolute(value) && !value.includes('\0') && exists(value);
function boundedRegularRead(file, limit) {
  const descriptor = fs.openSync(file, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
  try {
    const before = fs.fstatSync(descriptor);
    if (!before.isFile() || before.size < 1 || before.size > limit) throw new Error('not a bounded regular file');
    const data = Buffer.alloc(before.size); let offset = 0;
    while (offset < data.length) { const count = fs.readSync(descriptor, data, offset, data.length - offset, null); if (!count) break; offset += count; }
    const after = fs.fstatSync(descriptor), named = fs.lstatSync(file);
    const unchanged = value => value.isFile() && value.dev === before.dev && value.ino === before.ino &&
      value.size === before.size && value.mtimeMs === before.mtimeMs && value.ctimeMs === before.ctimeMs;
    if (offset !== before.size || !unchanged(after) || !unchanged(named))
      throw new Error('file changed while reading');
    return data;
  } finally { fs.closeSync(descriptor); }
}

function createMacRspduoBuild({exists = fs.existsSync, readFile = fs.readFileSync, run = execFile, env = process.env} = {}) {
  let running = false;
  const read = (file, limit) => readFile === fs.readFileSync ? boundedRegularRead(file, limit) : readFile(file);
  function status() {
    const builder = env.VECTORWARP_MACOS_RSPDUO_BUILD;
    const include = env.BLAH2_SDRPLAY_INCLUDE_DIR;
    const library = env.BLAH2_SDRPLAY_LIBRARY;
    if (!absoluteFile(builder, exists)) return {ok: false, state: 'unavailable', reason: 'This installation has no staged macOS RSPduo builder.'};
    if (!absoluteFile(include && path.join(include, 'sdrplay_api.h'), exists) || !absoluteFile(library, exists))
      return {ok: false, state: 'unavailable', reason: 'Install the SDRplay API manually and set its documented include and library paths before building.'};
    const current = env.VECTORWARP_MACOS_STATE && path.join(env.VECTORWARP_MACOS_STATE, 'adapters/rspduo/current');
    try {
      const module = current && path.join(current, 'blah2-receiver-rspduo.dylib'); const receipt = current && path.join(current, 'receipt.json');
      const manifest = JSON.parse(read(path.join(env.VECTORWARP_MACOS_ROOT, 'receiver-source/rspduo/kit.json'), 65536).toString('utf8'));
      const value = JSON.parse(read(receipt, 8192).toString('utf8'));
      const sha = (file, installed = false) => crypto.createHash('sha256').update(read(installed && readFile === fs.readFileSync ? fs.realpathSync(file) : file, 32 * 1024 * 1024)).digest('hex');
      if (manifest?.schema === 1 && value?.schema === 1 && value.kit_id === manifest.kit_id && value.cohort === manifest.cohort &&
          value.core_sha256 === manifest.core_sha256 && value.core_sha256 === sha(path.join(env.VECTORWARP_MACOS_ROOT, 'bin/libblah2-capture-core.dylib'), true) &&
          value.module_sha256 === sha(module) && value.sdk?.include === sha(path.join(include, 'sdrplay_api.h'), true) && value.sdk?.library === sha(library, true))
        return {ok: true, state: 'current', reason: 'The locally built RSPduo adapter matches this source kit and manual SDK.'};
    } catch (_) { /* Missing, changed, or malformed local publication is stale. */ }
    return {ok: true, state: 'missing', reason: 'The local RSPduo adapter can be built from this manually installed SDK.'};
  }
  async function start() {
    const snapshot = status();
    if (!snapshot.ok) { const error = new Error(snapshot.reason); error.code = 'BUILD_UNAVAILABLE'; throw error; }
    if (running) { const error = new Error('A local RSPduo adapter build is already running.'); error.code = 'BUILD_BUSY'; throw error; }
    running = true;
    try {
      await new Promise((resolve, reject) => run(env.VECTORWARP_MACOS_RSPDUO_BUILD, [], {cwd: '/', timeout: 180000, maxBuffer: 65536,
        env: {PATH: '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin', LANG: 'C', LC_ALL: 'C',
          BLAH2_SDRPLAY_INCLUDE_DIR: env.BLAH2_SDRPLAY_INCLUDE_DIR, BLAH2_SDRPLAY_LIBRARY: env.BLAH2_SDRPLAY_LIBRARY,
          VECTORWARP_MACOS_ROOT: env.VECTORWARP_MACOS_ROOT, VECTORWARP_MACOS_STATE: env.VECTORWARP_MACOS_STATE,
          VECTORWARP_MACOS_NODE: env.VECTORWARP_MACOS_NODE, HOME: os.homedir()}}, error => error ? reject(error) : resolve()));
      return {ok: true, state: 'current', message: 'Local RSPduo adapter build completed. Recheck receiver software; radar processing was not started.'};
    } finally { running = false; }
  }
  return {status, start, busy: () => running};
}
function installMacRspduoBuildRoutes(app, {allowedOrigins, preview = process.env.BLAH2_PREVIEW === 'true', build = createMacRspduoBuild(), transactionBusy = () => false} = {}) {
  const guard = (req, res, mutation = false) => {
    const good = sameReceiverOrigin(req, allowedOrigins) && (!mutation || (req.get('X-VectorWarp-Intent') === INTENT && /^application\/json(?:\s*;|$)/i.test(req.get('Content-Type') || '')));
    if (!good) { res.status(403).json({ok: false, code: 'RECEIVER_ORIGIN_REQUIRED', errors: ['Open Settings from the trusted VectorWarp address.']}); return false; }
    return true;
  };
  const read = () => preview ? {buildable: false, ok: false, state: 'unavailable', reason: 'Local receiver builds are disabled in preview mode.'} :
    (() => { const snapshot = build.status(); return {buildable: snapshot.ok === true, ...snapshot}; })();
  app.get('/api/sdrplay-build', (req, res) => { if (guard(req, res)) res.json(read()); });
  app.post('/api/sdrplay-build', async (req, res) => {
    if (!guard(req, res, true)) return;
    if (!req.body || typeof req.body !== 'object' || Array.isArray(req.body) || Object.keys(req.body).length) return res.status(422).json({ok: false, errors: ['Local adapter build takes no browser arguments.']});
    if (preview) return res.status(409).json({ok: false, errors: ['Local receiver builds are disabled in preview mode.']});
    if (transactionBusy()) return res.status(409).json({ok: false, code: 'BUILD_BUSY',
      errors: ['Another settings transaction is in progress. Wait for its result.']});
    try { const result = await build.start(); res.status(result.state === 'current' ? 200 : 202).json(result); } catch (error) { res.status(409).json({ok: false, errors: [error.message]}); }
  });
  return {busy: build.busy};
}
module.exports = {createMacRspduoBuild, installMacRspduoBuildRoutes, INTENT};
