# Raspberry Pi 4: VectorWarp, blah2 and blah2-arm

VectorWarp CPU processing took **7.5–12.6% less time than original blah2**
and **6.7–11.9% less than Off World Labs' ARM fork** in these matched tests.
All three used the same Pi 4, recorded IQ, four CPU cores and NEON-enabled FFTW.
These are processing-time improvements, not improved aircraft detection.

None of these 2.4 MS/s profiles kept up with a 200 ms CPI on the Pi 4. Narrowing
Doppler and adjusting the clutter window helped, but did not make them real-time.

## Results

Every row uses **527 MHz, 2.4 MS/s, two selected channels, 200 ms CPI, delays
−10…245 (256 bins), and 30.604 km maximum excess path**. Figures are mean / p95
processing milliseconds; **every result missed 24/24 measured deadlines**.

| Doppler and clutter window | Original blah2 | Off World Labs blah2-arm | VectorWarp CPU |
| --- | ---: | ---: | ---: |
| ±800 Hz; standard clutter −10…200 | 905.105 / 929.043 | 905.871 / 908.381 | **798.167 / 807.658** |
| ±100 Hz; clutter −10…189 | 782.120 / 789.523 | 775.027 / 781.409 | **723.229 / 752.861** |
| ±400 Hz; clutter −10…189 | 745.890 / 774.238 | 738.637 / 745.276 | **662.947 / 668.747** |
| ±800 Hz; clutter −10…189 | 853.212 / 943.153 | 846.539 / 847.130 | **745.769 / 750.082** |

The standard profile matches the settings in the
[desktop/laptop comparison](GPU_BENCHMARK_20260911.md). This Pi cohort uses its
own retained real recording; compare engines within a row, not recordings or
machines against each other.

The alternate clutter window uses 199 taps instead of 210. It also changes the
CPU convolution FFT from 480,211 to the more factorable 480,200 points. This is
a disclosed configuration tradeoff, applied to all three engines, not a
VectorWarp-only optimization. The displayed excess-path range is unchanged.
Narrower Doppler is not always faster because it also changes the FFT geometry.

## What still costs time

In VectorWarp's standard profile, clutter filtering takes **501.823 ms** of
the **798.167 ms** total. Delay–Doppler takes 172.454 ms and spectrum processing
68.139 ms. Smaller detector/output costs alone cannot make this workload fit
200 ms. A substantially lower real input sample rate or further acceleration
needs separate validation; changing only a recording's sample-rate label is
not a valid reduced-load test.

## What the ARM fork adds

The checked [Off World Labs version](https://github.com/offworldlabs/blah2-arm/tree/1d37e29c9f788bed2bc95b1b320ccf6478430111)
targets Raspberry Pi 5/RSPduo deployment. Its ARM SIMD compiler target and
NEON-enabled FFTW accelerate **CPU** work; it has no GPU radar-processing path.
It also contains capture, tracking-history and output-management changes.
We compiled its unchanged processing code, not a renamed upstream executable.

All three tests retain NEON FFTW. Fedora's installed double-precision library
identifies itself as `fftw-3.3.10-neon`; its NEON codelets were verified. The
ARM fork's explicit `-march=armv8-a+simd` target also produces the same architecture
macros as this compiler's default. This is a same-Pi-4 processing comparison,
not a test of its Pi 5/RSPduo acquisition, Docker deployment or web interface.

Pi GPU investigation is separate. No Pi GPU speedup follows from this CPU
table. See [GPU diagnostics](GPU_DIAGNOSTICS.md) and the
[historical Pi GPU evidence](PI4_VALIDATION_20260910.md).

## Evidence

The new campaign completed **24 runs / 480 CPIs / 12 result groups**. Each
group pools two reversed-order 20-frame runs, excluding the first eight frames
of each run, leaving 24 measured CPIs. All complex-map and fusion comparisons
passed the `1e-4` RMS/peak-relative gate. Known detection SNR corrections mean
the fork does not claim bit-identical original detection output.

Input was paced at its sample clock. Timers cover extraction through JSON;
file decoding, independent validation, startup and pacing waits are separate.
These are short recorded-IQ processing tests, not live RF or endurance tests.

The Pi stayed below 48.199°C, with at least 6.60 GiB available RAM and 31.15 GiB
free disk. The guard completed successfully in 465.976 s. Services stayed
inactive and SELinux stayed enforcing; no installed package or driver changed.

[Exact sources, settings and reproduction](benchmarks/20260911-pi-efficiency/README.md) ·
[CSV](benchmarks/20260911-pi-efficiency/comparison.csv) ·
[JSON and stage timings](benchmarks/20260911-pi-efficiency/comparison.json)
