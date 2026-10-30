const fs = require('fs');
const path = require('path');
const {spawn} = require('child_process');

function commandFromJson(value, name) {
  if (!value) return null;
  let command;
  try { command = JSON.parse(value); }
  catch (_) { throw new Error(`${name} must be a JSON array of non-empty strings`); }
  if (!Array.isArray(command) || command.length === 0 ||
      command.some(item => typeof item !== 'string' || item.length === 0))
    throw new Error(`${name} must be a JSON array of non-empty strings`);
  return command;
}

// Linux packages explicitly provide BLAH2_CONFIG_RESTART_COMMAND.  A macOS
// local launch only opts in when the launcher set both variables below; this
// keeps a manually run server from acquiring process-control privileges.
function restartCommandFromEnvironment({platform = process.platform, env = process.env} = {}) {
  if (env.BLAH2_CONFIG_RESTART_COMMAND)
    return commandFromJson(env.BLAH2_CONFIG_RESTART_COMMAND, 'BLAH2_CONFIG_RESTART_COMMAND');
  if (platform !== 'darwin' || env.VECTORWARP_MACOS_LIFECYCLE !== '1') return null;
  const launcher = env.VECTORWARP_MACOS_LAUNCHER;
  if (!launcher || !path.isAbsolute(launcher))
    throw new Error('VECTORWARP_MACOS_LAUNCHER must be an absolute path');
  return [launcher, 'restart'];
}

function commandAvailable(command, {accessSync = fs.accessSync, pathValue = process.env.PATH || ''} = {}) {
  if (!command) return false;
  const executable = command[0];
  const candidates = executable.includes(path.sep) ? [executable] : pathValue.split(path.delimiter)
    .filter(Boolean).map(folder => path.join(folder, executable));
  return candidates.some(candidate => {
    try { accessSync(candidate, fs.constants.X_OK); return true; }
    catch (_) { return false; }
  });
}

// A restart helper must return promptly after accepting the request. The API
// deliberately does not kill or supervise it: its parent can be the service
// manager, or the per-user macOS lifecycle launcher.
function launch(command, {onStarted, onComplete, spawnImpl = spawn, setTimeoutImpl = setTimeout} = {}) {
  const [executable, ...args] = command;
  let child;
  try { child = spawnImpl(executable, args, {detached: true, stdio: ['ignore', 'ignore', 'pipe']}); }
  catch (error) { onComplete?.({ok: false, error}); return null; }
  onStarted?.();
  let settled = false;
  let diagnostic = '';
  child.stderr?.on('data', chunk => {
    if (diagnostic.length < 512)
      diagnostic += String(chunk).replace(/[\x00-\x1f\x7f]+/g, ' ').slice(0, 512 - diagnostic.length);
  });
  const timeout = setTimeoutImpl(() => {
    if (settled) return;
    settled = true;
    onComplete?.({ok: false, timeout: true});
  }, 30000);
  timeout.unref?.();
  child.on('error', error => {
    if (settled) return;
    settled = true;
    clearTimeout(timeout);
    onComplete?.({ok: false, error});
  });
  child.on('close', (code, signal) => {
    if (settled) return;
    settled = true;
    clearTimeout(timeout);
    onComplete?.({ok: code === 0, code, signal, diagnostic: diagnostic.trim()});
  });
  child.unref?.();
  return child;
}

module.exports = {commandFromJson, restartCommandFromEnvironment, commandAvailable, launch};
