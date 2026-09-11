# Recorded-IQ comparison

This harness measures the signal-processing pipeline from an identical recorded
input: reference preparation, spectrum, clutter filtering, delay–Doppler maps,
detection, tracking and output serialization. It does **not** measure live
acquisition, API/network transport, browser rendering or end-to-end real-time
queue behavior. Per-frame deadline counts are offline processing deadlines, not
measured dropped live frames.

Set `BLAH2_BENCH_PACE=1` for sample-clock-paced replay. Each complete CPI is
released at its scheduled sample time; an overloaded processor falls behind
instead of silently dropping input. The summary records pacing and final lag
past the next CPI deadline (including read/validation overhead). DSP timing
still excludes pacing waits. This is lossless scheduled replay, not a hardware
capture/drop simulation or proof of sustained live acquisition.

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
first three delay–Doppler frames plus five composed-stage frames when GPU
clutter is enabled. Qualification is startup-only; accepted steady GPU frames
do not run recurring CPU accuracy oracles. The benchmark's independent CPU
comparisons remain enabled. GPU timing includes worker transfers and map conversion.

`record_iq.py` captures finite Suite V2 MCHQ input without changing tuning. It
retains the calibrated unsigned 8-bit I/Q wire stream, validates packet state and
stable metadata, and writes a SHA-256 manifest. This is raw input to blah2, not
untouched ADC samples. There is no hardware sample sequence counter in this
format: TCP order alone cannot prove no samples were lost before transmission.
`verify_recording.py` independently checks packet metadata, counts and checksum.

Copy `profile-example.json` and set its sample rate, frequency, channel count and
processing geometry to match your recording. It is an example, not a receiver
configuration. New campaign profiles must also set `benchmark_workers`,
`benchmark_fft_threads` and `round_hamming`. The same profile bytes must be used
for the upstream and VectorWarp physical-pair cases; pair profiles require one
worker, while the FFT thread count remains an explicit matched control. Inspect
every generated case before opening IQ data:

```sh
bin/bench-upstream --inspect-geometry profile.json pair
bin/bench-fast --inspect-geometry profile.json pair
```

The bounded JSON reports requested/effective CPI, samples, delay/Doppler bins,
correlation/range FFT sizes and thread controls. It also reports whether the
unchanged upstream Doppler scratch layout can safely represent the geometry.
An unsafe upstream case is reported as unsupported and a normal upstream run
rejects it before constructing the DSP; the harness never narrows or otherwise
silently repairs the requested geometry. VectorWarp may still run that exact
case because its wider Doppler scratch regression is fixed. Upstream remains a
physical reference/surveillance pair CPU comparison only; array scaling is
VectorWarp-only.

The current benchmark adapter reads MCHQ recordings; application
replay also supports the other formats documented in the setup guide.

`pipeline_ms` and its identical `dsp_ms` alias measure extract through JSON
serialization. IQ reading, construction/initialization and complex-map
validation are separate `read_ms`, `startup_ms`/`initialization_ms`, and
`validation_ms` values. Per-frame rows label cold/warmup, GPU qualification and
steady phases. Summary JSON records first-frame time, whole-run and steady
mean/p95/p99/max, deadline misses, requested/effective CPI, FFT geometry and
CPU/GPU frame counts. A forced `gpu` run fails unless it completes at least one
post-qualification frame on Vulkan and every such frame stays on Vulkan. AUTO
fallback remains valid evidence but cannot be presented as GPU performance.

VectorWarp's current processing path applies noncoherent magnitude fusion even
to one surveillance map; the fork pair harness does the same and times map
metrics/fusion separately. Correctness still compares each pre-fusion complex
channel map against the upstream CPU golden map. A separate validation checks
the post-fusion magnitude map. Detection delay, Doppler and SNR fields and track
outputs remain in JSON for separate comparison; a known SNR-field difference
must not be mislabeled as a complex-map mismatch or sensitivity gain.

Reduced-load Pi profiles must retain the standard excess-path window and the
recording's real 2.4-MS/s sample rate. A different sample rate requires genuinely
filtered/decimated samples for both binaries, never just a changed header.
Narrower Doppler or a different clutter window must be identical between
engines and disclosed beside the results, with the standard configuration kept
as a control. Pi results compare upstream and VectorWarp on that same Pi,
never against another host.

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
