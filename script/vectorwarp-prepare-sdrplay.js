#!/usr/bin/node
'use strict';

// Privileged restart step. Only installed, fixed argv invokes this module.
// Actual saved YAML, not the setup form's defaults, determines whether to act.
const fs = require('fs');
const crypto = require('crypto');
const path = require('path');
const {execFileSync} = require('child_process');
const STATUS = '/run/vectorwarp-sdrplay/status.json';
const ENV = {PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LANG: 'C', LC_ALL: 'C'};

function readRegular(filename) {
  const fd = fs.openSync(filename, fs.constants.O_RDONLY | fs.constants.O_NOFOLLOW | fs.constants.O_NONBLOCK);
  try {
    const info = fs.fstatSync(fd);
    if (!info.isFile() || info.size > 262144) throw new Error('Settings must be a regular file smaller than 256 KiB.');
    const buffer = Buffer.alloc(262145);
    let used = 0, count;
    while (used < buffer.length && (count = fs.readSync(fd, buffer, used, buffer.length - used, null))) used += count;
    if (used > 262144) throw new Error('Settings exceed 256 KiB.');
    return buffer.subarray(0, used);
  } finally { fs.closeSync(fd); }
}

function trusted(filename) {
  for (const original of [path.resolve(filename), fs.realpathSync(filename)]) {
    let current = original;
    while (true) {
      const info = fs.statSync(current);
      if (info.uid !== 0 || info.mode & 0o022) throw new Error('Restart code must be installed root-owned and not writable by other users.');
      if (path.dirname(current) === current) break;
      current = path.dirname(current);
    }
  }
}

function limitedTree(value, parents = new Set(), depth = 0, budget = {left: 4096}) {
  if (--budget.left < 0 || depth > 16 || parents.has(value)) throw new Error('Recursive or excessively complex settings are not supported.');
  if (!value || typeof value !== 'object') return;
  const next = new Set(parents).add(value);
  Object.values(value).forEach(child => limitedTree(child, next, depth + 1, budget));
}

function trustedTree(directory, check = trusted) {
  const root = fs.realpathSync(directory);
  const pending = [directory], seen = new Set();
  while (pending.length) {
    const current = pending.pop();
    check(current);
    const real = fs.realpathSync(current);
    if (real !== root && !real.startsWith(root + path.sep))
      throw new Error('Installed code links outside its reviewed code tree. Reinstall without external module links.');
    if (seen.has(real)) continue;
    seen.add(real);
    if (seen.size > 40000) throw new Error('Installed restart code tree exceeds its review limit.');
    const info = fs.statSync(current);
    if (info.isDirectory()) {
      for (const name of fs.readdirSync(current)) pending.push(path.join(current, name));
    } else if (!info.isFile()) throw new Error('Installed restart code contains a non-regular file.');
  }
}

function prepare(filename, {yaml, validateConfig, journal, start, report = () => {}}) {
  const raw = readRegular(filename);
  const revision = crypto.createHash('sha256').update(raw).digest('hex');
  try {
    const config = yaml.load(raw.toString('utf8'), {schema: yaml.JSON_SCHEMA});
    limitedTree(config);
    if (!config || typeof config !== 'object' || Array.isArray(config)) throw new Error('Save valid receiver settings before restarting.');
    if (config.capture?.device?.type !== 'RspDuo' || config.capture?.replay?.state === true) {
      report({state: 'not-required', configRevision: revision});
      return {state: 'not-required'};
    }
    if (config.capture?.replay?.state !== false || !validateConfig(config).valid)
      throw new Error('Save valid, explicit RSPduo live settings before starting SDRplay.');
    const state = journal.load();
    if (!state.exists || state.reconciliationRequired || !['not-required', 'already-matched', 'synchronized'].includes(state.state) ||
        state.configRevision !== revision)
      throw new Error('Receiver settings are pending or unresolved. Open Settings and Apply them first.');
    const unchanged = () => {
      if (!readRegular(filename).equals(raw) || JSON.stringify(journal.load()) !== JSON.stringify(state))
        throw new Error('Settings changed during SDRplay startup. The vendor service may already be running; processing was not started. Apply again.');
    };
    report({state: 'starting', configRevision: revision, message: 'Checking and starting the installed SDRplay API service…'});
    unchanged();
    const message = start();
    unchanged();
    report({state: 'ready', configRevision: revision, message});
    return {state: 'ready', configRevision: revision};
  } catch (error) {
    report({state: 'failed', configRevision: revision, message: error.message.slice(0, 1800)});
    throw error;
  }
}

