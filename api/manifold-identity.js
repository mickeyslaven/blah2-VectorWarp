'use strict';

const finite = value => typeof value === 'number' && Number.isFinite(value);
const TAG = Buffer.from('vectorwarp-manifold-identity-v2', 'ascii');

function compatibilityFingerprint(identity) {
  const bytes = [TAG];
  const u32 = value => { const out = Buffer.allocUnsafe(4); out.writeUInt32LE(value); bytes.push(out); };
  const f64 = value => { const out = Buffer.allocUnsafe(8); out.writeDoubleLE(value); bytes.push(out); };
  f64(identity.frequency_hz); f64(identity.sample_rate_hz);
  u32(identity.channel_mapping.length);
  identity.channel_mapping.forEach(u32);
  identity.requested_gain_db.forEach(f64);
  identity.positions_m.flat().forEach(f64);
  identity.receiver_location.forEach(f64);
  identity.transmitter_location.forEach(f64);
  f64(identity.x_axis_bearing_deg_true);
  let hash = 1469598103934665603n;
  for (const byte of Buffer.concat(bytes)) {
    hash ^= BigInt(byte);
    hash = BigInt.asUintN(64, hash * 1099511628211n);
  }
  return `fnv1a64:${hash.toString(16).padStart(16, '0')}`;
}

function validIdentity(identity, frequency, sampleRate, channels) {
  const vector = (value, length) => Array.isArray(value) && value.length === length &&
    value.every(finite);
  return identity && typeof identity === 'object' &&
    identity.frequency_hz === frequency && identity.sample_rate_hz === sampleRate &&
    Array.isArray(identity.channel_mapping) && identity.channel_mapping.length === channels &&
    identity.channel_mapping.every(value => Number.isInteger(value) && value >= 0) &&
    new Set(identity.channel_mapping).size === channels &&
    vector(identity.requested_gain_db, channels) &&
    identity.requested_gain_db.every(value => value >= 0 && value <= 50) &&
    vector(identity.actual_gain_db, channels) &&
    Array.isArray(identity.positions_m) && identity.positions_m.length === channels &&
    identity.positions_m.every(value => vector(value, 3)) &&
    vector(identity.receiver_location, 3) && vector(identity.transmitter_location, 3) &&
    finite(identity.x_axis_bearing_deg_true) &&
    identity.compatibility_fingerprint === compatibilityFingerprint(identity);
}

module.exports = {compatibilityFingerprint, validIdentity};
