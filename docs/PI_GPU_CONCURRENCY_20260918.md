# Pi CPU/GPU concurrency experiments — 2026-09-18

> **Maintained status:** This is a historical experiment ledger. For current
> installation, AUTO behavior, and limits, read the [Pi 4 guide](PI4_GUIDE.md)
> and the [production AUTO soak result](PI_AUTO_SOAK_20260919.md). The prior
> 40-minute soak passed while AUTO chose CPU. The repaired mixed worker subsequently passed
> short live AUTO comparisons at 341.508/337.463 ms mean; see the
> [repair report](PI_MIXED_REPAIR_20260919.md). It has not had a new long soak.

The useful result so far is to start the GPU reference FFT during CPU correlation and solving, then read back/subtract the GPU prefix on a persistent completion thread while the CPU filters the suffix. On the same recorded-IQ benchmark binary, the repeated 50%/2048 mixed run averaged **389.029 ms per steady CPI**, versus **405.937 ms CPU-only**, a **16.908 ms / 4.17% reduction**. Subsequent live tests confirmed the benefit; the combined winner below averages392.659ms. This remains an experimental benchmark integration, not a completed production GPU-worker integration.

## Fixed workload and acceptance

Pi4B8GB, fresh Raspberry Pi OS Lite64 Bookworm, stock 6.12.109 kernel, CPU1.8GHz performance governor, stock V3D around500MHz, no overclock. RSPduo geometry: two2MS/s channels,551MHz,500ms/1M-sample CPI,410 clutter taps,411 delay bins,301 Doppler bins. Detection and tracking are enabled. The recorded input is the preserved20-second live RSPduo IQ recording. Each trial processes40CPIs; summary times exclude the first8, but the complex-map gate checks every bin of all40frames against the frozen CPU maps.

All reported concurrency variants passed RMS and peak relative complex-map errors below1e-4. The winning variant had peak4.30e-5; detection and tracking JSONL matched CPU byte-for-byte, including6321 track records. Actual V3D work was logged on40/40frames at49.9895% of final FIR samples. Correlation, dense FP64 solve, ambiguity, detection and tracking remain CPU work.

## Recorded measurements

| Method | Steady mean CPI (ms) | Interpretation |
| --- | ---: | --- |
| CPU-only, fresh same-binary control |405.937| Matched reference for repeated winner |
| 50%GPU FIR, late submission and serial completion |402.818| Earlier mixed implementation |
| 50%GPU FIR, asynchronous completion only |400.960| Small improvement |
| 50%GPU FIR, early reference only |392.325| Most of the initial gain |
| 50%GPU FIR, early reference plus asynchronous completion |384.837| First run; p95389.101,max391.165 |
| Same combined method, repeated |389.029| Repeat remains4.17% below matched CPU |
| Combined method,25%/FFT2048 |394.292| Slower than50%/2048 |
| Combined method,25%/FFT1024 |395.383| Slower than50%/2048 |
| Combined method,50%/FFT1024 |393.562| Slower than50%/2048 |

The first combined run had correlation+solve114.17ms and clutter171.73ms, versus clutter187.28ms for the late/serial control. GPU reference submission precedes CPU correlation; GPU final filtering follows solved weights; the completion thread and CPU filter write disjoint output ranges and join before publication. Submit-to-fence logs include concurrent CPU work and must not be interpreted as pure GPU kernel timings.

An earlier25%/FFT1024 method without these overlap changes looked faster on recorded data but did **not** win its adjacent live test: steady407.496ms mixed versus406.248ms CPU. The earlier50% mixed live gain also came from a single pair. Subsequent repeated live CPU/mixed/mixed/CPU checks confirmed the new method, as described below.

## Reproduction and remaining work

Experiment source/apply instructions: `build/pi-os/mixed-concurrent-benchmark/README.md` in the working checkout. It uses `VECTORWARP_GPU_FIR_PERCENT=50`, `VECTORWARP_GPU_FIR_FFT=2048`, `VECTORWARP_GPU_FIR_EARLY_REFERENCE=1`, and `VECTORWARP_GPU_FIR_ASYNC_FINISH=1`, plus the qualified two-CPU-worker/FFTW-measure runtime settings. These are benchmark-overlay controls, not documented production configuration. Pi evidence is under `/var/tmp/vectorwarp-concurrent/`, initially `matrix50/e{0,1}a{0,1}.*`. The current detailed ledger is `build/pi-os/GPU_SWEEP_STATUS.md`.

