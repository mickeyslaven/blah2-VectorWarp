# Unified radar pipeline migration — 2026-11-16

## Target architecture

```text
receiver adapter -> canonical complex IQ + immutable lineage
                 -> optional coherent wideband channelizer
                 -> one resident radar pipeline per RF lane
                    reference synthesis -> clutter cancellation -> CAF
                    -> fusion -> integrated detector/TBD -> tracker
                 -> common product/diagnostic publisher
```

Receiver adapters may differ only where hardware APIs differ (Kraken, UHD,
SDRplay, HackRF, replay). Operating system code may differ only for device and
GPU transport. Detector, temporal integration, tracker, lineage, statistics,
and publication semantics must not fork by receiver or OS.

## Achieved source contract

Resident ABI 15 now separates canonical input count, reference mapping, and
surveillance mapping. It accepts array-reference identity mappings and normal,
reversed, or sparse dedicated-reference mappings, so paired USRP/B210 and
RSPduo captures no longer need a receiver-specific DSP route. Channelizer
outputs now feed bounded persistent resident sessions when admitted: at most
eight sessions share one execution lease and a 2-GiB aggregate mapping/device
allocation ceiling. Excess/allocation-failed lanes report
`aggregate-gpu-capacity-exhausted`; they cannot escape into independent portable
workers. Equivalent algorithms may fall back to CPU with explicit receipts;
resident-only statistic failure stops the affected processing contract.
The same ceiling covers FP32 portable-only AUTO and explicit-GPU cohorts.
Portable and resident Vulkan allocators consume the same strictly parsed child
quota: it cannot increase the device default, and host/device allocations share
one remaining lane allowance after the page-rounded transport reservation.
Portable decorators preserve optional worker interfaces and hold the lease
only for construction, device submissions and teardown; host copies, CPU
fallback and shared product processing remain independently schedulable.

Noncoherent resident lanes and single-lane resident processing use the same
FP64 host temporal/detector/tracker orchestration as CPU and portable GPU.
Coherent resident lanes retain the native coherent/noise-whitened/bearing
implementation and immutable products under the common lineage publisher.
CPU/portable coherent lanes use the existing coherent beam-bank algorithm.
Distinct shader implementations still require measured parity qualification;
shared configuration or product shape is not numerical parity evidence.

## Core unification implemented in this source pass

- `ProcessingTopology` resolves canonical physical roles before native feature
  admission. API/UI capabilities use the matching JS resolver. Explicit replay
  channel metadata overrides dormant live receiver roles; contradictory scalar
  and list dedicated-reference declarations fail closed. `fixed_site_projected`
  consumes this topology rather than a Kraken name check.
- `DetectorIntegration`, `detectProducts` and `trackProducts` now serve both
  single-lane CPU/portable-GPU and channelized CPU processing. Two-CPI startup,
  Gamma-CFAR, motion trials, capture interval resets and tracker resets use the
  same contract. The common host path also supports 2/4/8-CPI power TBD,
  integer/fractional motion paths, stationary suppression and lineage-safe
  guided recovery on macOS/FP32-only systems. Work is explicitly bounded:
  at most 1,048,576 cells, 256 MiB of temporal working storage and 268,435,456
  path-sample evaluations per CPI; exceeding a bound fails explicitly without
  precision reduction. Missing sample lineage cannot warm integrated
  detections; source capture epoch zero is valid.
- Temporal threshold allocation reserves half the configured PFA for the
  baseline bank and half across all pre-registered motion paths/depths, even
  while deeper banks warm. Each bank is centroided/interpolated against its
  own calibrated statistic before merge, retaining the matching support
  lineage. Detector/guided credits cannot reuse a CPI for the same track.
- Phase-preserving coherent lanes require explicit `transmitter_location`
  (latitude, longitude, altitude) for each enabled lane. No global transmitter
  direction is borrowed. Optional lane `detection_manifold_file` and
  `bearing_manifold_file` are checked against RF, rate, mapping, site and gain
  identity; frame-bound actual gain changes reset the lane. Ideal
  `fixed_site_projected` replay uses the same topology/geometry contract and
  rejects measured manifold files.
