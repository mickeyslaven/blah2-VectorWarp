'use strict';
// Shared field order/defaults are compiled into the native immutable plan.
// Integer-scaled cutoff/transition values avoid language-specific float JSON.
const contract = require('./frontend-plan-contract.json');
const channelizedFilter = 'frequency-shift-hamming-sinc-unit-dc-v1';
function frontendPlan(config, lane, channelCount, referenceChannels, surveillanceChannels) {
  const capture = config.capture || {}, replay = capture.replay?.state === true ? capture.replay : {};
  const sourceRate = replay.sample_rate_hz ?? capture.fs, center = replay.frequency_hz ?? capture.fc;
  const channelized = config.process?.multi_illuminator?.enabled === true;
  const output = lane.output_sample_rate_hz, occupied = channelized ? lane.occupied_bandwidth_hz : sourceRate;
  const taps = channelized ? (lane.filter_taps_per_phase ?? 24) : 0, ratio = sourceRate / output;
  const count = channelized ? 2 * Math.floor((taps * ratio + 1) / 2) + 1 : 1;
  const plan = {...contract, extraction: channelized ? 'channelized' : 'direct',
    filter: channelized ? channelizedFilter : contract.filter,
    source_sample_rate_hz: sourceRate, source_center_hz: center,
    output_sample_rate_hz: output, frequency_hz: lane.frequency_hz,
    occupied_bandwidth_hz: occupied, standard: channelized ? (lane.standard ?? '') : '',
    filter_taps_per_phase: taps, filter_tap_count: count,
    transition_width_hz_x2: channelized ? output - occupied : 0,
    cutoff_hz_x4: channelized ? output + occupied : 0,
    input_samples_per_output: ratio, group_delay_input_samples: (count - 1) / 2,
    channel_ids: Array.from({length: channelCount}, (_, index) => index),
    reference_channel_ids: [...referenceChannels], surveillance_channel_ids: [...surveillanceChannels]};
  if (![sourceRate, center, output, lane.frequency_hz, occupied, ratio, channelCount].every(value =>
      Number.isSafeInteger(value) && value > 0) || channelCount > 8 ||
      (channelized && (!Number.isSafeInteger(taps) || taps < 4 || taps > 128 || count > 131071 ||
        occupied >= output || 2 * Math.abs(lane.frequency_hz - center) + occupied >= sourceRate)) ||
      (!channelized && (output !== sourceRate || lane.frequency_hz !== center)))
    throw new Error('Invalid canonical frontend plan');
  for (const channels of [plan.channel_ids, plan.reference_channel_ids, plan.surveillance_channel_ids]) {
    if (!channels.length || new Set(channels).size !== channels.length ||
        channels.some(channel => !Number.isSafeInteger(channel) || channel < 0 || channel >= channelCount))
      throw new Error('Invalid canonical frontend channel mapping');
    Object.freeze(channels);
  }
  return Object.freeze(plan);
}
module.exports = {frontendPlan};
