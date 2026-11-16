# Scorable temporal challenger and frequency correction candidate — 2026-11-13

Source candidate: `/var/tmp/vectorwarp-config-preservation-b7c3b8b`.
No production promotion, threshold/PFA change, commit, push, merge or release.

## Temporal challenger

`TemporalShadowTracker` consumes the existing `TemporalEvidence` motion-2,
motion-4 and motion-8 banks. Each depth owns an independent instance of the
existing `Tracker`; there is no second tracking algorithm. A lane evaluates
only globally disjoint windows at its own depth, so overlapping rolling
windows cannot repeatedly confirm the same echo. Candidate output retains the
exact `EvidenceLineage` selected by the temporal bank.

The live observer can enable this with `score_tracks: true`. It inherits the
production detector geometry, PFA, tracker M/N/deletion and kinematic settings,
but keeps independent state and never publishes production detections or
tracks. Candidate count is bounded (default 256, maximum 4,096). Telemetry
reports per-depth evaluated/skipped windows, candidates, truncation, tracks,
active/coasting tracks, associations, promotions and misses. Full-precision
candidate maps and lineage remain outside the presentation transport.

Focused validation passes 269 assertions across the scorer and live adapter.
Configuration validation covers 787 device-setting leaves; all 269 editable
fields persist. A bounded Fedora GPU build completed on Display2 in
`/var/tmp/vectorwarp-shadow-acceptance.4Y7Ly9`: the full processor, USRP and
HackRF adapters, portable Vulkan, wideband Vulkan and resident Vulkan modules,
GPU worker, mixed worker and focused test executable all linked. The processor
reported `residentGpu:true`; the same focused suite passed 269 assertions in
the GPU build. GCC 15's `maybe-uninitialized` diagnostic is demoted from an
error only on targets that include the vendored VkFFT 1.3.4 headers; project
warnings remain errors.

Acceptance artifact hashes:

- `blah2` (counter-ledger rebuild): `fef634f92b4e3e3e9718a6d4f880c5c042737e890c6bc1658133f7e23110e5de`
- `blah2-gpu-vulkan.so`: `733c6828440ab34dbdd5a867012e5c716404fcb9aa7500c13048f6990eb5ed6e`
- `blah2-gpu-wideband-vulkan.so`: `40256b9dc673df261df00641c3fb48483806100836f07b6b1c18290b0cd37bf7`
- `blah2-gpu-resident-vulkan.so`: `75d7041d3f676e66a3f20b65e31a9dfc9873ce7c4a76512313b237b6e92fb05a`
- `blah2-receiver-usrp.so`: `ed542bfdca313e38f5ab529bff0b27f6d0f5d1b811656a59373933cae328c2ff`
- `blah2-receiver-hackrf.so`: `ebcfeb0ecc9a9a7cbb735c56be72e712cc4ad781d6716547d29b24afda9f3ad1`

## HP frequency-response candidate

HP's current direct-path observer remains highly coherent but differs from the
ideal array response. The 2026-11-13 latest observation showed channel-relative
group delays from about -44 to +97 ns, channel-2 phase curvature about 82
degrees peak-to-peak, median ideal correlation about 0.494 and worst current-to-
prior subband response correlation about 0.904. A single capture is therefore
not safe as an applied correction.

The frequency-resolved shadow now emits a phase-only, per-subband pre-CAF
equalization candidate. Every weight has unit magnitude, so the equalizer's
maximum receiver-noise gain is exactly one. It is fail-closed unless at least
eight consecutive observations meet both minimum rank-1 coherence (0.95) and
minimum prior-response correlation (0.98). It is explicitly marked
`production_authorized: false` and `shadow-replay-only`; amplitude whitening
remains the bounded downstream beam-bank's responsibility.

This candidate corrects stable channel/cable frequency phase response. It does
not claim to remove changing multipath, infer aircraft direction from the
direct path, or improve detections without paired replay evidence. The next
acceptance check is a same-IQ pre-CAF replay comparing unchanged production
against the stable phase-only candidate, at fixed thresholds/PFA, with pass-
paired detection, track continuity and unmatched/false-track burden.

## Live shadow activation

HomePortal is running the hashed Display2 GPU artifact from
`/opt/vectorwarp/releases/20261113T1914-shadow-score-ledger` (processor SHA-256
`fef634f92b4e3e3e9718a6d4f880c5c042737e890c6bc1658133f7e23110e5de`).
The WPXI observer has
`score_tracks: true` and `max_cpis: 8`; production detection/tracking remains
unchanged. After activation it reached all 1/2/4/8 banks with the portable GPU
path active. At 442 accepted observations it remained enabled with zero
rejections or copy overruns: latest copy time 0.59 ms, observed maximum 1.18 ms,
and latest work time 65.8 ms under the 100 ms cap. Its scored tracks are
explicitly unpublished.

The first live run also exposed that a correct lineage reset erased cumulative
trial counters. The scorer now rebuilds all temporal/tracker state on a gap but
preserves its process-lifetime evaluated-window, candidate, association,
promotion and miss ledger. Display2 passed 277 focused assertions. A live
50-second check crossed two additional resets without any counter regression:
110 observations accepted, two pacing drops, zero rejections/overruns, all
banks ready, latest work 63.4 ms. The unpublished 2-CPI lane reported seven
active tracks and two promotions; 4/8-CPI lanes had no promotions in that short
sample. Candidate burden is high and remains an experimental acceptance item;
nothing is fed into production.

HP is still running its unchanged resident-GPU processor. Only the API-side
candidate generator was staged in
`/opt/vectorwarp/releases/20261113T1902-frequency-phase-candidate`. Observation
3896 produced a unit-noise-gain candidate from eight history observations but
correctly rejected replay readiness: worst current/prior response correlation
was 0.9647, below the 0.98 gate. No correction weight was applied to radar data.

Both nodes reported `radar: receiving`, fresh processor status, and no service
errors after activation. No commit, push, merge, release, production threshold,
PFA, gain, detector or tracker-publication change was made.
