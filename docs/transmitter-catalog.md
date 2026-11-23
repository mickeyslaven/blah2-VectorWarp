# Portable transmitter catalog import

Settings accepts a reviewed station record or a bounded JSON snapshot through
the shared `html/js/transmitter_catalog.js` contract. Search is offline by city,
state, callsign, RF channel, or combined text. An import is a preview: the user
must review the passband, source, transmitter coordinates, reference method,
and any explicit override, then confirm before a configuration candidate changes.
The normal configuration validation/save path remains authoritative.

There is no automatic national station download yet. **FCC raw downloads must
be normalized and imported until the authoritative LMS field mapping is
supplied.** No CDBS scraper or guessed table-column mapping is included.

## Record contract

The following is a fictional fixture, not a real station or a regional default:

```json
{
  "schema": 1,
  "id": "example-facility-antenna",
  "provider": {
    "id": "operator-file",
    "region": "example-region",
    "source": "operator survey receipt example-123",
    "retrieved_at": "2026-11-23T00:00:00Z",
    "effective_at": null
  },
  "callsign": "EXAMPLE",
  "city": "Example City",
  "state": "Example State",
  "rf_channel": "27",
  "frequency_hz": 522000000,
  "channel_bandwidth_hz": 8000000,
  "occupied_bandwidth_hz": 7600000,
  "waveform": {"standard": "ATSC", "version": "3.0"},
  "transmitter": {
    "name": "Example antenna",
    "latitude": 0.5,
    "longitude": 1.5,
    "altitude": 100
  },
  "provenance": {
    "frequency_hz": "receipt example-123 assignment",
    "channel_bandwidth_hz": "receipt example-123 regional channel plan",
    "occupied_bandwidth_hz": "receipt example-123 measured waveform profile",
    "waveform": "receipt example-123 verified waveform version",
    "coordinates": "receipt example-123 WGS84 antenna position and AMSL altitude"
  }
}
```

Coordinates are WGS84; altitude is antenna altitude in metres AMSL, not height
above ground. Unknown altitude is `null` and prevents applying until the user
supplies an explicit verified altitude and reason. Unknown effective date is
`null` and is shown as unknown. RF channel is a string or `null`; virtual display
channel numbers must not substitute for RF assignments.

Channel bandwidth and occupied bandwidth are separate required source values.
The module never fills either from a channel number, region, or waveform name.
An occupied-width override must be positive whole Hz, no greater than source
channel width, accompanied by a reason, and pass the existing capture/output-rate
preview. Original values and field provenance remain in the preview record and
exported snapshot. Native selection fields carry the reviewed candidate; source
metadata is not injected into native processing settings.

ATSC requires version `1.0`, `3.0`, or `unknown`. Six MHz never implies ATSC 1.0.
Unknown/unrecognized waveforms map to `CUSTOM` raw IQ with a warning. Raw
dedicated and qualified array references use the shared channelizer; ATSC 1.0
pilot conditioning requires a dedicated reference. ATSC 3.0 pilot conditioning
and waveform payload synthesis are unavailable and produce actionable errors.
The same capability predicate is used by setup preview and API validation.

## Providers, bounded cache, and FCC ingestion dependency

`parseImport` accepts one record, an array, or
`{"schema":"vectorwarp-transmitter-provider-v1","records":[...]}`.
`createProvider` requires one provider ID/region and exposes `search(criteria)`
and `exportSnapshot()`. `createCache` holds at most eight providers, 2,000 total
records, and 4 MiB; a single import has the same record/byte bounds. Searches
return at most 100 rows. Snapshots are copied so callers cannot alter cached
source facts. Persistence is explicit through snapshot export/import; the
module does not write browser storage, files, or perform network requests.
Results expose retrieval/effective dates and freshness; the default stale
threshold is seven days. Stale/future sources remain searchable with warnings.
Retrieval age does not prove a license remains current.

The API's existing `illuminator-catalog.js` owns source discovery and wraps the
shared station provider with a SHA-256 snapshot fingerprint. Its older survey
catalog format remains a survey input; it lacks required setup facts and is not
silently converted to setup records. Other countries can supply the same
neutral record contract without implementing new downstream radar math.

The U.S. adapter preparation step must use the [FCC LMS public database
files](https://enterpriseefiling.fcc.gov/dataentry/public/tv/lmsDatabase.html)
for current facility/station/application facts and the [FCC Contours antenna
endpoint](https://geo.fcc.gov/api/contours/) for antenna coordinates. The latter
documents `serviceType` plus `facilityId`, `applicationId`, or `callsign` lookup.
FCC imports must identify official LMS frequency/source URLs and the Contours
antenna URL as coordinate provenance. URL validation checks claimed source
location; it does not authenticate an operator-supplied record's contents.

The remaining ingestion work requires a current LMS data dictionary and small
licensed fixture extracts establishing: facility/application/license status and
validity joins; physical RF channel/frequency; per-antenna or DTS identifiers;
explicit ATSC version (a generic digital-TV service code is insufficient); and
antenna coordinate datum/AMSL altitude. The adapter must retain ambiguous sites
separately and reject incomplete joins. Channel width needs a cited regional
assignment source; occupied width needs a verified waveform profile or measured
operator evidence, because channel width does not determine occupied carriers.
Unknown waveform versions must remain unknown. Pin normalized records with
retrieval/effective dates and per-field provenance before import. Large source
archives, generated national catalogs, and credentials do not belong in this
repository. The LMS landing page returned HTTP 403 during implementation, so no
unverified download/parser behavior is claimed.

Validation: `node api/transmitter-catalog.test.js`,
`node api/illuminator-catalog.test.js`, `node api/config-manager.test.js`, and
`node api/processing-capabilities.test.js`.
