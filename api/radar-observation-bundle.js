'use strict';

const finite = value => value !== null && value !== '' && value !== undefined &&
  Number.isFinite(Number(value));
const clone = value => value === undefined ? undefined : JSON.parse(JSON.stringify(value));

function compactTrack(track) {
  if (!track || typeof track !== 'object') return null;
  const state = String(track.state || '').toUpperCase();
  if (!['ACTIVE', 'CONFIRMED', 'COASTING', 'ASSOCIATED'].includes(state) ||
      track.delay === null || track.doppler === null ||
      !finite(track.delay) || !finite(track.doppler) || track.id === undefined || track.id === null)
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
  const epoch = frame?.capture_sample_position_available === true ? frame.capture_epoch : event?.epoch;
  const first = frame?.capture_sample_position_available === true ?
    frame.capture_first_input_sample : event?.firstSample;
  const end = frame?.capture_sample_position_available === true ?
    frame.capture_end_input_sample : event?.endSample;
  return {epoch: epoch === undefined || epoch === null ? null : String(epoch),
    first_sample: first === undefined || first === null ? null : String(first),
    end_sample: end === undefined || end === null ? null : String(end),
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
    frequencyHz: config?.capture?.fc,
    sampleRateHz: config?.process?.delay_reference_sample_rate_hz || config?.capture?.fs,
    cpiSeconds,
    frame: primaryTracks})];
  return {schema: 'vectorwarp-radar-observation-bundle-v1', node_id: nodeId,
    server_session_id: serverSessionId, config_revision: configRevision,
    generated_at: Date.now(), channels: channels.filter(Boolean)};
}

module.exports = {compactTrack, channelObservation, buildObservationBundle};
