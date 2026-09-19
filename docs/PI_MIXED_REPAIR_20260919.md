# Pi 4 isolated mixed worker repair — 19 September 2026

This follow-up restores CPU parallelism and removes redundant input copies in
production AUTO's isolated mixed worker. It uses the same full live RSPduo
workload as the [Pi 4 guide](PI4_GUIDE.md): two 2 MS/s channels, 500 ms CPIs,
410 clutter taps, a 301×411 map, ±300 Hz Doppler, detection and tracking on.
Changes are on `codex/pi-arm-improvements`.

The earlier [40-minute AUTO soak](PI_AUTO_SOAK_20260919.md) passed while
selecting CPU. That run used an older worker. It does not qualify endurance
for the repaired mixed path, and no replacement long soak is part of this
short comparison.

## Final short live comparison

**PASS for this short comparison.** Five successive 30-second runs used real
SDRplay capture with detection and tracking enabled, in CPU / AUTO / AUTO /
CPU / preserved-prototype order. Each observed 62 CPIs. Both AUTO runs selected
`vulkan+cpu` and retained it through the last frame; neither fell back to CPU.
All five runs stopped cleanly with zero capture/paired-queue drops, no reported
capture faults, and zero final complete-CPI backlog.

Times below are milliseconds. CPU/prototype rows exclude their first two
warm-up CPIs. AUTO rows start strictly after the frame that completes
qualification, excluding its final mixed trial as well as the accuracy checks.
Percentiles use linear interpolation.

| Run | CPIs in population | Mean | p95 | p99 | Maximum |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU 1 | 60 | 383.306 | 394.089 | 397.393 | 399.70 |
| AUTO mixed 1 | 51 | 341.508 | 358.235 | 370.465 | 380.83 |
| AUTO mixed 2 | 51 | 337.463 | 353.120 | 357.660 | 359.43 |
| CPU 2 | 60 | 378.377 | 400.449 | 409.066 | 416.96 |
| Preserved in-process mixed prototype | 60 | 334.582 | 350.536 | 352.525 | 353.31 |

The pooled means are **380.842 ms CPU versus 339.486 ms AUTO mixed**, a
**41.356 ms / 10.86% reduction**. Both mixed means meet the 350 ms target;
individual CPIs can exceed 350 ms. The repaired worker is within about 5 ms
of the preserved prototype's mean while retaining process failure isolation.
The prototype's older API labels its path `cpu`; its correlation, FIR and
75-row Vulkan traces confirm that it actually uses the experimental mixed
implementation. It is not the CPU-only control.

AUTO's startup CPU/mixed trial medians were 372.14/347.63 ms and
386.12/329.44 ms. Each run published 54 mixed frames and eight CPU frames,
including three mixed trial frames and the CPU outputs from two accuracy
shadow comparisons. Its 51 frames after qualification all used mixed.

Startup is retained in the evidence: all-observed AUTO means are 358.731 and
353.711 ms, with maxima 849.64 and 815.21 ms. Each has two ≥500 ms frames,
both during startup checking. No frame after qualification reached 500 ms.
These startup comparisons intentionally execute both pipelines on one input;
the unchanged capture guards kept them lossless in these runs.

Parent decode plus packed-input transfer averaged 16.889 and 17.296 ms,
versus 26.4 ms for the rejected FP64 mirroring attempt. The worker's startup
trace confirms four planner threads and two actual clutter CPU slots in both
runs. GPU work shares remain unchanged. A post-run hardware check reported
`throttled=0x0` and 37.4°C; this is a post-run snapshot, not continuous hardware
telemetry for these short tests.

**No endurance claim:** this MXP2 mixed build has not completed a new
40-minute soak. The earlier CPU-selected soak remains a separate result.

## Cause and changes

