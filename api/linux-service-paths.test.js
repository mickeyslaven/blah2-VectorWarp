'use strict';

const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {setupDefaults} = require('./config-store');
const {validateConfig, linuxServicePathPolicy} = require('./config-manager');

const clone = value => JSON.parse(JSON.stringify(value));
const source = setupDefaults();
const policy = linuxServicePathPolicy({VECTORWARP_LINUX_SERVICE: '1'});
const validateLinux = value => validateConfig(value, null, {pathPolicy: policy});

assert.equal(policy.kind, 'systemd-linux');
assert.equal(validateLinux(source).valid, true);

for (const recordingPath of [
  '/srv/vectorwarp-recordings/',
  '/var/lib/vectorwarp/',
  '/var/lib/vectorwarp/recordings-escape/',
  '/var/lib/vectorwarp/recordings/../outside/'
]) {
  const candidate = clone(source); candidate.save.path = recordingPath;
  const result = validateLinux(candidate);
  assert.equal(result.valid, false, recordingPath);
  assert.ok(result.errors.some(error => error.includes('packaged Linux service')),
    JSON.stringify(result.errors));
}

const nested = clone(source);
nested.save.path = '/var/lib/vectorwarp/recordings/site-a/';
assert.equal(validateLinux(nested).valid, true,
  'A nested packaged recording directory remains usable');

for (const replayFile of [
  'relative.blah2iq',
  '/home/operator/replay.blah2iq',
  '/root/replay.blah2iq',
  '/run/user/1000/replay.blah2iq',
  '/tmp/replay.blah2iq',
  '/var/tmp/replay.blah2iq'
]) {
  const candidate = clone(source); candidate.capture.replay.file = replayFile;
  assert.equal(validateLinux(candidate).valid, false, replayFile);
}

for (const replayFile of [
  '/var/lib/vectorwarp/recordings/replay.blah2iq',
  '/srv/radar/replay.blah2iq'
]) {
  const candidate = clone(source); candidate.capture.replay.file = replayFile;
  assert.equal(validateLinux(candidate).valid, true, replayFile);
}

const sourceMode = clone(source);
sourceMode.save.path = '/srv/vectorwarp-recordings/';
sourceMode.capture.replay.file = '/tmp/replay.blah2iq';
assert.equal(validateConfig(sourceMode, null, {pathPolicy: null}).valid, true,
  'Source deployments are not constrained by the packaged systemd sandbox');

assert.equal(linuxServicePathPolicy({}), null);
for (const unit of ['vectorwarp-api.service.in', 'vectorwarp-processor.service.in'])
  assert.match(fs.readFileSync(path.join(__dirname, '..', 'contrib', 'systemd', unit), 'utf8'),
    /^Environment=VECTORWARP_LINUX_SERVICE=1$/m,
    `${unit} must opt into the packaged path contract`);

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-path-policy-'));
try {
  const root = path.join(directory, 'recordings');
  const outside = path.join(directory, 'outside');
  fs.mkdirSync(root); fs.mkdirSync(outside);
  fs.symlinkSync(outside, path.join(root, 'escaped'));
  const candidate = clone(source);
  candidate.save.path = `${path.join(root, 'escaped')}/`;
  const localPolicy = {kind: 'systemd-linux', recordingRoot: root,
    hiddenReplayRoots: []};
  assert.equal(validateConfig(candidate, null, {pathPolicy: localPolicy}).valid, false,
    'An existing symlink must not escape the packaged writable root');
} finally {
  fs.rmSync(directory, {recursive: true, force: true});
}
console.log('Linux service path-policy tests passed.');
