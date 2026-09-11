# GPU verification — 10 September 2026

This is the initial delay–Doppler verification. For the later combined GPU clutter
and delay–Doppler build, see [final device confirmations](benchmarks/20260911/v8-all-device-confirmation.md)
and the [matched replay and live benchmark report](GPU_BENCHMARK_20260911.md).

Offline processing tests compare every complex delay–Doppler bin with independent
CPU processing. No test data is sent to the radar UI, and no live receiver settings
are changed.

| GPU | Full matrix | Maximum relative RMS error |
| --- | --- | --- |
| AMD Radeon 8060S / Strix Halo | Pass: 15 cases | 6.13 × 10⁻⁷ |
| NVIDIA RTX 4050 Laptop | Pass: 15 cases | 1.08 × 10⁻⁶ |
| AMD Radeon 540/550 family / Polaris 12 | Pass: 15 cases | 6.96 × 10⁻⁷ |
| Intel HD Graphics 630 / Kaby Lake | Pass: 15 cases | 2.75 × 10⁻⁷ |
| Intel Iris Plus G4 / Ice Lake | Not tested: host thermal alarms | — |

The AMD and Intel runs use Mesa 26.1.8 on Linux. The NVIDIA run uses the installed
NVIDIA Linux driver 580.173.02. These are verified devices, not a guarantee for every GPU or
driver version. The Polaris result provides direct coverage of older AMD hardware.

Each matrix uses five frames per case. It covers 1–8 surveillance paths, 50, 100,
200 and 500 ms input frames, offset Doppler windows, rounded/unrounded range FFTs,
and prime-length Doppler transforms. The acceptance threshold is 10⁻⁴ relative
RMS error, with an additional peak-error check during automatic qualification.
Axes and input consumption must also match the CPU. Forced-GPU cases fail if they
silently fall back to CPU; fallback cannot masquerade as a GPU pass.

Additional checks cover small frames, high/low signal scales, CPU preference for
small workloads, oversized/invalid GPU dimensions, missing drivers, software-only
Vulkan and a CPU-only build. Injected invalid output and reported device-loss tests
verify recovery without consuming a frame twice. The isolated-worker tests also
inject startup/frame hangs, crashes and malformed replies, verifying deadlines,
child cleanup and same-frame CPU recovery. These are controlled fault injections,
not physical GPU removal or a claim to recover a broken kernel.

Two bugs were fixed during verification:

- Prime-length FFT buffers were bound before VkFFT allocated their lookup tables,
  producing zero Doppler output. Binding at execution-recording time fixes this;
  AMD tests also pass with Vulkan validation enabled and no reported errors.
- The existing CPU Doppler scratch buffer used the range FFT length. Wide Doppler
  windows could overrun it. The buffer now uses the Doppler length, with a
  deterministic regression test and matching settings validation.

Fedora full-processor build, Ubuntu 22.04 standalone GPU and CPU-only builds,
all seven CTest suites, API/configuration tests and browser DOM tests pass. Resource
limits were used during hardware checks; reported timings are not whole-radar
benchmarks. This verification does not start or deploy a live radar service.

See [GPU acceleration](GPU_ACCELERATION.md) for configuration, reproducible test
commands, driver requirements and worker recovery limits.

## Raspberry Pi 4 follow-up

Fedora 44 / Mesa 26.0.3-4 on the Pi's V3D 4.2.14.0 passed six small GPU frames
against an independent direct CPU correlation/DFT calculation, with worst
relative RMS error `1.37223e-7`. This was a separate source-built diagnostic,
not the full matrix above or installed-package GPU acceptance. Production-size
recorded-IQ initialization still exceeded the 30-second deadline during driver
pipeline creation and safely fell back to CPU. No Pi GPU speedup is established.
The [Pi report](PI4_VALIDATION_20260910.md) records versions, geometry and limits;
[GPU diagnostics](GPU_DIAGNOSTICS.md) describes the opt-in test.
