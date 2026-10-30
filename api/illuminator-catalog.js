'use strict';

const crypto = require('crypto');

const SCHEMA = 'vectorwarp-illuminator-transmitter-catalog-v1';
const STANDARDS = new Set(['FM', 'DAB', 'ATSC', 'DVB-T/T2']);
const VERIFICATION = new Set(['regulator', 'operator-verified', 'unverified']);
const MAX_TRANSMITTERS = 500000;

// These are discovery endpoints, not silently trusted live dependencies. A
// refresh worker must download, normalize and pin an immutable snapshot before
// a survey preview can use it.
const SOURCES = Object.freeze([
  Object.freeze({id: 'fcc-lms-us', authority: 'US Federal Communications Commission',
    jurisdiction: 'US', standards: Object.freeze(['FM', 'ATSC']),
    landing_url: 'https://opendata.fcc.gov/Media/LMS-Public-Database-Files/nsck-y87u',
    update_mode: 'download-normalize-pin', coordinate_system: 'WGS84'}),
  Object.freeze({id: 'ofcom-txparams-vhf-uk', authority: 'Ofcom', jurisdiction: 'UK',
    standards: Object.freeze(['FM']),
    landing_url: 'https://www.ofcom.org.uk/tv-radio-and-on-demand/coverage-and-transmitters/radio-tech-parameters',
    download_url: 'https://www.ofcom.org.uk/siteassets/resources/documents/spectrum/tv-transmitter-guidance/tech-parameters/txparamsvhf.csv',
    update_mode: 'download-normalize-pin', coordinate_system: 'OSGB36-grid-source/WGS84-normalized'}),
  Object.freeze({id: 'ofcom-txparams-dab-uk', authority: 'Ofcom', jurisdiction: 'UK',
    standards: Object.freeze(['DAB']),
    landing_url: 'https://www.ofcom.org.uk/tv-radio-and-on-demand/coverage-and-transmitters/radio-tech-parameters',
    download_url: 'https://www.ofcom.org.uk/siteassets/resources/documents/spectrum/tv-transmitter-guidance/tech-parameters/txparamsdab.csv',
    update_mode: 'download-normalize-pin', coordinate_system: 'OSGB36-grid-source/WGS84-normalized'}),
  Object.freeze({id: 'ofcom-dtt-uk', authority: 'Ofcom', jurisdiction: 'UK',
    standards: Object.freeze(['DVB-T/T2']),
    landing_url: 'https://www.ofcom.org.uk/tv-radio-and-on-demand/coverage-and-transmitters/transmitter-frequency',
    update_mode: 'download-normalize-pin', coordinate_system: 'OSGB36-grid-source/WGS84-normalized'}),
  Object.freeze({id: 'operator-file', authority: 'Operator supplied', jurisdiction: 'any',
    standards: Object.freeze([...STANDARDS]), update_mode: 'validate-and-pin',
    coordinate_system: 'WGS84'})
]);

function catalogSources() {
  return SOURCES.map(source => ({...source, standards: [...source.standards]}));
}

