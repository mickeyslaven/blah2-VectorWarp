# Bundled Kraken validation, 2026-10-24

Base: dependency-complete PR #78, `20db8a8748fd5bf945908ae09d7a9113b476419f`.
Implementation: `44648ae` on `codex/bundled-kraken-platforms`.

| Check | Evidence |
|---|---|
| macOS arm64 production companion compiled | `macos-build.json` |
| Fedora 44 x86_64 production companion compiled | `fedora-build.json` |
| Real Heimdall, fake RTL ABI, MCHT timing enabled: 7 scenarios on each OS | `macos-simulation.json`, `fedora-simulation.json` |
| Native recording/socket/clip/replay regression suite, 7/7 each OS | `macos-recording.log`, `fedora-recording.log` |
| API timing/ownership/gain/watchdog, 22/22 | `api-tests.log` |
| Fedora installer/build regressions, 18/18 | `package-tests.log` |
| Fedora raw capture with timed/legacy framing, 4/4 | `raw-tests.log` |

Additional successful local checks: controller ownership/readiness 10/10;
bundled-package receipt/helper/owned gain application tests 4/4; simulation audit
16/16; receiver synchronization, frequency-resolved shadow, and macOS Kraken
Apply/restart loopback suites. Upstream source oracles cover GPIO error handling,
501 gain requests, convergence, calibration watchdog, TCP backpressure/tails,
socket lifecycle, control reads, driver events and orderly shutdown.

The timed socket/clip suite explicitly covers no-PPS coherent processing,
arbitrary fragmentation, changed acquisition epochs, dropped/reordered transfers,
legacy fallback, unknown versions, and partial-frame clipping. Cropped MCHT
retains the original clock-anchor sample while advancing the payload first-sample
counter. Event-time remains inexact and hardware UTC remains false.

Production binaries were compiled, not executed. Only dynamically audited fake
RTL binaries were run; the Linux audit rejects USB symbols, static RTL code,
ambient loader injection and a nonlocal fake driver. No live RF, Qwen/GPU service,
receiver deployment or installed package state was changed. Fedora work used an
isolated temporary tree; compact evidence was copied here before cleanup.

Reproduce the companion build with `python3 script/build-kraken.py --jobs 2
--test --output-dir build/companion`. Reproduce core decoding/recording checks
with `cmake -S test/recording -B build/recording`, build that directory, then run
`ctest --test-dir build/recording --output-on-failure`. Source-build dependencies
are documented in `docs/INSTALL.md`. CI adds Ubuntu x86/arm64, Fedora and Debian
12 arm64 (Pi userspace) source-build checks; those remote CI results are not
included in the local pass claims above.

## Local source-kit completion

The earlier binary-redistribution blocker is addressed by shipping only our
pinned build instructions/patches in DEB/RPM packages and stripping Heimdall,
its private driver and upstream assets. `vectorwarp build-kraken` explicitly
downloads and builds locally, under a dedicated unprivileged compiler identity.
Package installation and ordinary start never download software or start RF.

`sourcekit/fedora-systemd.log` records the real installed CLI/systemd path in a
disposable Fedora 44 container on Strix with no USB devices mounted. It compiled
the pinned sources, published a verified root-owned generation, rejected a
subsequently tampered source kit, and retained the prior generation. The final
build took 21.6 seconds wall time with a 913 MiB memory peak. Root publication
and compiler-boundary review passed. Environment/toolchain versions and receipt
hashes are preserved beside the log; no upstream binaries are committed.

Additional results: Linux installer/build contracts 19/19 (including a fresh
staged Kraken-kit install with stable helper-directory and launcher assertions);
kit/security/rollback
tests 7 passed plus one Node-dependent test skipped in the minimal Fedora
container (all 8 passed locally); launcher 33/33 locally; package quiescence/
upgrade 17/17 in Fedora. The general Linux build-contract fixture initially
lacked FFTW development libraries; installing `fftw-devel` inside the disposable
container made all 18 pass. The minimal container lacks Node, so its launcher
configuration-reader case was not counted as a Fedora pass. The new GitHub
installed-source-kit systemd job also passed on the eeda0e3 head.

Complete DEB/RPM package matrices run when this stacked PR targets main; their
results are separate from the source-kit acceptance above. Physical PPS/ADC UTC
and measured real USB/RF latency remain hardware limitations; software host
clocks do not remove them. Timing remains provisional and missing PPS does not
suppress coherent detections, provisional associations or corpus collection.
