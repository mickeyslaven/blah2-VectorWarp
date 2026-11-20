'use strict';

// Invalid CPI configuration cannot provide a time coordinate for aligned
// presentation. Null means unavailable; zero remains a valid explicit lag.
function configuredPresentationDelay(config) {
  const cpi = config?.process?.data?.cpi;
  if (!Number.isFinite(cpi) || cpi <= 0) return null;
  const lag = Number.isInteger(config?.truth?.adsb?.association_lag_cpis) ?
    config.truth.adsb.association_lag_cpis : 0;
  const delay = Math.max(0, lag * cpi * 1000);
  return Number.isFinite(delay) ? delay : null;
}

function frameTimestamp(frame) {
  let value = frame;
  if (typeof frame === 'string') {
    try { value = JSON.parse(frame); } catch (_) { return null; }
  }
  const raw = value?.frameTimestamp ?? value?.timestamp;
  const timestamp = Array.isArray(raw) ? raw.at(-1) : raw;
  const number = timestamp instanceof Date ? timestamp.getTime() : Number(timestamp);
  return Number.isFinite(number) ? number : null;
}

class PresentationHistory {
  constructor({maxAgeMs = 15000, maxBytes = 48 * 1024 * 1024} = {}) {
    this.maxAgeMs = maxAgeMs;
    this.maxBytes = maxBytes;
    this.entries = [];
    this.bytes = 0;
  }
  add(frame, arrivedAt = Date.now(), input = frame) {
    const timestamp = frameTimestamp(frame);
    if (timestamp === null) return;
    const size = value => Buffer.byteLength(typeof value === 'string' ? value : JSON.stringify(value));
    const bytes = size(frame) + (input === frame ? 0 : size(input));
    const entry = {timestamp, arrivedAt, frame, input, bytes};
    this.entries.push(entry);
    this.bytes += bytes;
    const oldestArrival = arrivedAt - this.maxAgeMs;
    while (this.entries.length > 1 &&
      (this.entries[0].arrivedAt < oldestArrival || this.bytes > this.maxBytes)) {
      this.bytes -= this.entries.shift().bytes;
    }
    return entry;
  }
  entryAt(timestamp) {
    let match = null;
    for (const entry of this.entries) {
      if (entry.timestamp <= timestamp && (!match || entry.timestamp > match.timestamp)) match = entry;
    }
    return match;
  }
  at(timestamp) { return this.entryAt(timestamp)?.frame ?? null; }
  timestampAt(timestamp) { return this.entryAt(timestamp)?.timestamp ?? null; }
  exact(timestamp) {
    for (let index = this.entries.length - 1; index >= 0; index -= 1) {
      if (this.entries[index].timestamp === timestamp) return this.entries[index].frame;
    }
    return null;
  }
  diagnostics(expectedIntervalMs = null) {
    const timestamps = this.entries.map(entry => entry.timestamp);
    const intervals = timestamps.slice(1).map((value, index) => value - timestamps[index])
      .filter(value => Number.isFinite(value) && value > 0);
    const sorted = [...intervals].sort((left, right) => left - right);
    const percentile = fraction => sorted.length ?
      sorted[Math.min(sorted.length - 1, Math.floor((sorted.length - 1) * fraction))] : null;
    const toleranceMs = Number.isFinite(expectedIntervalMs) ? 100 : null;
    return {
      samples: timestamps.length,
      latest_timestamp: timestamps.at(-1) ?? null,
      interval_median_ms: percentile(.5),
      interval_p95_ms: percentile(.95),
      interval_max_ms: sorted.at(-1) ?? null,
      expected_interval_ms: Number.isFinite(expectedIntervalMs) ? expectedIntervalMs : null,
      within_100ms_fraction: toleranceMs === null || !intervals.length ? null :
        intervals.filter(value => Math.abs(value - expectedIntervalMs) <= toleranceMs).length /
          intervals.length
    };
  }
  clear() { this.entries = []; this.bytes = 0; }
}

