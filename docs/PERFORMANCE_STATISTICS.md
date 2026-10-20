# Performance statistics

VectorWarp exposes persistent, configuration-aware evidence at
`GET /api/statistics` and in **Displays → Performance statistics**.

## Tracking attribution

The denominator is not every aircraft present in the ADS-B receiver. For every
ADS-B prediction, VectorWarp converts the active ambiguity delay bins to
physical bistatic kilometres using the active sample rate, then checks:

- the ambiguity-map delay limits;
- the detector's minimum delay;
- the ambiguity-map Doppler limits; and
- truth freshness.

Aircraft are grouped into passes using a two-minute absence boundary. Three
fresh, physically eligible observations are required before a pass enters the
performance denominator. The page reports four mutually useful outcomes:

- **Out of config:** the aircraft never had three opportunities inside the
  configured delay and Doppler search region. This is not a radar miss.
- **No radar return:** the pass was eligible but produced neither three strict
  detections nor three strict track matches. This isolates the combined RF,
  cancellation and detector loss.
- **Detected, not tracked:** the pass produced persistent strict detections but
  no persistent strict track. This isolates tracker/initiation loss.
- **Tracked:** the pass produced at least three strict published-track matches.

Matches use the manifold recorder's strict delay/Doppler gates (0.25 km and
5 Hz by default), assign a radar point to only one unambiguous aircraft, and
refuse near-tied assignments. Published hybrid tracks are scored because they
are the tracks exposed to downstream consumers. The page also reports stale
truth and opportunities outside the delay floor, delay ceiling, Doppler span,
or clutter-cancellation span.

Hourly, daily and weekly views are rolling windows. Lifetime statistics apply
to the current site fingerprint. The fingerprint includes frequency, sample
rate, receiver and transmitter sites, search limits, channel selection, array
geometry and bearing mapping; incompatible sites are never mixed.

Pass outcomes are finalized only after the aircraft leaves the opportunity
stream for two minutes. The API reports active and provisionally eligible pass
counts separately, so a newly appearing aircraft is never temporarily mislabeled
as out of configuration or missed.

The persistent file defaults to `evidence-statistics.json` beside the configured
manifold-recorder corpus. `BLAH2_STATISTICS_PATH` can select another writable
location.

## Bearing evidence

The same page consolidates VectorWarp's built-in bearing evidence:

- current bearing, publishable, resolved and stationary counts;
- manifold-recorder accepted and rejected sample counts;
- aircraft and azimuth-sector coverage;
- primary active-mapping median error and within-45-degree rate;
- truth-assisted best-mapping and heading-only diagnostics;
- reference sample count, phase stability and amplitude stability;
- measured-manifold training coverage;
- held-out pass and sector counts, ideal-versus-measured errors, and unmet
  qualification gates; and
- frequency-resolved reference/multipath diagnostics.

Historical bearing replay is intentionally isolated from radar processing. A
low-priority child process refreshes a durable cache at most hourly; API and UI
requests only read that cache. A slow or failed replay therefore cannot block
live CPI ingestion, detection, tracking, or the web API. The diagnostic endpoint
can enqueue an early refresh with `?refresh=true`, but still returns immediately.

Truth-assisted best-lobe, channel-order and heading results remain explicitly
diagnostic. They are not presented as calibrated bearing accuracy. A measured
manifold is called calibrated only after the existing independent-pass,
multi-aircraft, multi-sector and held-out validation gates qualify it.
