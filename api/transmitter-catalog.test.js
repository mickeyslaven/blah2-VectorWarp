'use strict';

const assert = require('node:assert/strict');
const catalog = require('../html/js/transmitter_catalog');
const {waveformReferenceCapability} = require('../html/js/processing_capabilities');
const {createStationProvider, catalogSources} = require('./illuminator-catalog');

// Synthetic records exercise widths; these values are fixture evidence, not
// a regional waveform plan or a claim about an actual transmitting station.
const now = Date.parse('2026-11-23T12:00:00Z');
function station(id = 'fictional-1', channelWidth = 8000000, occupiedWidth = 7600000) {
  return {schema: 1, id, provider: {id: 'fixture-regulator', region: 'EX',
    source: 'https://example.invalid/stations', retrieved_at: '2026-11-23T00:00:00Z',
    effective_at: '2026-11-22T00:00:00Z'}, callsign: 'TEST-TV', city: 'Example City',
    state: 'Example State', rf_channel: '27', frequency_hz: 522000000,
    channel_bandwidth_hz: channelWidth, occupied_bandwidth_hz: occupiedWidth,
    waveform: {standard: 'ATSC', version: '3.0'},
    transmitter: {name: 'Fictional test transmitter', latitude: 0.5, longitude: 1.5, altitude: 100},
    provenance: Object.fromEntries(['frequency_hz', 'channel_bandwidth_hz',
      'occupied_bandwidth_hz', 'waveform', 'coordinates'].map(key => [key, `fixture:${key}`]))};
}
const input = station(), original = structuredClone(input);
const records = catalog.parseImport(JSON.stringify(input));
assert.deepEqual(records, [original]);
const provider = catalog.createProvider(records);
assert.equal(provider.search({city: 'example', state: 'state', callsign: 'test', rf_channel: '27', now}).total, 1);
assert.equal(provider.search({query: 'EXAMPLE TEST-TV 27', now}).total, 1);
assert.equal(provider.search({rf_channel: '2', now}).total, 0);
assert.equal(provider.search({query: 'not-a-city', now}).total, 0);
assert.throws(() => provider.search({limit: 101}), /limit/);
assert.throws(() => catalog.parseImport([input, input]), /unique/);
assert.throws(() => catalog.parseImport(' '.repeat(catalog.LIMITS.bytes + 1)), /bytes/);
assert.throws(() => catalog.parseImport(Array(catalog.LIMITS.records + 1).fill(input)), /2000/);
const invalid = structuredClone(input);
delete invalid.occupied_bandwidth_hz;
assert.throws(() => catalog.parseImport(invalid), /occupied_bandwidth_hz/);
const missingVersion = structuredClone(input);
delete missingVersion.waveform.version;
assert.throws(() => catalog.parseImport(missingVersion), /version/);
const noProvenance = structuredClone(input);
delete noProvenance.provenance.coordinates;
assert.throws(() => catalog.parseImport(noProvenance), /coordinates/);

for (const [width, occupied] of [[6000000, 5832844], [7000000, 6800000], [8000000, 7600000]]) {
  const record = station(`atsc3-${width}`, width, occupied);
  const preview = catalog.previewSelection(record, {now});
  assert.deepEqual(preview.errors, []);
  assert.equal(preview.selection.standard, 'ATSC-3.0');
  assert.equal(preview.selection.occupied_bandwidth_hz, occupied);
  assert.equal(preview.channel_bandwidth_hz, width);
  assert.equal(preview.lower_hz, record.frequency_hz - occupied / 2);
  assert.equal(preview.source_occupied_bandwidth_hz, occupied);
  assert.deepEqual(preview.record, record);
}
const unknown = station('unknown', 6000000, 5400000);
unknown.waveform = {standard: 'ATSC', version: 'unknown'};
assert.equal(catalog.stationSelection(unknown, {now}).standard, 'CUSTOM', '6 MHz never implies ATSC 1.0');
assert.ok(catalog.previewSelection(unknown, {now}).warnings.some(warning => warning.includes('unverified')));
for (const standard of ['DVB-T', 'DVB-T2', 'ISDB-T', 'DTMB']) {
  const record = station(standard);
  record.waveform = {standard, version: 'explicit-fixture-version'};
  assert.equal(catalog.stationSelection(record, {now}).standard, standard);
}
assert.ok(catalog.previewSelection(input, {now, occupied_bandwidth_hz: 7000000}).errors.some(error => /reason/.test(error)));
const override = catalog.previewSelection(input, {now, occupied_bandwidth_hz: 7000000, reason: 'Verified measured passband'});
assert.deepEqual(override.errors, []);
assert.equal(override.source_occupied_bandwidth_hz, 7600000);
assert.equal(override.selection.occupied_bandwidth_hz, 7000000);
assert.deepEqual(input, original, 'preview never changes original provider record');
assert.ok(catalog.previewSelection(input, {occupied_bandwidth_hz: 9000000, reason: 'bad'}).errors.some(error => /no wider/.test(error)));
const unknownAltitude = station();
unknownAltitude.transmitter.altitude = null;
assert.throws(() => catalog.stationSelection(unknownAltitude), /altitude/);
assert.equal(catalog.stationSelection(unknownAltitude, {altitude: 123, reason: 'Surveyed antenna AMSL', now}).transmitter.altitude, 123);

