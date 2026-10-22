# ADS-B truth freshness and multi-source architecture

## Current state and the freshness bug

The configured path reads one dump1090/readsb-compatible `aircraft.json` once per
second. Feed discovery and HTTP/file reads are bounded and safe, and
`DelayDopplerHistory` correctly derives position event time as
`feed.now - aircraft.seen_pos`. The important distinction is that a newly
generated `aircraft.json` can contain an old position. A fresh top-level `now`
therefore does not make every aircraft fresh.

The evidence collector has a stricter, independent gate:
`process.bearing.manifold_recorder.max_truth_age_s` (3 seconds by default).
It increments `staleTruthFrames` and excludes older predictions from eligibility.
The geometry layer can retain positions up to `truth.adsb.max_position_age`
(30 seconds by default). This is intentional for display/motion continuity, but
it explains why an online feed can yield little usable recovery truth.

Before this change, `truth_samples` retained position event time and kinematics
but not the selected source, receive time, agreement set, quality score, or
disagreement status. Recovery builders deduplicate on `position_timestamp_s`
and linearly interpolate. They use the event prediction when no retained sample
is within 10 seconds; interpolation clamps outside the sampled interval to an
endpoint. That fallback is useful evidence, but it must not be confused with a
historically observed trajectory.

## Foundation now present

`api/adsb-fusion.js` is a dump1090/readsb-compatible fusion boundary. Even with
one configured source it now:

- normalizes every position to event time and rewrites `seen_pos` against the
  local receive time, preserving snapshot/network delay;
- rejects individually stale positions even when the containing file is fresh;
- attaches source, receive time, event time, age, and a bounded quality score;
- deduplicates repeated observations from one source;
- groups observations by ICAO address and geographic agreement; two independent
  agreeing sources beat a disagreeing singleton;
- selects an actually observed point rather than averaging two tracks into a
  position no source reported; and
- carries provenance into delay/Doppler predictions and persisted
  `truth_samples` as an additive field.

The live configuration is unchanged. This is deliberately a safe singleton
deployment of the multi-source boundary; it does not contact a new service or
touch replay manifests, state files, or the active 563/527 queues.

## Target adapters and normalized observation

Each adapter should emit the same immutable observation:

```text
icao24, callsign
position {lat_deg, lon_deg, geom_alt_ft, baro_alt_ft}
motion {ground_speed_kt, track_deg_true, vertical_rate_fpm}
event_time_utc, source_snapshot_time_utc
request_started_utc, received_at_utc, receive_monotonic
source_id, source_kind, license_class, receiver_ids
integrity {nic, nac_p, sil, rc_m, position_source}
raw_age_s, corrected_age_s, clock_offset_s, latency_uncertainty_s
```

Adapters, in preferred order:

1. Local readsb/dump1090 file or loopback HTTP. This stays primary: lowest
   latency, no third-party availability or licensing dependency. readsb documents
   atomic roughly one-second file updates, top-level `now`, and per-aircraft
   `seen_pos`.
2. An ADS-B Exchange-compatible point/radius adapter. `adsb.fi` v3 is suitable
   for a non-commercial shadow trial: radius is at most 250 NM, public access is
   limited to one request/second, attribution is required, and commercial use
   requires contact. Make endpoint, attribution, and license mode explicit.
3. OpenSky state vectors through OAuth2. Its timestamps (`time_position` and
   `last_contact`) map cleanly, but anonymous data is only 10-second resolution
   and authenticated data 5-second resolution. More importantly, operational
   REST integration requires a written license, so it is not a default live
   failover. `/states/own` is the exception for a user's own receivers.
4. A contracted source for operational live and archive needs. ADS-B Exchange
   advertises 5 s/500 ms/250 ms live products and daily position archives with
   historical backfills; FlightAware AeroAPI provides current and historical
   tracks under tier-specific storage/use rights and request limits. Select only
   after use and retention rights are reviewed.

Do not scrape map frontends or treat an undocumented same-day trace URL as an
archive. A failed/403 trace request is absence of entitlement, not evidence that
the aircraft had no track.

## Event time and clock/latency correction

Keep three clocks, never one overloaded timestamp:

- raw event time supplied or derived by the source;
- local wall-clock receive time for cross-process/replay correlation; and
- local monotonic receive time for timeout, ordering, and latency measurement.

For readsb, raw event time is `now - seen_pos`. For OpenSky it is
`time_position`. For ADS-B Exchange-compatible data it is the documented source
timestamp minus position age. Record request start/end; use the midpoint only to
estimate remote clock offset. Maintain a bounded robust EWMA of offset and RTT,
reject abrupt clock steps, and expose uncertainty. Never silently make an old
snapshot fresh by subtracting an unbounded clock offset. A correction is usable
for eligibility only when offset uncertainty is below 250 ms; otherwise use raw
event time and lower quality.

