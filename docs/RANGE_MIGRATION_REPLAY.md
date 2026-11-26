# Offline intra-CPI range migration experiment

`rangeMigrationReplayInspect` and `rangeMigrationReplayCpuContract` consume
`src/process/integration/RangeMigrationReplay.h`. This is an isolated numerical
experiment, with no installed runtime entry point, configuration, detector,
tracker, acquisition worker, GPU work, or promotion claim.

## Representation and sign

Canonical `Ambiguity` computes a block correlation
`r[b,d] = sum_n y[b*N+n] * conj(x[b*N+n-d])`, then a forward slow-time DFT
and centered Doppler ordering. For `y(t)=x(t-delay)*exp(+i*2*pi*f*t)`, increasing
excess delay is positive `d` and the displayed Doppler is positive `f`. A
nonzero Doppler center is removed by the canonical reference heterodyne; it
remains on the published frequency axis. The experiment leaves that convention
unchanged. Delay is excess path, so converting sample delay to distance uses
`c/fs`, with no monostatic factor of two. For a physical constant-rate path,
`f_D=-f_carrier*delay_rate_bins_per_second/fs` under this IQ convention.

A full complex CAF contains every slow-time Fourier coefficient. Its inverse
DFT reconstructs the block range correlations exactly, within arithmetic
precision. A rate trial samples
`r[b, d + rate*(b-(B-1)/2)*N/fs]`, then applies the same forward DFT.
Delay `d` therefore refers to the midpoint of the first and last correlation
block centers, equivalent to sample `(B*N-1)/2` for the source IQ. A
DC-preserving, 16-tap Lanczos-8 interpolator handles fractional range bins.
Reference heterodyning also leaves a known `exp(+i*2*pi*center*delay/fs)`
range phase. The interpolator removes this phase before sampling and restores
it at the output delay; otherwise a nonzero center would create a false
`center*rate/fs` Doppler offset after alignment.
This is a prescribed delay-rate bank on range/slow-time data; it does not
misapply range-frequency Keystone resampling to a Doppler/range display.

This operation cannot repair within-block motion, correlation-edge truncation,
Doppler aliasing, acceleration, reference decorrelation, or a missing complex
phase history. A power map or cropped Doppler cutout cannot supply its input.
The canonical block correlation remains the sole ambiguity implementation.

## Admission, lineage, and statistical contract

The CLI accepts one existing `.vwrps` plus its sealed sidecar through
`loadValidatedSnapshot`, then reuses the canonical native channel-map decoder.
The configured physical surveillance ID selects one channel. Rates are an
explicit, frozen experiment parameter; no truth/ADS-B argument or label enters
the numerical owner. Truth may label a later evaluation, never choose a rate,
phase, delay, Doppler, or channel during inference.

The receipt hashes the running executable and snapshot bytes, reuses the
canonical hash of all decoded sidecar fields plus snapshot identity, and preserves
source, RF, configuration, module and raw-IQ hashes, exact sample counters,
channel, sample rate, correlation length, and output axes. Sample/identity
integers are JSON strings to avoid 53-bit truncation. Hashes identify artifacts;
they do not themselves qualify a receiver or backend. Pin the source commit,
compiler and dependency versions with the executable's retained build receipt.

One to nine unique finite rates must include zero. Full Doppler axes must match
`fs/(B*N)` spacing; delay bins must be contiguous. The common delay mask removes
`8+ceil(maximum midpoint shift)` columns at both ends for **every** trial,
including zero. Missing interpolation support is refused, not padded or wrapped.
Zero-rate values on this common support are copied bit for bit from the CAF.

The corrected complex amplitudes retain canonical DFT normalization. Each bank
reports `white_noise_gain = mean_b(sum_j interpolation_weight[b,j]^2)`.
For independent white range/slow-time noise, dividing power by this quantity
preserves fixed-cell noise variance. It is not a general covariance correction:
real matched-filter range cells and real interference may be correlated.
Finite interpolation does not conserve total map energy, and cropping loses
edge energy; neither is represented as SNR improvement. Complex amplitude
linearity, squared power scaling, the identity baseline, stationary recovery,
and fixed-trial noise variance have dedicated tests.

