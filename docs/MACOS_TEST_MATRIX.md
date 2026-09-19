# macOS verification and remaining hardware coverage

Standalone installer development is separate from the accepted Homebrew/source
matrix below. Local Apple Silicon testing has verified relocation of 142 native
files, bundled Python/Node startup, synthetic CPU replay with browser Save &
Restart, unchanged bundle contents after use, and bundled MoltenVK ambiguity
and clutter execution. Universal app/package assembly and the active-process
installer guard pass small ARM64/Intel executable fixtures; those fixtures are
not full Intel VectorWarp runtime qualification. The local dependency set
requires macOS 26. The standalone CI workflow targets macOS 15 on both
architectures. The [Apple Silicon job for `36c70d8`](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35410057065/job/105807722419)
passed its dependency audit, 18 replay cases, 11 configuration cases, open-SDK
no-device handling, API lifecycle and unchanged-bundle audit with Homebrew hidden.
Intel subsequently passed the same CPU/replay, configuration, SDK, lifecycle and
unchanged-bundle checks with Homebrew hidden. [Run 35416945672](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35416945672) passed the current
macOS 15 arm64 and Intel runtime jobs for source revision
`75b93031835df06c5b1edc681f07356cfd0dacf6`. The Intel GPU driver probe returns
valid JSON in 0.135 seconds under the narrow Apple virtual-device workaround.
Forced GPU replay still fails accuracy qualification and correctly falls back to
CPU. Full combined installer assembly passed locally. Its Apple Silicon app
passed synthetic replay, web-only/start/restart/Save & Restart/stop, bootstrap and
strict signature verification. The expanded installer matches the app, both
runtime manifests remain unchanged and no test processes remain. The package
was inspected without installing it. These results do not qualify physical
Intel hardware or GPUs.
The later [standalone run 35420421113](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35420421113)
passed both architectures for source `f8135ac947a88dd416b0f0ae8d3d891dd546da1d`.
A local universal candidate from that run includes per-architecture third-party
notices and passed the finalizer's explicit native signatures, refreshed core and
runtime manifest bindings, and expanded-package equality checks. After local
ad-hoc signing, Apple Silicon hot JavaScript/Wasm, Python native imports, app
bootstrap, synthetic replay and Save & Restart passed. A synthetic GPU replay
executed both Vulkan ambiguity and clutter processing with finite output. Intel
Node/Python and processor-status smoke checks passed using an existing Rosetta
installation; that is emulated execution, not physical Intel qualification.
Ad-hoc mode deliberately omits hardened runtime. A small executable fixture
showed that hardened ad-hoc processes could not load the private libraries
without matching Team IDs. Developer ID mode retains the narrower production
entitlements and awaits actual certificate-backed execution and Apple checks.
Public binary distribution, Developer ID signing and notarization are not yet
qualified. See [MACOS_STANDALONE.md](MACOS_STANDALONE.md).

Local verification used macOS 26.6.1 on an Apple M2 with 8 GiB RAM,
AppleClang and Node 24.21.0. Results below distinguish actual Mac execution,
real SDK integration, simulated radio input, and a directly connected physical
KrakenSDR. No remote Kraken was used.
The working branch includes merged PR #26 (`a1fe4cd`); its updated browser
labels pass the full API and DOM regressions with the macOS changes.

## GitHub CI verification

The PR checks passed for head `520c2e9` on 2026-09-18:

| Platform | Executed checks | Remaining limits |
| --- | --- | --- |
| macOS Apple Silicon (`macos-15`) | CPU/open-SDK build and native tests, synthetic replay/configuration, native Kraken simulation, API/DOM, Chromium map, and 360-second lifecycle/recovery test; both Homebrew formulas installed and tested, installed replay/Kraken pipeline checked, packages removed | CI uses synthetic input; physical M2 and GPU results below are separate local evidence |
| macOS Intel (`macos-15-intel`) | CPU/open-SDK build and native tests, synthetic replay/configuration, native Kraken simulation, API/DOM, Chromium map, and 360-second lifecycle/recovery test | Physical receivers, GPU performance and installed Homebrew packages remain unverified on Intel |
| Linux x86_64 and ARM64 | CPU/API/replay checks and all ten Ubuntu 22.04/24.04/26.04, Debian 13 and Fedora 44 package jobs, including fresh installed services and Chromium checks | Synthetic receiver input does not qualify physical hardware or Linux GPU performance |