class IlluminatorFrameHistory {
  constructor({maxFramesPerIlluminator = 8, maxBytes = 32 * 1024 * 1024} = {}) {
    if (!Number.isInteger(maxFramesPerIlluminator) || maxFramesPerIlluminator < 1 ||
        !Number.isInteger(maxBytes) || maxBytes < 1)
      throw new Error('Invalid illuminator history limits');
    this.maxFrames = maxFramesPerIlluminator; this.maxBytes = maxBytes;
    this.frames = new Map(); this.bytes = 0; this.sequence = 0;
  }
  add(frame, bytes = Buffer.byteLength(JSON.stringify(frame))) {
    const previous = this.latest(frame.id);
    if (frame?.map?.data_omitted === true) {
      frame.display_map_json = previous?.display_map_json || previous?.map_json || null;
      frame.display_map_timestamp = previous?.display_map_timestamp ??
        previous?.map?.timestamp ?? previous?.timestamp ?? null;
    } else if (typeof frame?.map_json === 'string') {
      frame.display_map_json = frame.map_json;
      frame.display_map_timestamp = frame.map?.timestamp ?? frame.timestamp ?? null;
    }
    // Charge retained display bytes even after their original frame is evicted.
    const retainedBytes = frame.display_map_json && frame.display_map_json !== frame.map_json ?
      Buffer.byteLength(frame.display_map_json) : 0;
    const accountedBytes = bytes + retainedBytes;
    const lane = this.frames.get(frame.id) || [];
    const duplicate = lane.findIndex(item => item.frame.timestamp === frame.timestamp &&
      item.frame.first_input_sample === frame.first_input_sample &&
      item.frame.end_input_sample === frame.end_input_sample);
    if (duplicate >= 0) { this.bytes -= lane[duplicate].bytes; lane.splice(duplicate, 1); }
    lane.push({frame, bytes: accountedBytes, sequence: ++this.sequence});
    this.bytes += accountedBytes;
    while (lane.length > this.maxFrames) { this.bytes -= lane[0].bytes; lane.shift(); }
    this.frames.set(frame.id, lane);
    while (this.bytes > this.maxBytes) {
      let oldestId = null, oldest = null;
      for (const [id, values] of this.frames) if (values.length &&
          (!oldest || values[0].sequence < oldest.sequence)) {
        oldestId = id; oldest = values[0];
      }
      if (!oldest) break;
      const values = this.frames.get(oldestId); values.shift(); this.bytes -= oldest.bytes;
      if (!values.length) this.frames.delete(oldestId);
    }
  }
  latest(id) { return this.frames.get(id)?.at(-1)?.frame || null; }
  clear() {
    this.frames.clear(); this.bytes = 0; this.sequence = 0;
  }
  latestCohort(ids) {
    const requested = [...new Set((ids || []).map(String))];
    if (requested.length < 2) return null;
    const laneKeys = requested.map(id => new Map((this.frames.get(id) || [])
      .filter(item => item.frame.capture_sample_position_available === true &&
        Number.isSafeInteger(item.frame.capture_epoch) &&
        Number.isSafeInteger(item.frame.capture_first_input_sample) &&
        Number.isSafeInteger(item.frame.capture_end_input_sample))
      .map(item => [`${item.frame.capture_epoch}:${item.frame.capture_first_input_sample}:` +
        `${item.frame.capture_end_input_sample}`, item])));
    if (laneKeys.some(values => !values.size)) return null;
    const common = [...laneKeys[0].keys()].filter(key => laneKeys.every(values => values.has(key)))
      .sort((left, right) => Math.min(...laneKeys.map(values => values.get(right).sequence)) -
        Math.min(...laneKeys.map(values => values.get(left).sequence)))[0];
    return common ? laneKeys.map(values => values.get(common).frame) : null;
  }
  exact(id, {timestamp, firstInputSample, endInputSample} = {}) {
    const values = this.frames.get(id) || [];
    for (let index = values.length - 1; index >= 0; --index) {
      const frame = values[index].frame;
      if (timestamp !== undefined && frame.timestamp !== timestamp) continue;
      if (firstInputSample !== undefined && frame.first_input_sample !== firstInputSample) continue;
      if (endInputSample !== undefined && frame.end_input_sample !== endInputSample) continue;
      return frame;
    }
    return null;
  }
}

