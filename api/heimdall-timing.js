'use strict';
const LEGACY_MAGIC = 0x4d434851, TIMED_MAGIC = 0x4d434854;
function timingBytes(magic) {
  if (magic === LEGACY_MAGIC) return 0;
  if (magic === TIMED_MAGIC) return 96;
  throw new Error('Unsupported Heimdall frame magic');
}
function validateTiming(frame, channels) {
  if (!timingBytes(frame.readUInt32BE(0))) return;
  const at = 32 + channels * 8;
  if (frame.length < at + 96 || frame.readUInt32BE(at) !== 1 ||
      frame.readUInt32BE(at + 4) !== 96 || (frame.readUInt32BE(at + 8) & ~7) ||
      frame.readUInt32BE(at + 12) >= channels ||
      frame.readBigUInt64BE(at + 88) < frame.readBigUInt64BE(at + 24) + BigInt(frame.readUInt32BE(8)))
    throw new Error('Unsupported Heimdall ingestion timing extension');
}
module.exports = {timingBytes, validateTiming};
