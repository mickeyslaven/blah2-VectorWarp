# Pi 4 three-way CPU comparison

[Results](../../PI4_PERFORMANCE_20260911.md) · [CSV](comparison.csv) ·
[JSON and stage timings](comparison.json)

This is a separate Pi cohort, not an extension of the desktop timing averages.
All 24 runs completed, with 480 CPIs and 12 configuration/engine groups. Each
group uses two 20-frame runs in reversed engine order; the first eight frames
are excluded from steady statistics. The remaining 24 all miss the 200 ms
deadline in every group. All complex-map and fusion checks pass `1e-4` relative
RMS and peak error. Known original detection-SNR differences remain disclosed.

## Fixed settings and sources

- Raspberry Pi 4B, four Cortex-A72 cores, Fedora 44 ARM64, GCC 16.2.1.
- Real input: 527 MHz, 2.4 MS/s, reference 0 and surveillance 1 from five-channel
  MCHQ. SHA-256: `4188cd03768cf356b31e26759c1a75ec83ffc3c83ecfdc16bbb3e9bfc242d744`.
- Every run: 200 ms CPI, delays −10…245, 256 bins, maximum excess path
  **30.603813420833333 km**. Each run consumes the same first four seconds.
- Standard profile: ±800 Hz and clutter −10…200. Alternate profiles:
  ±100/400/800 Hz and clutter −10…189, identical between all three engines.
  The 199-tap alternate makes the CPU filter FFT 480,200 points
  (`2^3 × 5^2 × 7^4`); it does not shorten the displayed range.
- Original blah2: `c821bee3f0d27cf20c8447f3d908ef722905a4de`.
- [Off World Labs blah2-arm](https://github.com/offworldlabs/blah2-arm/tree/1d37e29c9f788bed2bc95b1b320ccf6478430111):
  `1d37e29c9f788bed2bc95b1b320ccf6478430111`.
- VectorWarp: `8ba6e1330fad9fcdbe8a5a54134a712d0959775e` (same processing code
  as the merged efficiency build). Source archive SHA-256:
  `ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6`.
- Off World source archive SHA-256:
  `9fcdc62d4e8b1acf004ce53429f8ff7cc6555a30555021032e8b54236b60f991`.

All three use Release/O3, one worker, four FFT threads, the same NEON-enabled
FFTW 3.3.10 and Armadillo, and single-threaded BLAS/OpenMP. Off World's explicit
`-march=armv8-a+simd` produces the same target macros as the compiler default.
Its unmodified DSP is compiled using the common benchmark's external-source
target, so its internal `engine` field says `upstream`; the filenames and
`variant=offworld` field identify it separately. No capture/SDK, container or
web timing is included for any engine.

## Limits and reproduction

Runtime CPU affinity is 0–3 with a 400% quota, 3 GiB maximum memory and no swap.
Builds use cores 0–1 and 200%. The finite guard retains a 75°C cutoff, 2 GiB
available-RAM floor and 8 GiB free-disk floor. No thermal/power/service settings
or installed packages were changed. Actual guard peak was 48.199°C, minimum
available RAM 6.605 GiB and minimum disk 31.156 GiB; exit code 0 after 465.976 s.

The input clock paces replay. Processing timers include extraction, spectrum,
clutter, delay–Doppler, detection, tracking and JSON. Startup, file decoding,
independent validation and waits are outside them. This is not real-time
acquisition or endurance evidence. No CPU fallback is counted as GPU work.

Contracts, exact profiles, command/executable hashes, per-frame CSVs and output
JSONL are included. Reconcile them with Python 3.11 or later:

```sh
python3 docs/benchmarks/20260911-pi-efficiency/summarize_cpu.py docs/benchmarks/20260911-pi-efficiency
```

`run_cpu.py` is the finite campaign driver with explicit local evidence paths,
not a general installer. Reproduction requires the recorded input and pinned
source bundles, the common `bench` build, a separate external-source build for
Off World, and the machine's own guard. Do not run it against live services.

Full source, maps and thermal traces are retained on the Pi under
`/var/tmp/vectorwarp-pi-efficiency-20260911.MJQNfT`. Small receipts are mirrored
at that path on Strix. A preliminary one-run result was retained separately
after a result-writer error; none of its timings enter this three-way campaign.
Raw IQ, full maps, libraries and source archives are not added to Git.
