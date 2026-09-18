'use strict';
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const {execFileSync} = require('child_process');
const {createMacRspduoBuild} = require('./macos-rspduo-build');

(async () => {
  const calls = [];
  const env = {VECTORWARP_MACOS_RSPDUO_BUILD: '/opt/vectorwarp/libexec/build-rspduo', VECTORWARP_MACOS_ROOT: '/opt/vectorwarp', VECTORWARP_MACOS_STATE: '/Users/test/Library/Application Support/VectorWarp', VECTORWARP_MACOS_NODE: '/opt/homebrew/bin/node', BLAH2_SDRPLAY_INCLUDE_DIR: '/sdk/include', BLAH2_SDRPLAY_LIBRARY: '/sdk/libsdrplay_api.dylib'};
  const build = createMacRspduoBuild({env, exists: file => ['/opt/vectorwarp/libexec/build-rspduo', '/sdk/include/sdrplay_api.h', '/sdk/libsdrplay_api.dylib'].includes(file), run: (file, args, options, done) => { calls.push({file,args,options}); done(null); }});
  assert.equal(build.status().ok, true);
  await build.start();
  assert.deepEqual(calls[0].args, []);
  assert.equal(calls[0].file, env.VECTORWARP_MACOS_RSPDUO_BUILD);
  assert.equal(calls[0].options.env.BLAH2_SDRPLAY_LIBRARY, env.BLAH2_SDRPLAY_LIBRARY);
  assert.equal(calls[0].options.env.VECTORWARP_MACOS_STATE, env.VECTORWARP_MACOS_STATE);
  assert.equal(createMacRspduoBuild({env: {}, exists: () => false}).status().state, 'unavailable');
  const fixture = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-rsp-status-'));
  try {
    const root = path.join(fixture, 'app'), state = path.join(fixture, 'state'), sdk = path.join(fixture, 'sdk');
    const generation = path.join(state, 'adapters/rspduo/generations/one');
    for (const directory of [path.join(root, 'bin'), path.join(root, 'receiver-source/rspduo'), generation, sdk]) fs.mkdirSync(directory, {recursive: true});
    fs.symlinkSync(generation, path.join(state, 'adapters/rspduo/current'));
    const core = path.join(root, 'bin/libblah2-capture-core.dylib'), moduleFile = path.join(generation, 'blah2-receiver-rspduo.dylib');
    const header = path.join(sdk, 'sdrplay_api.h'), library = path.join(sdk, 'runtime.dylib'), builder = path.join(root, 'builder');
    const receipt = path.join(generation, 'receipt.json');
    for (const file of [core, moduleFile, header, library, builder]) fs.writeFileSync(file, `synthetic ${path.basename(file)}`);
    const sha = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
    const manifest = {schema: 1, kit_id: 'kit', cohort: 'cohort', core_sha256: sha(core)};
    fs.writeFileSync(path.join(root, 'receiver-source/rspduo/kit.json'), JSON.stringify(manifest));
    const original = JSON.stringify({...manifest, module_sha256: sha(moduleFile), sdk: {include: sha(header), library: sha(library)}});
    fs.writeFileSync(receipt, original);
    const actualEnv = {...process.env, VECTORWARP_MACOS_ROOT: root, VECTORWARP_MACOS_STATE: state,
      VECTORWARP_MACOS_RSPDUO_BUILD: builder, BLAH2_SDRPLAY_INCLUDE_DIR: sdk, BLAH2_SDRPLAY_LIBRARY: library};
    // A FIFO regression must time out a disposable child, never hang the suite.
    const status = () => JSON.parse(execFileSync(process.execPath, ['-e',
      'console.log(JSON.stringify(require(process.argv[1]).createMacRspduoBuild().status()))',
      path.join(__dirname, 'macos-rspduo-build.js')], {env: actualEnv, encoding: 'utf8', timeout: 3000}));
    assert.equal(status().state, 'current');
    for (const file of [core, header, library]) {
      const saved = file + '.versioned'; fs.renameSync(file, saved); fs.symlinkSync(saved, file);
      assert.equal(status().state, 'current', 'Installed core and manual SDK symlinks are supported');
      fs.unlinkSync(file); fs.renameSync(saved, file);
    }
    const target = moduleFile + '.target'; fs.renameSync(moduleFile, target); fs.symlinkSync(target, moduleFile);
    assert.equal(status().state, 'missing', 'Published module symlinks must match native gate rejection');
    fs.unlinkSync(moduleFile); fs.renameSync(target, moduleFile);
    fs.writeFileSync(receipt, original + ' '.repeat(8192)); assert.equal(status().state, 'missing'); fs.writeFileSync(receipt, original);
    for (const file of [receipt, moduleFile, core, header, library]) {
      const saved = file + '.saved'; fs.renameSync(file, saved); execFileSync('/usr/bin/mkfifo', [file]);
      assert.equal(status().state, 'missing', `FIFO must be rejected without blocking: ${file}`);
      fs.unlinkSync(file); fs.renameSync(saved, file);
    }
    assert.equal(status().state, 'current');
  } finally { fs.rmSync(fixture, {recursive: true, force: true}); }
  console.log('macOS RSPduo build tests passed; browser data cannot supply commands or SDK paths.');
})().catch(error => { console.error(error); process.exitCode = 1; });