- Single/resident and multi-lane map, detection and tracker products use
  `NativeProductIdentity`, including channel roles, event provenance and an
  exact half-open input interval. Legacy inclusive end fields are retained and
  explicitly distinguished using `input_samples_per_output`.
- `StageExecution` binds actual clutter/CAF/wideband execution receipts to an
  exact capture interval and marks missing lineage. Frame updates and terminal
  capture accounting clear prior execution claims. Wideband receipts retain
  the selected device even when that same frame falls back to CPU. These are
  host completion receipts, not device-timestamp or numerical-parity proofs.
  Fusion, temporal, detector, tracker and bearing receipts identify actual
  product execution independently of frontend GPU execution. Mixed summaries
  retain exact per-lane backend membership; AUTO retains a correctness-qualified
  GPU even if a small startup timing sample is slower than CPU.
- Tracker kinematics use capture-sample time on all positioned routes;
  publication timestamps retain their existing event-time provenance. FFT
  thread defaults no longer depend on the selected receiver name.

Focused JS tests passed (including 799 setting leaves). New C++ regression
sources cover canonical topology, temporal warm-up/reset, tracker reset and
frame-bound exact telemetry. This pass performed no native builds, full CI,
service changes or deployment. Physical GPU receipts from earlier milestones
below do not qualify these newly changed execution paths.

## Ordered migration

1. **One algorithm contract.** Parse detector/tracker/integration settings once
   into a receiver-independent contract. Adapt that contract into resident and
   temporary CPU-oracle configuration. No route-specific defaults.
2. **Generalize resident topology (implemented in ABI 15).** Give the resident ABI separate canonical
   input channel count, reference channel mapping, and surveillance channel
   mapping. Size per-channel maps by surveillance count, not input count. Keep
   strict bounds, uniqueness, and mapping validation.
3. **Channelizer-to-resident adapter (source connected; device qualification pending).** Feed each phase-preserving
   `IlluminatorOutput` into a resident session with exact capture epoch/sample
   lineage. Batch sessions through the existing GPU worker/lease rather than
   cloning unbounded workers.
4. **Common product identity and diagnostics (implemented).** Single/resident
   and multi-lane products share identity/lineage annotation; CPU temporal
   diagnostics share serialization. Resident numerical payload/checkpoint
   encoding remains backend-specific. Display quantization remains downstream
   and never changes backend precision.
5. **Parity qualification.** Replay the same IQ through old CPU multi-lane and
   new resident lanes. Require geometry, detector, temporal lineage, tracker,
   reset, false-track, and failure-injection parity before live shadowing.
6. **Qualify implementations of one contract.** CPU remains the full-precision
   shared product implementation and fallback, not a receiver-specific route.
   Native coherent shaders retain their own implementation, admitted only
   through topology/capability and parity gates. Promotion still requires
   explicit approval after prospective qualification.

## Code that remains hardware-specific

- Receiver capture modules and sample-format conversion.
- Coherent wideband channelization when multiple RF lanes share one capture.
- GPU device discovery/transport and OS packaging.
- Array geometry/manifold data supplied as configuration.

In the target architecture, algorithm semantics after canonical per-lane IQ
are shared. CPU orchestration is now shared across single/multi-lane routes;
the resident advanced implementation and newly connected multi-lane adapter
still need compiled/device qualification. No deployment is claimed.
Irreducible physical distinctions remain channel count, actual shared phase,
sample/clock lineage, RF bandwidth and measured geometry/calibration. In
particular, paired HackRF sample synchronization does not certify coherence.

Explicit remaining software limits must not be described as physics: the
current resident fusion ABI represents an exact regular polygon, FP64 resident
shaders are not a macOS backend, and live manifold/diagnostic calibration workers
still require a lane-bound artifact orchestration extension. Existing per-lane
calibration files and ordinary coherent processing are not blocked by those
worker limitations. These boundaries remain fail-closed and visible.

## GPU portability milestone

