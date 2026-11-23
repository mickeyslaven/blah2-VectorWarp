'use strict';

const {processingCapabilities} = require('../html/js/processing_capabilities');

function recordingReplayTopology(config) {
  // A recording must retain the same physical roles as the live pipeline,
  // including every array-reference surveillance channel on paired receivers.
  const topology = processingCapabilities(config);
  return {channel_count: topology.channelCount ?? null,
    reference_channel: topology.referenceChannel,
    surveillance_channels: [...topology.surveillanceChannels]};
}

module.exports = {recordingReplayTopology};
