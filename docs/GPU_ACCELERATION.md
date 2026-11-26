# GPU acceleration

For macOS, see [macOS installation and GPU setup](MACOS.md). The driver-install
instructions below describe Linux. Portable FP32 and wideband GPU stages are
buildable on Linux and macOS/MoltenVK; execution depends on the installed module,
device capabilities and numerical qualification, never the receiver name.
The current build policy packages the FP64 resident module on Linux only;
Darwin reports it unavailable because its resident arithmetic proof is not
implemented. This is not a claim of qualified execution on every Linux GPU.

## Unified-artifact acceptance

Portable-worker host reference preparation (including ATSC pilot projection)
runs after clutter submission, alongside worker FP64 refinement. For a
controlled serial-order diagnostic, `BLAH2_GPU_CLUTTER_HOST_WORK_OVERLAP=0`
disables that overlap; absent or other values retain the production default.
Each worker snapshots the setting at construction. The switch changes
scheduling only, not precision, math, fallback, worker/session counts, or
allocation limits.
Evaluate inclusive `processingCycle` timing and capture-backlog growth, not
individual `cpi` samples, when validating this scheduling path.

Portable lane workers have independent transaction locks and may overlap within
the configured, CPU-bounded lane executor. Their shared reservation limits total
memory; it does not serialize GPU queues. Resident lane transactions retain the
shared execution lock. Wideband channelization can overlap prior-frame radar
work after qualification. Mock overlap tests establish this scheduling contract,
not device throughput: physical acceptance must cover all enabled lanes plus
wideband, inclusive cycle time, backlog/drop counts and reset/fallback behavior.
An already-ready portable GPU reply remains valid when host work outlasts the
frame deadline; unfinished work receives no additional timeout allowance.

Public native and macOS builds use `--backend all`. Partial adapter builds
require the internal `VECTORWARP_INTERNAL_ADAPTER_FIXTURE=true` test override;
they are not product variants. The all-receiver artifact contains the open SDR
adapters and the manual-SDK RSPduo source kit when the proprietary SDK is absent.

`test/gpu/product_parity_test.py` runs the same canonical target and two-CPI
temporal fixture through CPU, portable GPU and resident processing under all four receiver
identities, without opening an SDR. It compares complete maps, detections,
track updates/IDs and exact temporal support/reset lineage, including a second
replay epoch. Every receipt binds the source SHA, native artifact hashes,
fixture hashes, device `shaderFloat64`, and each observed execution frame's
process generation, CPI, capture epoch and sample window. Enumeration alone is
not qualification. The final two frames must execute GPU stages without the
startup CPU oracle. FP32-only devices must execute portable/wideband stages and
refuse both resident routes; Linux FP64 devices must also pass resident parity
for both the direct and channelized temporal fixture. This is a 24-case,
two-channel baseline per device class, not full unified-pipeline acceptance.

Full acceptance additionally requires `gpu-unified-features-fp32.json` and
`gpu-unified-features-fp64.json`, bound to the same source, native artifact,
device and feature-contract hash as the baseline. The machine-readable
requirements are in `test/gpu/unified_qualification_requirements.json`; print
them without starting a processor or GPU with:

```sh
python3 test/gpu/verify_qualification_receipts.py --requirements
```

Resident startup qualification uses a private worker instance for live and
replay alike. It checks configured 1/2/4/8-CPI history against canonical CPU
maps, detections, public tracks and hidden tracker state. A deterministic
no-target control must produce no detections/tracks; a delayed/Doppler replica
must produce nonempty mature detections and tracking when enabled. Target
placement and bounded amplitude derive from the actual geometry and
CFAR/trial/SNR gates. Impossible stimuli or mismatches explicitly refuse
qualification; these controls are not statistical false-alarm guarantees.
At most sixteen private frames are required, including warmup. The old worker
fully resets and acknowledges reference/temporal/tracker state before orderly
destruction. A fresh production worker must match config, loaded-module-image
and device/driver proof hashes, then begins at the normal frame-one origin.
No private products enter publication, replay lineage or recording. AUTO
timing is explicitly unavailable rather than consuming production frame IDs.

