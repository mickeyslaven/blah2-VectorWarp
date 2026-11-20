'use strict';

// The provider is installed only by the server's authenticated native-status
// ingress. Persisted YAML / recording side profiles are never status evidence.
let nativeStatus = () => null;
const laneSelection = Symbol('recorded-geographic-lane');
function forGeographicLane(config, id) { return {...config, [laneSelection]: id}; }
function setReplaySiteStatusProvider(provider) { nativeStatus = provider; }
const hash = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
const object = value => !!value && typeof value === 'object' && !Array.isArray(value);
const site = value => object(value) && ['latitude', 'longitude', 'altitude'].every(key =>
  typeof value[key] === 'number' && Number.isFinite(value[key])) &&
  Math.abs(value.latitude) <= 90 && Math.abs(value.longitude) <= 180 &&
  ['site_id', 'name'].every(key => value[key] === undefined ||
    typeof value[key] === 'string' && value[key].length <= 128);
function validRecordedSites(value) {
  return object(value) && JSON.stringify(value).length <= 16384 &&
    value.schema === 'vectorwarp-recorded-geographic-sites-v1' &&
    value.provenance === 'operator-configured-at-acquisition' && site(value.rx) &&
    (value.tx === null || site(value.tx)) && object(value.illuminators) &&
    Object.keys(value.illuminators).length <= 16 &&
    Object.entries(value.illuminators).every(([id, lane]) =>
      /^[A-Za-z0-9._-]{1,64}$/.test(id) && object(lane) &&
      Number.isSafeInteger(lane.frequencyHz) && lane.frequencyHz > 0 && lane.frequencyHz <= 4294967295 && site(lane.transmitter)) &&
    (value.tx !== null || Object.keys(value.illuminators).length > 0);
}
function geographicContext(config, processor = nativeStatus()) {
  const replay = config?.capture?.replay || {};
  const active = replay.state === true || typeof replay.state === 'boolean' && processor?.input === 'replay';
  const configured = config?.process?.multi_illuminator;
  if (!active) return {available: true, replay: false, reason: 'Configured live sites',
    location: config?.location, illuminators: configured?.illuminators || []};
  const unavailable = reason => ({available: false, replay: true, reason,
    location: null, illuminators: (configured?.illuminators || []).map(lane =>
      ({...lane, transmitter: null, transmitter_location: null}))});
  const proof = processor?.replayProvenance;
  const qualification = replay.qualification?.acquisition;
  const iqHash = replay.recording_sha256 || qualification?.recording_sha256;
  const metadataHash = replay.acquisition_sha256 || qualification?.acquisition_sha256;
  if (processor?.input !== 'replay' || processor.file !== replay.file || processor.state === 'error' ||
      !hash(iqHash) || proof?.recordingSha256 !== iqHash ||
      metadataHash && (!hash(metadataHash) || proof?.acquisitionSha256 !== metadataHash) ||
      proof?.acquisitionIntegrityValid !== true || proof?.acquisitionHashTrusted !== true ||
      proof?.geographicSitesAvailable !== true || !validRecordedSites(proof.geographicSites))
    return unavailable('Replay geographic sites require fresh native verification of externally pinned recorded acquisition provenance; live sites are not a fallback.');
  const sites = proof.geographicSites;
  const lanes = (configured?.illuminators || []).map(lane => {
    const recorded = sites.illuminators[lane.id];
    return {...lane, transmitter: recorded?.frequencyHz === lane.frequency_hz ? recorded.transmitter : null,
      transmitter_location: recorded?.frequencyHz === lane.frequency_hz ? recorded.transmitter : null};
  });
  if (configured?.enabled === true ? lanes.filter(lane => lane.enabled === true)
    .some(lane => !lane.transmitter) : !sites.tx)
    return unavailable('Recorded transmitter identity/frequency or site is missing for the selected replay route.');
  // Legacy primary sockets mirror the first enabled lane in multi mode.
  const selected = config[laneSelection] || (configured?.enabled === true ?
    lanes.find(lane => lane.enabled === true)?.id : null);
  const transmitter = selected ? lanes.find(lane => lane.id === selected)?.transmitter : sites.tx;
  if (selected && !transmitter) return unavailable('Recorded geographic lane identity is unavailable.');
  return {available: true, replay: true, reason: 'Authenticated operator-configured acquisition sites; not surveyed or hardware-attested.',
    location: {rx: sites.rx, tx: transmitter}, illuminators: lanes,
    recordingSha256: proof.recordingSha256, acquisitionSha256: proof.acquisitionSha256};
}
function geographicConfig(config, processor) {
  const context = geographicContext(config, processor);
  if (!context.replay) return config;
  return {...config, location: context.location,
    capture: {...config.capture, replay: {...config.capture?.replay, state: true}},
    process: {...config.process, multi_illuminator: {...config.process?.multi_illuminator,
      illuminators: context.illuminators}}};
}
module.exports = {validRecordedSites, geographicContext, geographicConfig, forGeographicLane, setReplaySiteStatusProvider};
