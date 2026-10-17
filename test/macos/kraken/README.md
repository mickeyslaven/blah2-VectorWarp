# Native Heimdall simulation

`native_simulation_test.py` runs the actual compiled Heimdall application with
`fake_rtlsdr.cpp`, an RTL-SDR ABI double that has no USB dependency. The fixture
on macOS copies the executable into a fresh evidence directory, changes its single
RTL-SDR dynamic-library load command to the fake library, ad-hoc signs the copy,
and verifies that no real RTL-SDR load command or direct libusb call remains.
The production executable is not changed. Each case has its own working
directory, settings, log, and simulated-device event log.

```sh
python3 test/macos/kraken/native_simulation_test.py \
  --heimdall build/kraken-macos/heimdall-build/heimdall \
  --include build/kraken-macos/prefix/include \
  --output build/kraken-macos/simulation-final
```

The output directory must not already exist. Four unused loopback ports default
to 29191–29194; the command-line options can select alternatives. No receiver
should be attached for this software test. `--prepare-only` performs compilation,
load-command replacement, and isolation checks without starting Heimdall.

On Linux, the source builder first compiles the pinned controller with its real
static RTL-SDR driver without running it. It then relinks the same native objects
with the fake driver. The production build is retained without execute permission.
The simulator verifies source and artifact hashes, the link map, ELF symbols and
the resolved dependency graph before every launch. This is necessary because
preloading a library cannot reliably replace an already linked static driver.

```sh
python3 test/kraken/build_linux_simulator.py \
  --output-dir build/kraken-linux-prepared --jobs 2
python3 test/macos/kraken/native_simulation_test.py \
  --heimdall build/kraken-linux-prepared/heimdall-simulated \
  --manifest build/kraken-linux-prepared/binary-manifest.json \
  --output build/kraken-linux-simulation
```

Linux requires a C++ compiler, CMake, make, pkg-config, binutils, patch, Python,
and development headers/libraries for Eigen, libusb, zlib and OpenSSL. The
current pinned Suite vendors pffft for Heimdall's correlation FFTs.
The builder does not install system packages or execute the production receiver.
It rejects ambient loader injection variables. Its dependency downloads are
immutable archives checked against `config/kraken-macos-sources.json`.

The deterministic five-channel input has known phase and gain offsets. It runs
through the real USB callback, queues, FFT correlation, lag state machine, phase
calibration, compensation, and MCHQ output path. Tests cover:

- Zero devices and an injected required sample-rate setup failure, with cleanup
  that explicitly disables noise before closing partially initialized devices.
- Initial calibration, frequency and gain changes, and fresh numerical checks.
- The phase-only convergence limit: residual phase at most 1 degree and a zero
  integer correlation lag. The fake's known per-channel gain offsets must be
  preserved after calibration; amplitude normalization is a regression.
- A twofold difference between simulated calibration-noise and live-signal
  envelopes, detecting old calibration payloads incorrectly labeled usable.
  Thirty-two pending noise-on completions arrive after a 450 ms delay, so the
  native 350 ms settling timer cannot pass this check without a completion gate.
- Real fork gain quantization for 20, 40, and 50 dB requests, fixed IF-VGA
  selection at startup and across manual/automatic gain transitions, zero-PPM
  setter no-op semantics, and rejection of a partial runtime frequency change.
- Consistent control calibration/noise/recovery status and MCHQ validity.
- Rejection of foreign or missing WebSocket origins, with both approved local
  origins accepted through a real socket handshake.
- A short USB callback and a missing channel's callbacks, visible loss, and
  rejection of false lock. After callbacks resume, the source must recover
  numerically verified coherence or report bounded failure and recover through
  an explicit full controller restart; indefinite PENDING cannot pass.
- SIGTERM with a deliberately delayed 600 ms async-reader exit; all readers must
  exit before device close, all simulated noise sources must be off, and no
  control write may occur after close.
- SIGTERM during an actual five-to-four element reconfiguration, while its
  detached worker is joining the delayed readers.

This is software qualification only. The fake sample clock is perfect and logs
sample-clock correction writes without actuating them. It cannot qualify the
hardware drift servo, USB throughput, actual RF switching, RF phase coherence,
or physical loss recovery. The loss case qualifies the native queue and
calibration state transitions against a resumed, perfectly shared sample clock.
Physical drift and loss recovery remain part of subsequent testing with the
user's local KrakenSDR.

Evidence includes `binary-manifest.json` with exact binary hashes and load
commands, `result.json`, per-case `heimdall.log`, and `driver.jsonl`. Failed runs
retain their evidence and still stop only their own simulated child process.

`gain_oracle_test.py --source <local Kraken librtlsdr source>` independently
compiles the pinned driver's actual pure gain arithmetic and compares all 501
integer-tenths requests from 0 through 50 dB, plus all 29 advertised gain values,
against the fake ABI. It loads no real RTL-SDR library and opens no USB device.

`convergence_oracle_test.py --source <Heimdall_v2 source>` compiles the actual
phase-only convergence-decision body with controlled measurement inputs and
doubles for its hardware side effects. It requires the caller's
`phases_measured` gate, verifies that 1.01-degree residual phase exhausts the
retry budget without declaring convergence, latches failure and disables noise,
and proves zero-phase and inclusive 1-degree observations require all four
successful readings. The test accepts `CXX` for a portable host compiler.


`api/receiver-sync-auto.test.js` exercises the VectorWarp control client with
loopback protocol fixtures. Together with the existing protocol tests, it covers AUTO acknowledgment followed by exactly
one supported full recalibration, strict acknowledgment/recovery ordering,
rejected/stale/failed responses, stock Linux status without optional calibration
fields, unsupported modes/scanning, AUTO retuning, and unchanged/manual settings.
It runs through the normal API test command on Linux and Mac CI. These protocol
fixtures do not measure RF or replace the native simulated-driver tests above.

On the original native build, a separate M2 USB test passed three manual-to-AUTO
changes followed by the existing full-recalibration command and one periodic
check. The user reported the new antenna array connected. That result supports
the command sequence. A subsequent bounded physical test of the actual Node
synchronizer passed manual-to-AUTO gain and AUTO retuning with gain Keep,
independently observed fresh recovery and five-channel output, and complete
shutdown. A separate Fedora 44 x86_64 USB test of pristine upstream Suite and
the same client passed AUTO, Keep-gain retuning, unchanged-setting no-op, the
original periodic check and manual restoration, with all five kernel-driver
bindings restored. The Linux native/control evidence passed independent review;
the current HP CPU build and dependency audit also passed independent review.
A subsequent live HP CPU/API test passed initial acquisition, two Save & Restart
settings changes, stop/start and restart, with fresh finite maps and complete
cleanup and independent acceptance. This used one surveillance worker and one
FFT thread; dropped packets and CPI overruns were observed. It establishes bounded
functional behavior, not lossless or real-time throughput, full multi-worker/GPU
performance, external RF coherence, long-term drift or physical Intel Mac behavior. No native calibration timing patch was
adopted. The exact current hosted and physical scope is recorded in
[the October update note](../../../docs/KRAKEN_SUITE_UPDATE_2026_10.md).
