'use strict';

function portableRecordingBytesPerSecond(config) {
  const rate = Number(config?.capture?.fs || 0);
  const channels = Number(config?.capture?.device?.channel_count) ||
    (config?.capture?.device?.type === 'Kraken' ? 5 : 2);
  if (!Number.isFinite(rate) || rate <= 0 || !Number.isSafeInteger(channels) ||
      channels < 1) return 0;
  // BLAH2IQ stores complex IEEE float32: I and Q are four bytes each.
  return rate * channels * 8;
}

function manualRecordingExpired(config, startedAt, now = Date.now()) {
  const configured = Number(config?.save?.retention?.maximum_manual_seconds);
  const seconds = Number.isFinite(configured) && configured >= 30 ? configured : 300;
  return Number.isFinite(startedAt) && startedAt > 0 && now - startedAt >= seconds * 1000;
}

module.exports = {portableRecordingBytesPerSecond, manualRecordingExpired};
