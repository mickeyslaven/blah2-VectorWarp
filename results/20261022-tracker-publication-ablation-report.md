# Tracker stationary-clutter / publication ablation — 2026-10-22

## Result

The retained 563 MHz truth-window recording does **not** provide evidence that
`stationary_clutter` suppressed a target. It contains 59 detector/track CPIs.
A fresh persistent map can gain at most one credit per cell per CPI and the
production threshold is 60; stationary retirement additionally requires a
100-sample track history. Therefore neither admission suppression nor retirement
can occur anywhere in this replay, regardless of its detection geometry.

The exact publication shadow does show a visibility effect. With fresh detector
input and no external events, `hybrid` reduced 20 observable native track IDs to
13 and reduced non-ADS-B-near track-frame instances from 201 to 172. It retained
the one ADS-B-near track (`a81461`) but exposed it in one frame rather than two:
its `n=2` ASSOCIATED appearance was hidden and its later `n=8` appearance was
published. This is initiation/publication visibility, not raw-detection loss.

| Layer | Native/off observation | Hybrid or stationary-on result |
|---|---:|---:|
| Raw detections | 249 in 59 frames; 3 near `a81461`; 246 unmatched | identical saved detector stream, SHA-256 `c4c55311e3c88dd724f027bf8f367d2552c11c58cf0953d727b01ef0d5dd2dd7` |
| Native tracker pressure | peak 155 total / 151 tentative | stationary filter provably cannot activate before clip ends |
| Observable formed IDs | 20 associated-or-better | stationary-on shadow: unchanged; hybrid public: 13 |
| Confirmed ACTIVE/COASTING IDs | 8 | hybrid public: 8 |
| Non-truth-near unique IDs | 19 | hybrid public: 12 |
| Non-truth-near track-frame instances | 201 | hybrid public: 172 |
| Target-near outcome | one ID, two frames | stationary-on shadow: unchanged; hybrid public: same ID, one frame |

“Unmatched” means outside the recorded eligible ADS-B gates, not proven false.
The saved public frame does not expose IDs for tentative hypotheses, so an exact
unique initiation count cannot be reconstructed. Peak total/tentative population,
20 first-observable formed IDs, and 8 confirmed IDs are reported separately.

Machine-readable measurements are in
`results/20261022-tracker-publication-shadow-ablation.json`.

## Exact execution boundary

The replay harness now accepts `--stationary-clutter on|off` and changes only
`process.tracker.initiate.stationary_clutter.enable`. Both paired resident
attempts failed before reading a CPI with:

> This cohort does not provide the resident ABI9 GPU route

The local `bin/blah2` is SHA-256
`a90cc321f31e31fd146aaa22ad3e6f27b322e3a9954fab366a281f6c172121d6`.
The compile-time rejection is at `src/blah2.cpp:667-668`. Consequently this is
not an executed native resident on/off comparison. The stationary-on result is
a bounded fresh-map proof for this 59-frame clip; it must not be generalized to
a warm live map or a recording of 60+ CPIs.

The proof follows the production code directly:

- `core/GpuTracker.private.h:147-152` suppresses only unmatched initiations whose
  cell evidence has reached `persistence_cpis`.
- `core/GpuTracker.private.h:298-313` advances each cell no more than once per
  frame, including the adjacent-row footprint.
- `core/GpuTracker.private.h:153-160` requires both learned-cell evidence and the
  configured retirement history; the production policy is 100 samples.
- `core/GpuTracker.private.h:643-650` runs association before persistence-based
  unmatched initiation filtering.

The exact publication arm called the production `TrackerPublication` class on
every saved native frame. `api/tracker-publication.js:123-147` shows that hybrid
keeps only native candidates with `n >= native_min_associations` when input is
fresh. No synthetic external tracks were supplied, isolating this filter.

## Artifact provenance

- IQ: `20261020-161830-563mhz-coherent-ab-truth-window.blah2iq`, SHA-256
  `651c267a8a69ea54b9d06dc96891556aeab620014b37599dbf6ba92bee99d6b1`.
- Saved replay: `20261020-563mhz-fusion-ab.json`, SHA-256
  `d120c108732e51288fb91240711368b53fbeaec133b67c4cb05a63571ab46531`.
- Truth: `20261020-161830-563mhz-adsb-truth.jsonl`, SHA-256
  `e0f49ad9c2290bc09f5da4337f0713f9cb57d0df6644aa61cadee4d446d91d36`.
- Time alignment and gates were reused from
  `20261020-563mhz-fusion-ab-analysis.json`: start epoch
  `1792527509.975899`, CPI 0.5 s, detection gates 0.25 km / 5 Hz, track gates
  2 km / 5 Hz.
- Persistent-cell implementation last changed in commit
  `022a210594ddc35b5b1ce8d585ea337c352788f3` (2026-10-21).
- Hybrid publication originated in
  `92bf429e486fe6300fd44844bfca056b31240c24` (2026-10-17); current unit conversion
  includes `f6b17970564ff7f4a0e2b2fd67875d68db877c09` (2026-10-19).

## Safe acceptance replay / shadow plan

1. On the qualified Linux resident host, run this same raw IQ twice from a fresh
   processor using identical configuration and `--stationary-clutter on` then
   `off`. Do not edit the service config. Require equal detector-frame count and
   byte-canonical detector hash before comparing tracker output.
2. For this 59-CPI file, require native tracker output to be identical. Any
   difference is a defect because neither threshold is reachable.
3. Repeat on a retained clip longer than 100 CPIs, again with a fresh process per
   arm. Report initiation counters from resident diagnostics, formed/confirmed
   IDs, unmatched burden, and truth-near outcomes. A warm-map live comparison is
   not interchangeable with this test.
4. Feed each saved native stream through the supplied publication analyzer for
   `native` and `hybrid`, freezing the same input-freshness and external-event
   stream across arms. First use no external events to isolate
   `native_min_associations`; then use a recorded external JSONL stream if one is
   available.
5. Acceptance: raw detector hashes match; the 59-CPI stationary pair is native
   identical; hybrid differences are confined to the documented publication
   filter; no target-near confirmed track is removed. This plan performs no live
   RF, config, or deployment mutation.

## Validation

- `python3 test/script/test_replay_tracker_ablation.py` — 2 tests passed.
- `node test/script/test_tracker_publication_ablation.js` — passed.
- `node api/tracker-publication.test.js` — passed.
- `python3 -m py_compile script/replay-fusion-ab-cass.py` — passed.
- Both bounded resident on/off invocations reached the same compile-time ABI9
  blocker before any detector frame.
