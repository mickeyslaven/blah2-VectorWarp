# VectorWarp compared with upstream blah2

This is the wording and capability reference for the README, installation guide
and release notes. Comparison baseline: upstream
[`30hours/blah2` at `c821bee`](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de),
not a moving branch. Machine-readable pins are in [UPSTREAM_BASELINE.json](UPSTREAM_BASELINE.json).
Last acceptance review: 2026-09-11. Source and CI artifacts are not a published release.

## Changes and current evidence

| Area | Regular upstream at the pinned baseline | VectorWarp | Evidence/status |
| --- | --- | --- | --- |
| Receiver processing | Dedicated reference/surveillance pair; receiver-specific acquisition | Kraken Suite V2 2–8 channels, optional synthesized reference, independent surveillance paths and map fusion | Five-channel physical Kraken; 2–8-channel parser/unit/replay tests. No physical eight-channel claim. |
| Acceleration | CPU/FFTW processing | Isolated Vulkan/VkFFT clutter FFT/filtering and delay–Doppler worker; startup accuracy qualification, automatic selection and independent CPU fallback | Matched upstream comparisons on RTX 4050 Laptop, Radeon 8060S, Intel HD 630 and AMD Polaris12. Earlier native live Strix cases used the version with recurring CPU checks; see the [current report](GPU_BENCHMARK_20260911.md). |
| CPU scheduling | Fixed FFT threading | Affinity/quota-aware automatic worker and FFT-team sizing; manual overrides | Unit tests. Automatic sizing is conservative, not guaranteed maximum throughput. |
| Web interface | Original radar/display pages | Redesigned navigation, readable views, fullscreen, configuration, health and recording/replay controls | API and browser-DOM tests; no claim that every browser/GPU combination has been tested. |
| Configuration | File-based setup | Validated browser editing, disk revision checks, atomic backups, receiver enrollment/readback and save/restart flow | Browser frequency and active-channel prefix synchronize with the configured Kraken Suite endpoint and are checked at startup. Enrollment is needed for privileged local service/package actions, not remote receiver readback. Kraken Suite/USB drivers and proprietary SDRplay installation remain external. |
| ADS-B | Separate adsb2dd converter for delay/Doppler overlays | Integrated converter; local decoder discovery or a configured remote tar1090 feed | WGS84, timestamps, derivatives, stale/invalid data, warmup, cache and integration tests. Explicit remote feeds never fall back silently to local data. ADS-B never enters detection/tracking inference. |
| Recording/replay | Receiver-specific recording; incomplete replay coverage | Portable `.blah2iq` for 2–8 channels, legacy readers, common paced replay, EOF/loop/error reporting, acknowledged recording controls | 13 full-processor replay cases in release and AddressSanitizer builds; the extracted Fedora package also passes these plus three invalid-startup checks. Physical recording evidence remains five-channel Kraken. |
| Tracking/math | Original tracking/spectrum/detection implementations | Bounded histories, corrected association/kinematics, spectrum axes/levels, boundary and nonfinite-value repairs | Focused C++ tests; not proof of real-aircraft tracker accuracy or reliable bearing. |
| Deployment | Container-oriented setup | Native build/install and isolated service accounts; no container runtime required to run VectorWarp | Staged install, configuration preservation and Node 24 deployment tests. Fedora 44 x86-64 native/live checks; Fedora 44 ARM64 RPM installed on Raspberry Pi 4 with 16/16 replay/startup cases and services disabled. The earlier seven-case Strix live campaign completed, peaked at 77.1°C, and restored the paused receiver without changing its configuration or CPU limits. |
| Package distribution | No VectorWarp packages | Ten native DEB/RPM targets across Ubuntu, Debian and Fedora; release automation and one signed APT/DNF repository implementation | All ten hosted native build/install-smoke jobs pass. Format/signature fixtures and Jammy/Noble/Resolute/Trixie APT indexes pass. Debian/Pi/DragonOS selector fixtures pass, including 32-bit-userland rejection. Fedora-on-Pi package evidence is recorded; Raspberry Pi OS and DragonOS physical validation remain pending. **No published repository or release yet.** |