- GPU qualification is now stage-specific. The FP32 wideband channelizer no
  longer inherits the resident `shaderFloat64` requirement; FP64 resident
  processing remains fail-closed.
- Linux builds expose portable, FP32 wideband, and FP64-resident modules.
  macOS/MoltenVK builds expose portable and FP32-wideband modules without
  falsely advertising resident support.
- Build capability now reports portable, wideband, and resident stages
  separately. Frame telemetry reports actual GPU and CPU execution separately
  for ambiguity, clutter, and wideband work, including rejected/fallback work.
- Ordinary Linux CI now compiles the unified open-receiver/GPU build. macOS CI
  pins VkFFT, installs the Vulkan/MoltenVK toolchain, enables physical GPU
  tests, and builds both architectures with GPU required.
- Bounded HP validation compiled all three Linux GPU modules. The FP32
  wideband physical test completed 24 frames/48 lane-frames with no misses and
  worst RMS error `3.29e-7`; resident shader compilation covered 76 shaders.
- The same 2-MS/s, two-channel replay under Kraken, UHD/USRP, RSPduo, and
  HackRF identities now compares six-frame map, detection, and tracker
  evolution, not only the first map. The fixture contains a known eight-sample,
  100-Hz echo for four CPIs followed by a two-CPI absent tail. It requires the
  expected echo detection and a multi-update track; it strips only event
  provenance and generated track IDs before comparing mathematical products.
- The common multi-illuminator unit suite already proves 2-CPI warmup,
  motion-bank output, detection/tracker use, exact sample-window lineage,
  discontinuity reset, capture-epoch reset, unavailable-lineage refusal,
  duplicate-CPI refusal, and explicit reset. It runs in ordinary CTest on the
  unified Linux and macOS builds.

Still required before a release claim: successful macOS CI on both runners, a
physical device that explicitly lacks Float64, end-to-end receiver-identity
equality through the multi-illuminator temporal route, and runtime capability
receipts that distinguish built, packaged, loadable, discovered, qualified,
selected, and executed. The current end-to-end fixture proves the common
single-CPI map/detector/tracker route; temporal lineage is proven at the shared
algorithm boundary but not yet through every acquisition adapter.

## CI acceptance architecture

Astra's oppositional CI review separates inventory checks from execution
proof. Source-policy, package-content, and capability-schema checks remain
supporting gates. Release acceptance additionally requires:

- one canonical exact-IQ fixture replayed under every receiver identity with
  equal maps, detections, tracker evolution, temporal windows, resets, and
  lineage (excluding provenance-only fields);
- Linux x86-64/arm64 and macOS x86-64/arm64 execution of the same CPU corpus;
- a physical FP32-only GPU lane proving qualified FP32 stages execute while
  FP64 resident processing refuses with a precise reason;
- a qualified FP64 GPU lane proving resident detector, tracker, temporal/TBD,
  snapshot, and CPU-oracle parity with frame-bound execution counters;
- no-device, missing-module, worker-failure, discontinuity, and recovery lanes
  proving truthful fallback and state reset rather than silent degradation;
- installed-package reconciliation among the build receipt, packaged modules,
  runtime loadability, device qualification, selection, and actual execution.

GitHub-hosted Linux runners can compile the GPU modules but do not constitute
physical GPU execution evidence. macOS GPU jobs are required execution gates,
not observation-only probes, but their real runner results are still pending.

Public native and macOS build entry points now reject receiver-subset builds
before dependency work and direct users to the unified `all` artifact. Narrow
receiver builds remain available only behind the explicit
`VECTORWARP_INTERNAL_ADAPTER_FIXTURE=true` test boundary, are marked test-only,
and cannot be newly installed. Historical partial Linux artifacts remain
readable by the installer solely for upgrade compatibility; release packaging
continues to require the unified all-receiver artifact and RSPduo local source
kit.

## Deletion gate

Legacy code is removed only after the new route passes exact-IQ replay on every
supported receiver profile, resident/CPU forced-backend parity, reset and
discontinuity tests, and prospective false-track limits. The development-switch
oracle is the required end state of migration, not the current state; today the
old route remains a second production behavior and must not be deleted.
