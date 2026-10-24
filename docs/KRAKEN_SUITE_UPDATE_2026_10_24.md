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

The rebased patch commits in the review source tree were `38dfde3` (macOS
companion), `f583ff7` (coherent channel gains), `e9154aa` (ingest timing) and
`69b9b86` (portable CPU). The checked-in patch SHA-256 values are recorded in
the [M2 build receipt](validation/kraken-f47-m2-20261024/build.json).

The MCHQ fixed header, channel frequency/gain metadata, channel-major unsigned
8-bit IQ and existing control/CLI contract remain compatible. VectorWarp's
opt-in MCHT-v1 extension and `set_channel_gains` remain companion additions.
The fake driver now implements and directly exercises the exact new fast-tune
ABI, including KEEP_AUTOTUNE pending state and `finish_tune` completion.

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
The local, read-only candidate tar is
`/private/tmp/vectorwarp-f47-candidate-immutable.tar`, SHA-256
`2492c14f12285c6ad956834efa48914db0fa35ee51c280afc3679a18d9ae6e9b`.
Its Heimdall and private-driver hashes are respectively
`8d8b43053c977c2dc0b43c56cbf00630858e803afedc8da14f8ac3ea8a67e79b`
and `8d8331691fecba01cece01c8f0725e4b274d4fdfd4be50f3f043bdf2c66ad26f`.
No binary is committed or distributed.

## Licensing and physical gate

The Suite revision still supplies no repository-wide license file; pffft's
license remains present. Kraken RTL-SDR retains `COPYING`, and the uSockets and
uWebSockets notices remain in the candidate. The source/build manifest and
CycloneDX SBOM are retained with the evidence. VectorWarp does not relicense
upstream code and does not publish a companion bottle.

No receiver, USB device, installed package, live service, RF setting or capture
was touched during this gate. The M2 software gate is **GO** for one separately
authorized bounded hardware test, not for unattended deployment. Before that
test, the live canonical configuration must still hash to
`7a61e0abb613cef8895c0e9fbc596f49f63705f4331d26816daafcfc79a489e0`
and read back 527 MHz, 2.4 MS/s, five coherent channels and gains
`[2.7,2.7,1.4,1.4,3.7]` dB. Any mismatch is a stop condition.

The hardware procedure must first snapshot the active binary, private driver,
service definition/state and canonical configuration by SHA-256; stage this
exact candidate without overwriting the rollback copy; stop the service once;
atomically select the candidate; and run a time-bounded calibration/frame and
control-status check without changing RF settings. A failed calibration,
missing/invalid frame, gain/frequency mismatch, USB error, process exit or
timeout immediately stops the candidate, atomically restores the prior binary
and driver, restarts the prior service and verifies the original config hash
and four readbacks. Success is followed by the same rollback unless a separate
deployment authorization is given.

Issue #66 should remain open through that physical gate. It can close after the
bounded live run and rollback are recorded, or earlier only if its scope is
explicitly limited to source-pin/software compatibility.
