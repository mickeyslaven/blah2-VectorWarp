'use strict';

function controlsAvailable(processor) {
  return processor?.input === 'replay' && processor.replayControlsAvailable === true &&
    ['playing', 'paused', 'draining', 'complete'].includes(processor.state);
}

function updateControl(processor, previous, body) {
  if (!controlsAvailable(processor))
    return {status: 409, error: 'Replay controls are unavailable; start or restart playback.'};
  const action = String(body?.action || '');
  if (action === 'pause' || action === 'play') {
    // A completed production player is still waiting for a new seek revision.
    const restart = action === 'play' && processor.state === 'complete';
    return {control: {...previous, paused: action === 'pause',
      ...(restart ? {seekSample: 0, revision: previous.revision + 1} : {})}};
  }
  if (action !== 'seek' && action !== 'skip')
    return {status: 400, error: 'Choose play, pause, seek, or skip.'};
  const seconds = Number(body?.seconds);
  if (!Number.isFinite(seconds)) return {status: 400, error: 'Replay position must be finite.'};
  const base = action === 'skip' ? processor.positionSamples : 0;
  const target = Math.max(0, Math.min(processor.totalSamples,
    Math.round(base + seconds * processor.sampleRate)));
  return {control: {...previous, seekSample: target, revision: previous.revision + 1}};
}

module.exports = {controlsAvailable, updateControl};
