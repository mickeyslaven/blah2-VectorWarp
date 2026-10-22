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
  {cpiMs = 500, maxBracketMs = 3000, maxRawOffsetS = 1, maxDisagreementKm = .5} = {}) {
  const ordered = (samples || []).filter(sample => finite(sample?.timestamp_ms))
    .sort((a, b) => a.timestamp_ms - b.timestamp_ms)
    .filter((sample, index, values) => !index || sample.timestamp_ms !== values[index - 1].timestamp_ms);
  const reasons = [], cpis = [];
  if (!finite(windowStart) || !finite(windowEnd) || windowEnd <= windowStart || !finite(cpiMs) || cpiMs <= 0)
    reasons.push('invalid-window');
  if (!reasons.length) for (let at = windowStart + cpiMs / 2; at < windowEnd; at += cpiMs) {
    const left = [...ordered].reverse().find(sample => sample.timestamp_ms <= at);
    const right = ordered.find(sample => sample.timestamp_ms >= at);
    if (!left || !right || left === right) { reasons.push('truth-bracket-missing'); break; }
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
  }
  let dataSha256 = null, sidecarSha256 = null;
  if (!reasons.length) {
    [dataSha256, sidecarSha256] = await Promise.all([sha256(filename, io), sha256(metadataFile, io)]);
    if (dataSha256 !== metadata.dataSha256) reasons.push('recording-sha256-mismatch');
  }
  return {valid: !reasons.length, reasons: [...new Set(reasons)], file: filename, metadata_file: metadataFile,
    data_bytes: dataStat?.size ?? null, sidecar_bytes: metadataStat?.size ?? null,
    data_sha256: dataSha256, sidecar_sha256: sidecarSha256, metadata};
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
module.exports = {rawOffset, validateTruthWindow, verifyCaptureIntegrity, writeAdmissionSidecar,
  captureArtifactState};
