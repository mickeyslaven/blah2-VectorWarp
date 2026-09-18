# macOS support and qualification boundary

Apple Silicon (`arm64`) execution evidence comes from an Apple M2 on macOS
26.6.1. It covers CPU/replay and browser/service lifecycle, a locally installed
Homebrew app plus Kraken companion through revision 17, and bounded physical
local-USB Kraken capture. Intel (`x86_64`) is only a proposed CI/build target:
its matrix has not run, so no Intel runtime, GPU, receiver, or performance
support is established. The detailed results are in
[MACOS_TEST_MATRIX.md](MACOS_TEST_MATRIX.md). A detected device, installed SDK,
or enumerated GPU is only an *available candidate* until VectorWarp's startup
and numerical qualification gates accept it.

No proprietary SDRplay SDK is bundled, fetched, installed, or auto-downloaded
by this repository or its local formula. The SDK used for local validation was
obtained separately and is never staged with VectorWarp; users obtain and
maintain their own vendor installation. Receiver probes only inspect local
state. A user-requested Homebrew action may install the open-source UHD or
HackRF formula, and the local Heimdall build fetches checksum-pinned public
Kraken sources; neither action supplies SDRplay software. See the Kraken
[source provenance and redistribution boundary](MACOS_KRAKEN.md#source-provenance).

## Receiver support

| Receiver/path | Vendor macOS position | VectorWarp boundary | macOS status |
| --- | --- | --- | --- |
| Ettus USRP B210 through UHD | UHD's current repository lists macOS among its tested/supported OSs; the B2x0 manual documents the B210 USB 3 host connection and requires UHD 3.8.4 or newer. | VectorWarp builds and loads the local Homebrew UHD module. SDK setters/readback and streaming are simulated, and real no-device startup has a bounded error and clean shutdown. It never invokes image/firmware download tooling. | Apple Silicon SDK integration and simulated capture are exercised; no USRP hardware, tuning, throughput, or RF qualification. |
| HackRF through libhackrf | The official HackRF install guide documents `brew install hackrf` and `port install hackrf`; its troubleshooting guide explicitly names macOS System Report. | VectorWarp builds and loads the local Homebrew `libhackrf` module. Paired selection, callbacks, settings and cleanup are simulated, with real no-device startup. | Apple Silicon SDK integration and simulated capture are exercised; no dual-HackRF hardware, synchronization, throughput, or RF qualification. |
| Local KrakenSDR / Heimdall V2 | KrakenRF provides the Suite and its fork of the RTL-SDR USB driver. | The macOS companion builds pinned native Heimdall and a private libusb driver; the launcher owns its loopback controller for live Kraken. VectorWarp consumes its `MCHQ` frames at 2.4 MS/s. Linux endpoint/service behavior remains separate. | Native build and simulated checks pass. A physical five-channel Kraken passed internal calibration, bounded USB capture, retune and restart on the development M2. External antenna coherence and other platforms remain unverified; see [MACOS_TEST_MATRIX.md](MACOS_TEST_MATRIX.md). |
| SDRplay RSPduo through SDRplay API | SDRplay's current Hardware API page supplies a macOS API 3.15 download and lists RSPduo among supported devices. The vendor's Apple-silicon install note shows its installer installs a local API service. | The local adapter source kit compiles only against a manually installed SDK. The repository and formula never fetch, bundle, install, or auto-download the SDK. Tests cover local compilation/loading, a controlled no-device service result, and synthetic replay. | Apple Silicon SDK integration is exercised; no physical RSPduo capture, tuning, throughput, or RF qualification. |

Relevant vendor sources: [UHD OS support](https://github.com/EttusResearch/uhd),
[B2x0/B210 host requirements](https://files.ettus.com/manual/page_usrp_b200.html),
[B2x0 image compatibility](https://files.ettus.com/manual/page_compat.html),
[HackRF installation](https://github.com/greatscottgadgets/hackrf/blob/main/docs/source/installing_hackrf_software.rst),
[HackRF macOS diagnosis](https://github.com/greatscottgadgets/hackrf/blob/main/docs/source/troubleshooting.rst),
[KrakenSDR Suite V2](https://github.com/krakenrf/krakensdr_suite), and
[SDRplay Hardware API](https://sdrplay.com/hardware-api/).

## Vulkan/MoltenVK

MoltenVK is the Vulkan-to-Metal implementation used by the local Apple Silicon
build. The current backend discovers instance extensions, enables
`VK_KHR_portability_enumeration` and
`VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` when offered, and enables
the advertised `VK_KHR_portability_subset` device extension. These are runtime
capability checks, not platform-name assumptions. The compute path uses no
presentation surface and still validates device limits and extensions at
runtime.

The compute inputs, shaders (`vec2`), and VkFFT plans are FP32. The CPU
ambiguity path remains FP64, and `Acceleration.cpp` independently checks every
complex bin on initial frames before allowing GPU output into detection, falling
back to CPU on accuracy failure. No GPU FP64 feature is requested, so lack of
Metal/Vulkan FP64 compute is not itself a blocker. FP32 numerical and
performance qualification on each Mac/GPU/driver combination remains required.

The historical Linux baseline used `memfd`, `/proc` descriptor handling and ELF
loader paths; it is not the current macOS implementation. The macOS worker now
uses private POSIX shared memory, Darwin-compatible Unix sockets and spawn
descriptor allowlisting, with a `kqueue` parent-exit watcher. CMake supplies a
Mach-O loader path and builds the portable Vulkan module when explicitly enabled
with locally available Vulkan, glslang, and VkFFT dependencies. See
[MACOS_GPU.md](MACOS_GPU.md) for the implemented behavior and repeatable
qualification commands. Homebrew provides the open GPU dependencies; VectorWarp
does not install a proprietary GPU driver.

Authoritative Vulkan sources: [MoltenVK runtime user guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md)
and [MoltenVK macOS setup guidance](https://github.com/KhronosGroup/MoltenVK/blob/main/README.md?plain=1).

## Qualification status

1. Receiver probes report only local dependency/device/network reachability;
   they do not start a vendor service, install a package, download an image, or
   claim streaming works without a bounded capture validation.
2. GPU setup reports `not installed`, `enumerated/unqualified`, or `qualified`
   separately. Enumeration requires the portability path above; qualification
   requires VectorWarp's existing replay/accuracy and timing gates.
3. The proposed Apple Silicon/Intel CI matrix must execute before it can be used
   as CI evidence. Each optional dependency still needs architecture-specific
   execution, and each receiver needs a physical-device capture test before its
   hardware status can be called qualified.
