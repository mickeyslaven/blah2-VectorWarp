# macOS GPU processing

The macOS worker runs Vulkan compute through a user-installed MoltenVK runtime.
GPU enumeration is separate from numerical qualification. Each new processing
instance checks its geometry against the FP64 CPU result before accepting GPU
maps. Auto mode also checks speed; CPU fallback remains available independently
for ambiguity and clutter. A qualified ambiguity stage does not qualify clutter.

## Source build

Install the open dependencies explicitly:

```sh
brew install vulkan-loader vulkan-headers molten-vk glslang
```

Supply an unpacked [official VkFFT 1.3.4 archive](https://github.com/DTolm/VkFFT/archive/refs/tags/v1.3.4.tar.gz),
whose SHA-256 is
`b61055393adb3adc79009fe12401cbfbbdfba584e665e9c35fcbf4b32fb31f30`.
The directory must contain `vkFFT/vkFFT.h` and the MIT `LICENSE` file. The source
build helper does not download this SDK or other dependencies:

```sh
VKFFT_ROOT=/absolute/path/to/VkFFT-1.3.4 script/build-macos.sh --gpu on --test
```

The worker and `blah2-gpu-vulkan.so` module are installed beside the processor.
The `.so` filename is CMake's module convention; the file is a native Mach-O
bundle on macOS. Vulkan, MoltenVK, glslang and FFTW remain external dependencies.
No proprietary receiver SDK is part of the GPU path.

## Runtime and numerical safeguards

The Vulkan instance discovers and enables `VK_KHR_portability_enumeration` and
its instance flag when advertised. Device creation enables the advertised
`VK_KHR_portability_subset` extension. Physical-device type, compute queues,
buffer/workgroup limits and allocation budgets remain runtime checks, following
the [MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md).

The Darwin worker uses deadline-bounded framed Unix sockets and private POSIX
shared memory, unlinked immediately and explicitly aligned to the VM page size.
Spawn uses a kernel descriptor allowlist; the worker closes the shared-memory
descriptor after mapping and before loading any driver. A `kqueue` parent-exit
watcher terminates even a worker stuck inside a driver call. Linux retains its
existing sequenced sockets, sealed `memfd`, descriptor isolation and parent-death
signal implementation.

Testing found intermittent incorrect and nonfinite prime FFT results with
VkFFT 1.3.4's native prime plans through MoltenVK on Apple M2. Changing its Rader,
Bluestein and twiddle-table tuning alone did not resolve the failures. macOS
portability devices therefore use an explicit Bluestein convolution whenever
the transform has a prime factor above 13. Constant chirp spectra are generated
once in FP64 with FFTW; every frame's input, convolution FFTs and output remain
on the GPU. The convolution uses smooth-length VkFFT transforms. Transform
dimensions, map coordinates and error tolerances are unchanged. All extra
device allocations pass through the existing allocation budget.

Clutter can safely remain on the CPU when its stricter weak-residual check
rejects an FP32 result. Such a rejection must not be described as fully
qualified GPU clutter.

## Reproducible synthetic qualification

The following opt-in suite requires a real GPU and cannot pass by substituting
CPU processing. The default CTest suite opens no physical GPU:

```sh
cmake -S test/gpu -B build/macos-gpu \
  -DCMAKE_BUILD_TYPE=Release -DBLAH2_GPU=ON \
  -DVKFFT_ROOT=/absolute/path/to/VkFFT-1.3.4 \
  -DBLAH2_TEST_PHYSICAL_GPU=ON
cmake --build build/macos-gpu -j 1
ctest --test-dir build/macos-gpu --output-on-failure -V
```

Tests include independent direct DFT/correlation (including prime transforms),
FP64 CPU comparisons at 1–8 channels, production frame lengths, signed delays,
Doppler offsets, prime sizes, capacity rejection, clutter fallback, and 128
production frames with five surveillance channels. Output reports completed
GPU dispatches, CPU qualification runs, relative RMS/peak error and timing.
The numerical thresholds remain `1e-4` for normalized RMS and peak error.
Process fixtures additionally check crashes, hangs, malformed responses,
descriptor isolation, parent death during startup/frame work, and symlink-safe
module discovery. Repeat direct DFT with `BLAH2_GPU_MEMORY_PATH=direct` and
`BLAH2_GPU_MEMORY_PATH=staged` to exercise both transfer paths.

Local evidence is geometry-, device- and driver-specific. Apple M2/MoltenVK
results cover both ambiguity and clutter in the installed application, where
they were faster than the matched CPU replay while retaining independent CPU
fallback. They do not establish Intel macOS performance, other GPU/driver
combinations, physical receiver capture, or a whole-pipeline real-time deadline.
The M2's bounded six-minute automatic/GPU observation had no processing
overruns, but it remains a local result rather than a general guarantee; see
[MACOS_TEST_MATRIX.md](MACOS_TEST_MATRIX.md) and
[MACOS_PROFILING.md](MACOS_PROFILING.md).
