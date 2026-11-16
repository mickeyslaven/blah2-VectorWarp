# Temporal integration route matrix — 2026-11-16

This is a source-level contract audit. ADS-B remains evaluation-only and is not
an inference input.

| Route | Native multi-CPI detector | Motion alignment | Operational effect | Current gap |
|---|---|---|---|---|
| Kraken / USRP / RSPduo, single-lane CPU or portable GPU | Shared two-CPI integration and 2/4/8-CPI power TBD | Integer delay/Doppler and explicitly selected fractional-delay trajectories | Common detector, stationary suppression, disjoint guided recovery and tracker | Native execution of new wiring remains pending; FP64 host arithmetic is bounded explicitly |
| Single-lane GPU-resident | Same shared host products for noncoherent fusion; native coherent statistics for qualified arrays | Same configured motion contract; beam-conditioned support requires coherent array/resident capability | Host or resident implementation under common identity/publication | Numerical parity requires device execution, not matching schemas |
| HackRF paired capture, CPU or portable GPU | Same power integration/TBD contract | Same power-motion alignment | Common detector and tracker recovery | Separate HackRF devices do not establish stable inter-channel phase; no coherent-array capability is invented |
| Explicit recording | Same capability/backend matrix as its recorded topology | Determined by requested statistic and qualified backend, not dormant live SDR name | Canonical roles and capture lineage feed the selected detector/tracker | Recording declaration is not evidence of physical phase calibration |
| Multi-illuminator channelizer | Same production integration/TBD and tracker recovery; bounded resident frontend or coherent product sessions | Same common host motion contract; native beam-conditioned histories where qualified | Independent lane state and common exact publisher, including resident coherent/noise-whitened/bearing products | Per-lane transmitter site and calibration identity required; new adapter still needs compiled/device parity checks |

## Shared CPU and portable-GPU integration contract

`DetectorIntegration` is the shared production orchestration for single-lane
CPU/portable-GPU and channelized CPU processing. It uses `TemporalEvidence`
with a bounded two-, four- or eight-CPI history. Its selected calibrated banks feed shared
`detectProducts` and `trackProducts` orchestration around `CfarDetector1D`,
`Centroid`, `Interpolate` and `Tracker`. There is no receiver-name or OS gate on
this power-integration path.
`CfarDetector1D` uses the matching Gamma(2/4/8) `IntegratedCfar` thresholds.
Ordinary two-CPI one-bin motion includes nine trials. TBD pre-registers half
the configured PFA for baseline2 and half across every configured motion
path/depth; warming banks do not reclaim another family's budget. Candidates
are centroided/interpolated within their originating bank before deduplication,
not against the maximum display map. The same threshold split is used by the
resident implementation. A single-CPI warm-up map produces no integrated
detection. Arithmetic remains FP64; the explicit cell, byte and path-work caps
reject oversized jobs instead of downsampling or narrowing precision.

Each lane owns its history. Sample gaps, replayed sample ranges, capture-epoch
or position-availability changes, clutter rejection, and explicit reset clear
the adaptive history. Unavailable capture positions cannot warm integration.
Frame diagnostics identify the capture epoch, history epoch, contiguous
support sequences and exact half-open input sample interval. Single-lane and
multi-lane publications and saves carry the same identity/diagnostic schema.
Signal-time deltas use capture samples, while publication retains event-time
provenance. Source capture epoch zero is valid. Ordinary confirmation remains
M-of-N; enabling guided promotion adds disjoint support credit and requires
the configured feasible confirmation window. Guided support never creates
measured target history or a track birth; shadow-only evaluation cannot mutate
operational state. This does not claim measured CPU/GPU parity.

Production power TBD, stationary suppression and guided recovery are now
implemented in the common host contract. They are not resident/OS/name-gated.
Coherent fusion, beam consistency and bearing still require capable topology
and are not implied by enabling power integration. The shared normalized
innovation gate setting now also reaches the CPU lane tracker.

Focused API validation passed for the two-CPI/topology change. C++ regression
cases were added, not built or run in this implementation pass. No services,
models, full CI jobs or deployed binaries were changed.

## Separate weak-support observer contract (not production detector depth)

- The weak-support observer accepts an explicitly selected 2, 4, or 8-CPI bank.
- Exactly one depth supplies operational survival credit. The other depths stay
  shadow-only, preventing overlapping windows from becoming independent votes.
- Every accepted window must carry contiguous epoch-bound lineage and positive
  CPI sequence numbers; a valid source capture epoch may be zero.
- A track may not credit the same source CPI twice, even through another depth.
- Diagnostics report both accepted windows and unique credited source CPIs.
- A temporally credited CPI is not relabelled as a direct single-CPI detection.

## Remaining qualification boundaries

Hardware limits are distinct from missing software: unsynchronized phase,
insufficient channels or unconfirmed geometry cannot be repaired by selecting
a backend. The channelizer preserves complex phase, so coherent lane processing
is now connected, not rejected by mode identity. Each enabled coherent lane
requires explicit transmitter coordinates; per-lane calibration binds RF,
sample rate, geometry, channel order and actual gains. At most eight resident
sessions share a 2-GiB aggregate allocation ceiling and one execution lease.
The same cap/lease applies to portable-only FP32 cohorts, with CPU work outside
the device lease. Both Vulkan allocators enforce the transported quota over
combined host/device allocations; transport mappings are reserved separately
within the same lane share. Capacity-excluded lanes fall back only when the selected algorithm has an
equivalent implementation; resident-only semantics fail closed.

- The resident advanced route is presently Linux-only. macOS and other OS
  builds must report that capability as unavailable and use the common CPU
  implementation for power TBD/stationary/guided behavior. Coherent bearing,
  noise whitening and beam-conditioned histories still require the applicable
  qualified resident capability. That implementation requirement is not a
  claim of physical impossibility on another OS.
- Receiver-name checks are not capability checks. Enabling coherent processing
  requires coherent capture, sufficient channels, explicit channel mapping,
  confirmed array geometry/orientation, and a qualified backend.

- Temporal shadow tracks cannot become production births until replay and
  prospective controls show no false-track regression.
- Direct-path-referenced complex accumulation remains replay-only until its null
  distribution and trial family are calibrated for each statistic family.
- The multi-illuminator route still owns one bounded live observer. Expanding it
  to every lane requires a shared worker/budget design; cloning one worker per
  lane would add avoidable memory copies and CPU load.
