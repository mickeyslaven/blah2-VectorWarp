'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('path');
const {configuredApiPort, defaultRegistryPath} = require('./node-federation');

test('federation uses the same configured API port as the server', () => {
  assert.equal(configuredApiPort({network: {ports: {api: 43821}}}, {}), 43821);
  assert.equal(configuredApiPort({network: {ports: {api: 43821}}},
    {BLAH2_SETUP_PORT: '45123'}), 45123);
  assert.equal(configuredApiPort({}, {}), 3000);
});

test('registry defaults are writable per-user paths on desktop platforms', () => {
  const home = path.join(path.sep, 'home', 'operator');
  assert.equal(defaultRegistryPath({}, 'darwin', home, 501),
    path.join(home, 'Library', 'Application Support', 'VectorWarp', 'nodes.json'));
  assert.equal(defaultRegistryPath({APPDATA: 'C:\\Users\\operator\\AppData\\Roaming'},
    'win32', 'C:\\Users\\operator', null),
  path.join('C:\\Users\\operator\\AppData\\Roaming', 'VectorWarp', 'nodes.json'));
  assert.equal(defaultRegistryPath({XDG_CONFIG_HOME: path.join(home, '.local-config')},
    'linux', home, 1000), path.join(home, '.local-config', 'vectorwarp', 'nodes.json'));
  assert.equal(defaultRegistryPath({}, 'linux', home, 0), '/etc/vectorwarp/nodes.json');
});

test('explicit registry path wins on every platform', () => {
  assert.equal(defaultRegistryPath({VECTORWARP_NODE_REGISTRY: '/srv/radar/nodes.json'},
    'darwin', '/Users/operator', 501), '/srv/radar/nodes.json');
});
