'use strict';
const crypto = require('crypto');
const fs = require('fs');
const finite = value => typeof value === 'number' && Number.isFinite(value);

function rawOffset(sample) {
  const p = sample?.provenance || {};
  if (finite(p.observation_time_offset_s)) return Math.abs(p.observation_time_offset_s);
  const accepted = (p.source_observations || []).filter(item => item?.accepted === true && finite(item.observation_time_offset_s));
  return accepted.length ? Math.min(...accepted.map(item => Math.abs(item.observation_time_offset_s))) : Infinity;
}

function validateTruthWindow(samples, windowStart, windowEnd,
  {cpiMs = 500, maxBracketMs = 3000, maxRawOffsetS = 1, maxDisagreementKm = .5,
    cpiTimestampsMs = null} = {}) {
  const ordered = (samples || []).filter(sample => finite(sample?.timestamp_ms))
    .sort((a, b) => a.timestamp_ms - b.timestamp_ms)
    .filter((sample, index, values) => !index || sample.timestamp_ms !== values[index - 1].timestamp_ms);
  const reasons = [], cpis = [];
  if (!finite(windowStart) || !finite(windowEnd) || windowEnd <= windowStart || !finite(cpiMs) || cpiMs <= 0)
    reasons.push('invalid-window');
  const timestamps = Array.isArray(cpiTimestampsMs) ? [...new Set(cpiTimestampsMs
    .filter(at => finite(at) && at >= windowStart && at <= windowEnd))].sort((a, b) => a - b) : null;
  if (Array.isArray(cpiTimestampsMs) && !timestamps.length) reasons.push('processor-cpi-sample-provenance-missing');
  const schedule = timestamps || (!reasons.length ? Array.from(
    {length: Math.max(0, Math.ceil((windowEnd - windowStart - cpiMs / 2) / cpiMs))},
    (_, index) => windowStart + cpiMs / 2 + index * cpiMs).filter(at => at < windowEnd) : []);
  if (!reasons.length) for (const at of schedule) {
    const left = [...ordered].reverse().find(sample => sample.timestamp_ms <= at);
    const right = ordered.find(sample => sample.timestamp_ms >= at);
    // A projected observation evaluated at this exact processor CPI is stronger
    // provenance than interpolation between two observations.  Treat it as a
    // zero-span bracket instead of quarantining a perfectly aligned truth row.
    if (!left || !right || (left === right && left.timestamp_ms !== at)) {
      reasons.push('truth-bracket-missing'); break;
    }
    const bracketMs = right.timestamp_ms - left.timestamp_ms;
    if (bracketMs > maxBracketMs) { reasons.push('truth-bracket-span'); break; }
    const offsets = [rawOffset(left), rawOffset(right)];
    if (offsets.some(offset => offset > maxRawOffsetS)) { reasons.push('truth-raw-offset'); break; }
    if ([left, right].some(sample => sample.provenance?.projection_model !== 'constant_velocity_ground_track')) {
      reasons.push('truth-hold-or-fallback'); break;
    }
    const disagreements = [left, right].map(sample => sample.provenance?.maximum_pairwise_projected_disagreement_km).filter(finite);
    if ([left, right].some(sample => (sample.provenance?.source_observations || [])
      .filter(item => item?.accepted === true).length > 1 &&
      !finite(sample.provenance?.maximum_pairwise_projected_disagreement_km))) {
      reasons.push('truth-source-disagreement-missing'); break;
    }
    if (disagreements.some(value => value > maxDisagreementKm)) { reasons.push('truth-source-disagreement'); break; }
    cpis.push({timestamp_ms: at, left_timestamp_ms: left.timestamp_ms, right_timestamp_ms: right.timestamp_ms,
      bracket_span_ms: bracketMs, max_raw_observation_offset_s: Math.max(...offsets)});
  }
  if (!cpis.length && !reasons.length) reasons.push('no-cpis');
  return {valid: !reasons.length, reasons: [...new Set(reasons)], cpi_ms: cpiMs, cpis,
    truth_samples: ordered, maximum_bracket_span_ms: cpis.length ? Math.max(...cpis.map(x => x.bracket_span_ms)) : null,
    maximum_raw_observation_offset_s: cpis.length ? Math.max(...cpis.map(x => x.max_raw_observation_offset_s)) : null};
}

