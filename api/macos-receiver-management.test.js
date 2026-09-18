'use strict';
const assert = require('assert/strict');
const {createMacReceiverManagement} = require('./macos-receiver-management');

(async () => {
  const calls = [];
  const manager = createMacReceiverManagement({home: '/Users/local test', exists: file => file === '/opt/homebrew/bin/brew', run: (file, args, options, done) => { calls.push({file, args, options}); done(null); }});
  assert.equal(manager.discover().available, true);
  assert.equal(manager.plan('macos-install-uhd').commandLabel, 'brew install uhd');
  assert.equal(manager.plan('macos-install-uhd').requiresAuthorization, false);
  assert.equal(manager.plan('macos-install-anything'), null);
  await manager.execute('macos-install-hackrf');
  assert.deepEqual(calls[0].args, ['install', 'hackrf']);
  assert.equal(calls[0].file, '/opt/homebrew/bin/brew');
  assert.equal(calls[0].options.cwd, '/');
  assert.equal(calls[0].options.maxBuffer, 65536);
  assert.equal(calls[0].options.env.HOME, '/Users/local test');
  assert.equal(calls[0].options.env.HOMEBREW_NO_AUTO_UPDATE, '1');
  assert.equal(calls[0].options.env.HOMEBREW_NO_INSTALL_CLEANUP, '1');
  assert.equal(calls[0].options.env.HOMEBREW_NO_AUTOREMOVE, '1');
  assert.equal(calls[0].options.env.NODE_OPTIONS, undefined);
  const localCalls = [];
  const local = createMacReceiverManagement({home: '/Users/local test',
    environment: {VECTORWARP_MACOS_LAUNCHER: '/Applications/VectorWarp/vectorwarp-macos'},
    exists: file => file === '/Applications/VectorWarp/vectorwarp-macos',
    run: (file, args, options, done) => { localCalls.push({file, args, options}); done(null); }});
  assert.deepEqual(local.discover().actions.map(action => action.id), ['macos-start-kraken']);
  assert.equal(local.plan('macos-start-kraken').commandLabel, 'start saved local Kraken controller');
  await local.execute('macos-start-kraken');
  assert.equal(localCalls[0].file, '/Applications/VectorWarp/vectorwarp-macos');
  assert.deepEqual(localCalls[0].args, ['start-kraken']);
  assert.equal(localCalls[0].options.env.PATH, '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin');
  const failedLocal = createMacReceiverManagement({
    environment: {VECTORWARP_MACOS_LAUNCHER: '/Applications/VectorWarp/vectorwarp-macos'},
    exists: file => file === '/Applications/VectorWarp/vectorwarp-macos',
    run: (_file, _args, _options, done) => { const error = new Error('adapter readiness timed out'); error.stderr = 'adapter readiness timed out'; done(error); }
  });
  await assert.rejects(failedLocal.execute('macos-start-kraken'), error => {
    assert.equal(error.code, 'KRAKEN_CONTROLLER_START_FAILED');
    assert.match(error.message, /^Could not start the saved local Kraken controller: adapter readiness timed out$/);
    return true;
  });
  const missing = createMacReceiverManagement({exists: () => false});
  assert.equal(missing.discover().code, 'HOMEBREW_REQUIRED');
  console.log('macOS receiver management tests passed; only fixed local commands are callable.');
})().catch(error => { console.error(error); process.exitCode = 1; });
