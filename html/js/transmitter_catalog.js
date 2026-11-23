(function(root, factory) {
  if (typeof module !== 'undefined' && module.exports)
    module.exports = factory(require('./processing_capabilities'));
  else root.VectorWarpTransmitterCatalog = factory(root.VectorWarpProcessingCapabilities);
})(typeof globalThis !== 'undefined' ? globalThis : this, function(capabilities) {
  'use strict';

  // Shared import/provider boundary. No receiver, OS, regional tuning defaults,
  // network fetches, or configuration mutations belong in this module.
  const LIMITS = Object.freeze({records: 2000, bytes: 4 * 1024 * 1024,
    results: 100, providers: 8, maxAgeMs: 7 * 86400000});
  const SNAPSHOT_SCHEMA = 'vectorwarp-transmitter-provider-v1';
  const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
  const text = (value, name, max = 512, empty = false) => {
    if (typeof value !== 'string' || value.length > max || (!empty && !value.trim()))
      throw new Error(`${name} must be ${empty ? 'a' : 'a nonempty'} string of at most ${max} characters`);
    return value;
  };
  const hz = (value, name) => {
    if (!Number.isSafeInteger(value) || value < 1 || value > 4294967295)
      throw new Error(`${name} must be positive whole Hz within the processor limit`);
    return value;
  };
  const date = (value, name) => {
    if (typeof value !== 'string' || !/^\d{4}-\d{2}-\d{2}T.*(?:Z|[+-]\d{2}:\d{2})$/.test(value) || !Number.isFinite(Date.parse(value)))
      throw new Error(`${name} must be an ISO-8601 timestamp with a time zone`);
    return value;
  };
  const sourceKeys = ['frequency_hz', 'channel_bandwidth_hz', 'occupied_bandwidth_hz', 'waveform', 'coordinates'];
  function validateProviderSource(record) {
    if (record.provider.id !== 'fcc-lms-us') return;
    if (record.provider.region !== 'US') throw new Error('FCC LMS records require region US');
    const official = (source, hosts) => {
      try { const url = new URL(source); return url.protocol === 'https:' && hosts.includes(url.hostname) && !url.username && !url.password && !/cdbs/i.test(url.pathname); }
      catch { return false; }
    };
    if (!official(record.provider.source, ['enterpriseefiling.fcc.gov', 'opendata.fcc.gov']))
      throw new Error('FCC LMS records require an official LMS source URL');
    if (!official(record.provenance.coordinates, ['geo.fcc.gov']) ||
        !new URL(record.provenance.coordinates).pathname.startsWith('/api/contours/antenna.'))
      throw new Error('FCC station coordinates require FCC Contours antenna provenance');
    if (!official(record.provenance.frequency_hz, ['enterpriseefiling.fcc.gov', 'opendata.fcc.gov']))
      throw new Error('FCC station frequency requires LMS station/facility provenance');
  }
  function validateRecord(input) {
    if (!object(input) || input.schema !== 1 || !object(input.provider) ||
        !object(input.waveform) || !object(input.transmitter) || !object(input.provenance))
      throw new Error('Station record requires schema 1, provider, waveform, transmitter, and field provenance');
    const p = input.provider, t = input.transmitter;
    const provider = {id: text(p.id, 'provider.id', 128), region: text(p.region, 'provider.region', 128),
      source: text(p.source, 'provider.source', 2048),
      retrieved_at: date(p.retrieved_at, 'provider.retrieved_at'),
      effective_at: p.effective_at === null ? null : date(p.effective_at, 'provider.effective_at')};
    if (Date.parse(provider.effective_at) > Date.parse(provider.retrieved_at))
      throw new Error('provider.effective_at cannot follow retrieved_at');
    if (![t.latitude, t.longitude].every(Number.isFinite) ||
        Math.abs(t.latitude) > 90 || Math.abs(t.longitude) > 180)
      throw new Error('Transmitter coordinates must be WGS84 latitude/longitude');
    if (t.altitude !== null && (!Number.isFinite(t.altitude) || t.altitude < -500 || t.altitude > 100000))
      throw new Error('Transmitter altitude must be AMSL metres from -500 to 100000, or null when unknown');
    const waveform = {standard: text(input.waveform.standard, 'waveform.standard', 64),
      version: text(input.waveform.version, 'waveform.version', 64)};
    if (waveform.standard === 'ATSC' && !['1.0', '3.0', 'unknown'].includes(waveform.version))
      throw new Error('ATSC waveform.version must explicitly be 1.0, 3.0, or unknown');
    const record = {schema: 1, id: text(input.id, 'id', 128), provider,
      callsign: text(input.callsign, 'callsign', 128, true), city: text(input.city, 'city', 128, true),
      state: text(input.state, 'state', 128, true),
      rf_channel: input.rf_channel === null ? null : text(input.rf_channel, 'rf_channel', 32),
      frequency_hz: hz(input.frequency_hz, 'frequency_hz'),
      channel_bandwidth_hz: hz(input.channel_bandwidth_hz, 'channel_bandwidth_hz'),
      occupied_bandwidth_hz: hz(input.occupied_bandwidth_hz, 'occupied_bandwidth_hz'), waveform,
      transmitter: {name: text(t.name, 'transmitter.name', 256), latitude: t.latitude,
        longitude: t.longitude, altitude: t.altitude},
      provenance: Object.fromEntries(sourceKeys.map(key => [key, text(input.provenance[key], `provenance.${key}`, 2048)]))};
    if (record.occupied_bandwidth_hz > record.channel_bandwidth_hz)
      throw new Error('occupied_bandwidth_hz cannot exceed the explicit channel_bandwidth_hz');
    if (record.frequency_hz <= record.channel_bandwidth_hz / 2)
      throw new Error('The channel lower frequency must be positive');
    validateProviderSource(record);
    return record;
  }

  function boundedJson(input) {
    const json = typeof input === 'string' ? input : JSON.stringify(input);
    if (typeof json !== 'string' || json.length > LIMITS.bytes || new TextEncoder().encode(json).length > LIMITS.bytes)
      throw new Error(`Catalog import is limited to ${LIMITS.bytes} bytes`);
    return JSON.parse(json);
  }
  function parseImport(input) {
    const parsed = boundedJson(input);
    const rows = Array.isArray(parsed) ? parsed : parsed?.schema === SNAPSHOT_SCHEMA ? parsed.records : [parsed];
    if (!Array.isArray(rows) || rows.length < 1 || rows.length > LIMITS.records)
      throw new Error(`Catalog import requires 1 to ${LIMITS.records} records`);
    const seen = new Set();
    return rows.map(row => {
      const record = validateRecord(row), key = JSON.stringify([record.provider.id, record.id]);
      if (seen.has(key)) throw new Error('Catalog record IDs must be unique within each provider');
      seen.add(key);
      return record;
    });
  }

  function freshness(record, {now = Date.now(), maxAgeMs = LIMITS.maxAgeMs} = {}) {
    if (!Number.isFinite(now) || !Number.isFinite(maxAgeMs) || maxAgeMs <= 0)
      throw new Error('Freshness requires a valid clock and positive maximum age');
    const ageMs = now - Date.parse(record.provider.retrieved_at);
    return {status: ageMs < -300000 ? 'future' : ageMs > maxAgeMs ? 'stale' : 'fresh',
      age_ms: ageMs, max_age_ms: maxAgeMs, retrieved_at: record.provider.retrieved_at,
      effective_at: record.provider.effective_at};
  }
  const normalized = value => String(value ?? '').normalize('NFKC').toLocaleLowerCase('en-US').trim();
  function search(records, criteria = {}) {
    if (!Array.isArray(records) || records.length > LIMITS.records)
      throw new Error('Search requires a bounded imported catalog');
    const limit = criteria.limit ?? 25;
    if (!Number.isInteger(limit) || limit < 1 || limit > LIMITS.results)
      throw new Error(`Search limit must be between 1 and ${LIMITS.results}`);
    const query = normalized(text(criteria.query ?? '', 'query', 256, true));
    const fields = ['city', 'state', 'callsign', 'rf_channel'];
    const filters = Object.fromEntries(fields.map(key => [key, normalized(text(criteria[key] ?? '', key, 128, true))]));
    const matches = records.filter(record => {
      const haystack = normalized([record.callsign, record.city, record.state, record.rf_channel,
        record.transmitter.name, record.provider.region].join(' '));
      return query.split(/\s+/).every(term => haystack.includes(term)) && fields.every(key =>
        !filters[key] || (key === 'rf_channel' ? normalized(record[key]) === filters[key] :
          normalized(record[key]).includes(filters[key])));
    });
    return {records: matches.slice(0, limit).map(record => ({...validateRecord(record),
      freshness: freshness(record, criteria)})), total: matches.length, truncated: matches.length > limit};
  }

  function nativeStandard(waveform) {
    if (waveform.standard === 'ATSC')
      return ['1.0', '3.0'].includes(waveform.version) ? `ATSC-${waveform.version}` : 'CUSTOM';
    if (['AM', 'FM', 'DAB', 'DRM', 'DVB-T', 'DVB-T2', 'ISDB-T', 'DTMB', 'CUSTOM'].includes(waveform.standard))
      return waveform.standard;
    return 'CUSTOM';
  }
  function previewSelection(input, options = {}) {
    const record = validateRecord(input), errors = [], warnings = [];
    const width = options.occupied_bandwidth_hz ?? record.occupied_bandwidth_hz;
    const altitude = options.altitude ?? record.transmitter.altitude;
    const override = width !== record.occupied_bandwidth_hz || altitude !== record.transmitter.altitude;
    if (override && (typeof options.reason !== 'string' || !options.reason.trim() || options.reason.length > 512))
      errors.push('An explicit bandwidth or altitude override requires a reason of at most 512 characters.');
    if (!Number.isSafeInteger(width) || width <= 0 || width > record.channel_bandwidth_hz)
      errors.push('Occupied bandwidth override must be positive whole Hz no wider than the source channel bandwidth.');
    if (!Number.isFinite(altitude) || altitude < -500 || altitude > 100000)
      errors.push('Antenna altitude AMSL is unknown or invalid; supply a verified explicit altitude before applying.');
    const standard = nativeStandard(record.waveform);
    const compatibility = capabilities.waveformReferenceCapability({standard,
      referenceMethod: options.reference_method ?? 'dedicated',
      atscReferenceMode: options.atsc_reference_mode ?? 'raw'});
    errors.push(...compatibility.errors);
    if (standard === 'CUSTOM') warnings.push('Waveform/version is unrecognized or unverified; raw IQ channelization remains available. Verify the waveform before selecting any specialized reference conditioning.');
    const age = freshness(record, options);
    if (age.status !== 'fresh') warnings.push(`Catalog source is ${age.status}; verify its current assignment and transmitter site before confirming.`);
    if (record.provider.effective_at === null) warnings.push('Source effective date is unknown; retrieval time does not establish current station authorization.');
    if (override) warnings.push('Explicit override retained separately from the original source values.');
    return {record, errors, warnings, freshness: age, compatibility,
      source_occupied_bandwidth_hz: record.occupied_bandwidth_hz,
      channel_bandwidth_hz: record.channel_bandwidth_hz,
      lower_hz: record.frequency_hz - width / 2, upper_hz: record.frequency_hz + width / 2,
      override: override ? {occupied_bandwidth_hz: width, altitude, reason: options.reason} : null,
      selection: {standard, frequency_hz: record.frequency_hz, occupied_bandwidth_hz: width,
        transmitter: {...record.transmitter, altitude},
        transmitter_location: {latitude: record.transmitter.latitude, longitude: record.transmitter.longitude, altitude}}};
  }
  // Returns a candidate only. The caller must display the preview, validate the
  // candidate against its capture plan, and obtain explicit confirmation.
  function stationSelection(record, options) {
    const preview = previewSelection(record, options);
    if (preview.errors.length) throw new Error(preview.errors.join(' '));
    return preview.selection;
  }

  // Providers expose the same bounded, offline search contract. Regulatory
  // download/normalization is a separate, explicit preparation operation.
  function createProvider(input) {
    const records = parseImport(input), id = records[0].provider.id;
    if (records.some(record => record.provider.id !== id || record.provider.region !== records[0].provider.region))
      throw new Error('A provider snapshot requires one provider ID and region');
    return Object.freeze({id, region: records[0].provider.region,
      search: criteria => search(records, criteria),
      exportSnapshot: () => ({schema: SNAPSHOT_SCHEMA, records: records.map(validateRecord)})});
  }
  function createCache() {
    const entries = new Map();
    return Object.freeze({
      put(input) {
        const provider = createProvider(input), snapshot = provider.exportSnapshot();
        const bytes = new TextEncoder().encode(JSON.stringify(snapshot)).length;
        const others = [...entries.entries()].filter(([id]) => id !== provider.id).map(([, value]) => value);
        if (others.length >= LIMITS.providers || others.reduce((sum, value) => sum + value.bytes, bytes) > LIMITS.bytes ||
            others.reduce((sum, value) => sum + value.count, snapshot.records.length) > LIMITS.records)
          throw new Error('Catalog cache capacity exceeded; remove an old provider snapshot before importing');
        entries.set(provider.id, {provider, bytes, count: snapshot.records.length});
        return provider;
      },
      get: id => entries.get(id)?.provider ?? null,
      remove: id => entries.delete(id),
      list: () => [...entries.values()].map(({provider, count}) => ({id: provider.id, region: provider.region, count}))
    });
  }
  return {LIMITS, SNAPSHOT_SCHEMA, validateRecord, parseImport, freshness, search,
    nativeStandard, previewSelection, stationSelection, createProvider, createCache};
});