Results: [macOS matrix](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739948),
[Linux CPU checks](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739840),
[Linux package matrix](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739837).
The [Homebrew publication policy checks](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/35400739938)
also passed. The main-only Homebrew build/publisher and release publication
correctly skipped on the PR; no public formula publication is established by
these results.

## Native processing and receivers

| Area | Executed check | What it establishes |
| --- | --- | --- |
| Current CPU/GPU application | 25/25 native CTest cases for the GPU/open-SDK/local-kit build, and 26/26 for the CPU build with all three compiled SDK adapters, including local adapter gate adversarial cases | The actual Apple-silicon application builds and its native software checks pass |
| Recording/replay | `recordingFormats` and all 18 `processor_replay_test.py` cases | Synthetic BLAH2IQ/legacy files, channel alignment, backpressure, cancellation, errors, 2–8 channels, and 6-MS/s USRP/HackRF inputs |
| Kraken ingress | `krakenSocketCapture` | Local loopback Heimdall frames, reconnects, malformed input and channel metadata; no physical or remote Kraken access |
| Native local Kraken build | Pinned Kraken RTL-SDR driver and Heimdall V2 build on Apple Silicon; explicit no-device startup | Actual native SDK/library and controller integration; no physical receiver opened |
| Kraken driver semantics | Exact-source gain oracle for 501 gain requests and all 29 supported values; all six GPIO USB transfer phases with negative and short return values | Real fork arithmetic and patched error propagation, compiled against mocks; no physical USB transfer |
| Native Kraken capture qualification | Seven cases and 14,615 simulated frames; maximum phase error 0.22302°, amplitude error 0.18776 dB, zero integer lag | Calibration, retuning, gain 20/40/50, delayed noise callbacks, FIR history, partial failures, short callbacks, Origin checks and orderly shutdown; unchanged 1°/2 dB/zero-lag bounds |
| Local Kraken adapter | Ten `kraken_adapter_test.py` checks | Private working directory, instance-bound loopback readiness, bounded regular-file reads, configuration/calibration errors, active port conflicts, TCP TIME_WAIT reuse, replay exclusion and orderly owned-child shutdown |
| Full Kraken software path | `kraken_pipeline_test.py` against installed Homebrew revision 14, five simulated channels through real native Heimdall, VectorWarp CPU processing and API | Fresh radar frames, repeated-start idempotence, retune/Save & Restart, owner-crash cleanup, native-crash supervisor recovery with a 30-second process-replacement bound, and complete supervisor shutdown; simulated RF only |
| Physical USB Kraken on this M2 | Installed Homebrew revision 14; five receivers with serials 1000–1004; 60 seconds stable at 100 MHz/28 dB, 30 seconds after API retune to 101 MHz/40 dB and Save & Restart, then 30 seconds after a full restart | Real internal-noise calibration, five-channel USB acquisition at approximately 2.4 MS/s per channel, four surveillance processing paths and fresh API updates. 17,934 usable frames; no reported coherence-loss events. Noise disabled and all five devices closed on shutdown; no raw IQ saved |
| USRP | `usrpAppliedReadback`, `usrpReceiveIngress`, real Homebrew UHD module and no-device startup | Simulated SDK setters/readback/streaming plus real linked UHD loading, bounded missing-device error and graceful shutdown |
| Dual HackRF | `hackrfAppliedSettings`, real Homebrew libhackrf module and no-device startup | Simulated paired serial selection, callbacks, settings and cleanup at 2/6/20 MS/s, plus real SDK integration |
| RSPduo | `rspduoStructuredFailures` in the earlier SDK-enabled build; actual local source-kit compilation and module loading | Official manually obtained SDK headers with simulated tuner/callback failures; real AppleClang build, loader and API integration remain separate checks |
| Real SDRplay runtime | `receiver_sdk_test.py` with a temporary foreground official API 3.15.1 service | `GetDevices` returns zero devices, the application reports a controlled error, stops gracefully and subsequently replays synthetic RSPduo IQ; the test-owned service is stopped afterward |
| Local adapter integrity | `macLocalReceiverGate` and API builder tests | Kit/core/SDK/module hashes, immutable generations, normal installed SONAME/SDK links, malformed receipts, unsafe paths and nonblocking bounded reads |

