# Recorded-IQ benchmark plan (historical)

The 2026-09-10 campaign has completed. See [results and limitations](RECORDED_IQ_BENCHMARK.md).
This original plan is retained for provenance, not as an outstanding job or a
request to resume thermally stopped machines.

User request: after completing GPU integration, record about two minutes of real
raw IQ, run both regular upstream blah2 and VectorWarp on every available processing
machine/GPU, and deliver all timing/resource data.

## Prerequisites

- Finish driver-hang/crash isolation and final integration checks first.
- Pin the latest upstream commit and the tested fork commit, compilers, build
  flags and libraries. Preserve all submitted PR branches and the user's ongoing
  main-branch writing changes. Fetch/merge latest main immediately before publishing.
- Coordinate live-service ownership before acquisition; do not stop training,
  retune the receiver, change machine.slice or restart research capture epochs.
  Follow the current thermal hold and resource gate. The user's request authorizes
  the later bounded recording/tests, not unrelated service changes or limit removal.
- Check capacity before recording/copying. The verified MCHQ wire format uses
  unsigned 8-bit I/Q: five channels at 2.4 MS/s require about 2.88 GB for 120
  seconds, plus headers and metadata (not the earlier float32 estimate). Retain prescribed
  disk/RAM floors and do not delete existing data to make room.
- Preserve the original recording on suitable storage; copy only the requested
  benchmark data to the tested processing hosts, never the controlling Mac or
  unrelated research datasets.

## Recording and provenance

Capture the incoming coherent IQ before reference synthesis, clutter filtering
or radar transforms. Preserve the stream's original precision and all channels,
including headers/metadata. This is raw input to blah2, not a claim that calibrated
Heimdall samples are untouched ADC bytes. Record the upstream calibration stage.

Save sample count, measured duration, UTC start/end, frequency/sample rate, channel
mapping, sample representation, receiver/DAQ configuration and versions, gains,
calibration/retuning flags, available acquisition continuity counters, bytes and
SHA-256. Reject truncated/malformed input. Disclose any continuity evidence that
the source cannot provide; do not invent sequence counters or gap-free claims.
Do not tune the benchmark after seeing which method wins.

## Fair comparisons

1. **Matching two-channel processing:** the same physical reference/surveillance
   pair, same samples, frames, CPI/hop, delay/Doppler grid and enabled processing
   stages. Compare upstream CPU, fork CPU, fork automatic and each explicit GPU.
   Preserve the original upstream DSP in the baseline; publish instrumentation
   and any input-adapter patch separately.
2. **Actual multi-channel setup:** replay the complete recording through fork CPU,
   automatic and each GPU using the same reference-synthesis/configuration path.
   Upstream has no matching multi-channel implementation; report its entry as
   unsupported, not an invented speedup. A pinned 2–8-channel pre-GPU branch can
   provide an additional matching multi-channel baseline, clearly labeled.

At the time this plan was written, Kraken replay was unimplemented in both the
inspected upstream and fork source. Implement/validate a common lossless replay/input adapter before measuring
real binaries. Keep DSP changes out of the upstream baseline. Test EOF, truncated
frames, sample alignment and identical consumed samples first. Distinguish full
application measurements from any isolated DSP microbenchmark.

## Runs and instrumentation

- Process a finite recording once per run. Unpaced throughput tests must not be
  limited by a real-time replay sleep, browser refresh or output backpressure.
  Separately run a paced real-time test to measure missed frame deadlines/backlog.
- At least three independent measured runs per comparable mode/device; alternate
  order to limit cache/thermal bias. Record cold-start/FFT-plan/GPU qualification
  time separately, plus the startup-inclusive total and steady-state result.
- Match CPU affinity, CPU quota, memory allowance and power settings within each
  pair; record all limits. The earlier .5-CPU correctness-test timings are **not**
  maximum machine speed. Do not quietly remove safety caps to manufacture a
  hardware-throughput claim. Obtain an appropriate safe benchmark budget first.
- Keep competing workloads stable, sample them, and label interference. Do not
  stop the user's other work. Check temperatures continuously and abort only the
  owned test at its thermal/memory/disk deadline. Intel Iris Plus G4 remains gated
  while critical thermal alarms persist.
- Log every frame: input sample interval, wall time, processing stages, active
  backend/device, initialization/fallback, output counts and correctness checks.
  Log timestamped process CPU time/utilization, RSS/peak memory, disk throughput,
  GPU utilization/memory and available power/temperature/throttling sensors.
  Mark unavailable telemetry explicitly.
- Compare complex maps, axes and frame counts in matching modes. Include
  detection/track agreement and numerical tolerances; faster but incorrect output
  is a failure, not a speedup. ADS-B is never an inference feature or benchmark
  input substitute.

## Deliverables

- Per-host CPU/GPU/driver/OS manifest, pinned source/build/configuration hashes,
  dataset manifest and input checksums, exact commands and baseline adapter diff.
- Per-run and per-frame CSV/JSON, stage profiles, resource/thermal traces, stdout,
  stderr, errors, fallback events and profiler artifacts where available.
- Summary by hardware and comparable mode: total seconds for the recording,
  real-time multiplier, samples/s and frames/s, mean/median/p95/p99/max frame time,
  startup cost, deadline misses, peak RAM/VRAM and whole-pipeline/stage speedups.
- Repeated-run spread and correctness results next to speedups. Explicitly list
  unsupported, unavailable, thermally blocked and resource-capped cases.
- Store the complete report and reproducible run manifest with the artifacts;
  link them from the final user summary. No performance claims until these actual
  recorded-data runs have completed.
