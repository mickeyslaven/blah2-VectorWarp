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

## Implementation handoff

The initial source work passed strict C++17 syntax-only checks for the numerical
test and inspector. A separate single-threaded NumPy analytic check found
37.64–45.53 times the uncompensated peak power on the declared off-bin examples,
and 1.043 times stationary peak power after interpolation white-noise
normalization. These are mechanism checks, not measured radar outcomes.
Native target compilation and `rangeMigrationReplayCpuContract` execution are
pending: release work had priority and inspected local available memory was
below the 48 GiB material-compute requirement. Do not interpret the committed test cases
as passed until their executable runs successfully on an admitted build host.
