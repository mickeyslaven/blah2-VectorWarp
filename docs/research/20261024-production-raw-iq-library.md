# Production raw-IQ library: verified parity slice and remaining resident seam

## Implemented and accepted locally

`blah2ProductionRawIq` is a linkable library used by `blah2` itself. Its
`RawIqStages` owns the persistent array-reference learner and clutter
workspaces, and runs the existing `WienerHopf::PreparedReference`, independent
surveillance filters, and `Ambiguity::process`/`process_borrowed` implementations.
The calling application still owns raw captured samples, ambiguity geometry,
acceleration selection, fusion, detection, tracking, and publication. No live
configuration or RF setting changes in this slice.

The extraction preserves the original per-path scheduling and rejection
ordering, atomic publication of lazily constructed shared-reference workspaces,
Pi workspace planning before capture, borrowed sample lifetimes, mixed fallback
through the first path, and learner-only reset at a replay-generation change.

`productionRawIqParity` compares the former inline orchestration with the
library from deterministic raw IQ over 12 CPIs for each of four combinations:
shared-reference/borrowed and consuming/preserving surveillance. It requires
exact equality of synthesized IQ, learner metrics and weights, every complex
CAF cell, axes, and surviving FIFO samples. The library uses two surveillance
workers while the independent direct-call oracle runs serially. A reset on CPI
7 exercises the reference analysis schedule. Additional cases cover no clutter,
clutter rejection followed by recovery, invalid dimensions, and mixed fallback
with three surveillance paths. This is CPU parity, not GPU equivalence.

Local validation (Apple Silicon, AppleClang 21, Release, 2026-10-24):

```sh
cmake -S . -B build/native -DBLAH2_KRAKEN_ONLY=ON -DBUILD_TESTING=ON \
  -DBLAH2_BUILD_OFFLINE_TOOLS=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/opt/homebrew
cmake --build build/native --target blah2 testProductionRawIq \
  testKrakenMultiChannel testAmbiguity testClutterFft testBearingPhaseProjection \
  --parallel 4
ctest --test-dir build/native \
  -R 'productionRawIqParity|testAmbiguity|testKrakenMultiChannel|bearingPhaseProjection|clutterFft' \
  --output-on-failure
```

All five focused suites passed. The first 48-CPI parity run completed in 1.18 s;
the five-suite regression run completed in 4.66 s. The mixed-fallback regression
was added after review and the library plus live executable were rebuilt.

## Explicitly not complete

There is no production-equivalent full-bank raw-IQ exporter in this slice.
`detectionManifoldGridReplay` remains a post-CAF diagnostic and must continue to
report `same_iq=false` and `production_equivalent=false`. CPU orchestration
parity cannot qualify GPU fusion, noise whitening, or multi-CPI/TBD behavior.
No replay result or held-out detection gain is claimed here.

## External qualification gate

The authorized non-live FP64 Vulkan host is Strix. On 2026-10-24 the supported
controller `GET /v1/compute-lease` reported an active, resident `FLASH_ACTIVE`
lease owned by `codex-vectorwarp-adversarial-review`, runtime
`qwen-code-vulkan-flash-next`. Available memory was 16,742 MiB, below the
controller's 96 GiB heavy-work admission minimum. No lease was taken, service
started/stopped, or guard bypassed. Its installed acquisition handler accepts
Qwen runtime profiles and Ollama only; it does not presently expose a native
radar replay runtime. Reusing a model lease for Vulkan replay would not be an
authorized controller path. Local CMake enables the resident module on Linux
only, so the macOS CPU result cannot substitute for resident qualification.

Resume with an admitted native-test lease and sufficient resources on Strix;
do not run heavy replay on the live receiver or stop the resident model directly.

## Next implementation boundary

The production resident pipeline already starts from canonical raw IQ.
`GpuResidentModule.cpp::privateConfig` maps the actual detector and fusion
configuration into `ResidentFrameConfig`; `finishRadar` exposes maps/fused
outputs but not the internal detector state. The required extraction is a
private, default-disabled snapshot/export interface on
`core/GpuResidentFrame.private.h::ResidentFramePipeline`, with corresponding
state access on `core/GpuPostprocess.private.h::gpu_postprocess::Stage`.
Keep ordinary output records and the existing ABI11 layout unchanged until a
separately versioned optional export section has complete coverage tests.

