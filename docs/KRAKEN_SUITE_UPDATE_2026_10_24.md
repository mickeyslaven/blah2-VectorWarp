# Kraken Suite pin promotion, 24 October 2026

VectorWarp now pins KrakenSDR Suite
[`f47ed2fb48ab5dba888b7d45fd08c9745ae22199`](https://github.com/krakenrf/krakensdr_suite/commit/f47ed2fb48ab5dba888b7d45fd08c9745ae22199)
and Kraken RTL-SDR
[`578b43ddd971786e84e2a6ab3c338ee48c8739c3`](https://github.com/krakenrf/librtlsdr/commit/578b43ddd971786e84e2a6ab3c338ee48c8739c3).
The Suite revision is 32 commits after the former `3a95e19f7f81` pin. The
driver advance is required by its `rtlsdr_set_center_freq_fast` and
`rtlsdr_finish_tune` ABI.

## Reviewed source and companion port

The official codeload archives and SHA-256 values are:

- Suite: `f8a875dfff025ad83b62fa4edb362bf46aa5d580566a9976c258db56a1b6a3d6`;
- Kraken RTL-SDR: `3683e33424b668224988acc367cf9b4c458d0765a5dfb7532db640de73e20614`.

All four companion patches were manually ported. The review retained the
upstream fast-retune API and `SdrUpdateResult` behavior while preserving
VectorWarp's calibration failure latches, acquisition-publication barrier,
validated gain/readback, coherent per-channel gain command and telemetry,
MCHT timing extension, per-client continuity epochs, bounded socket ownership,
managed ports/environment and Apple portability. The port also fixed partial
startup cleanup: a required-setting failure now switches calibration noise off
before any opened tuner handle is closed.

The final rebased patch commits in the review source tree were `3ff98c4`
(macOS companion), `22da62c` (coherent channel gains), `a3955f0` (ingest
timing) and `4f0b7a0` (portable CPU). The checked-in patch SHA-256 values are
recorded in the [M2 build receipt](validation/kraken-f47-m2-20261024/build.json).

The MCHQ fixed header, channel frequency/gain metadata, channel-major unsigned
8-bit IQ and existing control/CLI contract remain compatible. VectorWarp's
opt-in MCHT-v1 extension and `set_channel_gains` remain companion additions.
The fake driver now implements and directly exercises the exact new fast-tune
ABI, including KEEP_AUTOTUNE pending state and `finish_tune` completion.
The final portability patch also requests Eigen through its unversioned CMake
package contract. Fedora 44 supplies Eigen 5, whose config intentionally rejects
a `3.3` version request even though Heimdall's used API remains compatible; the
same source continues to build against Homebrew Eigen on Apple Silicon.

## M2 software gate

A clean Apple Silicon build from the two checksum-pinned archives passed the
GPIO error, 501-request gain, convergence, watchdog negative-control, timing,
complete-frame/backpressure, eight-client cap, socket lifetime and Python
control/driver/shutdown gates. The produced arm64 Heimdall and private driver
were ad-hoc signed and passed strict signature verification.

The clean seven-scenario native fake-driver run passed no-device and partial
startup rejection, forced noise/settings/short-callback failures, in-flight
reconfiguration shutdown, WebSocket origin restrictions, MCHT parsing, retune,
common and channel-specific gain changes, ADC headroom generations, FIR-history
quarantine, simulated coherence loss/recovery and ordered shutdown. Numerical
phase residuals stayed below one degree, amplitude errors stayed within the
fixture's 0.25 dB tolerance and integer lag remained zero. The separate fresh
FIR-history/restart check also passed.

Evidence is retained in
[`docs/validation/kraken-f47-m2-20261024`](validation/kraken-f47-m2-20261024/receipt.json).
The final local, read-only candidate tar is
`/private/tmp/vectorwarp-f47-candidate-final.tar`, SHA-256
`ef93c5f8f8c7a031df335768cd29d50fcf2469e181efc3c1308c178e499206a6`.
Its Heimdall and private-driver hashes are respectively
`1ba4427de358f5ddb8e30d01aa15e218ccdc75944e2aa9a290dc4a406c042874`
and `8d8331691fecba01cece01c8f0725e4b274d4fdfd4be50f3f043bdf2c66ad26f`.
No binary is committed or distributed.

## Licensing and live hardware gate

The Suite revision still supplies no repository-wide license file; pffft's
license remains present. Kraken RTL-SDR retains `COPYING`, and the uSockets and
uWebSockets notices remain in the candidate. The source/build manifest and
CycloneDX SBOM are retained with the evidence. VectorWarp does not relicense
upstream code and does not publish a companion bottle.

The separately authorized bounded live gate passed on the five-channel Fedora
Kraken. The config hash stayed
`7a61e0abb613cef8895c0e9fbc596f49f63705f4331d26816daafcfc79a489e0`
and readback stayed 527 MHz, 2.4 MS/s, coherent five-channel operation and
`[2.7,2.7,1.4,1.4,3.7]` dB. Calibration converged with no coherence event,
recovery or ADC rail sample. A 20-second observation accepted 2,931 MCHT frames
and 48,021,504 samples per channel with strictly advancing acquisition timing.

The candidate was stopped after the pass. The original binary, systemd unit,
port drop-in and config hashes were re-attested, the original receiver/API/
processor were restarted, and a fresh 733-frame legacy-MCHQ observation proved
the restored live path at the same configuration. The candidate was not left
deployed. Full bounded evidence is in
[`docs/validation/kraken-f47-live-20261024`](validation/kraken-f47-live-20261024/README.md).

The source, M2 and physical gates are **GO** for the reviewed pin. Issue #66 can
close when this evidence commit is integrated. Deployment remains a separate
release action; this review neither authorizes nor leaves the candidate active.
