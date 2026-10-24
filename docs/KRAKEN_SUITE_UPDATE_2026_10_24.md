# Kraken Suite observation review, 24 October 2026

This review acknowledges KrakenSDR Suite `main` commit
[`f47ed2fb48ab5dba888b7d45fd08c9745ae22199`](https://github.com/krakenrf/krakensdr_suite/commit/f47ed2fb48ab5dba888b7d45fd08c9745ae22199)
in the receiver catalog. It does **not** promote that commit into the bundled
companion. The validated source kit remains pinned to Suite `3a95e19f7f81` and
Kraken RTL-SDR `08fb08165ecf`.

## Review result

The 32 commits after `3a95e19f7f81` retain the upstream MCHQ data boundary:
the 32-byte big-endian fixed header, eight bytes of native float frequency and
gain metadata per channel, channel-major unsigned 8-bit IQ payload, magic,
ports and coherent-state bits consumed by VectorWarp are unchanged. The JSON
control command set and Heimdall command-line flags are also unchanged. The new
control code adds truthful error replies when a tuner update fails and, on
Windows only, may return a successful `superseded` reply for an overtaken drag
command. Neither change breaks VectorWarp's nominal request or status parsing.

That wire compatibility is not source-kit compatibility. The upstream delta
substantially ports Heimdall to Windows and changes socket, device-setting and
shutdown internals. It also requires Kraken RTL-SDR `578b43d` or later for the
new fast-retune entry points. VectorWarp's pinned `08fb081` driver predates
those entry points. The existing driver patch applies cleanly to `578b43d`, but
all four Suite patches require a reviewed rebase:

- `heimdall-v2-macos.patch` overlaps the new platform, socket, settings and
  shutdown work;
- `heimdall-v2-coherent-channel-gains.patch` must be reconciled with the new
  `SdrUpdateResult` and retune implementation;
- `heimdall-v2-ingest-timing.patch` must be carried forward to preserve the
  opt-in MCHT host-ingest timestamp extension used by VectorWarp; and
- `heimdall-v2-portable-cpu.patch` overlaps the changed CMake architecture
  selection.

The upstream source still emits MCHQ without VectorWarp's MCHT timing extension
and does not provide VectorWarp's `set_channel_gains` command or per-channel
coherent gain telemetry. Those are companion patches, not upstream promises,
so the new commit cannot replace the current pin until they are rebased and
the complete simulation and packaging gates pass.

## Reproducible evidence

The official commit is a descendant of `3a95e19f7f81` by 32 commits. Its
codeload archive SHA-256 is
`f8a875dfff025ad83b62fa4edb362bf46aa5d580566a9976c258db56a1b6a3d6`.
The required Kraken RTL-SDR commit is
`578b43ddd971786e84e2a6ab3c338ee48c8739c3`; its codeload archive SHA-256 is
`3683e33424b668224988acc367cf9b4c458d0765a5dfb7532db640de73e20614`.

An isolated candidate build using those exact archives stopped at the first
source gate: the Suite macOS patch had rejected hunks in CMake, status,
calibration, networking, device setup and shutdown sources. Independent
`git apply --check` runs rejected all four Suite patches and accepted the
Kraken RTL-SDR patch at `578b43d`. No binary or artifact was produced.

Running the current source-level gates directly against the unpatched candidate
confirmed why acknowledging it must not imply promotion. The gain oracle passed
against the new driver, while the calibration-convergence and watchdog gates
failed without VectorWarp's failure-latching changes. The MCHT back-pressure
gate stopped because the timing extension is absent, the socket-lifecycle gate
found the changed upstream method signature, and the focused data-writer compile
found an unreconciled macOS `TCP_KEEPIDLE` use in the new compatibility header.
These are pin-rebase blockers, not evidence of a deployed regression.

No receiver, USB device, service, installed package, RF setting or live capture
was touched. This review establishes only the observed upstream protocol shape
and the explicit reasons not to advance the bundled source pin.

## Licensing and next promotion gate

As at `3a95e19f7f81`, the Suite tree at `f47ed2fb48ab` has no repository-wide
license file; the supplied pffft license remains present. Kraken RTL-SDR
`578b43d` retains its `COPYING` file. Because the bundled pins do not change,
the source manifest, formula resources, SBOM inputs, build receipt and license
payload remain unchanged.

A future promotion must update both Suite and Kraken RTL-SDR pins and archive
hashes, rebase all companion patches, retain MCHT and coherent per-channel gain
semantics, and pass native source oracles, the fake-driver simulation, the
controller-to-processor pipeline, packaging checks and normal VectorWarp tests.
Physical-hardware acceptance remains separate.
