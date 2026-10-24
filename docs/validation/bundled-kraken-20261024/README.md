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

Not accepted for release: the pinned upstream Suite has no repository-wide
redistribution terms, and the existing Homebrew contract forbids binary bottles.
Published native packages are deliberately blocked pending the same local
source-build-kit experience for package users. Full DEB/RPM install/upgrade/
rollback testing and real systemd ownership validation remain required after
that path exists. Physical PPS/ADC UTC and measured real USB/RF latency remain
hardware limitations; software host clocks do not remove them.
