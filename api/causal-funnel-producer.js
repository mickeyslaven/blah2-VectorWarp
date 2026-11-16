'use strict';
const {encodeUint64} = require('./causal-funnel-contract');

const diagnosticKeys = ['no_op', 'detections', 'tracks_before', 'tracks_after',
  'candidate_track_pairs', 'pair_already_claimed', 'pair_delay_rejected',
  'pair_doppler_rejected', 'pair_innovation_rejected', 'pair_in_gate',
  'associations', 'misses', 'promotions', 'tentative_expired',
  'inactivity_deleted', 'birth_candidates', 'birth_snr_rejected',
  'birth_ridge_rejected', 'birth_exclusive_rejected', 'initiated',
  'weak_support_candidates', 'weak_support_in_gate', 'weak_support_accepted',
  'weak_support_credited_cpis',
  'weak_support_lineage_rejected'];

function createMultiIlluminatorCausalFrame(value, maximum = 32) {
  if (!value || typeof value.id !== 'string' ||
      !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(value.id) ||
      !Number.isSafeInteger(value.timestamp) || value.timestamp < 0 ||
      !Number.isSafeInteger(value.frequency_hz) || value.frequency_hz < 1 ||
      !Number.isSafeInteger(value.sample_rate_hz) || value.sample_rate_hz < 1 ||
      !Number.isSafeInteger(value.first_input_sample) || value.first_input_sample < 0 ||
      !Number.isSafeInteger(value.end_input_sample) ||
      value.end_input_sample < value.first_input_sample ||
      !Number.isInteger(maximum) || maximum < 1 || maximum > 64)
    throw new Error('Invalid multi-illuminator causal identity');
  const detection = value.detections || {};
  const arrays = [detection.delay, detection.doppler, detection.snr];
  if (arrays.some(item => !Array.isArray(item)) ||
      arrays.some(item => item.length !== arrays[0].length))
    throw new Error('Invalid multi-illuminator causal detections');
  const total = arrays[0].length, retained = Math.min(total, maximum);
  const sequence = value.end_input_sample;
  const candidates = Array.from({length: retained}, (_, index) => {
    const delay = Number(detection.delay[index]);
    const doppler = Number(detection.doppler[index]);
    const snr = Number(detection.snr[index]);
    if (![delay, doppler, snr].every(Number.isFinite))
      throw new Error('Invalid multi-illuminator causal detection value');
    return {id: `d${sequence}-${index}`, kind: 'candidate', reason: 'retained',
      state: 0, previous_state: 0, delay_km: delay, doppler_hz: doppler,
      snr_db: snr, bearing_candidates_deg: [], measured: true,
      confirmation_credit: null, support_depth: 1};
  });
  const sourceDiagnostics = value.trackerDiagnostics || {};
  const trackerDiagnostics = {};
  for (const key of diagnosticKeys) if (sourceDiagnostics[key] !== undefined) {
    const field = sourceDiagnostics[key];
    if (key === 'no_op') {
      if (typeof field !== 'boolean') throw new Error('Invalid causal tracker diagnostic');
    } else if (!Number.isSafeInteger(field) || field < 0) {
      throw new Error('Invalid causal tracker diagnostic');
    }
    trackerDiagnostics[key] = field;
  }
  const tracks = Number.isSafeInteger(trackerDiagnostics.tracks_after) ?
    trackerDiagnostics.tracks_after : 0;
  const capture = value.capture_sample_position_available === true;
  const epoch = capture && Number.isSafeInteger(value.capture_epoch) && value.capture_epoch >= 0 ?
    value.capture_epoch : 0;
  const lineage = {epoch: encodeUint64(epoch), generation: encodeUint64(sequence),
    consumed_sequence: encodeUint64(sequence), clock_precision_ns: '1000000'};
  if (capture && Number.isSafeInteger(value.capture_first_input_sample) &&
      Number.isSafeInteger(value.capture_end_input_sample) &&
      value.capture_end_input_sample >= value.capture_first_input_sample) {
    lineage.input_epoch = encodeUint64(epoch);
    lineage.first_sample = encodeUint64(value.capture_first_input_sample);
    lineage.end_sample = encodeUint64(value.capture_end_input_sample);
  }
  return {schema: 'vectorwarp-causal-funnel-frame-v2', source: 'native',
    event_time_ns: (BigInt(value.timestamp) * 1000000n).toString(),
    sequence: encodeUint64(sequence), stage: 'tracker_frame',
    reason: candidates.length ? 'interval' : 'no_candidates', channel: value.id,
    frequency_hz: value.frequency_hz, sample_rate_hz: value.sample_rate_hz,
    lane_first_input_sample: encodeUint64(value.first_input_sample),
    lane_end_input_sample: encodeUint64(value.end_input_sample), lineage,
    coverage: {candidates_total: total, tracks_total: tracks,
      candidates_retained: retained, tracks_retained: 0,
      truncated: retained < total || tracks > 0},
    candidates, tracker_diagnostics: trackerDiagnostics};
}

// A pure, non-blocking adapter for future native stage instrumentation.  It
// owns neither a socket nor radar state: callers supply only contemporaneous
// radar stage/reason facts, and a failed recorder admission is intentionally
// returned to the caller rather than affecting acquisition.
function createCausalFunnelProducer(recorder, {buildId, configFingerprint} = {}) {
  let sequence = 0n;
  const producer = {};
  if (typeof buildId === 'string' && buildId) producer.build_id = buildId;
  if (typeof configFingerprint === 'string' && configFingerprint) producer.config_fingerprint = configFingerprint;
  return Object.freeze({
    identity: Object.freeze({...producer, wired: false}),
    emit({eventTimeNs, stage, reason, candidates = [], lineage, coverage}) {
      let exact;
      try { exact = encodeUint64(eventTimeNs); } catch (_) { return false; }
      const frame = {schema: 'vectorwarp-causal-funnel-frame-v2', source: 'native',
        event_time_ns: exact, sequence: (sequence++).toString(), stage, reason, candidates,
        lineage, ...(coverage ? {coverage} : {}),
        ...(Object.keys(producer).length ? {producer} : {})};
      return recorder.accept(frame);
    }
  });
}

module.exports = {createCausalFunnelProducer, createMultiIlluminatorCausalFrame};
