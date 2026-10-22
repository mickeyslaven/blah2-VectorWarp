# Post-fix fresh cohort validation

Validated from the live evidence snapshot updated at
`2026-10-22T07:31:32.401Z`, with an independent cutoff of
`2026-10-22T06:23:13Z` (`1792650193000` ms).  The stricter cutoff is later than
the repository fresh-only policy cutoff.

## Admission result

The bounded cohort has **two positive clips and two geometry-eligible miss
clips**.  Every one of the 20 CPI midpoints in each 10-second clip is strictly
bracketed by event-time ADS-B projections.  Endpoint/nearest-sample hold and
event-prediction fallback are forbidden and were not used.  Maximum raw
observation offset is 0.845 s and maximum available two-source projected
disagreement is 0.005935 km, within the 1.0 s and 0.5 km limits.

| role | aircraft | clip | radar evidence inside clip | max raw offset | max disagreement |
|---|---|---:|---|---:|---:|
| positive | N445ME | r27 | first track +0.173 s; first detection +2.673 s; 1 tracked / 2 detected frames | 0.394 s | 0 km |
| positive | N520ME | r56 | first detection +0.927 s; first track +3.426 s; 4 tracked / 5 detected frames | 0.845 s | 0.005935 km |
| miss | N448CH | r63 | 0 tracked / 0 detected frames | 0.719 s | 0 km |
| miss | N307ME | r69 | 0 tracked / 0 detected frames | 0.684 s | 0 km |

The same physical r63 and r69 captures also reference pending N811ME and
SCX3046 events.  Those event/clip joins are retained in the manifest's rejected
rows and are not silently treated as the admitted miss truth.

## Geometry strata

Ranges below are over the 20 bracketed CPI truth points.  Drift is measured
from the first to last CPI, using 0.124913524 km delay bins and 4 Hz Doppler
bins.

| aircraft | altitude ft | speed kt | elevation deg | slant km | delay bins/s | Doppler bins/s |
|---|---:|---:|---:|---:|---:|---:|
| N445ME | 1778–1847 | 99.5–104.3 | 1.366–1.454 | 7.926–8.271 | +0.643 | -0.464 |
| N520ME | 1553–1577 | 79.3–86.4 | 0.842–0.942 | 7.759–8.181 | +0.351 | -0.691 |
| N448CH | 2179–2180 | 100.3–105.1 | 0.713–0.717 | 24.908–25.004 | +0.067 | -0.142 |
| N307ME | 2142–2150 | 122.0–122.2 | 0.477–0.480 | 35.778–36.264 | -0.777 | +0.078 |

This traffic does not contain a genuinely high-elevation or high-altitude
aircraft.  It therefore cannot distinguish an antenna dipole elevation-pattern
weakness from latency.  It does include the fastest aircraft, N307ME at about
122 kt, as a clean long-slant miss.  ADS-B latency is tightly bounded for every
row, so it is not a plausible explanation for these four classifications.

## RF identity and replay state

All rows share event config epoch
`d3ef34a4873e3946f68eb9e5c362740b637a8fa1b833a9c965888352b6808d29`.
The read-only live observation recorded config and loaded revision
`9d690eeedacb9553c92140d9a8a8cf7de0f606f6b7f9076ff17ad7801c491197`,
563 MHz, 2.4 MS/s, fixed gain index 2, and channel gains
19.7/25.4/20.7/25.4/22.9 dB.  No RF or service setting was changed.

Tailscale SSH was subsequently reauthorized.  All four files were streamed to
the CASS compute host, and their byte counts and SHA-256 identities matched the
recorder sidecars.  The existing frozen replay families were run against the
exact trajectories.  The compact, auditable matrix (including source-output
hashes, unthresholded ranks and margins) is
`results/20261022-postfix-fresh-replay-matrix.json`.

| clip | role | best loaded reciprocal | best noisy GLRT | HDR | best spatial | calibrated spatial stack |
|---|---|---:|---:|---:|---:|---:|
| r27 / N445ME | positive | -2.689 dB | -2.073 dB | -1.732 dB | -2.219 dB | -23.330 dB-eq |
| r56 / N520ME | positive | +2.663 dB | -1.256 dB | -1.529 dB | +2.369 dB | **+12.166 dB-eq** |
| r63 / N448CH | miss | -1.664 dB | -3.201 dB | -1.870 dB | -3.749 dB | -27.975 dB-eq |
| r69 / N307ME | miss | -2.471 dB | -0.261 dB | -1.456 dB | -1.786 dB | -15.654 dB-eq |

Margins are against each family's charged control p99; the stack margin is a
dB-equivalent empirical family-wise margin.  The r56 three-method spatial
stack opened the positive-control gate at +12.166 dB-equivalent.  Only then
were the two miss clips scored.  Neither miss is recovered by an individual
method or stack.  The r56 loaded-reciprocal target is +3.358 dB over the
single-loading p99 but +2.663 dB after the three-loading trial correction, so
it is not independently labeled +3 dB.

The strongest global noisy-GLRT/HDR peaks do not land in the predicted cell:
r27 is at (-13 delay, +79 Doppler) bins, r56 at (-8, +35), r63 at (-368, +4),
and r69's two families disagree at (-6, -235) and (-374, -107).  Sparse
delay-Doppler ranks are 246/324 (r27), 80/232 (r56), and 172/594 (r63); these
are not recoveries.  r69 cannot fit the method's full symmetric +/-4-delay-bin
corridor because its predicted delay is at the configured maximum-delay
boundary; no easier one-sided search was substituted.

The HDR output retains held-out-channel clipping and likelihood diagnostics,
but it does not define a separately calibrated per-channel echo margin.  Its
fixed five-channel/four-subband consensus margins above are the qualified
channel-integrity result; no post-hoc best-channel selection was performed.
All replay was offline.  Live RF and services were not changed.

The builder is deterministic and rejects stale provenance, missing event
times, dual-source disagreement, unbracketed CPI truth, non-final events, and
positive labels whose radar evidence does not overlap the actual IQ window.
