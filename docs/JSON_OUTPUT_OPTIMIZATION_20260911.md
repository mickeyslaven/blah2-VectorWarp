# Single-pass JSON output — 11 September 2026

The map-output path previously built a RapidJSON DOM, formatted the whole map,
parsed that JSON into another DOM, replaced its delay axis with kilometres,
and formatted the whole document again. The new `Map::to_json_km` writes the
existing display schema directly, with the same delay conversion and two-decimal
number formatting. It pre-sizes the output buffer without imposing a new output
limit. The old raw-bin serializer and JSON conversion API remain available.

`Detection::to_json_km` applies the same approach to live detection output.
Native call sites use the new methods. Only VectorWarp's fast benchmark target
uses the new map writer; the actual-upstream benchmark keeps its original path.
Its detection serialization remains unchanged, preserving that benchmark's
existing measurement contract.

The [combined real-IQ campaign](GPU_BENCHMARK_20260911.md) now measures this
change with the CPU and memory-path improvements. On Strix at 200 ms CPI,
±2400 Hz and the same 30.604 km maximum excess path, GPU-mode JSON time fell
from 23.488 to 17.066 ms; complete processing fell from 69.650 to 44.669 ms.
The total reduction includes all combined changes, not just this serializer.

## Bounded serializer measurements

These are deterministic synthetic-map microbenchmarks, **not live radar or
whole-pipeline speedups**. All cases use 256 delay bins, −10…245, at 2.4 MS/s
(30.604 km maximum excess path). The Doppler axes have five-Hz spacing, matching
the listed 200-ms geometries. Nine repeats alternate legacy and direct order;
each complete output is checked byte-for-byte outside the timed section.

| Doppler rows / span | Legacy CPU ms | Direct CPU ms | CPU time reduction | Legacy / direct wall ms | JSON bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| 321 / ±800 Hz | 9.957270 | 7.481168 | 24.9% | 10.610245 / 7.528636 | 419746 |
| 961 / ±2400 Hz | 26.632826 | 20.496018 | 23.0% | 58.814030 / 38.247185 | 1254243 |
| 1921 / ±4800 Hz | 51.022676 | 38.831836 | 23.9% | 101.128466 / 70.955106 | 2506759 |

Figures are medians. CPU time uses `CLOCK_THREAD_CPUTIME_ID`; wall time is
affected by the imposed quota and host scheduling. Environment: existing
`blah2-gpu-build`, x86-64, GCC 16.2.1, `-O3 -DNDEBUG`, 0.5 CPU on CPUs 8/24,
8-GiB memory limit. The fixture uses seed 193015, Gaussian complex samples and
zeros at every 97th cell. Input construction and map metrics are outside timing.
[Raw alternating measurements](benchmarks/20260911-json-output/microbenchmark.csv)
retain both clocks. No physical GPU, SDR, service or package installation is
needed for this test.

## Compatibility and limits

`test/json/LegacyJson.h` freezes the 2a9bfdf DOM/two-pass implementation as an
independent oracle. The compatibility suite covers both map scalar types, all
three full geometries, silence, signed delays, timestamp limits, subnormal and
extreme values, 4,096 seeded IEEE-pattern trials, empty detections and the
existing NaN/Inf/zero-rate failures. New direct methods also reject short axes
instead of indexing past their storage. Existing plot, frame-update and API
frame-history regressions pass.

The legacy parser can perturb low digits of large scientific-notation values
before its second formatting pass. Rather than change those emitted values,
unusual metadata outside ±1e9 and invalid metadata use the old compatibility
path. This is a serializer-path choice, not a new signal acceptance threshold.
Ordinary radar fields use the direct writer. Non-finite map samples still fail;
no schema, numeric representation, frame count or output resolution is reduced.

This is not zero-copy output: a final string is still returned and sent through
the existing complete-write socket path. Map log magnitudes are still computed
for serialization; there is no hidden cache over publicly mutable map data.
IQ spectrum and track serialization remain unchanged. Any further optimization
needs its own measured benefit and lifecycle/compatibility contract.

## Reproduce

Run in a resource-bounded source-test environment, not a live acquisition job:

```sh
cmake -S test/json -B build/json-checks -DCMAKE_BUILD_TYPE=Release
cmake --build build/json-checks -j1
ctest --test-dir build/json-checks --output-on-failure
build/json-checks/testJsonOutput --benchmark
```

The main build includes `cmake/JsonOutputTests.cmake` under `BUILD_TESTING`.
It adds `jsonOutputCompatibility`; the benchmark is opt-in and is not a timed CI
performance gate. Numerical equality is mandatory; timing improvement on other
CPUs, compilers or input distributions is not guaranteed by these measurements.
