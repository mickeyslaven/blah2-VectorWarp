(function(root, factory) {
  if (typeof module !== 'undefined' && module.exports) module.exports = factory();
  else root.VectorWarpProcessingCapabilities = factory();
})(typeof globalThis !== 'undefined' ? globalThis : this, function() {
'use strict';

// Adapter facts belong at the input boundary. Algorithm gates consume the
// resulting topology, never a receiver name. Synchronized HackRF inputs do
// not establish the stable inter-channel phase required by coherent fusion.
const ADAPTERS = Object.freeze({
  Kraken: {coherent: true, variableChannels: true, appliedGains: true, mchq: true},
  Usrp: {coherent: true},
  RspDuo: {coherent: true},
  HackRF: {coherent: false}
});

function processingCapabilities(config) {
  const device = config.capture?.device || {};
  const replay = config.capture?.replay || {};
  const adapter = ADAPTERS[device.type] || {};
  const recorded = replay.state === true && Number.isInteger(replay.channel_count);
  const channelCount = recorded ? replay.channel_count :
    adapter.variableChannels ? device.channel_count : 2;
  const referenceMode = (recorded ? replay.reference_mode : undefined) ??
    config.process?.reference_synthesis?.mode ?? 'dedicated';
  const configuredReferences = recorded ? replay.reference_channels :
    config.process?.reference_synthesis?.channels;
  const referenceChannel = (recorded ? replay.reference_channel :
    adapter.variableChannels ? device.reference_channel : undefined) ??
    (referenceMode === 'dedicated' && Array.isArray(configuredReferences) &&
      configuredReferences.length === 1 ? configuredReferences[0] : 0);
  const allChannels = Number.isInteger(channelCount) && channelCount >= 2 && channelCount <= 8 ?
    Array.from({length: channelCount}, (_, index) => index) : [];
  const configuredSurveillance = (recorded ? replay.surveillance_channels :
    adapter.variableChannels ? device.surveillance_channels : undefined) ??
    allChannels.filter(channel => referenceMode === 'array_eigenbeam' || channel !== referenceChannel);
  const references = configuredReferences ??
    (referenceMode === 'array_eigenbeam' ? allChannels : [referenceChannel]);
  // Canonical physical-channel order matches the native topology resolver;
  // sorting a copy must not silently mutate the saved configuration.
  const referenceChannels = Array.isArray(references) ? [...references].sort((a, b) => a - b) : references;
  const surveillanceChannels = Array.isArray(configuredSurveillance) ?
    [...configuredSurveillance].sort((a, b) => a - b) : configuredSurveillance;
  const validChannels = values => Array.isArray(values) && values.length > 0 &&
    new Set(values).size === values.length && values.every(channel =>
      Number.isInteger(channel) && channel >= 0 && channel < channelCount);
  const validTopology = allChannels.length > 0 && validChannels(referenceChannels) &&
    validChannels(surveillanceChannels) &&
    Number.isInteger(referenceChannel) && referenceChannel >= 0 && referenceChannel < channelCount &&
    (referenceMode === 'array_eigenbeam' ? referenceChannels.length >= 2 &&
      (recorded || adapter.coherent === true) :
      referenceMode === 'dedicated' && referenceChannels.length === 1 &&
      referenceChannels[0] === referenceChannel &&
      !surveillanceChannels.includes(referenceChannel));
  const coherent = validTopology && (recorded ?
    referenceMode === 'array_eigenbeam' : adapter.coherent === true);
  const geometry = device.array_geometry || {};
  const elements = Array.isArray(geometry.elements) ? geometry.elements : [];
  const positions = new Map(elements.filter(element => element &&
    Number.isInteger(element.daq_channel) && Array.isArray(element.position) &&
    element.position.length === 3 && element.position.every(Number.isFinite))
    .map(element => [element.daq_channel, element.position]));
  const completeGeometry = values => validChannels(values) &&
    new Set(elements.map(element => element?.daq_channel)).size === elements.length &&
    values.every(channel => positions.has(channel)) &&
    values.some(channel => positions.get(channel).some((coordinate, axis) =>
      coordinate !== positions.get(values[0])[axis])) &&
    Number.isFinite(geometry.x_axis_bearing_deg_true);
  const confirmedGeometry = geometry.mapping_confirmed === true &&
    geometry.geometry_confirmed === true && geometry.orientation_confirmed === true;
  const coherentSurveillance = coherent && surveillanceChannels.length >= 2 &&
    referenceMode === 'array_eigenbeam' && referenceChannels.length === surveillanceChannels.length &&
    referenceChannels.every(channel => surveillanceChannels.includes(channel));
  const heimdall = device.heimdall || {};
  return {channelCount, referenceChannel, referenceMode, referenceChannels,
    validTopology,
    surveillanceChannels, coherent, coherentSurveillance, confirmedGeometry,
    surveillanceGeometry: completeGeometry(surveillanceChannels),
    referenceGeometry: completeGeometry(referenceChannels),
    appliedGainMetadata: recorded || adapter.appliedGains === true,
    liveMchq: replay.state !== true && adapter.mchq === true &&
      typeof heimdall.host === 'string' && heimdall.host.length > 0 &&
      Number.isInteger(heimdall.port) && heimdall.port > 0 && heimdall.port <= 65535};
}

return {processingCapabilities};
});
