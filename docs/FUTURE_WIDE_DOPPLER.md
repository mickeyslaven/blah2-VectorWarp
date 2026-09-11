# Future work: wide Doppler at the standard excess-path range

This is an assessment, **not an implemented or benchmarked capability**. Current
supported geometry and all numerical gates remain unchanged. Wider-Doppler
tests must not silently substitute a shorter excess-path range.

## Why more FFT padding is insufficient

The current CPU/GPU block-CAF algorithm correlates two disjoint blocks of length
`L`, then Fourier-transforms their correlations across blocks. A block contains
only real lags `-(L-1)..+(L-1)`. At 2.4 MS/s, 200 ms and requested ±40 kHz,
the existing geometry chooses 16,001 blocks and `L=29`; delay +245 therefore
cannot represent the standard maximum excess path of approximately 30.604 km.
Increasing FFT storage does not supply the missing cross-block sample pairs.

Adding real input halos would recover those pairs. For a reference block
`x[bL+r]`, `r=0..L-1`, the surveillance halo is
`y[bL-10 .. bL+L-1+245]`: 284 samples when `L=29`. The corresponding linear
correlation needs an FFT length of at least 312; output lag `tau` is gathered
at offset `tau-(-10)`, not the old unshifted index. Every reference sample must
belong to exactly one block; overlapping surveillance samples are intentional.

However, **halos alone do not remove the intrablock Doppler approximation**.
The existing block sum discards the sample-dependent Doppler phasor before its
slow-time FFT. For a constant-amplitude matched target at 40 kHz and `L=29`,
the resulting boxcar response is approximately 0.658, or -3.64 dB, with an
additional block-origin phase. Correcting a single phase cannot recover the
lost contributions for arbitrary IQ. Halo-only output also differs from old
golden maps because those deliberately omit cross-block pairs.

## Exact candidate, then optimization

Use an independently specified finite-CPI cross ambiguity function:

`A(tau,k) = sum_n conj(x[n]) * y[n+tau] * exp(-i*2*pi*k*n/N)`.

A straightforward CPU FP64 oracle forms each delay-product sequence and performs
an `N`-point FFT. A GPU implementation can process small groups of delays,
retaining only the requested Doppler bins. This is exact for the declared
finite-CPI window/boundaries, unlike collapsing each short block first.

An exact polyphase optimization is also possible. For `N=B*L`, retain every
within-block phase `j`, FFT `p_tau[bL+j]` across `b`, then combine the `L` spectra
using `exp(-i*2*pi*k*j/N)`. With `N=480000`, `L=30`, `B=16000`, this preserves
the exact 5 Hz grid, all CPI samples, and distinct ±40 kHz endpoints. The phase
spectra use `k mod B`; the final phase combination must not be omitted.

For 256 delays, one channel requires 122.88 million complex delay products and
4,096,256 output complex bins. Storing every FP32 product would take 937.5 MiB;
an eight-delay tile takes 29.30 MiB, plus 31.25 MiB output per channel, input,
FFT scratch/LUTs and other pipeline allocations. Native FP64 output takes
62.50 MiB per channel. Tiling controls memory, **not execution time**. End-to-end
speed, transfer costs and larger-map API/UI costs remain unmeasured.

## Acceptance before any implementation claim

1. Specify whether CPI-edge samples are zero-extended or use real captured
   history/lookahead. Real halos require up to 10 earlier and 245 later samples;
   never wrap one CPI's end to its beginning or silently manufacture IQ.
2. Preserve an explicit reference-time origin and complete CPI. A
   surveillance-time definition differs by a delay-dependent Doppler phase.
   Nonzero Doppler-center mixing, signed lags, endpoint bins, timestamp and
   normalization conventions need independent tests.
3. Check a direct FP64 complex-sum oracle on deterministic small cases:
   impulses spanning every block/FFT/CPI boundary; ±lag; both Doppler endpoints;
   fractional-bin targets; nonzero center; multiple channels; cancellation and
   near-zero maps. A bounded toy audit matched the exact polyphase identity at
   normalized peak error 2.26e-15; halo-only error was 0.806 on that same case.
4. Compare full-size CPU FFTW and GPU results under the existing 1e-4 complex-map
   gates; qualify each vendor, startup checks and runtime fallbacks. Keep an
   independent CPU oracle in acceptance tests, not recurring production work.
   Legacy block maps are compatibility evidence, not an exact-CAF truth oracle.
5. Benchmark only after correctness, at the same standard excess-path range and
   stated CPI. Keep actual GPU selection, throughput, memory, dropped deadlines
   and thermal limits separate. No speedup or hardware capacity is promised.

The current range guard must remain until a separately reviewed algorithm and
its CPU/GPU reference, geometry, buffer and protocol changes are complete.