for (const referenceMethod of ['dedicated', 'array_eigenbeam']) {
  for (const standard of ['ATSC-1.0', 'ATSC-3.0', 'CUSTOM', 'DVB-T2']) {
    assert.equal(waveformReferenceCapability({standard, referenceMethod}).supported, true);
    assert.equal(waveformReferenceCapability({standard, referenceMethod}).raw_channelization, true);
  }
}
assert.equal(waveformReferenceCapability({standard: 'ATSC-1.0', atscReferenceMode: 'pilot_aided'}).supported, true);
assert.equal(waveformReferenceCapability({standard: 'ATSC-3.0', atscReferenceMode: 'automatic'}).effective_conditioning, 'raw');
assert.equal(waveformReferenceCapability({standard: 'ATSC-1.0', referenceMethod: 'array_eigenbeam', atscReferenceMode: 'automatic'}).effective_conditioning, 'raw');
assert.throws(() => catalog.stationSelection(input, {atsc_reference_mode: 'pilot_aided'}), /ATSC 1.0/);
assert.throws(() => catalog.stationSelection(input, {reference_method: 'synthesized'}), /dedicated physical reference/);
const atsc1 = station(); atsc1.waveform.version = '1.0';
assert.throws(() => catalog.stationSelection(atsc1, {reference_method: 'array_eigenbeam', atsc_reference_mode: 'pilot_aided'}), /array reference/);

assert.equal(catalog.freshness(input, {now}).status, 'fresh');
assert.equal(catalog.freshness(input, {now: now + 8 * 86400000}).status, 'stale');
assert.equal(catalog.freshness(input, {now: now - 86400000}).status, 'future');
const saved = provider.exportSnapshot(); saved.records[0].transmitter.latitude = 20;
assert.equal(provider.search({now}).records[0].transmitter.latitude, 0.5, 'provider snapshot cannot be mutated by callers');
const cache = catalog.createCache();
cache.put(provider.exportSnapshot());
assert.equal(cache.get(provider.id).search({now}).total, 1);
assert.equal(cache.get('absent'), null);
assert.equal(cache.list()[0].count, 1);
for (let i = 1; i < catalog.LIMITS.providers; i++) {
  const next = station(); next.provider.id = `provider-${i}`; cache.put(next);
}
const extra = station(); extra.provider.id = 'over-capacity';
assert.throws(() => cache.put(extra), /capacity/);
assert.equal(cache.list().length, catalog.LIMITS.providers, 'overflow preserves previous cache');
cache.remove(provider.id);
cache.put(extra);

const fcc = station();
fcc.provider = {...fcc.provider, id: 'fcc-lms-us', region: 'US',
  source: 'https://enterpriseefiling.fcc.gov/dataentry/public/tv/lmsDatabase.html'};
fcc.provenance.frequency_hz = fcc.provider.source;
fcc.provenance.coordinates = 'https://geo.fcc.gov/api/contours/antenna.json?serviceType=tv&facilityId=123';
const fccProvider = createStationProvider(fcc);
assert.match(fccProvider.fingerprint_sha256, /^[a-f0-9]{64}$/);
assert.equal(fccProvider.descriptor.ingestion_status, 'normalized-import-required');
const badFcc = structuredClone(fcc); badFcc.provenance.coordinates = 'https://example.invalid/antenna';
assert.throws(() => createStationProvider(badFcc), /Contours/);
badFcc.provenance.coordinates = 'https://geo.fcc.gov/api/contours/entity.json';
assert.throws(() => createStationProvider(badFcc), /Contours/);
const same = createStationProvider(provider.exportSnapshot());
assert.equal(same.descriptor.update_mode, 'validate-and-pin', 'other regions use the same import contract');
assert.ok(catalogSources().find(source => source.id === 'fcc-lms-us').antenna_url);

console.log('portable transmitter catalog/import/cache/reference capability tests passed');
