'use strict';

// Automatic evidence clips are useful only after the native live processor and
// its RF ring are running.  Keep this policy independent of receiver type and
// acceleration backend; callers provide the two freshness attestations owned
// by the API session.
function automaticCaptureReadiness({processor, radarFrameFresh}) {
  if (!processor || processor.input !== 'live' || processor.state !== 'live' ||
      typeof processor.error === 'string' && processor.error.length > 0 ||
      radarFrameFresh !== true)
    return {ready: false, reason: 'radar-recorder-warming'};
  return {ready: true, reason: null};
}

module.exports = {automaticCaptureReadiness};
