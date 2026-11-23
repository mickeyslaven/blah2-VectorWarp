'use strict';

const {createMultiIlluminatorCausalFrame} = require('./causal-funnel-producer.js');

// Tracker publication arrives either as a legacy product or inside a complete
// illuminator envelope.  Keep the common operational consumers here so their
// admission semantics cannot drift.  The envelope is optional: legacy tracker
// JSON deliberately has no invented lane/capture/diagnostic metadata.
function ingestTrackerFrame({tracker, healthTracker = tracker, channel = 'primary', diagnostics = null,
  resetOnTimestampRegression = false, radarPerformanceHealth,
  evidenceConsumers = [], multiIlluminatorEvidence = null,
  causalFunnelRecorder = null, causalFrame = null, causalMaximum = 32} = {}) {
  let causalAccepted = false;
  if (causalFrame && causalFunnelRecorder?.status?.().api_ingest_enabled) {
    try {
      causalAccepted = causalFunnelRecorder.accept(
        createMultiIlluminatorCausalFrame(causalFrame, causalMaximum));
    } catch (_) {
      // Optional diagnostics must never interrupt radar publication.
    }
  }
  const healthAccepted = radarPerformanceHealth ? radarPerformanceHealth.accept(healthTracker,
    {channel, diagnostics, resetOnTimestampRegression}) : false;
  for (const consumer of evidenceConsumers) consumer.acceptTracker(tracker);
  if (multiIlluminatorEvidence && causalFrame)
    multiIlluminatorEvidence.accept(causalFrame);
  return {healthAccepted, causalAccepted};
}

module.exports = {ingestTrackerFrame};
