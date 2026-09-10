# VectorWarp compared with upstream blah2

This is the wording and capability reference for the README, installation guide
and release notes. Comparison baseline: upstream
[`30hours/blah2` at `c821bee`](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de),
not a moving branch. Machine-readable pins are in [UPSTREAM_BASELINE.json](UPSTREAM_BASELINE.json).
Last acceptance review: 2026-09-10. Uncommitted development is not a published release.

## Changes and current evidence

| Area | Regular upstream at the pinned baseline | VectorWarp | Evidence/status |
| --- | --- | --- | --- |
| Receiver processing | Dedicated reference/surveillance pair; receiver-specific acquisition | Kraken Suite V2 2–8 channels, optional synthesized reference, independent surveillance paths and map fusion | Five-channel physical Kraken; 2–8-channel parser/unit/replay tests. No physical eight-channel claim. |
| Acceleration | CPU/FFTW processing | Optional isolated Vulkan/VkFFT ambiguity worker; automatic selection and CPU fallback | NVIDIA RTX 4050 Laptop, AMD Radeon 8060S and Radeon 540/550, Intel HD 630 functional matrices. The later sustained-cost safeguard has deterministic tests, not a new hardware benchmark. |
| CPU scheduling | Fixed FFT threading | Affinity/quota-aware automatic worker and FFT-team sizing; manual overrides | Unit tests. Automatic sizing is conservative, not guaranteed maximum throughput. |
| Web interface | Original radar/display pages | Redesigned navigation, readable views, fullscreen, configuration, health and recording/replay controls | API and browser-DOM tests; no claim that every browser/GPU combination has been tested. |
| Configuration | File-based setup | Validated browser editing, disk revision checks, atomic backups, startup recovery, save/restart flow | Active settings validated for all four processor receiver types. Physical tuning/driver limits still depend on the installed receiver and host. |
| ADS-B | Separate adsb2dd converter for delay/Doppler overlays | Integrated converter; local decoder discovery or a configured remote tar1090 feed | WGS84, timestamps, derivatives, stale/invalid data, warmup, cache and integration tests. Explicit remote feeds never fall back silently to local data. ADS-B never enters detection/tracking inference. |
| Recording/replay | Receiver-specific recording; incomplete replay coverage | Portable `.blah2iq` for 2–8 channels, legacy readers, common paced replay, EOF/loop/error reporting, acknowledged recording controls | 13 full-processor replay cases in release and AddressSanitizer builds; the extracted Fedora package also passes these plus three invalid-startup checks. Physical recording evidence remains five-channel Kraken. |
| Tracking/math | Original tracking/spectrum/detection implementations | Bounded histories, corrected association/kinematics, spectrum axes/levels, boundary and nonfinite-value repairs | Focused C++ tests; not proof of real-aircraft tracker accuracy or reliable bearing. |
| Deployment | Container-oriented setup | Native build/install and isolated service accounts; no container runtime required to run VectorWarp | Staged install, configuration preservation and Node 24 deployment tests. Fedora 44 x86-64 native installation and five-channel live smoke check alongside the unchanged older installation. Fedora 44 ARM64 RPM installed on Raspberry Pi 4; 16/16 offline replay/startup cases passed with services disabled/inactive. Full-power Strix run stopped at its thermal cutoff; reduced-power UI review passed. |
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

Use the [recorded-IQ report](RECORDED_IQ_BENCHMARK.md), including its frozen source
hashes. In that campaign, the RTX 4050 Laptop's matching two-channel AUTO median
was 1.305× upstream CPU throughput. Its five-channel GPU profile was slower than
fork CPU. Strix and HP results are partial thermal-stop observations, not full
replicated matrices. Those measurements predate later math/recording/selection
repairs; do not describe them as performance measurements of every current change.

The [Fedora 44 / Raspberry Pi 4 check](PI4_VALIDATION_20260910.md) recorded
917.031 ms/CPI upstream versus 902.441 ms/CPI VectorWarp for a two-channel CPU
run. The 1.6% timing difference is not an accepted speedup: detection SNR differed
in all ten frames despite matching maps and detection positions. Across three
two-core repeats, the upstream median was about 0.9% above VectorWarp's. The five-channel VectorWarp
run took 3748.025 ms/CPI with no upstream equivalent. Neither profile met its
200 ms CPI, and Pi GPU attempts fell back to CPU.

Acceptable: “Optional GPU acceleration, with measured gains for some workloads
and automatic CPU fallback.” Not supported: “Faster on every GPU,” “all processing
runs on GPU,” “all channels tested on physical hardware,” or “zero-copy raw ADC.”

## Release wording checklist

For every release:

1. Keep the comparison baseline pinned; record a separate upstream merge if it changes.
2. Review the actual diff and update this table plus the focused fix list.
3. Separate implemented code, automated tests, physical-hardware tests and planned work.
4. Describe package receiver support separately from source-build support: a
   Kraken-live package can replay other receiver recordings without containing
   their live hardware SDKs.
5. Use verified workflow artifacts and installation checks before changing package
   distribution from “unpublished” to “available.”
6. Preserve user-facing compatibility keys and 30hours attribution. Do not rewrite
   existing upstream PR branches to publish this fork.

Do not auto-generate claims from commit titles alone. CI and draft releases
produce pinned-baseline diff reports and package manifests; maintainers still review the meaning
and test evidence before public release notes are approved.
