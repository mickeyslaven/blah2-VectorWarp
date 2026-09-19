const assert = require('assert');
const {EventEmitter} = require('events');
const lifecycle = require('./lifecycle');

assert.deepEqual(lifecycle.restartCommandFromEnvironment({platform: 'linux', env: {
  BLAH2_CONFIG_RESTART_COMMAND: '["/fixed/restart", "go"]'}}), ['/fixed/restart', 'go']);
assert.equal(lifecycle.restartCommandFromEnvironment({platform: 'linux', env: {}}), null);
assert.deepEqual(lifecycle.restartCommandFromEnvironment({platform: 'darwin', env: {
  VECTORWARP_MACOS_LIFECYCLE: '1', VECTORWARP_MACOS_LAUNCHER: '/Applications/VectorWarp/vectorwarp-macos'}}),
['/Applications/VectorWarp/vectorwarp-macos', 'restart']);
assert.throws(() => lifecycle.restartCommandFromEnvironment({platform: 'darwin', env: {
  VECTORWARP_MACOS_LIFECYCLE: '1', VECTORWARP_MACOS_LAUNCHER: 'vectorwarp-macos'}}), /absolute path/);
assert.throws(() => lifecycle.restartCommandFromEnvironment({env: {BLAH2_CONFIG_RESTART_COMMAND: '["ok", 3]'}}), /JSON array/);
assert.equal(lifecycle.commandAvailable([process.execPath]), true);
assert.equal(lifecycle.commandAvailable(['/definitely/not/a/command']), false);

const child = new EventEmitter();
child.stderr = new EventEmitter(); child.unref = () => {};
let started = 0, result;
lifecycle.launch(['/fixed/restart'], {spawnImpl(executable, args, options) {
  assert.equal(executable, '/fixed/restart'); assert.deepEqual(args, []); assert.equal(options.detached, true);
  return child;
}, onStarted() { started += 1; }, onComplete(value) { result = value; }});
child.stderr.emit('data', Buffer.from('brief diagnostic\n'));
child.emit('close', 0, null);
assert.equal(started, 1); assert.deepEqual(result, {ok: true, code: 0, signal: null, diagnostic: 'brief diagnostic'});
console.log('Lifecycle command parsing and detached restart tests passed.');
