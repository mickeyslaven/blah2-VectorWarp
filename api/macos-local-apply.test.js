'use strict';
// Real API process and filesystem publications; the compiler/SDK are synthetic.
// Native module/SDK execution is separately checked by the macOS integration tests.
const assert = require('assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const net = require('net');
const crypto = require('crypto');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {setupDefaults} = require('./config-store');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-mac-apply-'));
const root = path.join(directory, 'app'), state = path.join(directory, 'state');
const current = path.join(state, 'adapters/rspduo/current');
const include = path.join(directory, 'sdk/include'), library = path.join(directory, 'sdk/runtime');
const core = path.join(root, 'bin/libblah2-capture-core.dylib');
const moduleFile = path.join(current, 'blah2-receiver-rspduo.dylib');
const receiptFile = path.join(current, 'receipt.json');
const marker = path.join(directory, 'build-started'), restarted = path.join(directory, 'restarted');
const filename = path.join(directory, 'config.yml'), builder = path.join(directory, 'builder');
const hash = value => crypto.createHash('sha256').update(value).digest('hex');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
let child, origin, revision, errors = '';
async function request(method, endpoint, body, intent = 'config-write-v1') {
  const response = await fetch(origin + endpoint, {method, headers: {Origin: origin,
    'Content-Type': 'application/json', 'If-Match': `"${revision}"`,
    'X-VectorWarp-Intent': intent, 'X-VectorWarp-Receiver-Sync': 'synchronize-v1'},
    ...(body === undefined ? {} : {body: JSON.stringify(body)})});
  return {status: response.status, body: await response.json()};
}
async function eventually(check) {
  for (let i = 0; i < 150; i++) { try { const result = await check(); if (result) return result; } catch (_) {} await delay(30); }
  throw new Error(`Timed out: ${errors}`);
}
(async () => {
  try {
    for (const dir of [current, include, path.dirname(core), path.join(root, 'receiver-source/rspduo')]) fs.mkdirSync(dir, {recursive:true});
    fs.writeFileSync(core, 'synthetic core'); fs.writeFileSync(library, 'synthetic SDK runtime');
    fs.writeFileSync(path.join(include, 'sdrplay_api.h'), 'synthetic SDK header');
    fs.writeFileSync(moduleFile, 'synthetic module');
    const manifest = {schema:1, kit_id:'a'.repeat(64), cohort:'b'.repeat(64), core_sha256:hash(fs.readFileSync(core))};
    fs.writeFileSync(path.join(root, 'receiver-source/rspduo/kit.json'), JSON.stringify(manifest));
    const receipt = {...manifest, module_sha256:hash(fs.readFileSync(moduleFile)),
      sdk:{include:hash(fs.readFileSync(path.join(include,'sdrplay_api.h'))), library:hash(fs.readFileSync(library))}};
    fs.writeFileSync(builder, `#!${process.execPath}\nconst fs=require('fs');fs.writeFileSync(${JSON.stringify(marker)},'started');setTimeout(()=>{fs.writeFileSync(${JSON.stringify(receiptFile)},${JSON.stringify(JSON.stringify(receipt))});console.log('built');},1500);\n`);
    fs.chmodSync(builder, 0o755);
    const config = setupDefaults(); config.network.ip = '127.0.0.1';
    const listeners = await Promise.all(Object.keys(config.network.ports).map(() => new Promise(resolve => {
      const server = net.createServer(); server.listen(0,'127.0.0.1',()=>resolve(server));
    })));
    Object.keys(config.network.ports).forEach((key,i)=>{config.network.ports[key]=listeners[i].address().port;});
    await Promise.all(listeners.map(server=>new Promise(resolve=>server.close(resolve))));
    fs.writeFileSync(filename,yaml.dump(config)); origin = `http://127.0.0.1:${config.network.ports.api}`;
    const platform = path.join(directory,'platform.js');
    fs.writeFileSync(platform,'Object.defineProperty(process,"platform",{value:"darwin"});\n');
    child = spawn(process.execPath,['--require',platform,path.join(__dirname,'server.js'),filename],{env:{...process.env,
      BLAH2_PREVIEW:'false', BLAH2_RECEIVER_TYPES:'Usrp,HackRF,Kraken',
      VECTORWARP_MACOS_ROOT:root,VECTORWARP_MACOS_STATE:state,VECTORWARP_MACOS_RSPDUO_BUILD:builder,
      VECTORWARP_MACOS_NODE:process.execPath,BLAH2_SDRPLAY_INCLUDE_DIR:include,BLAH2_SDRPLAY_LIBRARY:library,
      BLAH2_CONFIG_RESTART_COMMAND:JSON.stringify([process.execPath,'-e',`require('fs').writeFileSync(${JSON.stringify(restarted)},'yes')`])},stdio:['ignore','ignore','pipe']});
    child.stderr.on('data',value=>{errors+=value;});
    const capabilities = async () => (await request('GET','/api/config/capabilities')).body;
    const initial = await eventually(async()=>{const c=await capabilities();return c.configRevision&&c;});
    revision = initial.configRevision; assert.deepEqual(initial.localBuildLiveTypes,[]);
    assert.equal((await request('PUT','/api/config?restart=false',config)).body.code,'SDRPLAY_LOCAL_BUILD_REQUIRED');
    assert.equal((await request('POST','/api/sdrplay-build',{})).status,403);
    assert.equal((await request('POST','/api/sdrplay-build',{command:'anything'},'sdrplay-local-build-v1')).status,422);
    const building = request('POST','/api/sdrplay-build',{},'sdrplay-local-build-v1');
    await eventually(()=>fs.existsSync(marker));
    assert.equal((await request('PUT','/api/config?restart=false',config)).status,409,
      'Config writes must not race the local adapter build');
    const built = await building; assert.equal(built.status,200,JSON.stringify(built.body));
    assert.deepEqual((await capabilities()).localBuildLiveTypes,['RspDuo'], 'A build must become applicable without API restart');
    for (const file of [core, library, path.join(include,'sdrplay_api.h'), moduleFile]) {
      const original=fs.readFileSync(file);
      try {fs.writeFileSync(file,'changed');assert.deepEqual((await capabilities()).localBuildLiveTypes,[],file);}
      finally {fs.writeFileSync(file,original);}
    }
    const applied=await request('PUT','/api/config?restart=true',config);
    assert.equal(applied.status,200,JSON.stringify(applied.body));
    await eventually(()=>fs.existsSync(restarted));
    assert.deepEqual((await capabilities()).compiledLiveTypes,['Usrp','HackRF','Kraken']);
    console.log('macOS local RSP build -> capability -> Apply, current hashes, origins and transaction exclusion PASS (synthetic compiler/SDK).');
  } finally {
    if(child){child.kill('SIGTERM');if(child.exitCode===null)await new Promise(resolve=>child.once('exit',resolve));}
    fs.rmSync(directory,{recursive:true,force:true});
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
