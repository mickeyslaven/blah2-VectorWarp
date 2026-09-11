# GPU acceleration

The delay–Doppler processor can use a Vulkan GPU. The implementation targets AMD,
Intel and NVIDIA Vulkan drivers; it does not require CUDA or ROCm. Physical
verification is limited to the devices and driver versions in
[GPU hardware tests](GPU_HARDWARE_TESTS.md), not every GPU in those families.
Capture, reference synthesis, the small FP64 clutter coefficient solve,
detection and tracking remain on the CPU.

On a Raspberry Pi 4 running Fedora 44, production-size attempts in Automatic
and GPU modes timed out during driver pipeline creation and fell back to CPU.
A separate source-built diagnostic verified six small frames on its real V3D
GPU against an independent CPU reference. Production-size Pi GPU processing
and speedup remain unverified. See the [Pi validation report](PI4_VALIDATION_20260910.md)
and [startup diagnostics](GPU_DIAGNOSTICS.md).

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
comparisons remain separate. The input remains available for same-frame CPU
recovery. Software Vulkan
renderers such as lavapipe are not treated as GPUs.

## Measured performance

On the tested RTX 4050 Laptop, the GPU reduced a matched 200 ms, ±800 Hz
recorded-IQ replay from 218.679 ms/CPI in regular blah2 to 109.153 ms/CPI in
VectorWarp: 50.1% less processing time. This fixed-range comparison used the
same 527 MHz, 2.4 MS/s recording, delays −10…245 (256 bins; 30.604 km maximum
excess path), and clutter −10…200 for both engines. It accelerates clutter
FFT/filtering and delay–Doppler work; the small FP64 coefficient solve and
other pipeline stages remain CPU work. The
[GPU benchmark report](GPU_BENCHMARK_20260911.md) has the comparison, accuracy
checks, deadline counts and method.

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
driver and Vulkan libraries. Add `vectorwarp` only to the required `render` or
`video` group after reviewing the host device ownership. The installer does not
change device permissions or drivers. A missing module, driver or usable GPU
leaves automatic selection on CPU.

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
script/build-native.sh --backend all --gpu on       # require GPU plus all four receiver SDKs
```

Building creates an artifact only; it neither installs drivers nor changes the
active service. See [SETUP.md](SETUP.md) for the separate preflight/install step.

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
The processor does not repeatedly restart a failed worker. A restart of VectorWarp
allows a fresh GPU attempt.

This isolates user-space driver faults. It cannot reset a broken kernel driver
or recover a host-wide GPU/kernel failure. CPU mode does not start the worker.
