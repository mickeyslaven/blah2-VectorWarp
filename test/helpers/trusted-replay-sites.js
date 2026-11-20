'use strict';
// Unit fixture for the already-authenticated native status boundary. Native
// recording tests separately verify full-IQ/manifest hashing and tamper failure.
const {setReplaySiteStatusProvider} = require('../../api/replay-geographic-sites');
module.exports = function trustedReplaySites(config) {
  Object.assign(config.capture.replay, {file: '/unit-fixture.blah2iq', recording_sha256: 'a'.repeat(64)});
  const site = value => value ? {altitude: 0, ...value} : null;
  const sites = {schema: 'vectorwarp-recorded-geographic-sites-v1',
    provenance: 'operator-configured-at-acquisition', rx: site(config.location?.rx),
    tx: site(config.location?.tx), illuminators: Object.fromEntries(
      (config.process?.multi_illuminator?.illuminators || []).filter(lane => lane.enabled).map(lane =>
        [lane.id, {frequencyHz: lane.frequency_hz, transmitter: site(lane.transmitter)}]))};
  const status = {input: 'replay', state: 'playing', file: config.capture.replay.file,
    replayProvenance: {acquisitionIntegrityValid: true, acquisitionHashTrusted: true,
      geographicSitesAvailable: true, geographicSites: sites,
      recordingSha256: 'a'.repeat(64), acquisitionSha256: 'b'.repeat(64)}};
  setReplaySiteStatusProvider(() => status);
  return status;
};
