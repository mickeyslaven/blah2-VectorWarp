'use strict';

function recordingReplayTopology(config) {
  const device = config.capture?.device || {};
  const channelCount = device.channel_count || (device.type === 'Kraken' ? null : 2);
  const configuredReferenceChannels = config.process?.reference_synthesis?.channels;
  const dedicatedReference = config.process?.reference_synthesis?.mode === 'dedicated' &&
    Array.isArray(configuredReferenceChannels) ? configuredReferenceChannels[0] : undefined;
  const referenceChannel = device.reference_channel ?? dedicatedReference ?? 0;
  const surveillanceChannels = Array.isArray(device.surveillance_channels) ?
    [...device.surveillance_channels] : Number.isInteger(channelCount) ?
      Array.from({length: channelCount}, (_, index) => index)
        .filter(index => index !== referenceChannel) : [1];
  return {channel_count: channelCount, reference_channel: referenceChannel,
    surveillance_channels: surveillanceChannels};
}

module.exports = {recordingReplayTopology};
