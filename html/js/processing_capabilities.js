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

// Shared API admission and UI availability predicates. Native qualification
// remains authoritative; these checks keep both clients on the same contract.
function residentReferenceCompatible(topology) {
  const {referenceMode, coherent, referenceChannels, surveillanceChannels, channelCount} = topology;
  const allInputs = channels => channels.length === channelCount &&
    channels.every((channel, index) => channel === index);
  return referenceMode === 'array_eigenbeam' ?
    coherent && allInputs(referenceChannels) && allInputs(surveillanceChannels) :
    referenceMode === 'dedicated' && referenceChannels.length === 1 &&
      surveillanceChannels.length >= 1 && !surveillanceChannels.includes(referenceChannels[0]);
}

function regularSurveillancePolygon(geometry, channels) {
  if (geometry?.shape !== 'regular_polygon') return false;
  const byChannel = new Map((Array.isArray(geometry.elements) ? geometry.elements : [])
    .map(element => [element?.daq_channel, element?.position]));
  const positions = (Array.isArray(channels) ? channels : []).map(channel => byChannel.get(channel));
  if (positions.length < 3 || !positions.every(position => Array.isArray(position) &&
      position.length === 3 && position.every(Number.isFinite))) return false;
  const centre = [0, 1, 2].map(axis =>
    positions.reduce((sum, position) => sum + position[axis], 0) / positions.length);
  const radii = positions.map(position => Math.hypot(position[0] - centre[0], position[1] - centre[1]));
  const angles = positions.map(position => Math.atan2(position[1] - centre[1], position[0] - centre[0]));
  const radius = radii.reduce((sum, value) => sum + value, 0) / radii.length;
  const wrap = angle => Math.atan2(Math.sin(angle), Math.cos(angle));
  const step = wrap(angles[1] - angles[0]), tolerance = Math.max(1e-6, radius * 1e-5);
  return Number.isFinite(radius) && radius > 0 && Math.abs(step) > 1e-5 &&
    Math.abs(wrap(positions.length * step)) <= 1e-5 &&
    positions.every((position, index) => Math.abs(radii[index] - radius) <= tolerance &&
      Math.abs(position[2] - centre[2]) <= tolerance &&
      Math.abs(wrap(angles[index] - angles[0] - index * step)) <= 1e-5 &&
      angles.slice(0, index).every(prior => Math.abs(wrap(angles[index] - prior)) > 1e-5));
}

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

// Admission facts shared by API refusal and UI availability. Callers retain
// distinct enabled-state checks, diagnostics and dormant-value behavior.
function experimentalSettingSupport(config, topology = processingCapabilities(config)) {
  const process = config.process || {}, multi = process.multi_illuminator?.enabled === true;
  const replay = config.capture?.replay?.state === true;
  const hostFrontend = ['cpu', 'gpu'].includes(process.performance?.acceleration);
  const residentProducts = process.performance?.acceleration === 'gpu-resident' &&
    (process.surveillance_fusion?.mode ?? 'noncoherent') !== 'noncoherent';
  return {residentProducts,
    targetProtection: !multi && hostFrontend && topology.coherentSurveillance &&
      topology.confirmedGeometry && topology.referenceGeometry &&
      process.surveillance_fusion?.mode === 'coherent_beam_bank',
    complexCutout: replay && hostFrontend,
    ridgeBirth: !residentProducts && process.detection?.enable === true && process.tracker?.enable === true};
}

return {processingCapabilities, residentReferenceCompatible, regularSurveillancePolygon, experimentalSettingSupport};
});
