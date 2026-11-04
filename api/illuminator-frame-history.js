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
    const lane = this.frames.get(frame.id) || [];
    const duplicate = lane.findIndex(item => item.frame.timestamp === frame.timestamp &&
      item.frame.first_input_sample === frame.first_input_sample &&
      item.frame.end_input_sample === frame.end_input_sample);
    if (duplicate >= 0) { this.bytes -= lane[duplicate].bytes; lane.splice(duplicate, 1); }
    lane.push({frame, bytes, sequence: ++this.sequence}); this.bytes += bytes;
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
