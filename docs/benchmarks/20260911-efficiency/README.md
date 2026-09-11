# Combined processing improvements — 11 September 2026

Current matched results after combining CPU, IQ ownership, mapped GPU clutter
and single-pass JSON changes. The baseline is actual
[30hours/blah2](https://github.com/30hours/blah2/tree/c821bee3f0d27cf20c8447f3d908ef722905a4de),
not the Kraken pull request. These measure processing capacity, not better
aircraft detection.

[Readable results](../../GPU_BENCHMARK_20260911.md) · [CSV](comparison.csv) ·
[JSON with stage timings](comparison.json)

## One range for every test

Every profile uses **527 MHz, 2.4 MS/s, delays −10 through 245 (256 bins),
and maximum excess path 30.604 km**. The exact maximum is
30.603813420833333 km. Excess path means the extra transmitter–target–receiver
distance, not the aircraft's distance from the receiver. The clutter window
is −10 through 200 throughout. No wide-Doppler test shortens the delay window.

- 110 timed runs, 2,200 complete CPIs, three hosts and four physical GPUs.
  There are **41 current configuration/backend groups** and **14 prior-build
  groups** for a fresh before/after comparison.
- Each group has two 20-frame runs in reversed execution order. The first eight
  startup/qualification frames are retained but excluded from steady results.
  Mean, p95 and deadline counts pool the remaining **24 CPIs**. A mean below
  the CPI is not a zero-miss claim; every deadline miss remains in the results.
- Input is a whole-packet, 20.002-second recording of real five-channel Kraken
  IQ. Each engine gets the same samples, paced at their original sample clock.
  A 100 ms run consumes two seconds, a 200 ms run four, and a one-second run twenty.
  Input SHA-256:
  `1e8d50a5fe62410ead9094d95a57af1d03414e87ac00b8861b75444c7aab12aa`.
- Pair tests select reference channel 0 and surveillance channel 1. Array tests
  use five inputs and four surveillance workers. Original blah2 has no equivalent
  array mode; those rows show VectorWarp capacity, not a matched speedup.
- The timer covers extraction, reference synthesis, spectrum, clutter,
  delay–Doppler, fusion, detection, tracking and JSON. File reading, startup,
  independent output verification and pacing waits are outside this timer.
  These are sample-clock-paced recorded-IQ processing measurements, **not new
  live acquisition, sustained endurance or loss-free capture results**.

## Correctness and execution

All 110 runs and all three guarded campaigns exited successfully. Every complex
map and fusion result passed independent CPU comparison at `1e-4` relative RMS
and peak error. All current GPU groups used GPU delay–Doppler **and** GPU clutter
on 24/24 steady frames; none ran a steady CPU accuracy oracle. The startup
qualification and per-frame fault/finite/solve checks remain enabled.

The physical clutter/composed-path preflights passed in both `auto` and `staged`
memory modes on all four GPUs before timing. This is not a timing comparison of
the two memory modes: timed runs use `auto`.

Prior/current VectorWarp CPU detection and tracking output matches byte-for-byte
for both repeats of the two before/after profiles on every host. The original
blah2 comparison retains VectorWarp's documented SNR correction; original and
fork detection SNR is not claimed to be bit-identical.

Unsafe original-blah2 scratch geometry is excluded before timing, not represented
as a slow or crashed run. The full-range ±40 kHz profile is rejected by both
engines before processing. Wider Doppler at this range remains
[future algorithm work](../../FUTURE_WIDE_DOPPLER.md).

## Machines and resource controls

| Host | CPU / GPU | Identical CPU allowance for each backend | Peak monitored temperature | Minimum available RAM |
| --- | --- | --- | ---: | ---: |
| Strix | Ryzen AI MAX+ 395 / Radeon 8060S | Physical cores 0–7, 800%; pair: 1 worker × 8 FFT threads; array: 4 × 2 | 80.625°C | 108.52 GiB |
| NVIDIA laptop | Core i7-12650H / RTX 4050 Laptop | Physical cores 0,2,4,6, 400%; pair: 1 × 4; array: 4 × 1 | CPU 49°C; GPU 42°C | 9.12 GiB |
| Pavilion | Core i5-7300HQ / Intel HD 630 and AMD Polaris 12 | Cores 0–3, 400%; pair: 1 × 4; array: 4 × 1 | 82°C | 8.47 GiB |

Strix has a 12 GiB process allowance; laptops have 3 GiB, without swap. BLAS and
OpenMP use one thread. The campaigns retained their CPU/memory/thermal guards,
disk floors and Strix compute lease. No fans, global thermal limits, CPU frequency
settings, radar services or training jobs were changed. Guard wall durations
were 333.035 s, 218.178 s and 300.057 s respectively. These wall durations include
preflights and are not speedup measurements.

## Source and reproduction

| Artifact | Identity |
| --- | --- |
| Actual upstream DSP | `c821bee3f0d27cf20c8447f3d908ef722905a4de` |
| Current VectorWarp source | `8ba6e1330fad9fcdbe8a5a54134a712d0959775e` |
| Prior VectorWarp source | `2a9bfdf6d56f63e40d916a9176b40d2ddbeb459e` |
| Current source archive SHA-256 | `ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6` |
| Original-blah2 executable SHA-256 | `c21761bf1025f69493455929a4e2d1a1555485b032411486b4755b0032240230` |
| Current VectorWarp executable SHA-256 | `0ec780044859be6c463bc83d81e2cafee8c3a6118c4d1c294e949d8059751926` |
| Current GPU module SHA-256 | `3e5be3966ed90f1c639e1a97e1b2863050dc92ae69a9f7ffbb4961458d083963` |
| Prior GPU module SHA-256 | `6fee88d12562da3e5ae6ae48944629a8d34a7f3c47adaa14265f44cdc2c87a3d` |

Both engines use GCC 11 and the same libraries. Original processing/data source
was checked against the pinned upstream tree; its executable is unchanged from
the previous fixed-range campaign. Each binary loads its own sibling GPU module,
including the prior-build runs. Every command receipt records executable and
input hashes. This campaign's source preceded its documentation-only updates.

Each host folder contains contracts, profiles, preflight geometry/logs, exact
commands, per-frame timing CSVs, output JSONL and run summaries. Regenerate and
validate the comparison with Python 3.11 or later:

```sh
python3 docs/benchmarks/20260911-efficiency/summarize.py docs/benchmarks/20260911-efficiency
```

`run_equal_range.py` records a new campaign using the existing instrumented
benchmark bundle, a matching IQ file and a new output directory. It does not
establish machine resource guards. The source bundle is retained with the raw
evidence; this directory is a receipt set, not a standalone binary distribution.

Full maps, source bundle and thermal traces remain at
`/var/tmp/vectorwarp-efficiency-20260911.Cbm8Ep` on Strix. Original laptop evidence
is at `display1-worker:/var/tmp/vectorwarp-efficiency-20260911.6RP2oe` and
`pavilion-worker:/var/tmp/vectorwarp-efficiency-20260911.6moQ8K`. Raw IQ and binary
artifacts are retained separately, not added to the source repository.