At commit `9cfc783ca133308d28b73ef06cb66f89c9b3d367`, both architecture legs of
[CI](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/34534957702)
passed, including C++/API/replay checks. All ten native build/install-smoke jobs
passed in the [package run](https://github.com/mickeyslaven/blah2-VectorWarp/actions/runs/34534957708):
Ubuntu 22.04/24.04/26.04 and Debian 13 on x86-64 (amd64 / x86_64) and
ARM64 (arm64 / aarch64), plus Fedora 44 on both architectures. CI artifacts are
downloadable for testing; the signing/release job was skipped. These results
supersede the earlier partial run, but do not establish physical receiver or
clean-device compatibility on every target.

The initial integration base is the user's
[Kraken PR #45](https://github.com/30hours/blah2/pull/45). The submitted PR branches
remain unchanged. [UPSTREAM_FIXES.md](UPSTREAM_FIXES.md) maps the additional blah2
and adsb2dd PRs to the actual implementation and regression coverage.

## Performance wording

Direct upstream claims come only from the matched recorded-IQ replays in the
[2026-09-11 GPU benchmark report](GPU_BENCHMARK_20260911.md): same recording,
same host and CPU budget, pinned `30hours/blah2` baseline. Examples include the
RTX 4050 standard profile (230.457 ms/CPI upstream, 180.855 VectorWarp CPU,
63.638 GPU) and the Pavilion AMD wide profile (306.828, 229.883 and 88.079
ms/CPI respectively). Every current comparison uses 527 MHz, 2.4 MS/s,
delays -10 through 245 (256 bins; 30.604 km maximum excess path).
The GPU runs clutter FFT/filtering and ambiguity/
delay–Doppler work; the small FP64 coefficient solve remains CPU work.
At 200 ms CPI and ±2400 Hz, CPU-only processing uses 25.1–25.9% less time than
upstream, and GPU mode uses 63.8–71.3% less, on these hosts. These are efficiency
gains, not evidence of better detection accuracy. Separate same-campaign
before/after rows compare the combined optimization with VectorWarp `2a9bfdf`.

Native live results and wide-Doppler stress demonstrate VectorWarp processing
capacity, but are not upstream ratios because the RF/input conditions differ or
the upstream geometry is unsupported. Use the benchmark report for matched
comparisons, deadline distributions and exclusions. The refreshed replay
campaign includes all combined efficiency changes and uses startup-only CPU
accuracy qualification; the listed native live runs are earlier evidence from
the version with recurring checks, not new live
measurements of this change. Independent benchmark CPU comparisons remain
enabled, while production does not continuously revalidate later IQ against a
full CPU reference. Do not claim universal GPU
speedup, all-GPU processing, loss-free acquisition or verified high-frequency
receiver support from these measurements.

The [current Pi 4 comparison](PI4_PERFORMANCE_20260911.md) separately tests
original blah2, Off World Labs' `blah2-arm` and VectorWarp CPU on identical
settings with NEON FFTW enabled for all three. VectorWarp uses 7.5–12.6% less
processing time than original blah2 and 6.7–11.9% less than the ARM fork;
none of those 2.4 MS/s profiles meets its 200 ms budget. This is not Pi GPU
performance or a Pi 5/RSPduo deployment comparison.

## Release wording checklist

For every release:

1. Keep the comparison baseline pinned; record a separate upstream merge if it changes.
2. Review the actual diff and update this table plus the focused fix list.
3. Separate implemented code, automated tests, physical-hardware tests and planned work.
4. Keep package adapters separate from external receiver software: all four
   adapters ship together, while SDRplay's licensed API and Kraken Suite remain
   separate installations. A loadable adapter is not proof of RF reception.
5. Use verified workflow artifacts and installation checks before changing package
   distribution from “unpublished” to “available.”
6. Preserve user-facing compatibility keys and 30hours attribution. Do not rewrite
   existing upstream PR branches to publish this fork.

Do not auto-generate claims from commit titles alone. CI and draft releases
produce pinned-baseline diff reports and package manifests; maintainers still review the meaning
and test evidence before public release notes are approved.