The supplemental corpus must measure two/eight resident lanes, coherent arrays
with three/five/eight channels and calibrated manifold/bearing outputs,
four/eight-CPI power and coherent TBD with resets, guided recovery and disjoint
support credit, and portable/resident fault injection. Every scenario includes
an independent numerical oracle, a target-present and absent-target control,
false-track checks, and frame-bound physical execution. Scenario settings and
all named checks are required; a boolean feature claim cannot replace them.
The verifier requires each check's evidence hash and the retained structured
execution/product/oracle/check evidence in
`gpu-feature-evidence/<sha256>.json` alongside its supplemental receipt.
FP32 receipts cover supported portable features; FP64 receipts additionally
cover resident-only statistics. Receiver identities reuse the common baseline;
this contract does not introduce receiver- or OS-specific processing pipelines.

`unified_physical_corpus.py` is the trusted supplemental producer. Inspect its
expanded matrix without starting any processor or device:

```sh
python3 test/gpu/unified_physical_corpus.py --plan --device-class fp64
```

The workflow runs it after baseline qualification against the same artifact
and selected physical device. Deterministic completed BLAH2IQ scenes include
target/null controls, distinct RF lanes, unequal channel noise, array geometry,
integer/fractional target motion, fading recovery, capture gaps and epoch
rewinds. CPU controls establish complex reference/CAF and public-product
parity; a separate known-geometry beam calculation checks coherent/whitened
fusion. No device double can satisfy the execution predicates. Missing target,
guided recovery, calibrated bearing, reset, fault, or numerical evidence fails
closed, including an insufficiently exciting scene on the candidate artifact.

The replay-only `capture.replay.qualification` interface requires explicit
`enabled: true`, `confirmation: offline-physical-gpu-qualification-v1`, and
completed `blah2` input. Its bounded observer retains actual production input,
reference, CAF, fusion and acknowledged resident proof; fault hooks act at real
worker/capture boundaries. Single-lane AUTO uses portable processing, so
resident prepare/commit faults apply only to forced resident and eligible
multilane AUTO routes. The runner never changes modules to influence selection.
Per-case deadlines, 64-frame observation bounds and a 32-GiB evidence ceiling
are hard limits. Partial evidence uploads on failure, but a success receipt is
created only after every required measured predicate passes. Native opt-in
unit tests remain useful partial checks, not substitutes for these receipts.

The observer's optional `clutter: {enable: true, filter_policy: refit-weights}`
retains the exact production reference and complete surveillance samples before
and after clutter cancellation, channel order, half-open delay range, execution
flags and the same capture epoch/sample interval as the input and products.
It uses the existing CPU, portable and resident clutter stages; borrowed output
is copied before ambiguity consumes it. It is off by default and inherits the
observer's sample/frame/byte limits. Resident observation requests the existing
filtered-sample diagnostic section. Runs with both CPU and GPU qualification
work are labelled `cpu+vulkan-qualification`, not a GPU-only retention result.
Disabled clutter is identified explicitly and must leave surveillance unchanged.
`fixed-weights` is currently unsupported and is refused before an observer file
is created. Fixed-weight retention remains an unqualified research claim until a
reviewed canonical execution boundary exists; a refit observation cannot fill it.

Recorded-IQ research qualification is separate from release arithmetic parity
and bounded live acceptance. Its verifier requires `pre-soak-radar-evidence.json` for the exact
source and artifact, with hash-verified retained measurements under
`pre-soak-evidence/`. Required evidence includes physical FP32/FP64 paired
recorded-IQ injections, both fixed-weight and refit-weight differential echo
retention at both Doppler signs, detector/operational-track recovery, and
adjudicated whole-pass recorded-negative exposure with false-alarm and
false-track bounds. PFA needs a separate retained, reviewed statistical
analysis; a synthetic null's observed count is not an empirical PFA estimate.

`test/gpu/pre_soak_policy.json` deliberately contains unapproved/null risk and
exposure bounds. The research gate fails closed even if the synthetic corpus
passes; it no longer blocks stable publication. Stable release still requires
physical FP32/FP64 product parity, the complete unified feature corpus, and
exact-package bounded live acceptance with detection/track, false-track,
deadline and capture checks. Approve and pin research bounds before measurement, then
run the measured-evidence producer under the resource gates. The bounded
`test/gpu/recorded_iq_qualification.py` postprocessor consumes an independently
pinned authorized plan and retained canonical replay runs. It joins the actual
pre-centroid CFAR population, exact sample exposure, published detections and
unique operational tracks; it does not re-run radar algorithms or infer false
alarms from final detection counts. CPU/portable observation is optional and
resident counts are measured inside the dispatched CFAR kernel only when the
existing authenticated replay diagnostics request enables observation. Warmup,
masked CUTs and CUTs without valid training cells contribute no exposure.