// The API's only owner of admitted products, retained frames and connection
// generations. Renderers, evidence consumers and hardware validators are adapters.
const stashFactories = require('./stash/frame-history');
class RadarSessionStore extends IlluminatorFrameHistory {
  constructor({config = {}, now = Date.now, onProduct = () => {}, onReset = () => {},
      projectProduct = (_name, frame) => frame,
      ...limits} = {}) {
    super(limits);
    this.config = config; this.now = now;
    this.onProduct = onProduct; this.onReset = onReset;
    this.projectProduct = projectProduct;
    this.products = Object.fromEntries(['map', 'detection', 'tracker', 'bearing',
      'timestamp', 'timing', 'iqdata'].map(name => [name, '']));
    this.histories = Object.fromEntries(Object.keys(this.products)
      .filter(name => name !== 'timestamp').map(name => [name, new PresentationHistory()]));
    this.stashes = Object.fromEntries(['map', 'detection', 'timing', 'iqdata']
      .map(name => [name, stashFactories[name === 'map' ? 'maxhold' : name]()]));
    this.connections = {};
    this.restartGeneration = 0;
    this.processorStatus = null;
  }
  get lastFrameAt() { return this.channel('timestamp').receivedAt; }
  get timestampConnections() { return this.channel('timestamp').sequence; }
  channel(name) {
    return this.connections[name] ||= {generation: 0, sequence: 0, active: 0, receivedAt: null};
  }
  open(name) {
    const channel = this.channel(name);
    return {name, restart: this.restartGeneration, generation: channel.generation,
      id: ++channel.sequence, firstFrame: true};
  }
  eligible(connection) {
    return connection.restart === this.restartGeneration &&
      connection.generation === this.channel(connection.name).generation;
  }
  activate(connection) {
    if (!this.eligible(connection)) return false;
    this.channel(connection.name).active = connection.id;
    return true;
  }
  close(connection) {
    const channel = this.channel(connection.name);
    if (!this.eligible(connection) || channel.active !== connection.id) return false;
    channel.active = 0; channel.receivedAt = null;
    this.products[connection.name] = '';
    return true;
  }
  invalidate(name) {
    const channel = this.channel(name);
    channel.generation += 1; channel.active = 0; channel.receivedAt = null;
    this.products[name] = '';
  }
  restart() {
    this.restartGeneration += 1;
    for (const name of ['timestamp', 'timing']) this.invalidate(name);
  }
  generation(name) {
    const channel = this.channel(name);
    return `${this.restartGeneration}:${channel.generation}:${channel.active}`;
  }
  fresh(name, maxAgeMs, now = this.now(), inclusive = false) {
    return fresh(this.channel(name), now, maxAgeMs, inclusive);
  }
  acceptProduct(name, frame, options = {}) {
    const history = this.histories[name];
    this.products[name] = frame;
    this.channel(name).receivedAt = this.now();
    const published = this.projectProduct(name, frame);
    // Exact bytes preserve uint64 identities and equal-time CPIs.
    if (options.duplicate === true || options.history !== false && history?.entries
        .some(entry => entry.input === frame)) return false;
    this.stashes[name]?.update_data(frame);
    this.onProduct(name, frame, options, published);
    if (options.history !== false) history?.add(published, this.now(), frame);
    return true;
  }
  acceptIlluminator(frame, connection = {}) {
    const value = parseIlluminatorFrame(frame);
    if (value?.schema !== 'vectorwarp-multi-illuminator-frame-v1' ||
        typeof value.id !== 'string' ||
        !/^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/.test(value.id) ||
        !Number.isSafeInteger(value.frequency_hz) || value.frequency_hz < 1 ||
        !Number.isSafeInteger(value.sample_rate_hz) || value.sample_rate_hz < 1 ||
        (value.doppler_window !== undefined &&
          !['rectangular', 'hann', 'tukey25'].includes(value.doppler_window)) ||
        !Number.isSafeInteger(value.first_input_sample) || value.first_input_sample < 0 ||
        !Number.isSafeInteger(value.end_input_sample) ||
        value.end_input_sample < value.first_input_sample ||
        (value.capture_sample_position_available === true &&
          (!Number.isSafeInteger(value.capture_epoch) || value.capture_epoch < 0 ||
           !Number.isSafeInteger(value.capture_first_input_sample) ||
           value.capture_first_input_sample < 0 ||
           !Number.isSafeInteger(value.capture_end_input_sample) ||
           value.capture_end_input_sample <= value.capture_first_input_sample)) ||
        !Number.isSafeInteger(value.timestamp) || value.timestamp < 0 ||
        !value.map || typeof value.map !== 'object' ||
        !value.detections || typeof value.detections !== 'object' ||
        !value.tracks || typeof value.tracks !== 'object')
      throw new Error('Invalid multi-illuminator frame');
    const configured = (this.config.process?.multi_illuminator?.illuminators || [])
      .find(item => item?.enabled === true && item.id === value.id);
    if (!configured || configured.frequency_hz !== value.frequency_hz ||
        configured.output_sample_rate_hz !== value.sample_rate_hz)
      throw new Error('Multi-illuminator frame does not match the active configuration');
    const previous = this.latest(value.id);
    // A reconnect alone is not a restart; retain in-flight capture cohorts.
    if (connection.firstFrame === true && previous &&
        value.timestamp > previous.timestamp &&
        value.first_input_sample <= previous.end_input_sample) {
      this.clear();
      this.onReset();
    }
    const duplicate = this.exact(value.id, {timestamp: value.timestamp,
      firstInputSample: value.first_input_sample, endInputSample: value.end_input_sample});
    this.add(value, Buffer.byteLength(frame));
    return {value, duplicate: !!duplicate};
  }
  publishPrimary(value, duplicate = false) {
    const primary = (this.config.process?.multi_illuminator?.illuminators || [])
      .find(item => item?.enabled === true);
    if (primary?.id !== value.id) return false;
    if (value.map?.data_omitted !== true)
      this.acceptProduct('map', value.map_json || JSON.stringify(value.map));
    this.acceptProduct('detection', JSON.stringify(value.detections), {duplicate});
    this.acceptProduct('tracker', JSON.stringify(value.tracks), {admitHealth: false, duplicate});
    return true;
  }
  acceptProcessorStatus(checked, payload) {
    if (checked.valid) this.processorStatus = {value: checked.value, receivedAt: this.now()};
    else if (payload?.receiverHealth !== undefined || payload?.replayProvenance !== undefined)
      this.processorStatus = null;
  }
  currentProcessor() { return this.processorFresh() ? this.processorStatus.value : null; }
  processorFresh(now = this.now(), maxAgeMs = 5000) {
    return fresh(this.processorStatus, now, maxAgeMs);
  }
}

