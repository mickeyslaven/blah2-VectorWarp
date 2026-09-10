'use strict';

// An isolated, no-radio preview. Writes stay in a temporary directory. No
// restart hook, receiver sockets, IQ stream, truth feed or simulated radar data.
const fs = require('fs');
const os = require('os');
const path = require('path');
const yaml = require('js-yaml');
const {setupDefaults} = require('../../api/config-store');
const {getDeviceProfiles} = require('../../api/config-manager');
const config = setupDefaults();
const profile = getDeviceProfiles().find(item => item.type === 'Kraken');
config.capture.fs = profile.sampleRate;
config.capture.device = profile.device;
Object.assign(config.process, profile.process);
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'blah2-ui-preview-'));
const filename = path.join(dir, 'config.yml');
fs.writeFileSync(filename, yaml.dump(config));
process.argv[2] = filename;
process.env.BLAH2_PREVIEW = 'true';
delete process.env.BLAH2_CONFIG_RESTART_COMMAND;
require('../../api/server');