The plan schema is `vectorwarp-recorded-iq-qualification-plan-v1`: pin source,
artifacts and policy, name hash-pinned retained `runs`, predeclare `echoPairs`
with target matching gates and exact sample windows, and supply
`negativeWindows` with CPU baselines and reviewed whole-pass adjudication.
Retained runs contain the existing harness's `config`, `products`, `observer`,
`events`, `statuses`, `artifactBinding`, `artifactReceipt`, plus the production
recording inspector's `acquisition` result and exact source/artifact/device
identities. `pfaAnalyses` must retain a reviewed statistical method and separately
pinned `independenceEvidence`; raw cell counts never imply independent trials.
Invoke with `--plan PATH --plan-sha256 SHA256 --output NEW_DIRECTORY`.

The trusted GPU workflow builds one exact-source artifact and requires both
physical device classes to pass product parity and the unified feature corpus.
It retains and verifies their measured products and oracle evidence. Recorded-IQ
research runs instead use a separately authorized plan and retained inputs with
the explicit postprocessor command above. Their plan must cover both physical
device classes and the exact source, policy and unified artifact being studied.
Missing inputs, unapproved bounds or incomplete physical coverage refuse that
research qualification; a diagnostic report alone never supplies a passing receipt.
The producer writes a measured report and retained evidence, and writes the
release receipt only if the existing full admission gate passes. Current
fixed-weight replay remains unsupported and fails closed, so refit observations
alone cannot pass. Historical reports with null retention, PFA or false-track
fields cannot satisfy admission. No risk bounds are chosen by this producer.

The trusted `gpu-qualification.yml` workflow is manual-only. Administrators
must configure these repository variables as JSON arrays of runner labels:

- `VECTORWARP_GPU_FP32_RUNNER_LABELS`
- `VECTORWARP_GPU_FP64_RUNNER_LABELS`

Both classes require isolated Linux x64 runners compatible with the Ubuntu
24.04 artifact, the installed open-SDR runtime libraries, Python 3.11 or newer,
and their real Vulkan driver. The workflow builds one artifact on a hosted
runner and gives those exact bytes to both classes, sequentially. The GPU
runners must not control live acquisition or other production services.
Their administrator remains responsible for host resource/thermal limits.
There is no machine-specific label or GPU ID in the repository.

Protect the `vectorwarp-gpu-fp32` and `vectorwarp-gpu-fp64` environments with
required approval and default-branch-only deployment rules. An optional
environment variable `VECTORWARP_QUALIFICATION_GPU_DEVICE` selects an exact
enumerated device ID; otherwise the harness deterministically selects the
requested capability class. Missing variables, devices or proof fail closed.
Dispatch only at the exact current protected default-branch SHA. Pull requests
never execute this workflow on a self-hosted runner.

Explicit CI receipt verification and release signing require successful receipts downloaded from
that exact SHA's trusted dispatch; after qualification, rerun the receipt gate
if it previously failed for absent evidence. This is source/artifact-bound
qualification of the tested Linux artifact, not a claim that every separately
rebuilt distribution package or physical GPU has been qualified. Hosted macOS
jobs check loader/runtime behavior on both architectures, including the
standalone package with Homebrew hidden; they do not establish these physical
GPU results. Source-only harness unit tests establish neither physical parity
nor eligibility to start the candidate's bounded live acceptance. That acceptance follows
successful exact-candidate physical receipts.

The runtime capability API separately reports `compiled` (`built` alias),
`packaged`, `loadable`, `discovered`, `selectable`, `qualified`, `selected` and
`executed`. A null load/discovery value means unknown; FP64 feature enumeration
permits selection but does not establish residency. Qualification and execution
require a fresh frame bound to processor generation and capture sample lineage;
a saved configuration awaiting restart never relabels the observed backend.
Device fields carry exact driver IDs, not display names. Completed GPU work
remains recorded when its output is rejected and the same frame is retried on
CPU: `selected` is false, `executed` is true, and qualification is `fallback`.
Per-lane `executedBackend` identifies that work independently of the selected
backend; cleared or stale frame receipts never establish execution.