## Fusion and disagreement policy

Partition by normalized ICAO address, but do not merge non-ICAO (`~`) addresses
across receivers without another identity. Deduplicate same-source points by
event time and source message identity. Project candidates to a common fusion
time using reported ground speed/track/vertical rate for at most 5 seconds.

Cluster with an uncertainty-aware gate:

```text
horizontal gate = max(1 km, rc_a + rc_b + 0.15 km/s * age difference)
vertical gate   = max(500 ft, reported accuracy, 100 ft/s * age difference)
```

Rank clusters by independent source count, then integrity, corrected age,
latency uncertainty, receiver locality, and configured reliability. Use a
weighted medoid (an observed point) for two sources and a robust weighted median
for three or more. Record all contributors and rejected candidates. A lone
outlier remains available with `disagreement=true`, but cannot create recovery
eligibility unless its source is explicitly trusted. Callsign and altitude
disagreement do not overwrite a good position; fuse each field with its own
timestamp and provenance.

Source health is a circuit breaker, not just HTTP success: track poll success,
fresh-position fraction, timestamp regressions, clock uncertainty, duplicates,
and disagreement rate. Open after three failures or ten seconds without usable
positions, retry with exponential backoff plus jitter, and recover after three
healthy polls. The local source remains preferred when quality is equivalent;
an aggregator fills stale or missing aircraft rather than replacing the whole
snapshot.

## Historical replay

Persist the normalized pre-fusion observations and fusion decisions in an
append-only, hourly rotated journal (NDJSON initially; Parquet when volume
requires it). Each segment needs schema version, source/license identity,
request/receive/event timestamps, raw payload digest, adapter version, and
retention policy. Build an index by `(icao24, event_time)` and replay adapters
through the identical deterministic fusion function with an injected clock.

Recovery manifests should advance to v2 and include per-frame:

- `truth_status`: `bracketed`, `extrapolated`, or `fallback`;
- left/right observation IDs and sources;
- interpolation span and nearest-observation age;
- fused quality, disagreement, and uncertainty; and
- the fusion/adapter version and raw journal segment digest.

Do not silently endpoint-clamp an entire clip. Bracketed interpolation may be
accepted up to a 3-second sample gap. Extrapolation is capped at 2 seconds and
must use velocity with expanded uncertainty. Anything else remains a searchable
candidate but is excluded from truth-qualified acceptance. Provider terms must
permit the chosen raw retention and derivative use.

## Staged rollout and acceptance

1. **Singleton provenance (implemented):** all fresh derived predictions and new
   `truth_samples` carry provenance; stale aircraft never enter fusion. Existing
   source, UI shape, and geometry remain compatible.
2. **Shadow adapter:** poll one approved point/radius API no faster than its
   limit, journal observations, and calculate fusion without feeding evidence.
3. **Gated live fusion:** enable aggregator fill only after the shadow metrics
   pass. Keep a runtime switch back to local-only and never change receiver
   decoder configuration.
4. **Archive replay:** add the journal/index and v2 manifest builder; migrate old
   v1 samples as `source=legacy_unknown`, never fabricate provenance.

Acceptance over a representative 24-hour run:

- 100% of selected positions have event time, receive time, source ID, age, and
  fusion decision; zero timestamp regressions per aircraft;
- at least 95% of truth-qualified frames have corrected position age <= 3 s,
  reported separately for local-only and fused modes;
- fused common-aircraft residual is median <= 250 m and p95 <= 1 km after
  projection; all > 3 km disagreements are flagged and never blended;
- a source outage fails over within two polling intervals, with no duplicate
  eligibility event and no timestamp moving backward;
- disabling fusion produces the current local-only aircraft set and derived
  delay/Doppler within floating-point tolerance;
- every recovery frame accepted as truth is bracketed by observations no more
  than 3 seconds apart, or explicitly extrapolated no more than 2 seconds;
- replaying a captured journal twice produces byte-identical fusion decisions
  and manifest truth trajectories; and
- no active replay queue, generated result, live state, or receiver setting is
  modified by rollout or validation.

## Authoritative references checked 2026-10-21

- readsb JSON format: <https://github.com/wiedehopf/readsb/blob/dev/README-json.md>
- OpenSky REST API and current credit limits:
  <https://github.com/openskynetwork/opensky-api/blob/master/docs/free/rest.rst>
- OpenSky operational-use terms: <https://opensky-network.org/about/terms-of-use>
- adsb.fi API, limits, and terms: <https://github.com/adsbfi/opendata/blob/main/README.md>
- ADS-B Exchange live/archive product characteristics:
  <https://www.adsbexchange.com/data-products/>
- FlightAware AeroAPI tiers, history, limits, and use rights:
  <https://www.flightaware.com/commercial/aeroapi/>