The export must originate from the same native raw-IQ execution, before any
truth-selected cell/beam restriction, and carry all of the following:

| State | Exact production owner and required semantics |
| --- | --- |
| Raw input and identity | Canonical captured IQ hash, recording/source digest, sample interval, epoch/sequence, RF frequency/sample rate, physical channel mapping, actual complete per-channel gain vector, and config/binary/device hashes. Include baseline/candidate bank identities and the full RF identity used by the replay contract. |
| Reference and clutter | Full `ReferenceCheckpoint` plus its transient covariance-valid flag and complex covariance, synthesized reference, filtering configuration and raw-input provenance. The 256-byte wire checkpoint intentionally omits covariance; a reference-only restore clears it. Do not fabricate or silently infer covariance after restore. |
| Whitening and banks | `ResidentFramePipeline` calibrates fusion/refinement weights from the checkpoint and covariance. Mode 2 performs bounded diagonal residual-noise scaling, not full MVDR. Export the actual calibrated complex weight matrices, covariance/validity, reference loading, fusion and refinement beam counts/mappings. Refinement scan count can differ from four times the fusion count. |
| Every beam and CUT/training statistic | Export complete native beam powers for every grid cell and each production detector pass, exact edge-dependent training membership/count/sum, threshold/alpha and decisions. Reuse the production fusion arithmetic in the device exporter; do not rebuild channel vectors from selected CAF cutouts. Preserve the max-beam fused map actually consumed by the detector. |
| Multi-CPI/TBD | Stage owns integration history, current power, optional angle history, TBD banks, and metadata. CPI depths are 1/2/4/8; 4/8 use ring slots and 2/4/8-length detector banks. Metadata word 79 is the available count before advance; write/available indices and snapshot timing must be explicit. Fresh history contains NaN sentinels. |
| Familywise trials | Export beam correction and each temporal/motion pass correction; `TrackBeforeDetect.private.h` supplies exact pass count, trajectories, PFA and integrated Gamma alpha. Persist the per-column training-count/alpha table because edge columns differ. Do not divide PFA twice. |
| Tracker, if included | Preserve committed tracker slot/token separately from the prepared transaction. Reference state is acknowledged earlier than the radar/tracker commit. Partial snapshot failure must not create a resumable composite state. |

Make bounds explicit before allocating: maximum frames, cells, beams, channels,
history depth and total bytes; reject overflow and incomplete coverage rather
than truncate. Disabled export must add no allocation, GPU transfer, host wait,
or per-CPI serialization. Enabled snapshots require a settled transaction and
one dedicated batched transfer command/fence with GPU-to-host synchronization
and mapped-memory invalidation. Current diagnostic copy helpers submit and
fence per buffer/chunk, so enabling their pointers is not a bounded snapshot
implementation. A failed export must leave live state and ordinary output
semantics unchanged and mark the artifact incomplete.

## Next acceptance checks

1. Under an admitted test lease, run the unchanged native pipeline and the
   extracted replay pipeline from identical raw IQ and fresh state. Compare
   every cell/beam, detection record and state through 1/2/4/8 CPI warmup,
   reference-analysis boundaries and epoch resets, with whitening both on and
   off. On the same GPU require exact results; retain the existing explicit
   cross-backend numerical bounds for CPU qualification.
2. Only after that parity passes, enable bounded snapshot/export. Prove
   disabled-export behavior identical and enabled-export results identical;
   validate export coverage counts/hashes independently, edge training counts,
   all pass PFA/alpha values, circular beam tolerance and nondefault refinement
   scan counts. Observe exactly the intended transfer/fence budget.
3. If importing snapshots, validate version, epoch, configuration/identity hash,
   dimensions, temporal write/available state, finite data (allowing only the
   documented warmup sentinels) and tracker transaction atomically before any
   mutation. Compare uninterrupted raw-IQ execution with restore at every
   warmup depth. Reject partial/stale snapshots and full-gain/RF mismatches.

The next deliverable is this native parity plus default-off export slice in a
stacked PR. Updating evaluation claims waits for a complete exported full-bank
artifact and its held-out replay validation.
