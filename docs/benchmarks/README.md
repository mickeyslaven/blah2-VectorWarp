# Benchmark evidence index

These retained measurements document the recorded-IQ comparison contract; they
are not qualification of today's source or proof of live capture capacity.
[Harness assumptions and build instructions](../../bench/README.md) define the
shared input adapter, unchanged upstream DSP comparison, timing boundaries,
startup qualification, sample-clock pacing, and known SNR-field difference.
The sole canonical base profile is [bench/profile-example.json](../../bench/profile-example.json),
SHA-256 `a01d5b789cb17b0c8a500019954c4c546df53968801cf18a1396fb8ed317434e`.
The campaign bundle must contain those same bytes as `profile-example.json`;
the former efficiency-directory copy was redundant.

| Campaign | Retained comparisons | Fork source commit | Source archive SHA-256 |
| --- | --- | --- | --- |
| Equal range | [JSON](20260911-equal-range/comparison.json), [CSV](20260911-equal-range/comparison.csv) | `8fa12290c203d85d9b48f57a5a5c0a01a66f7551` | `2239b08347ef1b2466e7f9a10a269ed397939cc3bcea2aa5acc29d68b32fc9bc` |
| Efficiency | [JSON](20260911-efficiency/comparison.json), [CSV](20260911-efficiency/comparison.csv) | `8ba6e1330fad9fcdbe8a5a54134a712d0959775e` | `ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6` |

Both campaigns use the same input SHA-256
`1e8d50a5fe62410ead9094d95a57af1d03414e87ac00b8861b75444c7aab12aa`,
2.4 MS/s, 527 MHz, five channels, delays -10..245 (256 bins,
-1.249135..30.603813 km excess path), and clutter limits -10..200. They use
20 CPIs per run, eight startup and twelve steady frames, two repetitions with
reversed variant order, and no periodic steady CPU oracle. Pair cases use one
worker; array cases use four. FFT threads are the CPU budget divided by workers;
OpenBLAS and OpenMP each use one thread. Pair CPIs at 200 ms use Doppler half
spans 800/2400/4800 Hz; the array 200-ms case uses 800 Hz. Strix also includes
pair 100 ms/800 Hz, pair 1 s/4800 Hz, and array 1 s/2400 Hz.

| Host | CPU budget | Device IDs (historical enumeration) | Extended cases | Equal-range runs | Efficiency runs |
| --- | --- | --- | --- | --- | --- |
| strix | 8 | `4098:5510:0` | yes | 34 | 42 |
| nvidia | 4 | `4318:10401:0` | no | 20 | 28 |
| pavilion | 4 | `32902:22811:0`, `4098:27039:1` | no | 28 | 40 |

Upstream comparisons run only where its geometry inspection accepts the exact
profile: pair 200 ms/800 and 2400 Hz, plus Strix pair 100 ms/800 Hz. No upstream
array comparison is claimed. Both engines must reject the full-window
200-ms/±40-kHz case before processing; [check_limit.py](20260911-equal-range/check_limit.py)
retains its independent exact-error/binary-hash receipt check. Efficiency adds
auto and staged clutter preflights for each GPU and a before/after subset at
pair 200 ms/800 and 2400 Hz. Its before source is
`2a9bfdf6d56f63e40d916a9176b40d2ddbeb459e`; both CPU repetitions must have
byte-identical detection/tracking outputs. Complex and fusion map errors must
be finite and at most `1e-4` for every measured frame.

[bench/campaign.py](../../bench/campaign.py) is the supported shared runner and
reconciler. It replaces the four dated `run_equal_range.py`/`summarize.py` copies;
their paths have no repository consumers. Explicit campaign selection retains
the source pins, host run counts, contract fields, filenames, row order, JSON
and CSV schemas. Source-pin mismatches now fail before a run and when reconciling
either campaign; validation remains enabled under `python -O`.
[Offline regression tests](../../bench/test_campaign.py) mock all benchmark/GPU
processes and check both matrices, reversals, source/binary receipts, refusals,
before CPU parity, and summary schemas against the retained evidence.

Reproduction requires the original checksum-verified source bundles, unchanged
upstream build, libraries, and IQ recording. Per-host raw `contract.json`,
`summary.json`, generated profiles, frame CSVs, output JSONL, geometry exclusions,
command/binary-hash receipts and logs must be retained externally: this tree
contains the aggregate comparisons, not those raw inputs. Building current
source does not reproduce the pinned historical executable. Device IDs must be
confirmed on the original host. Run only under the host's resource/thermal
gate and a bounded external job; these commands do not manage services or host
limits. Keep fresh outputs outside this source tree.

Equivalent commands (Strix shown; substitute the host controls above):

```sh
python3 bench/campaign.py run equal-range \
  --bundle /path/to/equal-range-bundle --recording /path/to/input.mchq \
  --sha256 1e8d50a5fe62410ead9094d95a57af1d03414e87ac00b8861b75444c7aab12aa \
  --output /path/to/new-equal-range/strix --cpu-budget 8 \
  --devices 4098:5510:0 --extended
python3 bench/campaign.py run efficiency \
  --bundle /path/to/efficiency-bundle --before-bundle /path/to/before-bundle \
  --source-commit 8ba6e1330fad9fcdbe8a5a54134a712d0959775e \
  --source-sha256 ccbd2ec1c379310b0df1be1857570d93dd5306a6b435c63c8e785b7c8084dab6 \
  --recording /path/to/input.mchq \
  --sha256 1e8d50a5fe62410ead9094d95a57af1d03414e87ac00b8861b75444c7aab12aa \
  --output /path/to/new-efficiency/strix --cpu-budget 8 \
  --devices 4098:5510:0 --extended
python3 bench/campaign.py summarize equal-range /path/to/new-equal-range
python3 bench/campaign.py summarize efficiency /path/to/new-efficiency
python3 -m unittest discover -s bench -p test_campaign.py
```

Create each campaign's parent directory first; each per-host output directory
must be new. The reconciler accepts present hosts only, enforces that host's
complete run count, and writes the two comparison files in the supplied external
root. It requires raw frame evidence and cannot revalidate timing claims from
aggregate comparisons alone.

Other retained, separately scoped evidence: [Pi efficiency comparison](20260911-pi-efficiency/comparison.json)
and [contract](20260911-pi-efficiency/contract.json),
[JSON serialization microbenchmark](20260911-json-output/microbenchmark.csv),
[earlier paced comparison](20260910/paced-comparison.json), and
[earlier wide-Doppler exploration](20260911/strix-v8-wide-doppler.csv).
Their assumptions and schemas are distinct from the two fixed-range campaigns.
