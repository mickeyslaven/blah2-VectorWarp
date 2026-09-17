'use strict';

const fs = require('fs');
const path = require('path');
const {execFile} = require('child_process');
const INSTALL_COMMAND = 'sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --install-driver';
const ACCESS_COMMAND = 'sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --enable-service-access';
const STATES = new Set(['not-applicable', 'unqualified', 'driver-unavailable', 'driver-unverified', 'compiler-risk',
  'service-access-needed', 'access-configured']);

function isPi(read = fs.readFileSync) {
  for (const filename of ['/sys/firmware/devicetree/base/model', '/proc/device-tree/model']) {
    try { if (read(filename, 'utf8').slice(0, 256).startsWith('Raspberry Pi ')) return true; }
    catch (_) { /* Device-tree is absent on other supported systems. */ }
  }
  return false;
}

function fromRuntime(setup, {acceleration, clutterAcceleration, fresh} = {}) {
  // Runtime telemetry may demonstrate a distro backport works. Never reject a
  // qualified device solely by version or turn package/enumeration into proof.
  if (!setup.pi || !fresh) return setup;
  const ready = value => value?.active === 'vulkan' && value?.state === 'ready';
  const stages = {ambiguity: ready(acceleration), clutter: ready(clutterAcceleration)};
  if (!stages.ambiguity && !stages.clutter) return {...setup, runtimeStages: stages};
  return {...setup, state: stages.ambiguity && stages.clutter ? 'qualified' : 'partially-qualified',
    qualification: 'current-telemetry-generation', runtimeStages: stages,
    message: 'The current connected telemetry generation reports startup-qualified GPU ' +
      (stages.ambiguity && stages.clutter ? 'ambiguity and clutter' : stages.ambiguity ? 'ambiguity only' : 'clutter only') +
      '. This is not an endurance guarantee or a real-time deadline claim; other geometries qualify independently.'};
}

function createGpuSetupStatus({preview = false, pi = isPi(), execute = execFile, now = Date.now} = {}) {
  let cached = null;
  const unavailable = message => ({version: 1, pi, state: 'driver-unverified',
    qualification: 'not-run', message, command: INSTALL_COMMAND, accessCommand: ACCESS_COMMAND});
  return async runtime => {
    if (preview) return unavailable('GPU setup is not probed by the UI preview.');
    if (!cached || now() - cached.at >= 60000) {
      const candidates = [path.resolve(__dirname, '../../../libexec/vectorwarp-gpu-setup'),
        path.resolve(__dirname, '../libexec/vectorwarp-gpu-setup'),
        path.resolve(__dirname, '../script/vectorwarp-gpu-setup')];
      const helper = candidates.find(filename => fs.existsSync(filename));
      cached = {at: now(), promise: !helper ? Promise.resolve(unavailable('Packaged GPU setup helper is missing; update VectorWarp.')) :
        new Promise(resolve => execute('/usr/bin/python3', ['-I', helper, '--status', '--json'],
          {timeout: 8000, maxBuffer: 65536, env: {PATH: '/usr/sbin:/usr/bin:/sbin:/bin', LC_ALL: 'C'}},
          (error, output) => {
            try {
              if (error) throw error;
              const value = JSON.parse(output);
              if (value?.version !== 1 || value.pi !== pi || !STATES.has(value.state) ||
                  value.qualification !== 'not-run' || typeof value.message !== 'string' || value.message.length > 2048)
                throw new Error('Invalid GPU setup response');
              resolve({...value, command: INSTALL_COMMAND, accessCommand: ACCESS_COMMAND});
            } catch (_) { resolve(unavailable('Bounded driver diagnosis is unavailable. Run the local GPU setup check; no GPU qualification was inferred.')); }
          }))};
    }
    const setup = await cached.promise;
    // A setup query can take up to eight seconds. Take the live telemetry
    // snapshot only after it completes, so a reconnect or restart cannot turn
    // a formerly fresh frame into a current qualification.
    const currentRuntime = typeof runtime === 'function' ? runtime() : runtime || {};
    return fromRuntime(setup, currentRuntime);
  };
}

module.exports = {createGpuSetupStatus, fromRuntime, isPi, INSTALL_COMMAND, ACCESS_COMMAND};