The isolated worker requested two clutter CPU slots but never set FFTW's
planner thread count before constructing Wiener. Wiener caps its helper count
against that setting. The child therefore received only one CPU slot; the
experimental in-process prototype had set the planner count to four and
received two slots. Startup tracing confirmed the difference. This affected
both CPU correlation blocks and the CPU FIR suffix, despite identical GPU
shares. Transfer overhead alone was an incomplete explanation for the
regression.

The child now initializes FFTW and sets four planner threads before Wiener
construction, then requires exactly two actual clutter CPU slots: the caller
and one persistent helper. It fails startup if that invariant is not met.
This does not mean every FFT runs on four threads; individual block plans
retain their existing thread settings.

Two copy changes accompany that correction:

- The child reads the bounded shared input directly into its persistent
  working buffers, removing two intermediate million-sample deques.
- On direct paired live capture, selected mixed/shadow frames transfer the
  original 8 MB of signed16 IIQQ samples instead of 32 MB of expanded FP64
  samples. The child decodes directly into its existing rotated-reference
  and surveillance workspaces. The parent still decodes independent original
  samples for spectrum processing and recovery. CPU frames retain their
  original decoding path. Prepared shared input is consumed once.

The parent retains independent original samples throughout worker execution.
Crashes, hangs, corrupt messages, nonfinite results and failed map commits
still fall back to CPU. The allocation and output offset are unchanged. The
MXP2 protocol adds explicit packed-frame operations for each reference-channel order; a version mismatch
is rejected rather than interpreting the wrong input format.
AUTO still requires two full-map accuracy comparisons and a complete-CPI
median more than 5% faster than CPU; its continuing speed and capture guards
remain enabled. The paired choice moves before decoding, while the CPI timer
still starts before that choice. Decode, copying, IPC, detection, tracking,
and output costs remain included in the live CPI measurement.

This is not zero copy. Shared input writes, the child's necessary working
buffers, and validated output publication remain. GPU shares also remain
68 of 272 correlation blocks, 499895 of one million FIR samples, and 75 of
301 ambiguity rows, concurrent with the corresponding CPU portions.

## Evidence before the final copy integration

Removing only the child deques saved 8.688 ms in a matched recorded comparison,
but still lost to CPU. Restoring the second CPU slot then produced 333.750 ms
mean over the final 32 of 40 recorded mixed frames. AUTO selected mixed on
recorded input, with CPU/mixed trial medians of 355.501/335.722 ms.

Live results still exposed the remaining parent copy: one AUTO run averaged
347.933 ms across its 45 ready mixed frames before the speed guard switched
back to CPU; the second selected CPU during qualification. Adjacent CPU means
were 371.337 and 375.568 ms, while the preserved in-process prototype averaged
331.606 ms. The separate parent copy averaged 13.990 ms. All five 30-second
runs passed capture checks with zero drops, faults, or final complete-CPI
backlog. These intermediate results motivated the subsequent input experiments;
they are not presented as successful sustained mixed selection.

An intermediate attempt mirrored FP64 values into shared memory during parent
decoding. It passed correctness but still averaged 350–353 ms live, and one
repeat fell back to CPU. Decode plus mirroring cost 26.4 ms, largely replacing
the earlier decode-plus-copy cost rather than removing it. That path was
rejected in favor of transferring the smaller packed signed16 input.

## Correctness and failure checks

The native build includes matching application, mixed worker, receiver module,
and capture core. The rejected mirror-only IqData API was removed; the existing
CPU decoder remains unchanged.

Four focused native CTest groups passed in 2.08 seconds: AUTO policy,
full-map validation, worker faults, and IQ FIFO ownership. Prepared-input
fixtures check both reference-channel orders, signed IQ values, missing/stale/reused prepared input, and every existing
worker fault.
The faulty child deliberately corrupts shared input before replying or
failing; the parent's authoritative inputs and uncommitted outputs remain
unchanged. Tests retain assertions in the optimized build.

