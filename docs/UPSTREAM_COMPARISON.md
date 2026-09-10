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
| ADS-B | Separate adsb2dd converter for delay/Doppler overlays | Converter integrated into the API; raw aircraft feed is the only external ADS-B data source | WGS84, timestamps, derivatives, stale/invalid data, warmup, cache and integration tests. ADS-B never enters detection/tracking inference. |
| Recording/replay | Receiver-specific recording; incomplete replay coverage | Portable `.blah2iq` for 2–8 channels, legacy readers, common paced replay, EOF/loop/error reporting, acknowledged recording controls | 13 full-processor replay cases in release and AddressSanitizer builds; the extracted Fedora package also passes these plus three invalid-startup checks. Physical recording evidence remains five-channel Kraken. |
| Tracking/math | Original tracking/spectrum/detection implementations | Bounded histories, corrected association/kinematics, spectrum axes/levels, boundary and nonfinite-value repairs | Focused C++ tests; not proof of real-aircraft tracker accuracy or reliable bearing. |
| Deployment | Container-oriented setup | Native build/install and isolated service accounts; no container runtime required to run VectorWarp | Staged install, configuration preservation and Node 24 deployment tests. No installation over the existing radar. |
| Package distribution | No VectorWarp packages | Native DEB/RPM packaging, release automation and one signed APT/DNF repository implementation | Fresh Fedora 44 x86-64 RPM built, inspected and replay-tested after extraction. Real format/signature fixtures and APT indexes pass. Actual DEB/ARM64 builds and clean-host installs remain unverified. **No published repository or verified hosted release yet.** |

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
