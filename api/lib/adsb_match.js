const bistatic = require('../bistatic.js');

/**
 * Match each detection in a frame to its best-fitting ADS-B aircraft.
 *
 * An aircraft's expected bistatic delay and Doppler do not depend on the
 * detection, so they are computed once per aircraft per frame and then
 * compared against every detection. This used to be recomputed inside the
 * detection loop, detections x aircraft times (~280 aircraft on jn1). The
 * result is identical: same functions, same inputs, same aircraft order, and
 * ties still go to the first aircraft to reach the best score.
 *
 * @param {number[]} delays Detection bistatic delays (km).
 * @param {number[]} dopplers Detection Dopplers (Hz), parallel to delays.
 * @param {object[]} aircraft tar1090-style aircraft records.
 * @param {object} rx Receiver {latitude, longitude, altitude}.
 * @param {object} tx Transmitter {latitude, longitude, altitude}.
 * @param {number} fc Centre frequency (Hz).
 * @param {number} delayTol Delay match tolerance (km).
 * @param {number} dopplerTol Doppler match tolerance (Hz).
 * @returns {(object|null)[]} One entry per detection: the best match or null.
 */
function matchDetections(delays, dopplers, aircraft, rx, tx, fc, delayTol, dopplerTol) {
  const candidates = [];
  if (delays.length > 0) {
    for (const ac of aircraft) {
      if (!ac.lat || !ac.lon || (!ac.alt_geom && !ac.alt_baro)) continue;
      const expected_delay = bistatic.computeBistaticDelay(ac, rx, tx);
      const expected_doppler = bistatic.computeBistaticDoppler(ac, rx, tx, fc);
      if (expected_delay === null || expected_doppler === null) continue;
      candidates.push({ ac, expected_delay, expected_doppler });
    }
  }

  return delays.map((delay, idx) => {
    const doppler = dopplers[idx];
    let bestMatch = null;
    let bestScore = Infinity;
    for (const { ac, expected_delay, expected_doppler } of candidates) {
      const delay_err = Math.abs(delay - expected_delay);
      const doppler_err = Math.abs(doppler - expected_doppler);
      if (delay_err < delayTol && doppler_err < dopplerTol) {
        const score = delay_err / delayTol + doppler_err / dopplerTol;
        if (score < bestScore) {
          bestScore = score;
          bestMatch = {
            hex: ac.hex,
            lat: ac.lat,
            lon: ac.lon,
            alt: ac.alt_geom ?? ac.alt_baro,
            gs: ac.gs,
            track: ac.track,
            expected_delay: Math.round(expected_delay * 100) / 100,
            expected_doppler: Math.round(expected_doppler * 100) / 100,
            delay_residual: Math.round((delay - expected_delay) * 100) / 100,
            doppler_residual: Math.round((doppler - expected_doppler) * 100) / 100,
            position_age: ac.position_age === undefined
              ? undefined : Math.round(ac.position_age * 10) / 10
          };
        }
      }
    }
    return bestMatch;
  });
}

module.exports = { matchDetections };
