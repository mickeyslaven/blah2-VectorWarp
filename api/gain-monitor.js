'use strict';

const {enabled, validateProbe, LIMITS} = require('./automatic-gain');

const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));

function eligible(config) {
  const gain = config?.capture?.device?.heimdall?.gain;
  return enabled(config) && typeof gain === 'number' && Number.isFinite(gain) &&
    gain >= 0 && gain <= 50 && Math.round(gain * 10) === gain * 10;
}

/**
 * Observe delivered calibrated wire headroom. This module never controls hardware.
 * The supplied measureGain owns bounded worker I/O; onOverload owns the normal
 * settings transaction and must check the supplied revision when acquiring it.
 */
function createGainMonitor({getConfig, getRevision, busy, measureGain, onOverload,
  report = () => {}, intervalMs = 60000, setTimeoutImpl = setTimeout,
  clearTimeoutImpl = clearTimeout} = {}) {
  if (![getConfig, getRevision, busy, measureGain, onOverload, report, setTimeoutImpl, clearTimeoutImpl]
    .every(value => typeof value === 'function') || !Number.isFinite(intervalMs) ||
    intervalMs < 1 || intervalMs > 0x7fffffff)
    throw new TypeError('Gain monitoring requires callbacks and a positive bounded interval.');

  let running = false, active = false, triggered = false, timer = null, controller = null, generation = 0;
  let unsafeKey = null, probes = [];

  function status() {
    return {running, active, triggered, consecutiveUnsafe: probes.length, intervalMs};
  }
  function emit(event) {
    // Observability must not change whether a hardware action is authorized.
    try { Promise.resolve(report({...event, monitor: status()})).catch(() => {}); }
    catch (_) { /* A reporter is not a safety or control dependency. */ }
  }
  function reset() { unsafeKey = null; probes = []; }
  function snapshot() {
    const config = clone(getConfig()), revision = clone(getRevision());
    if (revision === undefined || revision === null)
      throw new Error('Gain monitor requires a known configuration revision.');
    // The capture tuple protects against mutation even if a caller fails to
    // advance its revision. The revision also covers non-capture settings.
    return {config, revision, generation, key: JSON.stringify([revision, config?.capture])};
  }
  function current(expected) {
    if (!running || triggered) return 'stopped';
    if (expected.generation !== generation) return 'stopped';
    if (busy()) return 'busy';
    const now = snapshot();
    if (!eligible(now.config)) return 'disabled';
    return now.key === expected.key ? null : 'configuration-changed';
  }
  function clearTimer() {
    if (timer !== null) clearTimeoutImpl(timer);
    timer = null;
  }
  function schedule() {
    if (!running || triggered || active || timer !== null) return;
    timer = setTimeoutImpl(() => { timer = null; return run(); }, intervalMs);
    timer?.unref?.();
  }
  async function run() {
    if (!running || triggered || active) return;
    active = true;
    let before;
    try {
      before = snapshot();
      if (!eligible(before.config) || busy()) {
        reset(); emit({status: 'skipped', reason: !eligible(before.config) ? 'disabled' : 'busy'});
        return;
      }
      if (unsafeKey !== before.key) reset();
      controller = new AbortController();
      const measured = await measureGain(clone(before.config), {captureSeconds: LIMITS.probeSeconds,
        timeoutMs: LIMITS.probeTimeoutMs, signal: controller.signal});
      let stale = current(before);
      if (stale) { reset(); if (stale !== 'stopped') emit({status: 'skipped', reason: stale}); return; }
      // Tuple/metrics failures are measurement errors, never overload evidence.
      const probe = validateProbe(measured, before.config);
      if (probe.safe) {
        reset(); emit({status: 'safe', revision: before.revision, probe}); return;
      }
      unsafeKey = before.key;
      probes.push(clone(probe));
      emit({status: 'unsafe', revision: before.revision, probe});
      if (probes.length < 2) return;
      // report() may synchronously change settings; recheck at the action boundary.
      stale = current(before);
      if (stale) { reset(); if (stale !== 'stopped') emit({status: 'skipped', reason: stale}); return; }
      triggered = true; running = false; clearTimer();
      const action = onOverload({config: clone(before.config), revision: clone(before.revision), probes: clone(probes)});
      emit({status: 'overload', revision: before.revision});
      await action;
    } catch (error) {
      reset();
      if (running || triggered) emit({status: 'error', revision: before?.revision,
        phase: triggered ? 'onOverload' : 'probe',
        error: {code: error.code || 'GAIN_MONITOR_FAILED', message: String(error.message || error)}});
    } finally {
      controller = null; active = false; schedule();
    }
  }
  function start() {
    if (triggered || running) return;
    ++generation; running = true; schedule();
  }
  function stop() {
    ++generation; running = false; clearTimer(); reset(); controller?.abort();
  }
  return {start, stop, status};
}

module.exports = {createGainMonitor};