The delay–Doppler processor can use a Vulkan GPU. The implementation targets AMD,
Intel and NVIDIA Vulkan drivers; it does not require CUDA or ROCm.
In the portable `auto`/`gpu` routes, capture, reference synthesis, the FP64 clutter
coefficient solves and linear baseline, detection and tracking remain on the CPU.
The resident frontend moves reference, clutter and ambiguity work to an eligible
FP64 GPU. Noncoherent products use the shared host fusion/temporal/detector/tracker
contract. Coherent resident products also execute those stages on the GPU;
capture, startup validation and result formatting remain host work. Multi-lane
AUTO may select a qualified resident frontend, with independent lane receipts.

Raspberry Pi uses this same capability-selected Vulkan/CPU processing pipeline.
The former Pi-specific `vulkan+cpu` worker and its frozen DSP implementations are
removed from the source and build graph; historical worker artifacts are rejected
by packaging and installation. Platform and receiver identity never choose
a separate detector, temporal integrator or tracker. See the
[Pi 4 guide](PI4_GUIDE.md) for hardware setup and historical evidence limits.

In an earlier Fedora 44 Pi 4 test, installed Mesa 26.0.3-4 timed out during production-size
pipeline creation and fell back to CPU. A later diagnostic loaded Mesa 26.1.8-1
without installing it and ran both GPU stages, reducing processing from
797.228 ms/CPI on CPU to 578.138 ms in Automatic mode. That replay still missed
its 200 ms deadline. These earlier measurements do not qualify the current
unified artifact. See the [Pi 4 guide](PI4_GUIDE.md).

## Selection

Settings → Processing → Acceleration provides:

- **Automatic** (default): check GPU accuracy before selecting it. Delay–Doppler
  qualification takes three frames; GPU clutter adds five combined-stage checks.
  These pre-capture qualification frames use CPU results. Correctness-qualified
  GPU stages remain selected even when their measured latency exceeds the CPU
  baseline. CPU/GPU timings are diagnostics, not an admission veto. Unavailable,
  unqualified or failed GPU work falls back to CPU; hard runtime-health guards
  remain enforced.
- **CPU**: do not open a GPU device.
- **GPU**: use the GPU after accuracy checks, even if the CPU is faster. Reported
  initialization, capacity, invalid-output and device errors still fall back to CPU.

The file setting is `process.performance.acceleration`: `auto`, `cpu`, `gpu`, or
`gpu-resident`. The first three modes retain the behavior described above.

For multi-lane AUTO, resident FP64 probing never pins the portable GPU selector.
An absent/disabled resident module, failed initialization or failed resident
qualification permits the portable FP32-capable worker to qualify under the same
lane/session/memory reservation. Runtime resident failure resets adaptive state
before trying that route once. Required resident-only features and explicit
`gpu-resident` never silently substitute portable products. A retired resident
worker that has not actually exited keeps replacement GPU admission blocked, so
a stuck kernel worker cannot consume a second allocation allowance.

Multi-illuminator wideband channelization is optional in every mode, including
`gpu` and `gpu-resident`; the requested downstream lane policy is unchanged.
Unsupported geometry, startup failure, reset/frame timeout or failed numerical
qualification retires that channelizer worker and recomputes the same input on
CPU at an explicit FIR/radar history boundary. This is reported as wideband
`fallback`, not as GPU execution or a reason to terminate replay. Worker frame
waits retain the normal five-second stuck-worker bound. Exceeding one CPI does
not reject numerically correct GPU output; successful warmup does not waive
first-real-block numerical admission.
Sample rate, common decimation and FFT folding are not admission thresholds.
Valid non-folded/mixed-decimation plans use the generic FP32 FFT implementation;
checked geometry, shared-memory and device-buffer limits remain mandatory.
Split-frame collection drains an already-ready reply even if prior-CPI host work
delayed collection, without granting unfinished work another full timeout.
Resident ReferenceReady/Prepared use the original frame deadline with the same
zero-wait queued-reply drain. Once Prepared is acknowledged, commit, abort and
publication are new control requests with their own bounded acknowledgment
windows; host-side oracle or publication time cannot exhaust a prior request's
window. Restore and reset likewise retain independent bounded windows.

`VECTORWARP_WIDEBAND_TRACE_FRAMES` is honored only by explicit offline replay.
It collects at most 256 parent and 256 worker transport records in memory and
writes them at teardown. Live capture ignores the setting, and neither process
writes trace records during a CPI. Offline teardown may wait for its diagnostic
sink; interrupted or failed workers may lose buffered records.
The private wideband wire tag is v2; mixed old/new workers are rejected instead
of inheriting the former trace semantics. The public numerical module ABI is unchanged.

