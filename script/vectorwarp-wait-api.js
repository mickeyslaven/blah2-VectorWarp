#!/usr/bin/env node
'use strict';

// Readiness probe used only by the root-owned restart oneshot. Paths are fixed
// in its unit; no value from the web request becomes a command or argument.
const fs = require('fs');
const http = require('http');
const net = require('net');
const path = require('path');

if (process.argv.length !== 4) {
  console.error('usage: vectorwarp-wait-api.js CONFIG MODULE_DIRECTORY');
  process.exit(64);
}
const yaml = require(path.join(process.argv[3], 'js-yaml'));
const config = yaml.load(fs.readFileSync(process.argv[2], 'utf8'), {schema: yaml.JSON_SCHEMA});
const configuredHost = net.isIP(config?.network?.ip || '') ? config.network.ip : '0.0.0.0';
const host = configuredHost === '0.0.0.0' ? '127.0.0.1' : configuredHost === '::' ? '::1' : configuredHost;
const configuredPort = config?.network?.ports?.api;
const port = Number.isInteger(configuredPort) && configuredPort > 0 && configuredPort <= 65535 ?
  configuredPort : 3000;
const deadline = Date.now() + 20000;

function probe() {
  let settled = false;
  const again = () => {
    if (settled) return;
    settled = true;
    retry();
  };
  const request = http.get({host, port, path: '/api/system/status', timeout: 1000}, response => {
    response.resume();
    if (response.statusCode === 200) return process.exit(0);
    again();
  });
  request.on('timeout', () => request.destroy());
  request.on('error', again);
}
function retry() {
  if (Date.now() >= deadline) {
    console.error(`VectorWarp API was not ready at http://${host}:${port} within 20 seconds`);
    process.exit(1);
  }
  setTimeout(probe, 200);
}
probe();
