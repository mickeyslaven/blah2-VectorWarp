'use strict';

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
    // A compact frame can outlive the full frame whose map string it retains.
    // Charge that retained payload to the compact entry so the global byte
    // bound remains conservative even after the original entry is evicted.
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

module.exports = {IlluminatorFrameHistory};