The SDK stays outside the repository and package. The application and formula
never fetch, bundle or install SDRplay. Real missing-device tests use explicit
nonexistent serials; they do not establish tuning accuracy, coherent acquisition,
USB throughput or RF behavior. The earlier CPU-only/open-SDK/precompiled-RSPduo/
combined builds passed 20/21/21/22 native tests respectively; those are historical
build profiles, not the current 25-test count.

The first physical Kraken run used a reduced CPU profile: 50 ms CPI, a dedicated
channel-0 reference, delay bins 0–16, and clutter removal, detection and tracking
disabled. Its settings changes used direct HTTP API calls. That result alone
does not qualify the normal operating workload or live browser interaction.

The subsequent normal-profile runs use 527 MHz, 2.4 MS/s, 200 ms CPI,
delay bins −10…245 (256 bins, maximum excess path 30.603813 km), ±800 Hz
Doppler, clutter lags −10…200, five surveillance maps and an array-eigenbeam
reference. Detection and tracking are enabled. These are the published
five-channel benchmark's signal-processing settings; the operational profile's
automatic worker count, one FFT thread and six-interval buffer are retained.
The actual service configuration and its running API agree on receiver and
transmitter coordinates. Those private values are retained outside the checkout
and source package. An unrelated preview server's example configuration was
explicitly rejected.

Installed Homebrew revision 15 completed a 90-second CPU-only observation:
446 distinct warm timing frames, mean 142.29 ms, p95 174.41 ms, maximum
457.95 ms; seven frames exceeded the 200 ms processing budget. All 89 status
observations remained calibrated with no reported coherence-loss events.
The corresponding installed automatic/GPU run completed 90 seconds with
445 distinct warm timing frames: mean 80.71 ms, p95 85.73 ms, maximum
271.03 ms; one frame exceeded 200 ms. Both ambiguity and clutter used
Vulkan on the Apple M2. All 89 status observations remained calibrated with
no reported coherence-loss events. Both runs closed all five devices cleanly.
These are live application measurements, not identical-IQ comparisons against
the historical Linux benchmark, and a mean below 200 ms does not imply every
deadline was met.

The latest six-minute CPU check retained the installed revision 16 processor
and used a native candidate with diagnostic logging only: 1,788 measured frames,
108.33 ms median, 155.23 ms maximum, no processing overruns or reconnects.
Timestamped events directly verified the five-minute calibration check passing
in 1.150 seconds; its deliberate noise gate accounts for the only long output
gap (1.790 seconds). All sampled maps were valid and shutdown completed.
An earlier revision 16 CPU run degraded and reconnected under reduced host
execution capacity, so this repeat is bounded acceptance, not proof that the
earlier condition cannot recur. The six-minute automatic/GPU check had
1,790 frames, 73.44 ms median, 135.96 ms maximum and no reconnects. Full results
and the retained failure evidence are in [MACOS_PROFILING.md](MACOS_PROFILING.md).

Real headless Chrome drives frequency, CPI, range and both site editors,
Save for later, reload and Save & Restart. It checks the saved and running
configuration, actual location-map markers and center, and the live 321×256
delay–Doppler map. The actual ADS-B feed is read without changing its source
service. A browser test exposed two shared map bugs: resizing during Mapbox
initialization and text symbols unsupported by the raster map style. Revision
15 defers that resize and draws escaped site labels in the existing HTML overlay.
The real-Chromium synthetic map regression passed in both macOS architecture
jobs and all ten Linux package jobs linked above. Its resize assertion checks
Plotly's computed width, since automatic sizing can leave the input width unset.