All banks and all common-support cells are emitted. The receipt counts the
baseline as a trial, gives trials per cell, and counts all evaluated cells.
Selecting the strongest bank increases the null statistic: the seeded noise
control explicitly checks this. There is no CFAR/PFA claim, fitted threshold,
automatic target choice, or uncharged trial pruning. Any later scan over
channels, time windows, bandwidths, or interpolation settings adds to the
declared search and must be reproduced on held-out negatives.

## Refusal and work limits

The first experiment accepts odd `3..511` Doppler rows, at most 4096 delay
columns, at most 65536 input cells, and at most nine trials. It refuses motion
above 0.25 delay bin per correlation block or 32 bins from the midpoint.
It rejects nonfinite/oversized amplitudes, duplicate rates, wrong axes,
insufficient support, and more than `2^28` declared complex multiply-adds.
The operation estimate is
`B^2*D + trials*B*valid_D*(B+16)`. Admission precedes transform allocation.
The CLI bounds snapshot/executable files to 128 MiB and uses one CPU thread.
No production CPI deadline is claimed. Transform scratch is under 6 MiB;
bank storage is under 9 MiB, excluding the existing validated snapshot and JSON
serialization. An FFT optimization needs equivalent numerical regression and
an explicit measured work/memory contract before this ceiling is expanded.

## Reproduce and next acceptance

With a guarded, warning-as-error CMake build, build targets
`rangeMigrationReplayInspect` and `testRangeMigrationReplay`, then run CTest
`rangeMigrationReplayCpuContract`. Before an all-target build, use
`script/prebuild_acceptance.py` with both `--native-target` values and that
`--native-test`, plus a receipt path. Run the repository budget and canonical
processing architecture guards. The synthetic suite covers both walk signs,
fractional delay, off-bin positive/negative Doppler, noise-only/zero/stationary
controls, common support, power normalization and refusal bounds. Independent
bandlimited-profile synthesis and canonical `Ambiguity` IQ processing both
demonstrate uncompensated smearing. The physical IQ cases obey the bistatic
delay-rate/carrier-Doppler relation and test nonzero Doppler centers.

Example invocation with a path to the retained executable:

```sh
./build/rangeMigrationReplayInspect --channel 0 --rates-bins-per-second 0,-4,4 retained-frame.vwrps
```

The values are explicit experiment inputs, not defaults or operating guidance.
Keep outputs outside source. This command does not acquire missing snapshots.

Before integration, replay frozen complete positive and adversarial-negative
whole passes using the same hashed IQ/configuration, baseline and candidate.
Preregister rates from radar-only kinematic bounds; calibrate the **entire**
trial maximum on untouched negatives, including stationary clutter,
interference, off-grid rates and acceleration. Evaluate true detections,
measurement-backed track continuity, false tracks, CPI deadlines, CPU/memory
cost, capture health, and applicable receiver/backend/OS parity. Then require
an untouched prospective holdout and explicit promotion approval. The test
suite alone cannot establish operational gain or justify a new live lane.

## Composed coherent and fallback histories

`ExperimentReader::appendAll` composes migration with the canonical
`DirectPathCoherentExperiment` through one shared sealed-frame decoder.
`Config::migrationRates` is immutable after construction. Each rate owns a
separate coherent and noncoherent history and receives the same measured raw-IQ
pilot. Every rate is returned, including warm-up and pilot-failure outcomes;
there is no upstream selection or splicing of rate winners across CPIs.
The legacy `append` API refuses configured migration rates.

All channels, delay axes and recorded beam IDs use the same interpolation
crop. The known reference-heterodyne range phase is removed before temporal
integration, leaving physical Doppler as the only inter-CPI phase convention.
Raw sample spans, correlation length and the sealed configuration identity
remain attached to every rate result. Canonical unused tail samples remain
in raw lineage: they never masquerade as additional correlation blocks.

With migration enabled, fallback power is explicitly `norm(sum(weight[c] *
corrected_CAF[c]))` for that rate's immutable spatial projection. It is not the
recorded native selected-beam power. Recorded beam IDs are preserved as source
provenance and do not label this fixed projection. With migration disabled,
the legacy recorded FP64 power and selected-beam compatibility remain intact.
Pilot rejection clears only the coherent history; the matching corrected
power history remains available. Move-only rate results own their fallback
maps, so later appends do not invalidate previously returned results.

