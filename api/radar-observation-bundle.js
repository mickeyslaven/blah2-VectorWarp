'use strict';
const {geographicConfig, geographicContext} = require('./replay-geographic-sites');

const finite = value => typeof value !== 'boolean' && value !== null && value !== '' && value !== undefined &&
  Number.isFinite(Number(value));
const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));

function compactTrack(track) {
  if (!track || typeof track !== 'object') return null;
  const state = String(track.state || '').toUpperCase();
  if (!['ACTIVE', 'CONFIRMED', 'COASTING', 'ASSOCIATED'].includes(state) ||
      track.delay === null || track.doppler === null ||
      !finite(track.delay) || !finite(track.doppler) || track.id === undefined || track.id === null ||
      track.delay_sigma_bins !== undefined &&
        (!finite(track.delay_sigma_bins) || Number(track.delay_sigma_bins) < 0))
    return null;
  return {id: String(track.id), state, delay: Number(track.delay), doppler: Number(track.doppler),
    n: Number(track.n) || 0,
    ...(finite(track.delay_sigma_bins) ? {delay_sigma_bins: Number(track.delay_sigma_bins)} : {}),
    ...(track.false_alarm !== undefined ? {false_alarm: clone(track.false_alarm)} : {}),
    ...(track.false_alarm_probability !== undefined ?
      {false_alarm_probability: clone(track.false_alarm_probability)} : {}),
    ...(track.pfa !== undefined ? {pfa: clone(track.pfa)} : {}),
    ...(track.provenance !== undefined ? {provenance: clone(track.provenance)} : {})};
}

function lineage(frame, timestamp) {
  const event = frame?.tracks?.eventTime || frame?.eventTime || null;
  const native = frame?.tracks?.input_lineage || frame?.input_lineage;
  const exact = value => typeof value === 'string' && /^\d+$/.test(value) &&
    BigInt(value) < 2n ** 64n ? value :
    Number.isSafeInteger(value) && value >= 0 ? String(value) : null;
  // Native exact strings take precedence over rounded compatibility fields.
  // Never manufacture an exact sample identity from an unsafe JS integer.
  const positioned = frame?.capture_sample_position_available === true;
  const epoch = native ? native.available === true ? native.captureEpochExact : null :
    event?.epoch ?? (positioned ? frame.capture_epoch : null);
  const first = native ? native.available === true ? native.firstInputSampleExact : null :
    event?.firstSample ?? (positioned ? frame.capture_first_input_sample : null);
  const end = native ? native.available === true ? native.endInputSampleExact : null :
    event?.endSample ?? (positioned ? frame.capture_end_input_sample : null);
  return {epoch: exact(epoch), first_sample: exact(first), end_sample: exact(end),
    frame_timestamp: timestamp,
    event_time: clone(event)};
}

function measurementTime(frame) {
  const event = frame?.tracks?.eventTime || frame?.eventTime || null;
  const uncertaintyNs = event?.uncertaintyNs;
  if (event?.estimateAvailable === true && event?.uncertaintyBounded === true &&
      finite(uncertaintyNs) && Number(uncertaintyNs) >= 0)
    return {basis: 'producer-bounded-event-time', uncertainty_ms: Number(uncertaintyNs) / 1e6,
      source: String(event.source || 'producer-event-time')};
  return {basis: 'producer-unbounded-event-time', uncertainty_ms: null,
    source: String(event?.source || 'unavailable')};
}

function channelObservation({nodeId, serverSessionId, configRevision, id, receiver, transmitter,
    frequencyHz, sampleRateHz, cpiSeconds, frame}) {
  if (typeof frame === 'string') try { frame = JSON.parse(frame); } catch (_) { return null; }
  const tracks = frame?.tracks || frame;
  for (const name of ['sample_rate_hz', 'frequency_hz']) {
    if ([tracks, frame].some(item => item?.[name] !== undefined &&
        (!finite(item[name]) || Number(item[name]) <= 0))) return null;
    if (tracks?.[name] !== undefined && frame?.[name] !== undefined &&
        Number(tracks[name]) !== Number(frame[name])) return null;
  }
  // Tracker delay is in the native product's actual sample bins. The saved
  // process.delay_reference_sample_rate_hz is only a configuration coordinate
  // convention and cannot label published tracker measurements.
  sampleRateHz = tracks?.sample_rate_hz ?? frame?.sample_rate_hz ?? sampleRateHz;
  frequencyHz = tracks?.frequency_hz ?? frame?.frequency_hz ?? frequencyHz;
  const rawTimestamp = tracks?.timestamp ?? frame?.timestamp;
  const timestamp = Number(rawTimestamp);
  if (!nodeId || !serverSessionId || !configRevision || !id || !finite(rawTimestamp) || timestamp <= 0 ||
      !finite(frequencyHz) || Number(frequencyHz) <= 0 ||
      !finite(sampleRateHz) || Number(sampleRateHz) <= 0 ||
      !finite(cpiSeconds) || Number(cpiSeconds) <= 0 ||
      !receiver || !transmitter) return null;
  return {id, source_key: `${nodeId}:${id}`, node_id: nodeId,
    server_session_id: serverSessionId, config_revision: configRevision,
    receiver: clone(receiver), transmitter: clone(transmitter),
    altitude_datum: 'WGS84-ellipsoid',
    frequency_hz: Number(frequencyHz), delay_reference_sample_rate_hz: Number(sampleRateHz),
    cpi_seconds: Number(cpiSeconds),
    measurement_time: measurementTime(frame),
    timestamp, delay_units: 'bins-at-delay-reference-sample-rate',
    doppler_units: 'hz-positive-closing', lineage: lineage(frame, timestamp),
    tracks: {timestamp, data: (Array.isArray(tracks?.data) ? tracks.data : [])
      .map(compactTrack).filter(Boolean)}};
}

function buildObservationBundle({nodeId, serverSessionId, configRevision, config,
    illuminatorFrames, primaryTracks}) {
  const geographic = geographicContext(config);
  config = geographicConfig(config);
  const multi = config?.process?.multi_illuminator;
  const cpiSeconds = Number(config?.process?.data?.cpi);
  let channels = [];
  if (multi?.enabled === true) channels = (multi.illuminators || [])
    .filter(item => item?.enabled === true).map(item => channelObservation({nodeId,
      serverSessionId, configRevision, id: item.id, receiver: config.location?.rx,
      transmitter: item.transmitter, frequencyHz: item.frequency_hz,
      sampleRateHz: item.output_sample_rate_hz, cpiSeconds,
      frame: illuminatorFrames?.latest(item.id)}));
  else channels = [channelObservation({nodeId, serverSessionId, configRevision, id: 'primary',
    receiver: config?.location?.rx, transmitter: config?.location?.tx,
    frequencyHz: config?.capture?.replay?.state === true ?
      config.capture.replay.frequency_hz : config?.capture?.fc,
    sampleRateHz: config?.capture?.replay?.state === true ?
      config.capture.replay.sample_rate_hz : config?.capture?.fs,
    cpiSeconds,
    frame: primaryTracks})];
  return {schema: 'vectorwarp-radar-observation-bundle-v1', node_id: nodeId,
    server_session_id: serverSessionId, config_revision: configRevision,
    generated_at: Date.now(), geographic_provenance: geographic,
    channels: channels.filter(Boolean)};
}

module.exports = {compactTrack, channelObservation, buildObservationBundle};
