'use strict';
// Isolated Darwin API path: loopback Suite plus a fixed fake launcher only.
const assert = require('assert/strict');
const fs = require('fs');
const http = require('http');
const net = require('net');
const os = require('os');
const path = require('path');
const {spawn} = require('child_process');
const yaml = require('js-yaml');
const {setupDefaults} = require('./config-store');

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'vectorwarp-mac-kraken-'));
const state = path.join(directory, 'state'), filename = path.join(directory, 'config.yml');
const launcher = path.join(directory, 'launcher'), marker = path.join(directory, 'launcher.json');
const restart = path.join(directory, 'restart');
let api, stderr = '', rejectNext = false;
const suiteState = {frequency: 204640000, channels: 5};
const status = () => ({settings: {center_freq: suiteState.frequency, sample_rate: 2400000, gain: 20},
  num_channels: suiteState.channels, max_elements: 8, operating_mode: 'coherent', reconfiguring:false,
  recovering:false, cooldown_active:false, calibration_state: 'PENDING'});
const suite = net.createServer(socket => {
  socket.setEncoding('utf8'); socket.write(`${JSON.stringify(status())}\n`); let buffer = '';
  socket.on('data', text => { buffer += text; const lines = buffer.split('\n'); buffer = lines.pop();
    for (const line of lines) if (line) { const command = JSON.parse(line);
      if (rejectNext) { rejectNext=false; socket.write(`${JSON.stringify({status:'error',message:'synthetic sync failure'})}\n`); continue; }
      if (command.command === 'set_frequency') { suiteState.frequency = command.frequency; socket.write(`${JSON.stringify({status:'success',frequency:command.frequency})}\n`); }
      if (command.command === 'set_num_elements') { suiteState.channels = command.num_elements; socket.write(`${JSON.stringify({status:'success',num_elements:command.num_elements})}\n`); }
      socket.write(`${JSON.stringify(status())}\n`);
    }});
});
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
async function request(port, method, route, body, revision) {
  const payload = body && JSON.stringify(body);
  return new Promise((resolve, reject) => { const req = http.request({hostname:'127.0.0.1',port,path:route,method,headers:{
    Origin:`http://127.0.0.1:${port}`, ...(payload ? {'Content-Type':'application/json','Content-Length':Buffer.byteLength(payload)} : {}),
    ...(revision ? {'If-Match':`"${revision}"`}: {}), ...(method === 'PUT' ? {'X-VectorWarp-Intent':'config-write-v1','X-VectorWarp-Receiver-Sync':'synchronize-v1'} : {})}}, res => {
    let text='';res.on('data',chunk=>{text+=chunk;});res.on('end',()=>resolve({status:res.statusCode,body:text&&JSON.parse(text)}));});
    req.on('error',reject);if(payload)req.write(payload);req.end(); });
}
async function ready(port) { for(let i=0;i<120;i++){try { const result=await request(port,'GET','/api/config/capabilities');if(result.status===200)return result.body;}catch(_){}await wait(25);} throw Error(stderr); }
(async()=>{ try {
  await new Promise(resolve=>suite.listen(0,'127.0.0.1',resolve));
  fs.mkdirSync(state,{recursive:true});
  fs.writeFileSync(launcher,`#!/bin/sh\nnode -e 'const fs=require("fs");const p=process.env.VECTORWARP_MACOS_KRAKEN_CONFIG;if(process.argv[1]==="start-kraken"){if(process.env.KRAKEN_LAUNCH_FAIL)process.exit(9);fs.writeFileSync(${JSON.stringify(marker)},JSON.stringify({candidate:p,config:fs.readFileSync(p,"utf8")}));}else fs.writeFileSync(${JSON.stringify(marker)},"stopped");' "$1"\n`);
  fs.chmodSync(launcher,0o755);
  const current=setupDefaults(); current.network.ip='127.0.0.1';
  const port=21000+(process.pid%1000); Object.keys(current.network.ports).forEach((key,index)=>{current.network.ports[key]=port+index;});
  fs.writeFileSync(filename,yaml.dump(current));
  const candidate=yaml.load(fs.readFileSync(path.join(__dirname,'..','config','config-kraken.yml'),'utf8'));
  candidate.network=current.network; candidate.capture.device.heimdall.control_port=suite.address().port;
  const platform=path.join(directory,'darwin.js');fs.writeFileSync(platform,'Object.defineProperty(process,"platform",{value:"darwin"});');
  api=spawn(process.execPath,['--require',platform,path.join(__dirname,'server.js'),filename],{env:{...process.env,VECTORWARP_MACOS_LIFECYCLE:'1',VECTORWARP_MACOS_LAUNCHER:launcher,VECTORWARP_MACOS_STATE:state,BLAH2_RECEIVER_TYPES:'Kraken',BLAH2_CONFIG_RESTART_COMMAND:JSON.stringify([process.execPath,'-e',`require("fs").writeFileSync(${JSON.stringify(restart)},"ok")`])},stdio:['ignore','ignore','pipe']});api.stderr.on('data',x=>{stderr+=x;});
  let revision=(await ready(port)).configRevision;
  const accepted=await request(port,'PUT','/api/config?restart=true',candidate,revision);
  assert.equal(accepted.status,200,`${JSON.stringify(accepted.body)} ${stderr}`);
  const launched=JSON.parse(fs.readFileSync(marker,'utf8'));assert.ok(launched.candidate.startsWith(path.join(state,'kraken-controller','candidates')));assert.equal(yaml.load(launched.config).capture.device.type,'Kraken');
  assert.equal(yaml.load(fs.readFileSync(filename,'utf8')).capture.device.type,'Kraken');
  for(let i=0;i<80&&!fs.existsSync(restart);i++)await wait(20);assert.ok(fs.existsSync(restart),'restart command must follow save');
  const candidates=path.join(state,'kraken-controller','candidates');const beforeFailure=fs.readdirSync(candidates).sort();
  // A control/readback failure after the candidate controller starts stops it,
  // preserves YAML, and removes only that new candidate.
  api.kill('SIGTERM'); await new Promise(resolve=>api.once('exit',resolve)); api=null;
  fs.writeFileSync(filename,yaml.dump(current));
  const baseEnv={...process.env,VECTORWARP_MACOS_LIFECYCLE:'1',VECTORWARP_MACOS_LAUNCHER:launcher,VECTORWARP_MACOS_STATE:state,BLAH2_RECEIVER_TYPES:'Kraken'};
  api=spawn(process.execPath,['--require',platform,path.join(__dirname,'server.js'),filename],{env:baseEnv,stdio:['ignore','ignore','pipe']});api.stderr.on('data',x=>{stderr+=x;});revision=(await ready(port)).configRevision;
  const rejected=JSON.parse(JSON.stringify(candidate));rejected.capture.fc+=100000;rejectNext=true;
  const failed=await request(port,'PUT','/api/config?restart=false',rejected,revision);assert.equal(failed.status,409,JSON.stringify(failed.body));assert.equal(fs.readFileSync(marker,'utf8'),'stopped');assert.equal(yaml.load(fs.readFileSync(filename,'utf8')).capture.device.type,current.capture.device.type);
  assert.deepEqual(fs.readdirSync(candidates).sort(),beforeFailure,'failed sync candidate must be removed');
  api.kill('SIGTERM'); await new Promise(resolve=>api.once('exit',resolve)); api=null;
  api=spawn(process.execPath,['--require',platform,path.join(__dirname,'server.js'),filename],{env:{...baseEnv,KRAKEN_LAUNCH_FAIL:'1'},stdio:['ignore','ignore','pipe']});api.stderr.on('data',x=>{stderr+=x;});revision=(await ready(port)).configRevision;
  const launchFailed=await request(port,'PUT','/api/config?restart=false',candidate,revision);assert.equal(launchFailed.status,503);assert.equal(fs.readFileSync(marker,'utf8'),'stopped');
  assert.deepEqual(fs.readdirSync(candidates).sort(),beforeFailure,'failed launch candidate must be removed');
  console.log('macOS Kraken candidate Apply/save/restart and cleanup PASS (loopback Suite).');
} finally {if(api){api.kill('SIGTERM');await new Promise(resolve=>api.once('exit',resolve));}await new Promise(resolve=>suite.close(resolve));fs.rmSync(directory,{recursive:true,force:true});}})().catch(error=>{console.error(error);process.exitCode=1;});
