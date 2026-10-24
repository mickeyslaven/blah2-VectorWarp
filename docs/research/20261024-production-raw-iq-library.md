# Production raw-IQ library and bounded native snapshot contract

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

## Native snapshot implementation (qualification pending)

ABI12 / config schema4 adds a default-disabled `Config::replay` and idle-only
`Session::snapshot()`. No live configuration enables it. Opt-in requires source
and RF digests, full actual per-channel gains, RF frequency, original configured
PFA, a byte ceiling (at most 256 MiB) and a frame ceiling (at most 128). The
session exports each newly settled/committed frame at most once, before the next
frame; it rejects pending, uncommitted, duplicate, disabled and over-budget
requests locally. A malformed worker result poisons the isolated session.

`ResidentReplaySnapshot` is a canonical little-endian envelope: a 2048-byte
header, 20 fixed-order typed blocks, exact dimensions and no truncation. SHA256
binds the entire envelope with its digest field zeroed. Identity includes
source/RF/config/module/device/raw-IQ digests, epoch/frame/consumed sequence,
timestamp, full gain vector, sample rate, beam/refinement counts, per-pass PFA,
beam and motion/temporal trial correction. Configuration bytes retain channel
mapping, measured manifolds, reference/clutter settings and bearing options.

The blocks contain the reference checkpoint plus its otherwise transient
covariance/validity, actual calibrated fusion/refinement weights, complete
postprocess metadata, rolling power/angle history, current power, all TBD
detector banks, every native fusion-beam power, every pass/CUT's exact power,
training mean **and pre-division sum**, threshold and decision, fused/channel
maps, original raw IQ, synthesized reference, complete filtered IQ, final
decision flags/detections and the committed tracker checkpoint. CFAR statistics
are five FP64 values per pass/cell: CUT, training mean, training sum, threshold,
flags (0 skipped, 1 evaluated, 3 passed, 5 two-CPI warmup). Metadata preserves
edge-dependent training membership/count/alpha and the exact axis values.

Temporal snapshots are **after** the production advance: metadata word79 is the
next write counter, modulo the 4/8-CPI depth; for 1/2 CPI it remains zero and
history holds the current power. Unfilled power slots retain NaN; undefined
angle slots alone are canonicalized to zero in the exported host copy. Disabled
angle banks are empty. This is an export-only ABI: it deliberately provides no
device-state restore operation and does not claim arbitrary-CPI restart parity.

With export disabled, shader strings, bindings, command graphs and allocations
are unchanged. Opt-in adds native observer buffers and saves raw/reference IQ
before workspace reuse; it uses the already-existing full residual path. The
explicit idle export batches all resident buffer copies in one command with one
fence and one mapped-memory invalidation; it does not enable ordinary CFAR or
centroid diagnostic pointers. Envelope construction, validation and publication
happen afterward in temporary storage. Neither raw-IQ processing nor the
detector is emulated by this exporter.

Validation on 2026-10-24:

- Apple Silicon: live executable builds; `productionRawIqParity`,
  `residentWireCpuBoundary`, `residentProcessCpuBoundary`,
  `residentReplaySnapshotCpuContract` (402 checks) and `trackBeforeDetectMath`
  pass. Contract tests cover 1/2/4/8 warmup, complete banks, bounds, RF/gain/hash
  corruption, edge training alpha, immutable failure, invalid beam/history and
  temporal state. CPU process doubles exercise early/duplicate export rejection
  and malformed-result poisoning; they are not hardware evidence.
- Strix CPU only: the complete native module links with undefined-symbol
  rejection; native module/contract/physical-test/shader-test sources also pass
  `g++ -Wall -Werror -fsyntax-only` with the repository's system dependency
  include policy. All 32
  normal/observer shader variants pass GLSL-to-SPIR-V compilation with the
  repository-pinned VkFFT commit `066a17c17068c0f11c9298d848c2976c71fad1c1`.
  The work directory is `/var/tmp/vectorwarp-replay-compile.05qFHt`. No Vulkan
  instance/device or replay job was created by those checks.
- `gpuPhysicalResidentReplay` is added behind the existing explicit physical-GPU
  option/device selector. It compares 144 native frames, exact outputs with
  opt-in off/on across 1/2/4/8 CPIs and whitening off/on, taking validated
  snapshots through warmup and reference-analysis updates. It has **not run**.

## Explicitly not complete

There is no hardware-qualified production-equivalent export claim in this slice.
`detectionManifoldGridReplay` remains a post-CAF diagnostic and must continue to
report `same_iq=false` and `production_equivalent=false`. CPU orchestration
parity cannot qualify GPU fusion, noise whitening, or multi-CPI/TBD behavior.
No replay result or held-out detection gain is claimed here.

## External qualification gate

The authorized non-live FP64 Vulkan host is Strix. On 2026-10-24, at the user's
explicit request, the exact controller lease owned by
`codex-vectorwarp-adversarial-review` was released through
`POST /v1/compute-lease/release`. The controller confirmed `active=false` but
retained `qwen-code-vulkan-flash-next` in `FLASH_IDLE`, `resident=true`, with
`idle_expires_at=null`. Available memory remained approximately 16.7 GiB; GPU
memory was **not** reclaimed. The renewal job then failed normally because its
lease was revoked. Existing review directory
`/var/tmp/vectorwarp-qwen-audit.CpkOmR` was preserved. No model process/service
was directly started/stopped and no guard was bypassed.

The installed controller defaults to indefinite Flash retention when
`CASS_AI_NODE_FLASH_WARM_IDLE_SECONDS` is unset; a nonzero override is capped at
3600 seconds, and zero disables retention. Its authenticated HTTP API has no
cold-release/drain parameter or endpoint. `_drain_flash_warm` is internal safety
machinery, not an operator API. A supported authenticated operator drain or
bounded-idle configuration/reload workflow is needed to reclaim it. Do not
invoke private functions or edit service environment as a workaround.

Its installed acquisition handler accepts
Qwen runtime profiles and Ollama only; it does not presently expose a native
radar replay runtime. Reusing a model lease for Vulkan replay would not be an
authorized controller path. Local CMake enables the resident module on Linux
only, so the macOS CPU result cannot substitute for resident qualification.

Resume with an admitted native-test lease and sufficient resources on Strix;
do not run heavy replay on the live receiver or stop the resident model directly.

## Native seam reference and remaining acceptance boundary

The production resident pipeline already starts from canonical raw IQ.
`GpuResidentModule.cpp::privateConfig` maps the actual detector and fusion
configuration into `ResidentFrameConfig`; `finishRadar` exposes maps/fused
outputs but not the internal detector state. The new extraction is a
private, default-disabled snapshot/export interface on
`core/GpuResidentFrame.private.h::ResidentFramePipeline`, with corresponding
state access on `core/GpuPostprocess.private.h::gpu_postprocess::Stage`.
Ordinary output records are unchanged; ABI12/config4 explicitly rejects older
cohorts instead of reinterpreting their object or wire layouts.

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

The next acceptance step is running `gpuPhysicalResidentReplay` under a supported
native-test lease, inspecting an actual exported full-bank artifact and measuring
the promised single-export-fence behavior. Add full snapshot restoration only
with uninterrupted-versus-restored native parity; reference-only restore remains
distinct and resets whitening validity. Adaptive bearing refinement's per-CUT
inversion/score workspace and per-batch clutter solve coefficients are not a
restart image: this export carries their configuration, source maps/reference,
actual calibrated weights and final residual instead. If qualification requires
those intermediate arithmetic observations, add them explicitly before claiming
that scope. No held-out detection gain, deployment or RF change is authorized by
these CPU/compiler checks.