function finite(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

function nonempty(value) {
  return typeof value === 'string' && value.trim().length > 0;
}

function timestamp(value, field) {
  if (!nonempty(value) || !Number.isFinite(Date.parse(value)))
    throw new Error(`${field} must be an ISO-8601 timestamp`);
}

function canonical(value) {
  if (Array.isArray(value)) return `[${value.map(canonical).join(',')}]`;
  if (value && typeof value === 'object') return `{${Object.keys(value).sort().map(key =>
    `${JSON.stringify(key)}:${canonical(value[key])}`).join(',')}}`;
  return JSON.stringify(value);
}

function catalogFingerprint(catalog) {
  return crypto.createHash('sha256').update(canonical(catalog)).digest('hex');
}

function validateCatalog(catalog) {
  if (!catalog || typeof catalog !== 'object' || Array.isArray(catalog))
    throw new Error('Transmitter catalog must be an object');
  if (catalog.schema !== SCHEMA) throw new Error(`Transmitter catalog schema must be ${SCHEMA}`);
  const source = catalog.source;
  if (!source || !nonempty(source.id) || !nonempty(source.authority) ||
      !nonempty(source.jurisdiction) || !nonempty(source.url))
    throw new Error('Transmitter catalog source metadata is incomplete');
  timestamp(source.retrieved_at, 'source.retrieved_at');
  timestamp(source.effective_at, 'source.effective_at');
  if (!Array.isArray(catalog.transmitters) || catalog.transmitters.length > MAX_TRANSMITTERS)
    throw new Error(`Transmitter catalog must contain at most ${MAX_TRANSMITTERS} transmitters`);

  const ids = new Set();
  for (const [index, transmitter] of catalog.transmitters.entries()) {
    const prefix = `transmitters[${index}]`;
    if (!transmitter || !nonempty(transmitter.id) || ids.has(transmitter.id))
      throw new Error(`${prefix}.id must be unique and non-empty`);
    ids.add(transmitter.id);
    if (!STANDARDS.has(transmitter.standard)) throw new Error(`${prefix}.standard is unsupported`);
    if (!Number.isSafeInteger(transmitter.frequency_hz) || transmitter.frequency_hz <= 0)
      throw new Error(`${prefix}.frequency_hz must be a positive integer`);
    if (!finite(transmitter.latitude) || transmitter.latitude < -90 || transmitter.latitude > 90 ||
        !finite(transmitter.longitude) || transmitter.longitude < -180 || transmitter.longitude > 180)
      throw new Error(`${prefix} must have valid WGS84 latitude and longitude`);
    if (!finite(transmitter.coordinate_uncertainty_m) || transmitter.coordinate_uncertainty_m < 0)
      throw new Error(`${prefix}.coordinate_uncertainty_m must be non-negative`);
    if (!VERIFICATION.has(transmitter.verification_status))
      throw new Error(`${prefix}.verification_status is unsupported`);
    if (!nonempty(transmitter.source_record_id))
      throw new Error(`${prefix}.source_record_id must be non-empty`);
    if (transmitter.antenna_altitude_m_amsl !== null &&
        !finite(transmitter.antenna_altitude_m_amsl))
      throw new Error(`${prefix}.antenna_altitude_m_amsl must be a number or null`);
    if (transmitter.valid_from !== undefined) timestamp(transmitter.valid_from, `${prefix}.valid_from`);
    if (transmitter.valid_to !== undefined) timestamp(transmitter.valid_to, `${prefix}.valid_to`);
    if (transmitter.valid_from && transmitter.valid_to &&
        Date.parse(transmitter.valid_from) >= Date.parse(transmitter.valid_to))
      throw new Error(`${prefix} validity interval is empty`);
  }
  return {schema: SCHEMA, fingerprint_sha256: catalogFingerprint(catalog),
    source: {...source}, transmitters: catalog.transmitters.map(item => ({...item}))};
}

function haversineKm(a, b) {
  const radians = degrees => degrees * Math.PI / 180;
  const dLat = radians(b.latitude - a.latitude);
  const dLon = radians(b.longitude - a.longitude);
  const lat1 = radians(a.latitude), lat2 = radians(b.latitude);
  const chord = Math.sin(dLat / 2) ** 2 + Math.cos(lat1) * Math.cos(lat2) *
    Math.sin(dLon / 2) ** 2;
  return 6371.0088 * 2 * Math.atan2(Math.sqrt(chord), Math.sqrt(1 - chord));
}

function resolveCatalog(catalog, {plan, receiverLocation, radiusKm, asOf}) {
  const snapshot = validateCatalog(catalog);
  if (!plan || !STANDARDS.has(plan.standard) || !Array.isArray(plan.candidates))
    throw new Error('Catalog resolution requires a supported band plan');
  if (!receiverLocation || !finite(receiverLocation.latitude) ||
      receiverLocation.latitude < -90 || receiverLocation.latitude > 90 ||
      !finite(receiverLocation.longitude) || receiverLocation.longitude < -180 ||
      receiverLocation.longitude > 180)
    throw new Error('Catalog resolution requires a valid WGS84 receiver location');
  if (!finite(radiusKm) || radiusKm <= 0 || radiusKm > 2000)
    throw new Error('Catalog resolution radiusKm must be greater than 0 and at most 2000');
  timestamp(asOf, 'asOf');
  const instant = Date.parse(asOf);
  const frequencies = new Set(plan.candidates.map(item => item.frequency_hz));
  const matches = snapshot.transmitters.filter(transmitter =>
    transmitter.standard === plan.standard && frequencies.has(transmitter.frequency_hz) &&
    (!transmitter.valid_from || Date.parse(transmitter.valid_from) <= instant) &&
    (!transmitter.valid_to || instant < Date.parse(transmitter.valid_to)))
    .map(transmitter => ({...transmitter,
      distance_km: haversineKm(receiverLocation, transmitter)}))
    .filter(transmitter => transmitter.distance_km <= radiusKm)
    .sort((a, b) => a.distance_km - b.distance_km ||
      a.frequency_hz - b.frequency_hz || a.id.localeCompare(b.id));
  return {schema: 'vectorwarp-illuminator-catalog-resolution-v1',
    catalog_fingerprint_sha256: snapshot.fingerprint_sha256,
    catalog_source: snapshot.source, as_of: asOf, radius_km: radiusKm,
    receiver_location: {...receiverLocation}, transmitters: matches};
}

module.exports = {SCHEMA, catalogSources, catalogFingerprint, validateCatalog,
  resolveCatalog, haversineKm};