The composed CLI accepts `--migration-rates 0,RATE,...` immediately before the
snapshot list. It reports rate index, delay rate, interpolation noise gain,
fallback kind, and per-rate and combined trial counts. These counts include
both coherent and power families across every configured spatial/path/depth
trial and every rate, including zero, warm-up and pilot rejection. They do not
replace empirical null calibration or charge undeclared tuning choices.

`directPathCoherentReplayCpuContract` now generates continuous fractional-delay
IQ, processes it with canonical `Ambiguity`, seals snapshots, and sends them
through the composed reader. It checks separate nonzero-rate complex 2/4/8-CPI
sums against an explicit phase oracle, changing reference gauge, overlapping
CPIs with noninteger inter-CPI phase, exact sample counters above `2^53`, corrected fallback after
pilot loss, beam cropping, config/span/reorder refusal and result lifetime.
The superseded standalone adapter and its narrower zero-rate test were removed.
The native sealed snapshot contract currently admits a zero Doppler center;
the separate canonical-IQ range tests retain nonzero-center phase coverage.

The composed implementation passed both native CTest contracts on an isolated
Linux CPU build on 2026-11-26: `rangeMigrationReplayCpuContract` (0.85 s) and
`directPathCoherentReplayCpuContract` (0.72 s). The inspector also compiled with
warnings treated as errors. The build used one job and a 2 GiB memory cap,
including the canonical processing and offline replay CMake owners. All 86
architecture checks and 943 repository-budget ceilings passed. This validates
the numerical contracts, not recorded-radar benefit or live promotion.

## Fractional inter-CPI range and residual phase

The replay experiment can refine residual slow-time phase with `--refine-phase`:
depths 2/4/8 search 4/8/16 unique frequency offsets over one `1/hop` alias
interval. The zero hypothesis is retained and the negative Nyquist endpoint is
included only once. This is a charged frequency search, not an unambiguous
Doppler estimate. At half-between-grid frequencies the ideal 8-CPI worst-case
coherence exceeds 0.81; the former nine-point grid could fall below 0.42.
Every immutable rate history remains independent. Combined trial accounting
charges all 28 phase/depth trials plus the three power-fallback depths, each
multiplied by every spatial, delay-motion, Doppler-motion and migration trial.

When the sealed configuration enables fractional delay, only the coherent
history samples the continuous physical range path. It reuses the prepared
Lanczos-8 owner in `RangeMigrationReplay`, caches rows across residual integer
paths, refuses missing interpolation support and records fixed-path white
range-noise gain. That gain is provenance, not a search-null calibration.
Selected-beam inputs cannot be fractionally mixed. The canonical noncoherent
nearest-cell contract is unchanged. Direct complex inputs specify their
remaining heterodyne range ramp; the composed reader supplies zero after its
existing complete de-ramp, preventing a double correction.

The reader derives inter-CPI drift from the locked raw-sample **hop**, not the
CPI span. Until the second sealed frame establishes that hop, it retains only
the first prepared maps, fallback and pilot. It then seeds the immutable rate
engines and releases this bounded bootstrap storage. The first call remains
warm-up; a later hop change is refused. Rate results expose both the hop and
the bound drift coefficient. Fractional-on overlapping-window regressions
compare complex 2/4/8-CPI output and canonical nearest-cell fallback against
the direct CPU owner, including sample counters above `2^53`.

Calibration-only continuous-IQ injections localized three distinct failures:
missing temporal drift in the development harness, under-resolved residual
phase trials, and nearest-cell discontinuities on a moving complex path.
After correction the moving-path true-phase oracle recovered
2.98/5.96/8.97 dB at 2/4/8 CPIs, with approximately 0.994/0.986/0.985 coherence.
This gain is relative to the matched path's single-CPI signal power, **not**
relative to an already-integrating noncoherent baseline. Near-threshold tests
also found that a 0.6 coherence pre-gate can discard weak coherent returns.
No live gate or default is changed: an ungated replay statistic still needs
full joint trial calibration, untouched RF controls and prospective acceptance.
Current single-core full-bank replay exceeds the 100 ms experimental cadence;
correct numerical gain is not a runtime or radar-promotion result.