// Receiver-status envelopes deliberately keep their inclusive expiry boundary;
// stream freshness uses a strict bound. Neither predicate attests hardware.
function fresh(entry, now = Date.now(), maxAgeMs = 5000, inclusive = true) {
  return !!entry && Number.isFinite(entry.receivedAt) && (inclusive ?
    now - entry.receivedAt <= maxAgeMs : now - entry.receivedAt < maxAgeMs);
}

function parseIlluminatorFrame(frame) {
  // Retain large matrices as JSON; evidence/fusion consume compact metadata.
  const mapKey = '"map":', detectionsKey = ',"detections":';
  // Production map follows any nested experimental maps, immediately before detections.
  const productionMapStart = frame.lastIndexOf(`,${mapKey}`);
  const mapStart = productionMapStart < 0 ? -1 : productionMapStart + 1;
  const detectionsStart = mapStart < 0 ? -1 :
    frame.indexOf(detectionsKey, mapStart + mapKey.length);
  if (mapStart < 0 || detectionsStart < 0) return JSON.parse(frame);
  const mapJson = frame.slice(mapStart + mapKey.length, detectionsStart);
  const dataKey = ',"data":';
  const dataStart = mapJson.indexOf(dataKey);
  let mapSummary;
  if (dataStart < 0) mapSummary = JSON.parse(mapJson);
  else mapSummary = JSON.parse(`${mapJson.slice(0, dataStart)}}`);
  const compact = `${frame.slice(0, mapStart + mapKey.length)}null` +
    frame.slice(detectionsStart);
  const value = JSON.parse(compact);
  value.map = mapSummary;
  value.map_json = mapJson;
  if (value.map && value.map.eventTime === undefined)
    value.map.eventTime = value.detections?.eventTime || value.tracks?.eventTime || null;
  return value;
}

module.exports = {RadarSessionStore, PresentationHistory, IlluminatorFrameHistory,
  frameTimestamp, configuredPresentationDelay, fresh};
