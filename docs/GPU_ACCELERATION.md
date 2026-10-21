# GPU acceleration

For macOS, see [Vulkan/MoltenVK support and M2 validation](MACOS_GPU.md) and
[Homebrew installation](MACOS_HOMEBREW.md). The driver and package instructions
on this page describe Linux. Legacy modes retain CPU fallback on both platforms;
the explicit resident mode described below is Linux-only.

The delay–Doppler processor can use a Vulkan GPU. The implementation targets AMD,
Intel and NVIDIA Vulkan drivers; it does not require CUDA or ROCm. Physical
verification is limited to the devices and driver versions in
[GPU hardware tests](GPU_HARDWARE_TESTS.md), not every GPU in those families.
In the legacy `auto`/`gpu` routes, capture, reference synthesis, the FP64 clutter
coefficient solves and linear baseline, detection and tracking remain on the CPU.
The explicit `gpu-resident` route moves the computational stages to the GPU;
capture, startup validation and result formatting remain host work.

The selection details below describe the generic Vulkan path. On the qualified
Raspberry Pi 4B Bookworm geometry, AUTO also has an isolated whole-CPI
`vulkan+cpu` mixed candidate. It needs verified maps and a greater than 5%
whole-CPI median benefit before selection; a crash, hang, invalid map, or missed
safeguard returns processing to CPU. It does not replace generic AUTO on other
hardware or geometries, and has not completed mixed-worker endurance acceptance.
See the [Pi 4 guide](PI4_GUIDE.md).

On the Fedora 44 Pi 4, installed Mesa 26.0.3-4 timed out during production-size
pipeline creation and fell back to CPU. A later diagnostic loaded Mesa 26.1.8-1
without installing it and ran both GPU stages, reducing processing from
797.228 ms/CPI on CPU to 578.138 ms in Automatic mode. That replay still missed
its 200 ms deadline. See [Pi results and supported driver updates](PI_GPU_SETUP.md#pi-4-driver-diagnostic)
and the [initial validation report](PI4_VALIDATION_20260910.md).

## Selection

Settings → Processing → Acceleration provides:

- **Automatic** (default): check GPU accuracy before selecting it. Delay–Doppler
  qualification takes three frames; GPU clutter adds five combined-stage checks.
  Then
  use it only if the measured delay–Doppler stage is at least 5% faster, including
  worker transfers and map conversion. Those first frames use
  CPU results.
  After selection, automatic mode also checks successive groups of five complete GPU frames, including
  input retirement, and returns to CPU if sustained processing loses that margin.
  This is an ambiguity-stage safeguard, not a whole-pipeline speed guarantee.
- **CPU**: do not open a GPU device.
- **GPU**: use the GPU after accuracy checks, even if the CPU is faster. Reported
  initialization, capacity, invalid-output and device errors still fall back to CPU.

The file setting is `process.performance.acceleration`: `auto`, `cpu`, `gpu`, or
`gpu-resident`. The first three modes retain the behavior described above.

`gpu-resident` selects the Linux resident pipeline for array-reference synthesis,
spectrum, clutter cancellation, ambiguity, fusion, detection and tracking. It
requires native `shaderFloat64`, an explicit `BLAH2_GPU_DEVICE`, array reference,
clutter, and all capture
channels in their original surveillance order. It performs eight labeled CPU
correctness checks at startup; qualified frames use GPU computations. A device,
accuracy, capacity, or worker error stops this strict route. It does not switch
to CPU DSP.

When tracking is enabled, set `process.tracker.gpu_capacity` explicitly. This is
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
`process.tracker.initiate.acceleration_model` only with `gpu-resident`.

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

For weaker echoes on coherent arrays, the Linux `gpu-resident` route also offers
an explicit track-before-detect path:

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
per CPI. Track-before-detect assigns one GPU workgroup to each map cell and
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
Track-before-detect requires coherent surveillance fusion and `gpu-resident`.
The browser Settings page validates those dependencies before restart.

Omitting it selects `auto`. Settings shows the backend reported by the running
processor, not the computer displaying the browser.

The following qualification and recovery rules describe the legacy modes.
Startup qualification is per processor instance, selected device and geometry:
GPU results must agree with the CPU's complex-valued map before detection sees
them. After qualification, accepted GPU frames do not repeat the full CPU
clutter oracle or complex-map accuracy comparison. Every-frame finite-output,
numerical-solve,
worker-error and timeout protections remain, as does AUTO's sustained GPU timing
comparison against the CPU cost measured at startup. This does not continuously
revalidate accuracy against changing signal conditions; independent benchmark
comparisons remain separate. Before accepting GPU output, input remains
available for same-frame CPU recovery. Once validated clutter output is committed
in place, an unexpected exception aborts the frame rather than retrying CPU
filtering on already changed IQ. Software Vulkan
renderers such as lavapipe are not treated as GPUs.

## Clutter precision and capacity

The legacy `auto`/`gpu` Vulkan clutter path keeps its initial coefficients and linear
clutter estimate in FP64 on the CPU. It then uses GPU correlation and filtering
for one residual correction, with coefficient solves still on the CPU. This
reduces rounding error when cancelling strong clutter. Existing accuracy checks
and CPU fallback remain in force in those legacy modes; GPU double-precision
shaders are not required for them. The explicit resident route instead performs
its FP64 baseline and coefficient solves on the GPU and stops on failure.

The reference transform and matrix factorization are reused across channel
batches within a frame and refreshed for the next frame. If all clutter channels
cannot fit at once, processing tries smaller batches under the same allocation
limit. This preserves the configured channel count and all-or-nothing frame
acceptance.

## Measured performance

On the tested RTX 4050 Laptop, the GPU reduced a matched 200 ms, ±800 Hz
recorded-IQ replay from 230.457 ms/CPI in regular blah2 to 63.638 ms/CPI in
VectorWarp: 72.4% less processing time. VectorWarp CPU averaged 180.855 ms/CPI.
This fixed-range comparison used the same 527 MHz, 2.4 MS/s recording,
delays −10…245 (256 bins; 30.604 km maximum
excess path), and clutter −10…200 for both engines. These legacy-mode measurements accelerate clutter
FFT/filtering and delay–Doppler work; the small FP64 coefficient solve and
other pipeline stages remain CPU work. The
[GPU benchmark report](GPU_BENCHMARK_20260911.md) has the comparison, accuracy
checks, deadline counts and method.
The GPU missed none of the 24 measured steady CPI deadlines, versus 22 for
regular blah2. These replay measurements use startup-only qualification; the
report separately labels earlier native live measurements with recurring checks.
The same-campaign before/after table separates the combined efficiency changes
from the original-blah2 comparison. CPU-only processing also improved: the
200 ms, ±2400 Hz cases use 25.1–25.9% less time than upstream on the tested hosts.

## Drivers and older hardware

Use a working Vulkan driver for the processing host. The application requests
Vulkan 1.0. Legacy modes use single-precision GPU compute and do not require
ray tracing, tensor cores, double-precision shaders or a modern RDNA-only
instruction set. The Linux resident route requires native double-precision
shader support. Device limits and allocation failures are checked; legacy modes
can use CPU fallback on cards with limited memory, while resident mode stops.

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
script/build-native.sh --backend kraken --gpu auto  # GPU when available, CPU fallback otherwise
script/build-native.sh --backend kraken --gpu off   # CPU-only artifact
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