The first normal-profile automatic-mode attempt suffered a capture-buffer
overflow during calibration, then exhausted its recovery deadline and correctly
reported failure with noise off. Full restarts subsequently converged. No native
recovery change was made in response: cold-start scheduling under load and
automatic recovery from arbitrary USB sample loss remain unqualified. Successful
later runs do not erase that initial failure or establish loss-free acquisition.

The native Kraken suite also exercises the full 120-second recovery deadline:
unrecoverable simulated input reports `FAILED`, turns noise off and remains
invalid until an explicit retry. A full controller restart then produces fresh,
numerically checked five-channel data. The fake driver does not apply physical
sample-clock corrections, so this establishes bounded failure and restart,
not successful physical clock-servo recovery. A deterministic calibration-tail
marker demonstrates that the previous FIR-history behavior fails and the fixed
implementation passes. The exact-source convergence oracle likewise rejects
the upstream behavior that claimed success after exhausting failed attempts.

## GPU on the actual Apple M2

The production Vulkan worker now runs through MoltenVK. Physical qualification
passed 5/5 cases and worker/isolation tests passed 5/5 separately. Numerical
checks included prime-length transforms, direct DFT oracles, staged and direct
buffers, and a 128-frame five-surveillance-channel run. The qualification run
performed 203 GPU dispatches; the endurance segment lasted about 75.8 seconds.
Worst reported normalized RMS/peak error was 8.20e-7/8.42e-7, below the unchanged
1e-4 bounds. Additional repeated prime-transform oracle checks also passed.
The full installed application separately completed synthetic 6-MS/s, 0.2-second
CPI replay with CPU mode, automatic Vulkan selection, and explicit Vulkan
ambiguity plus clutter, with finite output and accurate stage telemetry.

VkFFT prime transforms were not reliable on this MoltenVK device until the
portable Bluestein path was implemented. Ambiguity and clutter qualify
independently; a clutter accuracy failure retains CPU clutter processing.
See [MACOS_GPU.md](MACOS_GPU.md) for implementation, evidence and limits.
These measurements do not guarantee sustained whole-application RF throughput
or qualify Intel Macs, every Apple GPU or every Vulkan driver.

## Configuration, browser, services and packaging

| Area | Executed check | Boundary |
| --- | --- | --- |
| Declared editable fields | `config-settings-matrix.test.js` covers all 84 `FIELD_RULES` fields with valid non-default persistence cases and invalid-value checks; corresponding jsdom matrix edits and reloads every field | API validation/save/load and mocked-fetch DOM payloads; these are not 84 real-browser/live-hardware checks. Antenna geometry has separate schema/API/DOM fixtures |
| Processor settings | All 11 `config_runtime_test.py` CPU cases | Actual output geometry, finite data, detector/tracker/save toggles, paths with spaces, six replay formats, CPU fallback, clutter and multichannel reference synthesis |
| ADS-B | Discovery, source and geometry suites plus `macos-adsb.test.js` | Actual temporary Homebrew/per-user aircraft files and loopback HTTP, freshness/staleness, ambiguity and path restrictions; synthetic aircraft only |
| Receiver installation | Allowlisted macOS receiver-management API tests and actual Homebrew UHD/HackRF actions | Brew integration on this Mac; no privileged Linux helper or proprietary SDK installation |
| RSPduo browser build | Builder and real API process tests | Fixed no-argument build, trusted origin/intent, concurrent configuration exclusion, receipt invalidation and live capabilities after building; synthetic compiler tests are distinct from actual SDK compilation |
| Launcher | `vectorwarp-macos.test.sh`, lifecycle server tests and actual API/native replay | Web-only launch, idempotent start, stop/restart, Save & Restart, configuration revision, process identity, locking and foreign-server rejection |
| Homebrew | Actual local source-snapshot install, CPU-to-GPU revision upgrade, `brew test`, synthetic replay under `brew services run`, stop and uninstall | Temporary local tap without a Git commit; per-user launchd service. Public tap publication, login after reboot and another physical Mac are not tested; the separate ARM CI install is recorded above |
| Latest Homebrew Kraken packages | Revision 17 app and companion installed; both formula tests pass, native patch receipt matches the source, and stable `opt` links select the upgraded companion | All six application/receiver/GPU binaries and 79 installed API/UI/launcher source files are byte-identical to revision 16. The companion adds the physically checked diagnostic logging. Upgrade validation caught and corrected links pinned to an older companion Cellar version; earlier physical limitations remain documented |
| Browser | Actual Chrome 153 and Safari 26.6 Save & Restart with native replay, plus API/DOM/deployment regression suites | Firefox launched but its UI automation timed out, so Firefox interaction remains unverified. Test browsers were closed |

