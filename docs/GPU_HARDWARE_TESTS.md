# GPU verification — 10 September 2026

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
