'use strict';

// Regression purpose: retired publishers cannot create a second radar stream;
// upgrades preserve unsupported intent and canonical native bytes.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const net = require('node:net');
const http = require('node:http');
const {spawn} = require('node:child_process');
const yaml = require('js-yaml');
const {FIELD_RULES, RETIRED_FIXED_SETTINGS, validateConfig} = require('./config-manager');
const {configurationForReceiver, readConfig, saveConfig, migrateKnownDefaults} = require('./config-store');
const {RadarSessionStore} = require('./presentation-history');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-publication-retirement-'));
const file = path.join(directory, 'config.yml');
const clone = value => JSON.parse(JSON.stringify(value));
const set = (value, dotted, child) => {
  const keys = dotted.split('.'), leaf = keys.pop();
  for (const key of keys) value = value[key] ||= {};
  value[leaf] = child;
};
const get = (value, dotted) => dotted.split('.').reduce((item, key) => item?.[key], value);
const base = () => {
  const config = configurationForReceiver('Usrp');
  config.network.ip = '127.0.0.1';
  config.save.path = directory + '/';
  return config;
};
const port = () => new Promise((resolve, reject) => {
  const probe = net.createServer();
  probe.once('error', reject);
  probe.listen(0, '127.0.0.1', () => { const value = probe.address().port; probe.close(() => resolve(value)); });
});
const request = (port, route) => new Promise((resolve, reject) => {
  const req = http.get({hostname:'127.0.0.1',port,path:route,timeout:500}, response => {
    let text=''; response.setEncoding('utf8'); response.on('data', chunk => { text+=chunk; });
    response.on('end', () => resolve({status:response.statusCode,body:JSON.parse(text)}));
  });
  req.on('error', reject); req.on('timeout', () => req.destroy(new Error('timeout')));
});

(async () => {
  const native = JSON.stringify({timestamp:1000,n:1,data:[{id:7,state:'TENTATIVE',n:1,delay:9,doppler:20}]});
  const session = new RadarSessionStore({});
  session.acceptProduct('tracker', native);
  assert.equal(session.products.tracker, native, 'canonical publication retains native states and bytes');

  for (const mode of ['external','hybrid']) {
    const config = base();
    config.process.tracker.publication = {mode,event_file:path.join(directory,'events.jsonl')};
    fs.writeFileSync(config.process.tracker.publication.event_file,
      JSON.stringify({track_id:'unqualified',timestamp:Date.now(),detections:[{delay:1,doppler:2}]})+'\n');
    const validation = validateConfig(config);
    assert.equal(validation.valid,false);
    assert(validation.errors.some(message => /publication is retired/.test(message)));
    fs.writeFileSync(file,yaml.dump(config));
    const original=fs.readFileSync(file,'utf8'),document=readConfig(file);
    assert.equal(document.config.process.tracker.publication.mode,mode);
    assert.equal(document.setupRequired,true);
    assert.throws(()=>saveConfig(file,config,document.revision),/publication is retired/);
    assert.equal(fs.readFileSync(file,'utf8'),original,'refusal does not rewrite active policy');
    const repaired=clone(document.config);delete repaired.process.tracker.publication;
    assert.equal(validateConfig(repaired).valid,true,validateConfig(repaired).errors.join('; '));
    saveConfig(file,repaired,document.revision);
    assert.equal(fs.readFileSync(file+'.bak','utf8'),original);

    for (const key of Object.keys(config.network.ports)) config.network.ports[key]=await port();
    fs.writeFileSync(file,yaml.dump(config));
    const child=spawn(process.execPath,['--max-old-space-size=192',path.join(__dirname,'server.js'),file],{
      env:{...process.env,BLAH2_PREVIEW:'true'},stdio:['ignore','ignore','pipe']});
    let errors='';child.stderr.on('data',chunk=>{errors+=chunk;});
    try {
      let ready;
      for(let attempt=0;attempt<100;attempt++){
        if(child.exitCode!==null)throw new Error(errors);
        try{ready=await request(config.network.ports.api,'/api/config');break;}catch(_){}
        await new Promise(resolve=>setTimeout(resolve,50));
      }
      assert(ready,'API remains available to repair retired intent: '+errors);
      assert.equal(ready.body.process.tracker.publication.mode,mode);
      for(const route of ['/api/tracker','/api/tracker/native','/api/radar/observations']){
        const blocked=await request(config.network.ports.api,route);
        assert.equal(blocked.status,503,route);
        assert.match(blocked.body.reason,/publication is retired/);
      }
    }finally{
      child.kill('SIGTERM');
      await new Promise(resolve=>child.once('exit',resolve));
    }
  }
  const config=base();
  const allowed={'save.iq':true,'save.timing':false,'network.ports.config':4003,
    'process.multi_illuminator.input_phase_correction':'off',
    'process.multi_illuminator.doppler_window':'rectangular','process.bearing.candidate_peaks':3};
  for(const [name,value]of Object.entries(allowed))set(config,name,value);
  config.process.tracker.publication={mode:'native',event_file:'/unused'};
  const original=yaml.dump(config);fs.writeFileSync(file,original);fs.chmodSync(file,0o640);
  assert.equal(migrateKnownDefaults(file),true);
  assert.equal(fs.readFileSync(file+'.bak','utf8'),original);
  assert.equal(fs.statSync(file).mode&0o777,0o640);
  assert.equal(migrateKnownDefaults(file),false,'retirement is idempotent');
  const migrated=readConfig(file);
  assert.equal(migrated.validation.valid,true,migrated.validation.errors.join('; '));
  assert.equal(migrated.config.process.tracker.publication,undefined);
  for(const name of Object.keys(RETIRED_FIXED_SETTINGS)){
    assert.equal(FIELD_RULES[name],undefined,'not advertised: '+name);
    assert.equal(get(migrated.config,name),undefined,'removed: '+name);
    const invalid=clone(migrated.config);set(invalid,name,'unsupported');
    const validation=validateConfig(invalid);
    assert.equal(validation.valid,false,name);
    assert(validation.errors.some(message=>message.includes(name)),name);
    fs.writeFileSync(file,yaml.dump(invalid));
    assert.equal(get(readConfig(file).config,name),'unsupported','unsupported intent must remain visible');
    assert.equal(migrateKnownDefaults(file),false,'cannot erase unsupported alternative');
  }
  console.log('Canonical native publication, active-policy refusal, API interlock and atomic fixed-setting retirement passed.');
})().catch(error=>{console.error(error);process.exitCode=1;})
  .finally(()=>fs.rmSync(directory,{recursive:true,force:true}));