Repeated short live checks are complete. The next gate is an isolated-worker integration with bounded CPU fallback and truthful mixed-FIR telemetry, followed by revalidation of that implementation. The prototype runs Vulkan inside the benchmark process, so production isolation remains unfinished. Separate prepared diagnostics test GPU ambiguity with a4096-point FFT and partial blocked GPU correlation; neither has a performance claim yet. A supervised 40-minute soak with spike diagnostics is now authorized, but no outcome is available and no 40-minute soak has passed.

## Completed live validation and combined winner — 2026-09-19

The original concurrent GPU method passed live ABBA: pooled steady CPU419.110ms versus mixed405.324ms, a3.29% gain. Every run stopped cleanly with zero sample loss/faults/final backlog. Startup had a >=500ms CPI in each arm; no endurance claim is made.

A separate ambiguity experiment found that CPU range-FFT padding to4096 was faster than3750 at unchanged physical geometry, while GPU ambiguity remained slower. Recorded CPU3750/CPU4096/GPU4096 means were401.277/385.229/431.738ms. CPU4096 live ABBA gained2.56% overCPU3750 but had one steady deadline miss; this alone was not the final candidate.

Combining CPU4096 ambiguity and concurrent50%/2048 GPU FIR passed the full40-frame map gate (peak relative4.30e-5) and exact detection/tracker JSONL comparison. Recorded steady CPU4096 was390.949ms versus373.547ms mixed.

The final short live ABBA used the same application binary for both arms:

| Metric | CPU4096 | CPU4096 + concurrent GPU FIR |
| --- | ---: | ---: |
| Steady CPIs |120|120|
| Mean CPI |405.024ms|392.659ms|
| p95 CPI |420.94ms|417.02ms|
| p99 CPI |432.61ms|463.63ms|
| Maximum CPI |452.17ms|475.63ms|
| Steady CPIs >=500ms |0|0|

The mixed mean improved12.365ms/3.05%; its p99 and maximum were higher in these short runs, so tail latency and endurance remain open checks. All four runs had zero drops, capture faults, and final backlog, and clean stop/return0. Each mixed run logged V3D on62/62observedCPIs,49.9895% of FIR samples, with early submission and asynchronous completion enabled. Detection/tracking and the full RF workload stayed enabled.

Winning app `/var/tmp/vectorwarp-combined/app/bin/blah2`, SHA256`6b7659fea8370cd8d2031dce85da6a8805ad4da0ed303cfcf1d27e1da02587fa`. Live evidence `/var/tmp/vectorwarp-soaks/combined-abba-{cpu1,mixed1,mixed2,cpu2}-20260919/`; reproducibility details in `build/pi-os/GPU_COMBINED_WINNER.md`. No production binary was replaced and no long soak ran. Production transport/backend implementation has now been delegated, with CPU runtime behavior preserved until the new path is qualified.

The production branch now includes opt-in `VECTORWARP_RANGE_FFT=power2` for the same extra range-FFT padding; unset or `default` preserves previous selection. The application and fast benchmark share the selector. Local regression tests passed for complete signed-delay/Doppler maps, invalid settings, and transform-size bounds. Experimental frozen Pi binaries still use `BLAH2_BENCH_RANGE_POWER2=1`.

## Additional GPU correlation and the 350 ms target

The isolated `gpu-blocked-full-benchmark` adds concurrent GPU correlation for
68 of 272 blocks, with the remaining 204 on the existing CPU workers. It
completes correlation before submitting FIR reference work, which overlaps
the unchanged FP64 dense solve. The combined path retains the prior GPU FIR
split, CPU4096 ambiguity, detection, tracking, and full RF geometry.

Six adjacent 40-frame recorded runs passed every complex-map check below
`1e-4` and exact detection/tracker comparison. CPU4096 averaged 387.133 ms;
FIR-only controls averaged 373.401 and 375.279 ms; correlation-only averaged
375.429 ms; both GPU stages averaged 361.142 and 355.061 ms. The mean of the
combined repeats is 358.102 ms. This is recorded performance, not a live
350 ms result. Evidence is in `build/pi-os/GPU_BLOCKED_FULL_RESULTS.md` and
the Pi directory `/var/tmp/vectorwarp-blocked-full/results`.

Root reviewed CPU/GPU partition boundaries, scheduling, finite validation
before FP64 merge, and exception draining. The recorded results qualify the
isolated binary for four supervised 30-second live FIR/BOTH/BOTH/FIR runs;
those checks were dispatched with no production deployment or long soak.

The next independent experiment processes ambiguity range blocks across
persistent CPU workers using single-threaded FFTs, rather than invoking
threaded FFTs for each sequential block. The earlier live ambiguity stage
was approximately 125 ms, making a 20% reduction worth about 25 ms of total
CPI time; this is target arithmetic, not an expected or measured gain.
Partial GPU ambiguity is another hypothesis under review. All candidate
comparisons retain the original output and numerical acceptance gates.