The source includes opt-in native replay endurance/crash recovery checks in
`processor_lifecycle_test.py`. An initial six-minute run exposed excessive
API detection-history allocation. Detection history now skips empty storage,
expires consumed frames and materializes the five-minute view only on reads. A retained-heap test
demonstrably failed before expired payload slots were cleared, then passed on
Node 22 and 24 across two complete additional history-window turnovers. On Node
24 it retained about 0.5 MB above a full-window baseline and released the payload
on expiry; forced garbage collection is confined to that disposable test.
The application endurance check measures steady-state RSS after the full
300-second history warmup, comparing first/last 20-sample medians against the
unchanged 64-MiB growth bound; startup and peak RSS are reported separately.
The final installed-package run passed for 420 seconds with 413 fresh-frame
observations, recovery after both API and processor crashes, and process
pause/resume. It collected 119 post-warmup samples: API RSS median rose from
123.0 to 137.9 MiB, while processor RSS fell from 23.5 to 20.3 MiB; API peak
RSS was 167.2 MiB. Earlier startup-to-final checks are retained as failed
investigation evidence, not counted as passing.

## Linux and remaining qualification

The final shared CPU workspace, reference reuse and browser performance changes
passed a fresh source regression on Fedora 44 x86_64: 24/24 native tests,
18/18 synthetic replay cases, 11/11 configuration runtime cases and seven
affected API/DOM scripts. The packaging suite ran 293 tests: 288 passed and five
skipped. Four skips require an explicit isolated signed-repository runner;
one requires locally installed Linux SDRplay headers. One commit-producing
fixture was excluded to respect the local-only task. The 17 tests in the
installer module were also excluded because its setup requires unavailable
`dpkg`; this leaves its Debian and Fedora installer scenarios unverified by
this run. Initial missing Node/module-path prerequisites were corrected in
the harness, and the failed attempts remain in the local evidence.

This run used an isolated temporary directory on the existing Linux host,
synthetic replay and loopback peers, task-local dependencies and no system
package installation. Production service state and process identity remained
unchanged. Logs were retrieved and the owned temporary directory was removed.
The tested archive matches the final executable, API, UI and test sources;
only documentation changed afterward. This is a CPU source regression, not
installed-package, systemd lifecycle, Linux GPU or live-receiver qualification.

An earlier task-owned Ubuntu 24.04 ARM64 VM passed a CPU build, 24/24 native
tests, 18/18 replay cases, 11/11 configuration runtime cases and API/DOM
regressions. Its packaging suite reported 310 tests with five skips, with the
commit-producing fixture excluded. That VM was removed after preserving logs.
Those local ARM64 checks preceded the final shared optimizations. The subsequent
GitHub runs above cover the updated source on Linux ARM64 and all ten Linux
OS/architecture package combinations, including installed-service checks.

Physical capture with USRP/HackRF/RSPduo, external antenna/RF coherence,
Kraken recovery from physical drift or USB interruption, long-duration RF/USB
throughput, real Mac sleep/wake, login after reboot, long thermal endurance, and
Intel physical receivers/GPU/installed Homebrew remain unverified. Process SIGSTOP/SIGCONT checks
are process-pause simulation, not a Mac sleep/wake test. These limits prevent a
claim of 100% hardware/platform coverage even when local software checks pass.
The physical Kraken result establishes its internal calibration procedure and
the bounded acquisition/processing run above. It does not independently measure
external RF phase error, antenna geometry, cable matching or radar target accuracy.
The public runtime stores antenna layout and true-north orientation metadata but
does not implement direction-of-arrival or target bearing computation. Geometry
form tests therefore establish metadata behavior, not bearing measurement.