function writeReport(value) {
  trusted(path.dirname(STATUS));
  const temporary = `${STATUS}.${crypto.randomBytes(8).toString('hex')}.tmp`;
  try {
    fs.writeFileSync(temporary, JSON.stringify({inProgress: true, ...value, updatedAt: Date.now()}), {flag: 'wx', mode: 0o644});
    fs.renameSync(temporary, STATUS);
  } finally { try { fs.unlinkSync(temporary); } catch (_) {} }
}

function finishReport(previous, exitCode) {
  const failed = exitCode !== '0';
  return {...previous, inProgress: false, state: failed ? 'failed' : 'complete',
    message: failed ? (previous.state === 'failed' ? previous.message :
      'VectorWarp restart failed. Settings remain available; check vectorwarp-restart and processor service logs.') :
      'Restart command finished. Waiting for fresh radar data.'};
}

function verifyInstalledCode(api, helper) {
  for (const target of [__filename, process.execPath, api, path.join(api, 'config-manager.js'),
    path.join(api, 'receiver-journal.js'), path.join(api, 'node_modules/js-yaml'), helper]) trusted(target);
  // Check transitive code and package metadata, not only entrypoint files.
  trustedTree(api);
  trustedTree(path.join(api, '../html/js'));
  trustedTree(path.dirname(helper));
}

function main(args) {
  if (args.length === 2 && args[0] === '--finish' && /^(0|[1-9][0-9]{0,2})$/.test(args[1]) && process.getuid() === 0) {
    trusted(STATUS);
    const previous = JSON.parse(readRegular(STATUS).toString('utf8'));
    writeReport(finishReport(previous, args[1]));
    return;
  }
  if (args.length === 2 && args[0] === '--begin' && process.getuid() === 0) {
    const revision = crypto.createHash('sha256').update(readRegular(args[1])).digest('hex');
    writeReport({state: 'starting', configRevision: revision, message: 'Restarting VectorWarp and checking receiver software…'});
    // This happens before even the existing root API-readiness helper imports
    // js-yaml. Recheck below after the API restart as well.
    verifyInstalledCode(path.resolve(__dirname, '../current/api'), path.join(__dirname, 'vectorwarp-sdrplay-service'));
    return;
  }
  if (args.length !== 3 || process.getuid() !== 0) throw new Error('Use the installed VectorWarp restart service.');
  const [filename, api, helper] = args;
  trusted(path.dirname(path.dirname(filename)));
  verifyInstalledCode(api, helper);
  const yaml = require(path.join(api, 'node_modules/js-yaml'));
  const {validateConfig} = require(path.join(api, 'config-manager.js'));
  const {createReceiverJournal} = require(path.join(api, 'receiver-journal.js'));
  return prepare(filename, {yaml, validateConfig, journal: createReceiverJournal(filename), report: writeReport,
    start: () => {
      try { return execFileSync('/usr/bin/python3', ['-I', helper, 'start'],
        {env: ENV, cwd: '/', timeout: 45000, maxBuffer: 65536, encoding: 'utf8'}).trim(); }
      catch (error) {
        throw new Error(error.stderr?.toString().trim() ||
          'SDRplay startup failed or timed out. Check its local service and https://sdrplay.com/hardware-api/. VectorWarp did not start processing.');
      }
    }});
}

if (require.main === module) {
  try { main(process.argv.slice(2)); }
  catch (error) { console.error(`SDRplay startup: ${error.message}`); process.exitCode = 1; }
}
module.exports = {prepare, readRegular, limitedTree, trusted, trustedTree, finishReport, main};
