# Kraken Suite compatibility review, 17 October 2026

This review pins KrakenSDR Suite `main` at immutable commit
[`3a95e19f7f81b078e60a4f6eb86e738418756f5e`](https://github.com/krakenrf/krakensdr_suite/commit/3a95e19f7f81b078e60a4f6eb86e738418756f5e).
It replaces VectorWarp's previously reviewed `f648514df41d` pin and acknowledges
83 intervening upstream commits.

## Relevant upstream change

Most of the upstream delta is confined to Kraken's separate DoA, decoder and
web applications. Only three commits touch `heimdall_v2` or the top-level
installer. Two add optional voice-to-text installer support and ignore rules.
The functional Heimdall change, `c76cb5e`, replaces FFTW with the vendored pffft
implementation used by correlation and calibration. It does not change the MCHQ
sample frame, control commands, status fields, receiver setup, buffering or
network protocol consumed by VectorWarp.

VectorWarp's native companion patch is rebased onto the new source. The managed
macOS build retains its private Kraken RTL-SDR dylib and local-only listener
policy, builds the upstream pffft sources with the architecture-selected SIMD
path, and no longer requires Homebrew FFTW for the companion. The pffft license
is copied into every staged companion artifact because pffft is statically
linked. VectorWarp itself still uses FFTW independently.

## Software evidence

On Apple Silicon with Apple clang 21, the pinned archive checksum was verified,
the rebased patch applied, and the native companion built without opening a
receiver. Source-level tests passed for gain quantization, convergence and
failure latching, watchdog recovery, complete TCP framing, bounded slow-client
handling, the eight-client limit, socket ownership and shutdown.

The executable-level fake-driver suite passed all seven scenarios: no devices,
required setup failure, calibration-noise failure, settings failure, short USB
callback, shutdown during reconfiguration and five-channel coherence. The
coherent scenario processed 2,961 frames and passed initial calibration,
retuning, manual gain changes at 20/40/50 dB and simulated stream-loss recovery.
Worst reported phase residual was 0.227 degrees, gain-offset error remained below
0.013 dB, and every checked integer lag was zero.

The current VectorWarp CPU build passed all 38 native tests. The complete
simulated controller-to-processor-to-API pipeline then passed five-channel
capture, Save & Restart retuning, idempotent start, parent-death cleanup,
supervisor recovery and clean stop. The API and browser suites, receiver catalog
tests and configuration round trips also passed.

## Limits

These checks establish source, protocol, numerical simulation and local process
compatibility. They do not establish physical USB throughput, external-RF
coherence, sample-clock drift, power stability, antenna geometry, target or
bearing accuracy, long-term operation, or physical Intel Mac behavior. The
artifact receipt therefore continues to record `hardwareTested: false`.

The Suite still has no repository-wide license file. The companion remains a
local source build with no published bottle; the newly linked pffft component's
upstream license is preserved separately in the artifact.
