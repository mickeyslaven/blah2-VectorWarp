'use strict';

// Hardware-independent runtime acceptance.  This deliberately diagnoses only
// sustained missed real-time deadlines; platform code decides whether a safe
// recovery exists (PCI re-enumeration, worker restart, or operator action).
function createPerformanceHealth(cpiSeconds, options = {}) {
  const budgetMs = Number(cpiSeconds) * 1000;
  if (!Number.isFinite(budgetMs) || budgetMs <= 0) throw new Error('CPI budget must be positive.');
  const required = options.requiredConsecutive ?? 3;
  let baselineMs = null;
  let consecutive = 0;
  let samples = 0;
  let latestCpiMs = null;
  let priorDrops = null;
  let dropsIncreasing = false;
  let state = 'warming';

  function accept(value) {
    let frame;
    try { frame = typeof value === 'string' ? JSON.parse(value) : value; }
    catch (_) { return; }
    if (!frame || !Number.isFinite(frame.cpi)) return;
    const ready = frame.acceleration?.state === 'ready' ||
      (!frame.acceleration && Number.isFinite(frame.cpi));
    if (!ready) { state = 'warming'; consecutive = 0; return; }
    latestCpiMs = frame.cpi;
    samples++;
    if (baselineMs === null) baselineMs = latestCpiMs;
    else baselineMs = Math.min(latestCpiMs, baselineMs * 1.001);
    const drops = Array.isArray(frame.captureDroppedSamples) &&
      frame.captureDroppedSamples.every(Number.isFinite) ?
      Math.max(0, ...frame.captureDroppedSamples) : null;
    dropsIncreasing = drops !== null && priorDrops !== null && drops > priorDrops;
    priorDrops = drops;
    const missedDeadline = latestCpiMs > budgetMs * 1.05;
    const severe = latestCpiMs > budgetMs * 1.2;
    const regressed = samples >= required && latestCpiMs > baselineMs * 1.35;
    consecutive = missedDeadline && (severe || regressed || dropsIncreasing) ? consecutive + 1 : 0;
    state = consecutive >= required ? 'degraded' : samples < required ? 'warming' : 'healthy';
  }

  function reset() {
    baselineMs = null; consecutive = 0; samples = 0; latestCpiMs = null;
    priorDrops = null; dropsIncreasing = false; state = 'warming';
  }

  function status(fresh = true) {
    const visibleState = fresh ? state : 'unavailable';
    return {state: visibleState, budgetMs, latestCpiMs, baselineMs,
      consecutiveMisses: consecutive, dropsIncreasing,
      reason: visibleState === 'degraded' ?
        'Processing has persistently missed the configured CPI deadline. Inspect stage timing and platform telemetry; recovery is hardware-specific.' :
        visibleState === 'healthy' ? 'Processing is meeting the configured CPI deadline.' :
        visibleState === 'warming' ? 'Waiting for enough qualified timing frames.' :
        'Current processor timing is unavailable.'};
  }

  return {accept, reset, status};
}

module.exports = {createPerformanceHealth};
