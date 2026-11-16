# GPU acceleration

For macOS, see [macOS installation and GPU setup](MACOS.md). The driver-install
instructions below describe Linux. Portable FP32 and wideband GPU stages are
buildable on Linux and macOS/MoltenVK; execution depends on the installed module,
device capabilities and numerical qualification, never the receiver name.
The current build policy packages the FP64 resident module on Linux only;
Darwin reports it unavailable because its resident arithmetic proof is not
implemented. This is not a claim of qualified execution on every Linux GPU.

## Unified-artifact acceptance

Public native and macOS builds use `--backend all`. Partial adapter builds
require the internal `VECTORWARP_INTERNAL_ADAPTER_FIXTURE=true` test override;
they are not product variants. The all-receiver artifact contains the open SDR
adapters and the manual-SDK RSPduo source kit when the proprietary SDK is absent.

`test/gpu/product_parity_test.py` runs the same canonical target and two-CPI
temporal fixture through CPU and GPU processing under all four receiver
identities, without opening an SDR. It compares complete maps, detections,
track updates/IDs and exact temporal support/reset lineage, including a second
replay epoch. Every receipt binds the source SHA, native artifact hashes,
fixture hashes, device `shaderFloat64`, and each observed execution frame's
process generation, CPI, capture epoch and sample window. Enumeration alone is
not qualification. The final two frames must execute GPU stages without the
startup CPU oracle. FP32-only devices must execute portable/wideband stages and
refuse the resident route; Linux FP64 devices must also pass resident parity.

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

Main-branch CI and release signing require successful receipts downloaded from
that exact SHA's trusted dispatch; after qualification, rerun the receipt gate
if it previously failed for absent evidence. This is source/artifact-bound
qualification of the tested Linux artifact, not a claim that every separately
rebuilt distribution package or physical GPU has been qualified. Both macOS
architectures additionally run the same harness, including in the standalone
package with Homebrew hidden. None of these physical results is established by
the source-only harness unit tests.

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
not built, packaged or selected by the production processor. Historical fixture
sources remain for comparison only. Platform and receiver identity never choose
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
`max(100, N)` instead of always reserving 255 entries. The public 100-point
trail and every supported M-of-N confirmation window are unchanged. With the
usual `N: 5`, each record falls from 8,224 to 3,264 bytes, so larger capacities
use about 60% less tracker-bank memory and copy bandwidth.

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
The processor does not repeatedly restart a failed worker. Use **Apply &
Restart** in Settings or `vectorwarp restart` (version 0.1.7 or newer) for a
fresh GPU attempt; merely reopening the web page does not restart processing.

This isolates user-space driver faults. It cannot reset a broken kernel driver
or recover a host-wide GPU/kernel failure. CPU mode does not start the worker.
