// Equivalence test for lib/adsb_match.js: it must produce exactly what the
// detection handler in server.js produced before the per-aircraft geometry was
// hoisted out of the detection loop.
//
// referenceMatch() below is that old inline code, copied verbatim apart from
// reading its config values from arguments. Both are run over randomised
// frames, with aircraft that have missing or zero fields, many aircraft placed
// right on a detection so several compete and ties occur, and the outputs are
// compared as serialised JSON, so any difference in a field, a rounding or the
// order of keys fails.
//
// Run with: node test_adsb_match.js

const bistatic = require('./bistatic.js');
const { matchDetections } = require('./lib/adsb_match');

let passed = 0;
let failed = 0;

function check(name, cond, detail) {
  if (cond) {
    passed++;
    console.log(`  PASS: ${name}`);
  } else {
    failed++;
    console.error(`  FAIL: ${name}${detail ? `  <- ${detail}` : ''}`);
  }
}

// The pre-change body of `det.adsb = det.delay.map(...)` in server.js, as it
// stood after e53f528 added position_age.
function referenceMatch(det, aircraft, config) {
  return det.delay.map((delay, idx) => {
    const doppler = det.doppler[idx];
    let bestMatch = null;
    let bestScore = Infinity;
    for (const ac of aircraft) {
      if (!ac.lat || !ac.lon || (!ac.alt_geom && !ac.alt_baro)) continue;
      const expected_delay = bistatic.computeBistaticDelay(ac,
        config.location.rx, config.location.tx);
      const expected_doppler = bistatic.computeBistaticDoppler(ac,
        config.location.rx, config.location.tx, config.capture.fc);
      if (expected_delay === null || expected_doppler === null) continue;
      const delay_err = Math.abs(delay - expected_delay);
      const doppler_err = Math.abs(doppler - expected_doppler);
      const delay_tol = config.truth.adsb.delay_tolerance || 2.0;
      const doppler_tol = config.truth.adsb.doppler_tolerance || 5.0;
      if (delay_err < delay_tol && doppler_err < doppler_tol) {
        const score = delay_err / delay_tol + doppler_err / doppler_tol;
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

// What server.js now calls.
function newMatch(det, aircraft, config) {
  return matchDetections(det.delay, det.doppler, aircraft,
    config.location.rx, config.location.tx, config.capture.fc,
    config.truth.adsb.delay_tolerance || 2.0,
    config.truth.adsb.doppler_tolerance || 5.0);
}

function rng(seed) {
  let s = seed >>> 0;
  return () => {
    s = (s * 1664525 + 1013904223) >>> 0;
    return s / 4294967296;
  };
}

// An aircraft near rx/tx, with each field independently sometimes missing,
// zero or present, since the filters and bistatic.js treat those differently.
function makeAircraft(r, i, rx) {
  const pick = (v) => {
    const roll = r();
    return roll < 0.06 ? undefined : roll < 0.1 ? 0 : v;
  };
  return {
    hex: i.toString(16).padStart(6, '0'),
    lat: pick(rx.latitude + (r() - 0.5) * 1.5),
    lon: pick(rx.longitude + (r() - 0.5) * 1.5),
    alt_geom: r() < 0.5 ? pick(1000 + r() * 39000) : undefined,
    alt_baro: pick(1000 + r() * 39000),
    gs: pick(80 + r() * 420),
    track: pick(r() * 360),
    geom_rate: r() < 0.3 ? (r() - 0.5) * 4000 : undefined,
    // Set by aircraftAtTimestamp() upstream; absent when tar1090 gave no age.
    position_age: r() < 0.3 ? undefined : r() < 0.1 ? 0 : r() * 9.87,
  };
}

function runCase(seed, nAircraft, nDetections, config) {
  const r = rng(seed);
  const { rx, tx } = config.location;
  const aircraft = Array.from({ length: nAircraft }, (_, i) => makeAircraft(r, i, rx));

  // Half the detections sit on a real aircraft's expected position (so they
  // match, often several aircraft at once), the rest are random.
  const delay = [];
  const doppler = [];
  const valid = aircraft.map((ac) => [
    bistatic.computeBistaticDelay(ac, rx, tx),
    bistatic.computeBistaticDoppler(ac, rx, tx, config.capture.fc),
  ]).filter(([d, f]) => d !== null && f !== null);
  for (let k = 0; k < nDetections; k++) {
    if (valid.length && r() < 0.5) {
      const [d, f] = valid[Math.floor(r() * valid.length)];
      delay.push(d + (r() - 0.5) * 3);
      doppler.push(f + (r() - 0.5) * 8);
    } else {
      delay.push(r() * 120);
      doppler.push((r() - 0.5) * 600);
    }
  }
  const det = { timestamp: 1757400000000, delay, doppler, snr: delay.map(() => 10) };
  return {
    want: JSON.stringify(referenceMatch(det, aircraft, config)),
    got: JSON.stringify(newMatch(det, aircraft, config)),
    matched: referenceMatch(det, aircraft, config).filter(Boolean).length,
  };
}

const baseConfig = {
  capture: { fc: 213000000 },
  location: {
    rx: { latitude: -34.9, longitude: 138.6, altitude: 40 },
    tx: { latitude: -34.98, longitude: 138.71, altitude: 600 },
  },
  truth: { adsb: {} },
};

console.log('adsb_match equivalence:');
let totalMatched = 0;
let mismatches = 0;
let firstMismatch = null;
for (let seed = 1; seed <= 300; seed++) {
  const nAircraft = [0, 1, 5, 40, 280][seed % 5];
  const nDetections = [0, 1, 3, 25, 120][Math.floor(seed / 5) % 5];
  const config = seed % 3 === 0
    ? { ...baseConfig, truth: { adsb: { delay_tolerance: 5, doppler_tolerance: 20 } } }
    : baseConfig;
  const res = runCase(seed, nAircraft, nDetections, config);
  totalMatched += res.matched;
  if (res.want !== res.got) {
    mismatches++;
    if (!firstMismatch) firstMismatch = `seed ${seed}`;
  }
}
check('300 randomised frames match the old inline code exactly', mismatches === 0,
  `${mismatches} differ, first at ${firstMismatch}`);
check('the frames exercised real matches, not only nulls', totalMatched > 1000,
  `${totalMatched} matched detections`);

// Ties: two identical aircraft at the same position. The first must win in
// both, which is what `score < bestScore` (strict) gives.
{
  const ac = { lat: -34.7, lon: 138.5, alt_baro: 20000, gs: 300, track: 45 };
  const aircraft = [{ ...ac, hex: 'aaaaaa' }, { ...ac, hex: 'bbbbbb' }];
  const { rx, tx } = baseConfig.location;
  const d = bistatic.computeBistaticDelay(ac, rx, tx);
  const f = bistatic.computeBistaticDoppler(ac, rx, tx, baseConfig.capture.fc);
  const det = { delay: [d + 0.1], doppler: [f - 0.5] };
  const want = referenceMatch(det, aircraft, baseConfig);
  const got = newMatch(det, aircraft, baseConfig);
  check('an exact tie goes to the first aircraft in both', got[0] && got[0].hex === 'aaaaaa'
    && JSON.stringify(want) === JSON.stringify(got));
}

// Speed, for the record: the point of the change.
{
  const r = rng(99);
  const { rx } = baseConfig.location;
  const aircraft = Array.from({ length: 280 }, (_, i) => makeAircraft(r, i, rx));
  const det = {
    delay: Array.from({ length: 50 }, () => r() * 120),
    doppler: Array.from({ length: 50 }, () => (r() - 0.5) * 600),
  };
  const time = (fn) => {
    let best = Infinity;
    for (let k = 0; k < 20; k++) {
      const t0 = process.hrtime.bigint();
      fn(det, aircraft, baseConfig);
      best = Math.min(best, Number(process.hrtime.bigint() - t0) / 1e6);
    }
    return best;
  };
  const a = time(referenceMatch);
  const b = time(newMatch);
  console.log(`  info: 280 aircraft x 50 detections, old ${a.toFixed(2)} ms, new ${b.toFixed(2)} ms per frame`);
}

console.log(`\n${passed} passed, ${failed} failed`);
process.exit(failed === 0 ? 0 : 1);