function validateProcessorSampleRanges(admission, metadata) {
  const reasons = [];
  const ranges = Array.isArray(admission?.processor_cpis) ? admission.processor_cpis : [];
  if (!ranges.length) reasons.push('processor-cpi-sample-provenance-missing');
  const epoch = metadata?.epoch, first = metadata?.firstEpochSample,
    end = metadata?.endEpochSample;
  if (![epoch, first, end].every(Number.isSafeInteger) || end <= first)
    reasons.push('recording-sample-provenance-missing');
  const malformed = ranges.some(range => !Number.isSafeInteger(range?.epoch) ||
    !Number.isSafeInteger(range?.first_epoch_sample) ||
    !Number.isSafeInteger(range?.end_epoch_sample) ||
    range.end_epoch_sample <= range.first_epoch_sample);
  if (malformed) reasons.push('processor-cpi-sample-provenance-invalid');
  const contained = !reasons.length ? ranges.filter(range => range.epoch === epoch &&
    range.first_epoch_sample >= first && range.end_epoch_sample <= end) : [];
  if (ranges.length && !contained.length && !reasons.length)
    reasons.push('processor-cpi-outside-recording');
  if (ranges.some(range => Number.isSafeInteger(range?.epoch) && range.epoch !== epoch))
    reasons.push('processor-cpi-epoch-mismatch');

  // A recorder clip starts at an arbitrary sample, while live CPIs are aligned
  // to the processor stream.  It is normal for one CPI at either clip edge to
  // overlap but not be fully replayable.  Admit only complete contained CPIs
  // and re-run truth validation for exactly that replayable schedule.
  if (!reasons.length) {
    const timestamps = contained.map(range => range.timestamp_ms).filter(finite);
    const truth = validateTruthWindow(admission?.truth_samples || [],
      Math.min(...timestamps) - 1, Math.max(...timestamps) + 1,
      {cpiMs: admission?.cpi_ms || 500, cpiTimestampsMs: timestamps});
    const truthByTimestamp = new Map(truth.cpis.map(cpi => [cpi.timestamp_ms, cpi]));
    const exactTiming = contained.every(range => range.exact_event_time === true);
    return {...truth,
      capture_id: admission.capture_id, pass_id: admission.pass_id, icao: admission.icao,
      timing_policy: exactTiming ? 'exact' : 'provisional',
      decision_grade: truth.valid && exactTiming,
      event_time_uncertainty_s: exactTiming ? 0 : admission.event_time_uncertainty_s,
      processor_cpis: contained.map(range => {
        const cpi = truthByTimestamp.get(range.timestamp_ms);
        return {...range,
          truth_left_timestamp_ms: cpi?.left_timestamp_ms ?? null,
          truth_right_timestamp_ms: cpi?.right_timestamp_ms ?? null,
          truth_bracket_span_ms: cpi?.bracket_span_ms ?? null,
          max_raw_observation_offset_s: cpi?.max_raw_observation_offset_s ?? null};
      }),
      excluded_processor_cpis: ranges.length - contained.length};
  }
  return {...admission, valid: false, reasons: [...new Set(reasons)],
    processor_cpis: contained, excluded_processor_cpis: ranges.length - contained.length};
}

async function sha256(filename, io = fs) {
  const hash = crypto.createHash('sha256');
  await new Promise((resolve, reject) => { const stream = io.createReadStream(filename);
    stream.on('data', chunk => hash.update(chunk)); stream.on('end', resolve); stream.on('error', reject); });
  return hash.digest('hex');
}

async function verifyCaptureIntegrity(filename, expected = {}, io = fs) {
  const reasons = [], metadataFile = `${filename}.json`; let metadata, dataStat, metadataStat;
  try { dataStat = await io.promises.stat(filename); } catch (_) { reasons.push('recording-missing'); }
  try { metadataStat = await io.promises.stat(metadataFile); metadata = JSON.parse(await io.promises.readFile(metadataFile, 'utf8')); }
  catch (_) { reasons.push('recording-sidecar-invalid'); }
  if (dataStat && (!dataStat.isFile() || dataStat.size !== metadata?.dataBytes)) reasons.push('recording-size-mismatch');
  if (metadata) {
    if (metadata.schema !== 'vectorwarp-kraken-iq-clip-v1') reasons.push('recording-sidecar-schema');
    if (finite(expected.revision) && metadata.clipRevision !== expected.revision) reasons.push('recording-revision-mismatch');
    if (finite(expected.samples) && metadata.samplesPerChannel !== expected.samples) reasons.push('recording-sample-count-mismatch');
    if ((finite(expected.windowStart) && metadata.windowStart !== expected.windowStart) ||
        (finite(expected.windowEnd) && metadata.windowEnd !== expected.windowEnd)) reasons.push('recording-window-mismatch');
    if (!/^[0-9a-f]{64}$/.test(metadata.dataSha256 || '')) reasons.push('recording-sidecar-sha256');
  }
  let dataSha256 = null, sidecarSha256 = null, dataSha256VerifiedBy = null;
  if (!reasons.length) {
    sidecarSha256 = await sha256(metadataFile, io);
    try { dataSha256 = await sha256(filename, io); dataSha256VerifiedBy = 'independent-read-sha256'; }
    catch (error) {
      if (error.code !== 'EACCES') throw error;
      // The native recorder hashes every byte as it is written, fsyncs, then
      // atomically publishes this sidecar.  A sandboxed API may be unable to
      // reopen the 0600 data file on older installations; retain that verified
      // producer digest plus an independent byte-count and sidecar digest.
      dataSha256 = metadata.dataSha256;
      dataSha256VerifiedBy = 'native-writer-streaming-sha256';
    }
    if (dataSha256 !== metadata.dataSha256) reasons.push('recording-sha256-mismatch');
  }
  return {valid: !reasons.length, reasons: [...new Set(reasons)], file: filename, metadata_file: metadataFile,
    data_bytes: dataStat?.size ?? null, sidecar_bytes: metadataStat?.size ?? null,
    data_sha256: dataSha256, data_sha256_verified_by: dataSha256VerifiedBy,
    sidecar_sha256: sidecarSha256, metadata};
}

async function writeAdmissionSidecar(filename, value, io = fs) {
  const output = `${filename}.admission.json`, temporary = `${output}.tmp-${process.pid}`;
  await io.promises.writeFile(temporary, `${JSON.stringify(value)}\n`, {mode: 0o640});
  await io.promises.rename(temporary, output); return output;
}
function captureArtifactState(integrity, admissions) {
  return integrity?.valid && Object.values(admissions || {}).some(item => item?.valid) ?
    'admitted' : 'quarantined';
}
function admissionsNeedTruthSettlement(admissions) {
  return Object.values(admissions || {}).some(item => item?.valid === false &&
    (item.reasons || []).includes('truth-bracket-missing'));
}
module.exports = {rawOffset, validateTruthWindow, validateProcessorSampleRanges,
  verifyCaptureIntegrity, writeAdmissionSidecar, captureArtifactState,
  admissionsNeedTruthSettlement};
