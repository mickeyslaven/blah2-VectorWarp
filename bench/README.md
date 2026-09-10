# Recorded-IQ comparison

This harness measures the signal-processing pipeline from an identical recorded
input: reference preparation, spectrum, clutter filtering, delay–Doppler maps,
detection, tracking and output serialization. It does **not** measure live
acquisition, API/network transport, browser rendering or end-to-end real-time
queue behavior. Per-frame deadline counts are offline processing deadlines, not
measured dropped live frames.

`bench-upstream` compiles unchanged DSP/data sources from an explicitly selected
upstream checkout. `bench-fast` compiles this fork's sources. Both use the same
MCHQ adapter, input normalization, configuration, compiler and DSP libraries.
The adapter's packet-wise DC removal is shared input preparation, not a change
to upstream DSP. A physical reference/surveillance pair permits a matching
upstream comparison. The all-channel synthesized-reference profile has no
upstream equivalent and must be reported separately.

```sh
cmake -S bench -B build/bench -DCMAKE_BUILD_TYPE=Release \
  -DUPSTREAM_ROOT=/path/to/pinned/upstream \
  -DBLAH2_GPU=ON -DVKFFT_ROOT=/path/to/VkFFT
cmake --build build/bench
ctest --test-dir build/bench --output-on-failure
python3 -m unittest discover -s bench -p test_record_iq.py
```

The executable prints its arguments when called without them. Use unique output
prefixes. CPU reference runs write complete complex maps; matching runs compare
every map bin outside the timed processing section. Save both summaries and
per-frame CSV/JSON output. Setup, input decoding, validation and wall time are
reported separately. The fixed, sample-derived tracking clock ensures replay
speed cannot change tracker timing. GPU qualification uses CPU outputs for the
first three frames. GPU timing includes worker transfers and map conversion.

`record_iq.py` captures finite Suite V2 MCHQ input without changing tuning. It
retains the calibrated unsigned 8-bit I/Q wire stream, validates packet state and
stable metadata, and writes a SHA-256 manifest. This is raw input to blah2, not
untouched ADC samples. There is no hardware sample sequence counter in this
format: TCP order alone cannot prove no samples were lost before transmission.
`verify_recording.py` independently checks packet metadata, counts and checksum.

Copy `profile-example.json` and set its sample rate, frequency, channel count and
processing geometry to match your recording. It is an example, not a receiver
configuration. The current benchmark adapter reads MCHQ recordings; application
replay also supports the other formats documented in the setup guide.

`run_matrix.py` takes an explicit recording, profile and verified checksum. It
runs three repeats in alternating mode order, verifies identical sample counts,
and stops on a failed run. Use GPU IDs from `testAcceleration --list`:

```sh
python3 bench/run_matrix.py --root /path/to/benchmark-package \
  --recording /path/to/input.mchq --profile /path/to/profile.json \
  --sha256 VERIFIED_SHA256 --devices GPU_ID --output /path/to/new-results
```

The package directory must contain `bin/`, `lib/`, `package-sha256.json` and
`guard.py`. Omit `--devices` to skip forced-device cases. Set `--expected-frames`
to additionally check a known recording length. Input data and machine-specific
service-control scripts are not bundled. Coordinate service lifecycle with the
deployment owner; these tools never stop live receivers or other jobs.

`guard.py` applies a finite deadline and checks temperature, critical alarms and
memory/disk floors. It stops only its owned process group, never restarts a failed
test, and does not change power profiles, fans, training, CPU affinity or shared
system limits. Run the matrix in a dedicated memory-bounded systemd scope. Existing
host limits and activity still affect measurements; an uncapped scope is not
proof of an idle machine or maximum theoretical hardware performance.

`package.py` bundles identical non-driver DSP libraries for cross-host checks,
leaving each host's Vulkan loader/driver in place. `summarize.py` produces CSV/JSON
from completed runs, including per-stage statistics, thermal peaks, deadline
counts and detection/track agreement. Retain incomplete logs separately and label
partial or thermally blocked results; do not turn an aborted prefix into a
completed performance claim. Driver caches are not cleared: fresh processes are
not necessarily cold driver-cache starts.
