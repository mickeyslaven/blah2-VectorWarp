'use strict';

const fs = require('fs');
const os = require('os');
const {execFile} = require('child_process');

const BREW_PATHS = ['/opt/homebrew/bin/brew', '/usr/local/bin/brew'];
const ACTIONS = Object.freeze({
  'macos-install-uhd': {receiverType: 'Usrp', formula: 'uhd', review: 'Homebrew will install the open-source UHD formula.'},
  'macos-install-hackrf': {receiverType: 'HackRF', formula: 'hackrf', review: 'Homebrew will install the open-source HackRF formula.'},
  'macos-start-kraken': {receiverType: 'Kraken', launcher: 'start-kraken',
    review: 'Start the saved, local loopback Kraken controller through the VectorWarp launcher.'}
});
const safeText = value => String(value || '').replace(/[\x00-\x1f\x7f]/g, ' ').slice(0, 1800).trim();

function createMacReceiverManagement({exists = fs.existsSync, run = execFile, home = os.homedir(), environment = process.env} = {}) {
  let running = false;
  const brew = () => BREW_PATHS.find(file => exists(file)) || null;
  const launcher = () => {
    const candidate = environment.VECTORWARP_MACOS_LAUNCHER;
    return typeof candidate === 'string' && candidate.startsWith('/') && exists(candidate) ? candidate : null;
  };
  function discover() {
    const executable = brew();
    const localLauncher = launcher();
    const actions = Object.entries(ACTIONS).flatMap(([id, action]) => {
      if (action.launcher) return localLauncher ? [{id, ...action, kind: 'start-local-controller',
        available: true, ready: true, message: 'Review starting the saved local Kraken controller.'}] : [];
      return executable ? [{id, ...action, kind: 'install-dependency', available: true, ready: false,
        message: `Review Homebrew installation of ${action.formula}.`}] : [];
    });
    if (!executable && !localLauncher) return {ok: true, available: false, actions: [], code: 'HOMEBREW_REQUIRED',
      message: 'Homebrew was not found. Install Homebrew yourself, then recheck receiver software.'};
    return {ok: true, available: actions.length > 0, actions,
      message: 'Reviewed macOS actions use fixed local commands only.'};
  }
  function plan(actionId) {
    const action = ACTIONS[actionId]; const executable = brew(), localLauncher = launcher();
    if (!action || (action.launcher ? !localLauncher : !executable)) return null;
    if (action.launcher) return {ok: true, status: 'ready', actionId, receiverType: action.receiverType,
      review: action.review, commandLabel: 'start saved local Kraken controller', requiresAuthorization: false,
      lifetimeSeconds: 300};
    return {ok: true, status: 'ready', actionId, receiverType: action.receiverType, review: action.review,
      commandLabel: `brew install ${action.formula}`, requiresAuthorization: false, lifetimeSeconds: 300};
  }
  async function execute(actionId) {
    const action = ACTIONS[actionId]; const executable = brew(), localLauncher = launcher();
    if (!action || (action.launcher ? !localLauncher : !executable)) { const error = new Error('This reviewed macOS action is unavailable.'); error.code = 'ACTION_NOT_REVIEWED'; throw error; }
    if (running) { const error = new Error('Another macOS receiver action is running.'); error.code = 'MANAGEMENT_BUSY'; throw error; }
    running = true;
    try {
      if (action.launcher) {
        await new Promise((resolve, reject) => run(localLauncher, [action.launcher], {
          cwd: '/', timeout: 70000, maxBuffer: 65536,
          env: {...environment, PATH: '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin', LANG: 'C', LC_ALL: 'C', HOME: home}
        }, error => error ? reject(error) : resolve()));
        return {ok: true, status: 'complete', message: 'The saved local Kraken controller is ready. Apply receiver settings to synchronize and start processing.'};
      }
      await new Promise((resolve, reject) => run(executable, ['install', action.formula], {
        cwd: '/', timeout: 600000, maxBuffer: 65536,
        env: {PATH: '/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin', LANG: 'C', LC_ALL: 'C', HOME: home,
          HOMEBREW_NO_AUTO_UPDATE: '1', HOMEBREW_NO_INSTALL_CLEANUP: '1', HOMEBREW_NO_AUTOREMOVE: '1'}
      }, error => error ? reject(error) : resolve()));
      return {ok: true, status: 'complete', message: `Homebrew installed ${action.formula}. Recheck receiver software before use.`};
    } catch (error) {
      const detail = safeText(error.stderr || error.message);
      if (action.launcher) {
        const failure = new Error(detail ? `Could not start the saved local Kraken controller: ${detail}` : 'Could not start the saved local Kraken controller.');
        failure.code = 'KRAKEN_CONTROLLER_START_FAILED'; throw failure;
      }
      const failure = new Error(detail ? `Homebrew could not install ${action.formula}: ${detail}` : `Homebrew could not install ${action.formula}.`);
      failure.code = 'HOMEBREW_INSTALL_FAILED'; throw failure;
    } finally { running = false; }
  }
  return {discover, plan, execute, busy: () => running};
}
module.exports = {createMacReceiverManagement, ACTIONS, BREW_PATHS};
