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

Finalized passes are also broken down by receiver slant range, excess bistatic
delay, true azimuth, geometric elevation, and a joint azimuth-by-elevation
matrix. Every cell reports its eligible-pass denominator separately from
detection and tracking yield so sparse traffic is visible instead of being
mistaken for a directional conclusion.

ADS-B must be enabled to collect these pass statistics. A local receiver alone
can add feed and host-arrival timing uncertainty. Enabling an online ADS-B
source provides an independent coverage and observation-time cross-check, but
does not replace the radar receiver's exact SDR event time. VectorWarp labels
local-only statistics with this limitation and keeps inexact timing out of the
decision-grade scorecard.

Opportunity admission is driven by a bounded, unreferenced one-second ADS-B
sampler, not by detector output. On the first fresh prediction inside the
physical delay/Doppler span, `/api/statistics` adds one `pending` eligibility
event for the pass. The event has a stable pass ID, receiver/configuration/site
epoch fingerprints, predicted delay, Doppler and geometry, and an explicit
capture state (`not_integrated` without a trigger consumer, `trigger_pending`
when queued, `acknowledged` with its clip path/sample count, `failed` with the
writer error, or `dropped` with a queue failure reason). When the pass closes, the same event
is finalized as tracked, detected-not-tracked, no-radar-return,
truth-unusable, or insufficient-opportunities. An event can therefore record a
single valid opportunity without adding that pass to the three-frame tracking
denominator. Events persist with the statistics file, are bounded to 10,000 on
disk, and the API returns the most recent 250.

When **Automatic aircraft clips** is explicitly enabled, each new eligibility
event also enters a 64-clip API-side trigger queue with a
conservative two-second pre-roll and three-second post-roll request. Physical
clip windows are hard-capped at ten seconds. Pending windows overlap-coalesce
only while their union stays within that cap; an extension that would exceed it
rolls the event into a new clip that covers the event's complete five-second
window. Each event remains linked to its clip revision. If all 64 slots are
pending, the new event is retained with an explicit `dropped` state and
`queue-capacity` failure instead of silently evicting another request.
The setting is off by default. `GET /capture/request` and
`GET /capture/status` expose the current request under `clip`, including its
event/pass IDs, event time, pre/post-roll, window bounds, pending/acknowledged
state and failure text. The legacy manual `recording` and `revision` fields and
the `/capture` boolean remain unchanged. Live Kraken capture keeps a bounded
compact raw-wire ring and hands complete clip jobs to a bounded low-priority
writer without waiting on disk from the receive thread. The default request
records two seconds before the eligibility event and three seconds after it;
no physical clip may exceed ten seconds. Completed clips are replayable MCHQ
files with all configured channels in Heimdall order and an adjacent JSON
sidecar containing event/pass associations, exact window and sample bounds,
tuning/gain metadata, and the IQ file SHA-256. A retune, synchronization loss,
ring/writer saturation, or disk failure fails the clip explicitly through
processor status without stopping radar. Other live receiver types acknowledge
the request as unsupported. No detector or track result can enqueue a clip.

### Cumulative recovery promotion

Replay experiments are cumulative. A candidate that recovers at least one
previously missed, physically eligible ADS-B aircraft without losing any
baseline-matched aircraft enters an **active guarded bank**; it does not replace
the baseline detector or tracker. The bank must also stay inside the processing
deadline and its unmatched detection/track increase must remain within the
declared experiment bound. The event ledger records the candidate/config
fingerprint and the aircraft it uniquely recovered. Subsequent experiments run
the baseline plus every retained bank and optimize only the remaining misses.

One recovered recording is sufficient for guarded canary admission, not
unconditional global promotion. Live evidence automatically retains the bank
only while net eligible-aircraft yield is nondecreasing, previously tracked
aircraft remain tracked, capture drops stay flat, and track fragmentation and
stationary/unmatched output remain bounded. A violated guard removes that bank
and restores the prior cumulative set. The target is 100% of fresh ADS-B
aircraft whose predicted delay and Doppler are inside the configured physical
support; out-of-span or stale-truth aircraft never enter that denominator.

Each pass retains bounded diagnostic truth: its first and last complete
position/kinematic snapshots; geometric and barometric altitude; true bearing,
elevation, ground/slant/transmitter ranges; ground speed, course and vertical
rates; eight true-azimuth sector counts; and maximum observed delay and Doppler
rates. A site-keyed descriptor preserves the exact RF, geometry, cancellation,
detection, integration and tracking settings used for the pass. This supports
later direction, altitude, range and fast-motion miss analysis without keeping
an unbounded duplicate of the raw ADS-B feed. Version-1 statistics files are
upgraded in place; geometry that was not historically recorded remains unknown.

The persistent file defaults to `evidence-statistics.json` beside the configured
manifold-recorder corpus. `BLAH2_STATISTICS_PATH` can select another writable
location.

The opt-in ADS-B-triggered IQ window defaults to two seconds before and three
seconds after the eligibility event. Configure it in **Settings → Receiver →
Automatic aircraft clips**; the sum must fit the native ten-second clip limit.
Environment overrides remain available for managed installations. The status
endpoint reports the applied bounds.

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
