'use strict';

const assert = require('assert');
const {bandPlans} = require('./illuminator-survey');
const {SCHEMA, catalogSources, validateCatalog, resolveCatalog} = require('./illuminator-catalog');

const sources = catalogSources();
assert.ok(sources.some(source => source.id === 'fcc-lms-us' &&
  source.standards.includes('FM') && source.standards.includes('ATSC')));
assert.ok(sources.some(source => source.id === 'ofcom-txparams-dab-uk' &&
  source.standards.includes('DAB')));
assert.ok(sources.some(source => source.id === 'ofcom-dtt-uk' &&
  source.standards.includes('DVB-T/T2')));

const source = {id: 'fixture', authority: 'Test regulator', jurisdiction: 'US',
  url: 'https://example.invalid/catalog', retrieved_at: '2026-10-19T12:00:00Z',
  effective_at: '2026-10-19T00:00:00Z'};
const transmitter = (id, frequency_hz, latitude, extra = {}) => ({id, standard: 'ATSC',
  frequency_hz, latitude, longitude: -80, antenna_altitude_m_amsl: null,
  coordinate_uncertainty_m: 25, verification_status: 'regulator',
  source_record_id: id, ...extra});
const catalog = {schema: SCHEMA, source, transmitters: [
  transmitter('near-a', 563000000, 40),
  transmitter('near-b-same-channel', 563000000, 40.01, {cochannel_group_id: 'rf29'}),
  transmitter('other-channel', 569000000, 40.02),
  transmitter('expired', 563000000, 40.03, {valid_to: '2026-10-18T00:00:00Z'}),
  transmitter('far', 563000000, 60),
  {...transmitter('fm', 563000000, 40.01), standard: 'FM'}
]};

const validated = validateCatalog(catalog);
assert.match(validated.fingerprint_sha256, /^[a-f0-9]{64}$/);
assert.equal(validated.transmitters.length, 6);
const resolved = resolveCatalog(catalog, {plan: bandPlans()['atsc-us-uhf'],
  receiverLocation: {latitude: 40, longitude: -80}, radiusKm: 25,
  asOf: '2026-10-19T12:00:00Z'});
assert.deepEqual(resolved.transmitters.map(item => item.id),
  ['near-a', 'near-b-same-channel', 'other-channel']);
assert.equal(resolved.transmitters.filter(item => item.frequency_hz === 563000000).length, 2);
assert.equal(resolved.catalog_fingerprint_sha256, validated.fingerprint_sha256);
assert.ok(resolved.transmitters[1].distance_km > resolved.transmitters[0].distance_km);

const duplicate = structuredClone(catalog);
duplicate.transmitters.push({...duplicate.transmitters[0]});
assert.throws(() => validateCatalog(duplicate), /unique/);
const badLocation = structuredClone(catalog);
badLocation.transmitters[0].latitude = 91;
assert.throws(() => validateCatalog(badLocation), /WGS84/);
const unknownStandard = structuredClone(catalog);
unknownStandard.transmitters[0].standard = 'unknown';
assert.throws(() => validateCatalog(unknownStandard), /unsupported/);
assert.throws(() => resolveCatalog(catalog, {plan: bandPlans()['atsc-us-uhf'],
  receiverLocation: {latitude: 40, longitude: -80}, radiusKm: 0,
  asOf: '2026-10-19T12:00:00Z'}), /radiusKm/);

console.log('illuminator catalog tests passed');
