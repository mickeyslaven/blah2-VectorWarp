# Native Heimdall simulation

`native_simulation_test.py` runs the actual compiled Heimdall application with
`fake_rtlsdr.cpp`, an RTL-SDR ABI double that has no USB dependency. The fixture
copies the executable into a fresh evidence directory, changes its single
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

The deterministic five-channel input has known phase and gain offsets. It runs
through the real USB callback, queues, FFT correlation, lag state machine, phase
calibration, compensation, and MCHQ output path. Tests cover:

- Zero devices and an injected required sample-rate setup failure, with cleanup
  that explicitly disables noise before closing partially initialized devices.
- Initial calibration, frequency and gain changes, and fresh numerical checks.
- The native convergence limits: residual phase at most 1 degree, amplitude at
  most 2 dB, and a zero integer correlation lag.
- A twofold difference between simulated calibration-noise and live-signal
  envelopes, detecting old calibration payloads incorrectly labeled usable.
  Thirty-two pending noise-on completions arrive after a 450 ms delay, so the
  native 350 ms settling timer cannot pass this check without a completion gate.
- Real fork gain quantization for 20, 40, and 50 dB requests, zero-PPM setter
  no-op semantics, and rejection of a partial runtime frequency change.
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
convergence-decision body with controlled measurement inputs and doubles for
its hardware side effects. Residual phase of 1.01 degrees and amplitude of
2.01 dB must exhaust the retry budget without declaring convergence, latch a
failure, and disable noise. Valid measurements at the 1-degree phase boundary
and 1.9 dB amplitude require all four successful readings. The original upstream
forced-convergence fallback fails this regression.
