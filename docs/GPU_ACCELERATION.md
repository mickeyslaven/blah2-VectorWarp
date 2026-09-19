# GPU acceleration

For macOS, see [Vulkan/MoltenVK support and M2 validation](MACOS_GPU.md) and
[Homebrew installation](MACOS_HOMEBREW.md). The driver and package instructions
on this page describe Linux; CPU fallback remains available on both platforms.

The delay–Doppler processor can use a Vulkan GPU. The implementation targets AMD,
Intel and NVIDIA Vulkan drivers; it does not require CUDA or ROCm. Physical
verification is limited to the devices and driver versions in
[GPU hardware tests](GPU_HARDWARE_TESTS.md), not every GPU in those families.
Capture, reference synthesis, the small FP64 clutter coefficient solve,
detection and tracking remain on the CPU.

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

The file setting is `process.performance.acceleration`: `auto`, `cpu` or `gpu`.
Omitting it selects `auto`. Settings shows the backend reported by the running
processor, not the computer displaying the browser.

Startup qualification is per processor instance, selected device and geometry:
GPU results must agree with the CPU's complex-valued map before detection sees
them. After qualification, accepted GPU frames do not repeat CPU clutter or
complex-map accuracy computations. Every-frame finite-output, numerical-solve,
worker-error and timeout protections remain, as does AUTO's sustained GPU timing
comparison against the CPU cost measured at startup. This does not continuously
revalidate accuracy against changing signal conditions; independent benchmark
comparisons remain separate. Before accepting GPU output, input remains
available for same-frame CPU recovery. Once validated clutter output is committed
in place, an unexpected exception aborts the frame rather than retrying CPU
filtering on already changed IQ. Software Vulkan
renderers such as lavapipe are not treated as GPUs.

## Measured performance

On the tested RTX 4050 Laptop, the GPU reduced a matched 200 ms, ±800 Hz
recorded-IQ replay from 230.457 ms/CPI in regular blah2 to 63.638 ms/CPI in
VectorWarp: 72.4% less processing time. VectorWarp CPU averaged 180.855 ms/CPI.
This fixed-range comparison used the same 527 MHz, 2.4 MS/s recording,
delays −10…245 (256 bins; 30.604 km maximum
excess path), and clutter −10…200 for both engines. It accelerates clutter
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
Vulkan 1.0 and uses single-precision compute; it does not require ray tracing,
tensor cores, double-precision shaders or a modern RDNA-only instruction set.
Device limits and allocation failures are checked. Large configurations can use
CPU fallback on cards with limited memory.

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