The recorded oracle checks every complex-map bin of all 40 frames, with RMS
and peak relative error each bounded by `1e-4`, and compares detector, tracker,
and IQ JSON against the frozen CPU output byte for byte. The benchmark's
`VECTORWARP_BENCH_PAIRED_INPUT=1` fixture exercises the actual packed prepared-input
entry point with losslessly reconstructed signed16 samples. It is restricted
to forced mixed mode, rejects nonintegral/out-of-range input, and fails on any
CPU fallback. Its synthetic replay timing is not a live performance claim.

The final recorded CPU, normal mixed, and prepared-input mixed arms all passed
40/40 map comparisons and byte-exact downstream JSON. Mixed RMS relative error
was `3.9344904e-6` and peak relative error `4.7094878e-5`. Both mixed arms
published 40 worker results and zero CPU fallbacks. The prepared-input fixture
verified that every frame used packed input. Unsupported forced-mixed geometry
was rejected before any result file was created. Swapping reference and
surveillance channels also passed an independent eight-frame CPU golden-map
comparison and exact downstream JSON (RMS `2.9855421e-6`, peak `3.5168291e-5`).

## Reproduction

Raw evidence remains on the Pi in
`/var/tmp/vectorwarp-mixed-shared-20260919/`. The earlier worker and binary
cohorts remain in `two-slot-unfused/` and `fused-fp64/`; the original fast
prototype remains at
`/var/tmp/vectorwarp-known-good-337/`. The previous soak evidence is unchanged.
Builds run natively with `-j1`; no image or heavy build is performed on the Mac.

The final tested application SHA-256 is
`d5d98652b5adb97aeddd53da54a3c37bca3846dfc09131fa3f9b1ad4c0a1b007`;
the mixed worker is
`7b1fe22764a2438fd5d754aa1936d3344f663527c71b6a01bdc99874dde63097`.
The RSPduo module remains
`d9b1d1ab993f361dae6d99adbfedb46cdbbb0ea4f8a698244dd363780c05b2fc`
and capture core remains
`ec636987608bd0953e880dbb10a16335175c86bcdafadf7fd84f3d9e3be835ea`.
All four current binaries were checked against `packed-binaries.sha256`;
19 critical source files match the local branch byte for byte in
`packed-source-manifest.json`. The final live summary is
`results/packed-live-summary.json`; raw evidence/config/timing files use
`results/packed-live-*`. Recorded gates use `results/packed-{cpu,mixed,paired}*`
and `results/packed-reverse-*`. The final native fault-test report is
`packed-ctest.txt`.

The paired recorded gate uses the normal benchmark interface:

```sh
VECTORWARP_BENCH_PAIRED_INPUT=1 bench-fast recording.raw profile.json result \
  pair mixed 40 compare cpu.maps
cmp result.outputs.jsonl cpu.outputs.jsonl
```

It is a verification fixture: input reconstruction is outside the processing
timer. Use live harness results for performance claims. Short live checks use
`bench/live-rspduo-check.py --binary /path/to/blah2 --seconds 30 --output PREFIX
--acceleration cpu` or `--acceleration auto`, with the sibling worker and
matching receiver/core libraries in place. The trial environment is:

```sh
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
export VECTORWARP_FFTW_PLAN=measure VECTORWARP_CLUTTER_WORKERS=2
export VECTORWARP_RSPDUO_CPI_QUEUE=1 VECTORWARP_RSPDUO_USB_MODE=bulk
export VECTORWARP_RSPDUO_COUNTER_SCALE=3
```

Keep other builds/profilers off the Pi during timing. This comparison includes
opt-in component timing logs but no sampling profiler or FFTW preload tracer.

The opt-in `VECTORWARP_MIXED_PROCESS_LOG` reports packed `decode_and_pack_ms`
separately from the later IPC `total_ms`; the former is already included in
live `extract_buffer`. Do not treat the absence of a separate `input_copy_ms`
as free decoding, or compare these component logs without accounting for
that boundary. `VECTORWARP_MIXED_WORKER_LOG` records the actual startup thread
settings and per-frame child stages. These are timing traces, not GPU
occupancy measurements.