`BLAH2_GPU_CLUTTER_PROFILE_FRAMES=1..64` likewise requires explicit offline
replay (or an offline test caller's existing `offlineTransportTrace` option).
Every worker spawn strips inherited profiler settings and stale authorization;
only the offline parent supplies a bounded, parent-bound private recipe.
Portable clutter and native resident frame diagnostics retain at most 4 MiB
per log/module and perform no sink I/O at frame, rejection, or commit boundaries.
They drain only after the worker loop returns, during module/process teardown.
Healthy offline shutdown allows 100 ms for that drain before the existing
bounded kill/reap; slow sinks or failed workers may lose diagnostic text.
Truncation is explicitly marked and never changes numerical acceptance.

The optional physical fixture can exercise high-rate non-folded processing with
`VW_INPUT_RATE=3200000 VW_OUTPUT_RATE=400000 VW_SECOND_OUTPUT_RATE=320000
VW_CHANNELS=2 VW_LANES=2 VW_FRAMES=3 VW_SAMPLES=4096 VW_RASTER_HZ=100000
testGpuWidebandPhysical`; it compares both lane rates with independent CPU output.

`gpu-resident` requests the packaged resident pipeline for array-reference synthesis,
spectrum, clutter cancellation and ambiguity, plus coherent products when configured. It
requires native `shaderFloat64`, clutter cancellation, and a valid array or
dedicated-reference input topology. Unset or `auto` `BLAH2_GPU_DEVICE` selects
the first sorted eligible exact device ID in a bounded worker query and pins
it for the session. An explicit missing or FP32-only ID fails closed; it is
never silently replaced. Array reference retains all capture channels in
their original surveillance order. It performs eight labeled CPU
correctness checks at startup; accepted frontend frames use GPU computations.
The single-lane strict coordinator stops on device, accuracy, capacity or worker
failure. Multi-lane CPU-equivalent frontends can instead publish an explicit
per-lane fallback reason and reset adaptive state before CPU continuation.
Noise-whitened fusion, bearing and beam-conditioned resident history fail closed
when their required implementation cannot execute; fallback never substitutes
a different configured statistic.

When resident GPU tracking is enabled, set `process.tracker.gpu_capacity` explicitly. This is
a storage capacity, independent of the wrapping track ID. No default is inferred.
The processor checks the complete allocation against its existing device/shared
memory limits before starting. Existing tracks are never evicted to admit a
burst; if configured storage is exhausted, unmatched initiations are counted in
`dropped_capacity_detections`. Numeric configuration validity alone does not
establish memory fit or real-time performance.

GPU-resident tracking also supports two acceleration initiation models:

- `hypothesis_bank` preserves the original behavior and creates one tentative
  track for every discrete acceleration hypothesis.
- `adaptive_gate` creates one tentative track per unmatched detection. Until
  its first association, it solves the joint delay/Doppler acceleration
  interval continuously. Every candidate accepted by the discrete hypothesis
  bank remains eligible, while incompatible corners of a simply widened box
  are rejected. The first match then supplies the measured acceleration. This
  reduces tentative tracker count and association work without reducing
  `maxAcc`.

GPU-resident checkpoints also size each track's circular history to
`max(100, N)` instead of always reserving 255 entries. Schema5 uses
`20 + 8*max(100,N)` words: 3,280 bytes at N<=100 versus 8,240 at N=255.
Public trails contain measured points from the last100 updates; lifetime
measurement counts remain exact. Guided/coasted points are unmeasured.

The portable and resident trackers share `ConfirmationContract.h`: the seed
is one vote, and subsequent detector/guided votes require disjoint contiguous
1/2/4/8-CPI support in the same epoch. Overlaps cannot update tracks, reset
inactivity or create duplicate births. Guided sustain keeps tracks COASTING.
M-of-N counts N CPI endpoints including the seed. For deepest bank D, admission
requires `N >= 1 + D*(M-1)` and `nDelete >= D-1`; endpoint N remains eligible,
and the first miss beyond nDelete deletes. For M=4, D=1/2/4/8 requires
N>=4/7/13/25 and nDelete>=0/1/3/7. Existing single-CPI 4-of-6 remains valid;
temporal 4-of-6 is refused, never silently widened. API/browser and native
admission report the required values; weak-target presets preserve M/N/deletion.

ABI17/schema5 adds accumulated actual missed time and lifetime measured count
after the history ring; header27 selects ordinary/disjoint history encoding.
Legacy schema1/3/4 is refused by JSON, live wire, snapshots and restore because
irregular elapsed time and lifetime measurement counts cannot be reconstructed.
Use a cold input reset; temporal tracker-only restore requires the identical
checkpoint and warm ring. Module, worker and host must be updated together.
No live migration, receiver recording-format or configuration-default change
is performed.

`AssociationContract.h` shares acceleration-fit/NIS, circular three-peak gating,
actual elapsed intervals and stationary quarantine. Quarantine suppresses
publication while retaining measured state/counts and confirmation, so hidden
measurements cannot cause alternating re-publication. Association uses prior
occupancy; births include the current occupancy update.
`testGpuTrackerCorrectness --temporal-parity` and `--lineage-only` exercise both
backends, irregular gaps, acceleration, multi-peak matching, stationary
enter/hold/exit, hidden state, public JSON and negative controls on software
Vulkan. They do not qualify hardware cadence or false-track rates. Temporal
settings still require positive/negative whole-pass replay qualification of
latency, continuity, fragmentation and false tracks; ADS-B is evaluation only.

The compatibility default is `hypothesis_bank`. Select `adaptive_gate` under
`process.tracker.initiate.acceleration_model`; the host and resident trackers
share these initiation semantics.

GPU-resident tracking defaults to a persistent clutter map that protects tracker
capacity without adding a global zero-Doppler detector notch:

```yaml
process:
  tracker:
    initiate:
      stationary_clutter:
        enable: true
        persistence_cpis: 60
```

The default is 60 CPIs (30 seconds at a 500 ms CPI) across the configured
ambiguity Doppler span. Each detection strengthens only its delay-Doppler cell
and the immediately adjacent Doppler rows. Evidence leaks by one observation
for every four missed CPIs, so intermittent recurring returns accumulate while
isolated crossings expire. This learns narrow persistent ridges despite row
quantization jitter without creating a global Doppler notch or collapsing
unrelated Dopplers at the same delay. Once a cell reaches `persistence_cpis`,
it cannot start another track. A positive
optional `max_doppler_hz` retains the former bounded-band behavior for sites
that explicitly need it; zero or omission means the full configured span.
The same learned map also retires startup artifacts conservatively. A track must
remain within a one-delay-unit and 1.5-Doppler-cell span for a full 100-sample
history, with at least 90 associated samples, while occupying a learned cell.
Association runs before this admission filter, so an established moving track
continues through a learned cell; its motion also prevents retirement. Raw
detection output is unchanged. Set `enable: false` to retain the previous
initiation and retirement behavior. Saving a changed receiver site,
transmitter site, array geometry/orientation, frequency, sample rate, or channel
mapping restarts the processor and therefore starts a fresh temporal clutter map.

Resident postprocessing normally reuses a native clutter-work allocation. If a
larger clutter span makes that allocation too small, startup now allocates a
dedicated scratch buffer of the exact validated postprocess size. The extra
allocation remains subject to the same GPU heap and storage-buffer limits; no
configured geometry is silently shortened.

### Weak-target integration and track before detect

The compatibility detector remains unchanged when these settings are omitted or
disabled. `process.detection.integration_cpis: 2` averages the current and prior
power maps. Its optional `integration_motion_bins: 1` searches one delay and one
Doppler cell of inter-CPI motion.

For weaker echoes, the shared power track-before-detect contract supports
CPU/portable products as well as coherent resident products:

```yaml
process:
  detection:
    track_before_detect:
      enable: true
      max_cpis: 8       # 4 enables 2/4 banks; 8 enables 2/4/8 banks
      motion_bins: 1    # backwards-compatible common value for both axes
      delay_motion_bins: 1
      doppler_motion_bins: 4 # follow fast Doppler slew without wasteful delay trials
```

The delay and Doppler axes are independently configurable up to 2 and 16 bins
per CPI. The resident implementation assigns one GPU workgroup to each map cell and
evaluates as many as 64 delay-Doppler trajectories concurrently in one native
AMD wavefront, then reduces the exact 2/4/8-CPI maxima with a parallel shared-memory
tree. Larger envelopes use ordered 64-path chunks without a trajectory-sized scratch
buffer. The rolling history uses the host-visible heap so it does not displace the
resident clutter workspace from limited discrete-GPU VRAM. Select the smallest envelope that covers
the site's physically eligible aircraft because every additional trajectory is
included in the detector's multiple-hypothesis false-alarm correction.

This path keeps the ordinary detector available as the default. When enabled,
it retains the conventional two-CPI same-cell detector, keeps fused power maps
on the GPU, searches constant-velocity paths at 2-, 4-, and optionally 8-CPI
timescales, and unions candidates from every pass. The conventional pass keeps
its beam-corrected PFA so enabling weak-target search cannot simply replace the
known detector. Each motion bank uses its exact Gamma-distribution cell-averaging
CFAR threshold and an additional correction for the enabled time/motion trials.
Only qualified candidates are then revisited
against the five current complex per-channel maps with a four-times-finer
coherent beam bank, so full-resolution spatial refinement does not multiply the
whole-map workload.

The delay-Doppler display remains a clean current-CPI ambiguity map.  Motion-
integrated TBD hypotheses stay internal to detection and tracking; they are not
painted onto the public map as diagonal path energy.

Temporal accumulation is noncoherent power integration; the candidate refinement
is coherent across antennas. This makes the feature tolerant of unknown echo
phase between CPIs while still recovering spatial array gain. A 4-CPI bank adds
up to two seconds and an 8-CPI bank up to four seconds of history at 500 ms CPI,
so it is intended for weak-target initiation rather than lower-latency display.
The spatial refinement described here requires coherent resident products;
power integration itself does not require a particular receiver, OS or GPU.
Settings validates the selected statistic and current capability dependencies.

Omitting it selects `auto`. Settings shows the backend reported by the running
processor, not the computer displaying the browser.

The following qualification and recovery rules describe the portable modes.
Startup qualification is per processor instance, selected device and geometry:
GPU results must agree with the CPU's complex-valued map before detection sees
them. After qualification, accepted GPU frames do not repeat the full CPU
clutter oracle or complex-map accuracy comparison. Every-frame finite-output,
numerical-solve,
worker-error and timeout protections remain. CPU/GPU timings do not veto a
correctness-qualified AUTO selection. This does not continuously
revalidate accuracy against changing signal conditions; independent benchmark
comparisons remain separate. Before accepting GPU output, input remains
available for same-frame CPU recovery. Once validated clutter output is committed
in place, an unexpected exception aborts the frame rather than retrying CPU
filtering on already changed IQ. Software Vulkan
renderers such as lavapipe are not treated as GPUs.

## Clutter precision and capacity

The portable `auto`/`gpu` Vulkan clutter path keeps its initial coefficients and linear
clutter estimate in FP64 on the CPU. It then uses GPU correlation and filtering
for one residual correction, with coefficient solves still on the CPU. This
reduces rounding error when cancelling strong clutter. Existing accuracy checks
and CPU fallback remain in force in those portable modes; GPU double-precision
shaders are not required for them. The explicit resident route instead performs
its FP64 baseline and coefficient solves on the GPU. Failure follows the strict
single-lane or explicitly reported CPU-equivalent multi-lane policy above.

The reference transform and matrix factorization are reused across channel
batches within a frame and refreshed for the next frame. If all clutter channels
cannot fit at once, processing tries smaller batches under the same allocation
limit. This preserves the configured channel count and all-or-nothing frame
acceptance.

## Measured performance

On a matched RTX 4050 Laptop replay, VectorWarp processed the tested workload in
63.638 ms/CPI on GPU and 180.855 ms/CPI on CPU. Automatic mode reports timings
for the installed hardware and workload while selecting correctness-qualified
GPU work without a comparative-speed veto.

## Drivers and older hardware

Use a working Vulkan driver for the processing host. The application requests
Vulkan 1.0. Portable modes use single-precision GPU compute and do not require
ray tracing, tensor cores, double-precision shaders or a modern RDNA-only
instruction set. The currently Linux-packaged resident route requires native
double-precision shader support. Build/package presence and feature enumeration
do not establish arithmetic qualification. Device limits and allocation failures
are checked before admitting work; CPU-equivalent fallback remains explicit.

On Linux, Mesa RADV supports AMD GCN and RDNA hardware. Some early GCN systems
need a different kernel-driver configuration before RADV can expose their GPU;
follow the [Mesa driver documentation](https://docs.mesa3d.org/drivers/radv.html).
VectorWarp does not change kernel drivers or boot settings automatically.

The native processor account needs access to the selected GPU device. AMD/Intel
normally use `/dev/dri` and the host's Mesa Vulkan driver; NVIDIA uses its host
driver and Vulkan libraries. Package installation and real native installation
append `vectorwarp` to an existing `render` or `video` group only when that group
owns a root-owned DRM render device with group read/write access. The API account
does not receive GPU access. Installation does not change device permissions,
drivers, or running services; an already-running processor needs an explicit
restart to acquire its new groups. With VectorWarp 0.1.7 or newer, use
`vectorwarp restart` for an ordered stop/start of the VectorWarp services;
it does not stop or restart vendor receiver services.

If a GPU is added later, or Settings reports missing service access, run:

```sh
sudo /opt/vectorwarp/libexec/vectorwarp-gpu-setup --enable-service-access
```

The command checks the actual device ownership, asks for confirmation, and
preserves existing groups. It works on desktop GPUs as well as Raspberry Pi.
Unexpected ownership or restrictive permissions require local review; it does
not change ACLs or SELinux policy. Then use **Apply & Restart** in Settings.
Group access alone does not establish a working driver or numerical
qualification. A missing module, driver or usable GPU leaves automatic selection
on CPU.

Hardware test results do not guarantee every driver version or GPU model. Keep
automatic selection enabled on older machines unless measuring a particular GPU.

## Build and verify

Keep both `blah2-gpu-vulkan.so` and `blah2-gpu-worker` beside the `blah2` executable.
`BLAH2_GPU=ON` requires Vulkan/glslang development packages and `VKFFT_ROOT`
pointing to VkFFT 1.3.4 (commit `066a17c17068c0f11c9298d848c2976c71fad1c1`).
`BLAH2_GPU=AUTO` builds without it if dependencies are missing;
`BLAH2_GPU=OFF` produces a CPU-only build.

The native build wrapper fetches that exact VkFFT commit and passes the correct
CMake paths:

```sh
script/build-native.sh --backend all --gpu auto    # all adapters; optional GPU modules
script/build-native.sh --backend all --gpu off     # all adapters; CPU-only artifact
script/build-native.sh --backend all --gpu on       # require GPU plus UHD and HackRF SDKs; RSPduo stays locally buildable
```

Building creates an artifact only; it neither installs drivers nor changes the
active service. See [building from source](INSTALL.md#build-from-source) for dependencies and the separate
preflight/install step.

Standalone processing checks do not require an SDR or modify live configuration:

```sh
cmake -S test/gpu -B build/gpu-checks -DBLAH2_GPU=ON -DVKFFT_ROOT=/path/to/VkFFT
cmake --build build/gpu-checks
build/gpu-checks/bin/testAcceleration --list
build/gpu-checks/bin/testAcceleration auto --quick
build/gpu-checks/bin/testAcceleration auto --matrix
build/gpu-checks/bin/testAcceleration --limits
build/gpu-checks/bin/testAccelerationFallback
```

Replace `auto` with an ID from `--list` to check each GPU separately. IDs are
session-local diagnostics, not persistent hardware identifiers. Test fixtures
stay offline and are never sent to live radar pages.

The matrix checks 1–8 surveillance paths, 50–500 ms frames, offset Doppler windows
and prime-length FFTs against independent CPU processing. Timing numbers from
resource-limited checks are not whole-radar performance benchmarks.

## Driver recovery

GPU drivers run in a separate worker process. Startup has a 30-second deadline;
individual GPU frames have a five-second deadline. A timeout, worker crash or
invalid response disables GPU use and retries the same frame on CPU. These are
failure deadlines, not promises that a failed frame meets real-time cadence.
Portable, resident and wideband workers bind their spawning parent's PID in a
private per-child launch environment. A live container entrypoint with PID 1 is
valid; an adopted worker whose original parent died is not. Linux parent-death
signals and macOS process-exit watches stop workers even while driver work is
blocked. The binding is checked before and after installing that guard and does
not change the numerical or module ABI. The GPU-free `gpuWorkerLifetime` test
checks both orphan races; `gpuWorkerPidNamespace` exercises a real PID-1 parent
when the host permits nested PID namespaces (otherwise reports an explicit skip).
The processor does not repeatedly restart a failed worker. Use **Apply &
Restart** in Settings or `vectorwarp restart` (version 0.1.7 or newer) for a
fresh GPU attempt; merely reopening the web page does not restart processing.

This isolates user-space driver faults. It cannot reset a broken kernel driver
or recover a host-wide GPU/kernel failure. CPU mode does not start the worker.
