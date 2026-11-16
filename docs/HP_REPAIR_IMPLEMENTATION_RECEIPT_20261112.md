# HP causal evidence and weak-statistic repair — 2026-11-12

Source-only candidate: `/var/tmp/vectorwarp-config-preservation-b7c3b8b`.
No deployment, live restart, threshold/PFA/birth-SNR change, speculative gate
widening, guided promotion, commit, push or release. Existing dirty work was
preserved. The frozen plan is `HP_REPAIR_PLAN_20261112.md`; the antenna/noise
mechanism and live/source capability inventory is `HP_ARRAY_SNR_AUDIT_20261112.md`.

## Outcome and demonstrated fixes

1. **Exact causal time and an actual bounded native producer.** v2 encodes
   event nanoseconds, sequence and lineage as canonical unsigned decimal strings.
   Safe legacy v1 values remain accepted; rounded/unsafe Numbers, overflow,
   malformed strings and truth-shaped fields remain rejected. Native event time
   currently derives from the actual millisecond signal clock, explicitly tagged
   with `clock_precision_ns="1000000"`; exact representation does not invent RF
   nanosecond precision or repair a bad truth/recording time offset.
2. **Radar-only native funnel.** Optional resident ABI14 section32 carries a
   fixed8,256-byte diagnostic snapshot. At most32 candidate and32 prior-track
   records retain exclusive admission reasons, actual seed ID/count, measured
   provenance, support depth/confirmation credit, association ID, state changes,
   promotion/expiry/deletion, predicted coordinates, gate margins/residuals and
   rejection mask. A miss examines at most32 candidates for diagnostic residuals;
   prefix truncation is explicit. Unknown candidate-level credit is null, not a
   fabricated rejection. Seed ID ranges advance modulo65535, matching native IDs.
   Generation, consumed sequence, capture epoch/sample interval, config hash,
   module hash, reset/duplicate and transport-drop facts stay separate from truth.
   API status reports native wiring only after a validated native frame arrives.
   The extra miss search is bounded to32×32 pairs/CPI, not total track×candidate
   population. Prefix omission or transport loss must not be scored as absence.
3. **Non-blocking diagnostic transport.** No thread, DNS or synchronous wait:
   four ready completions per offer, one≤64KiB newline frame, two-second pending
   deadline, five-second reconnect delay and drop-on-pressure. Canceled writes
   retain their buffer until completion; stale callbacks cannot mutate a newer
   generation. In-flight plus canceling storage is bounded to about128KiB.
   Existing recorder queue/byte/file/disk gates are preserved. Failures drop
   diagnostics rather than stopping radar. This is opt-in and currently admitted
   only for the single-lane GPU-resident tracker, not CPU/multi-illuminator.
4. **Resident envelope integration defect.** The outer resident view still
   rejected schema3/4 despite serializer/shader support. Ordinary policy now
   admits compatible schemas1–3; experimental policy requires4. The defect could
   reject candidate tracker products; it is not proof that the separately
   deployed HP proof1 artifact has that defect.
5. **Zero-acceleration GPU defect.** An adaptive configured/quantized acceleration
   span of zero incorrectly required a nonempty open interval, rejecting even
   identical detections. The zero span is now the single admissible acceleration0
   inside the unchanged strict outer gates. Positive-span HP maxAcc10 behavior
   is untouched. CPU behavior is unchanged: its first adaptive range half-width
   is historically2 bins, versus GPU zero-span1; tests enforce each existing
   boundary, not an invented equality of those different gate models.
6. **Fail-closed replay provenance.** `noise_whitened_beam_bank` claims require
   every retained CPI's native applied complex fusion weights, matching mode and
   multiply-then-sum convention. Missing weights yield clearly separated forensic
   coherent diagnostics and no production-margin claim. Partial/malformed/mode-
   mismatched receipts cannot silently become equivalent. Reference eigenweights
   alone do not encode the active residual-noise scaling.

Evaluation-only exclusion counters are a separate parent-owned change in
`api/evidence-statistics.js`; no native producer imports evaluation truth.
The independent temporal review also removed magnitude-only input from the
coherent phase-compensated shadow: `MultiIlluminatorRadar.cpp:99` rejects that
configuration and retains real-valued `TemporalEvidence` for energy-only shadow.
`CoherentPathEvidence` remains a source contract, not a qualified wired recovery.

## Offline experiment actually completed

The bounded runner uses the actual portable C++ `PostClutterBootstrap` for
diagonal and fixed0.75-shrinkage statistics. The immutable policy, baseline,
2/4/8 depth bank, motion grid and training settings were frozen before this run.
Every complete path surface is computed before target coordinates are used for
evaluation-only summaries. IQ/config/cutout hashes are checked; the manifest
rejects whole-pass/sample overlap and unqualified held-out/negative rows.
Disjoint-credit tests cover mixed depths, impulse duplication, endpoints, gaps,
resets, duplicates and long wrap. Analysis-window markers are not tracker votes.

These are already-inspected **development** artifacts, not untouched holdout:
MXY1663 r13 SHA52b207e5f80177fdb3347a5ac1d8822c2425507b958fda8263737c9b999789a4;
AAL743 r28 SHAb611620982cf4a990ddea833be1d5fb178e4c511e63561f5188a6da080ebc104;
SWA4870 r120 SHAda806ace00d188211b9a8253b96282a4f9eb7df9bc3dbe7f47897c0729d407ce.
All three byte hashes were verified on Display2. SWA remains quarantined for
truth-raw-offset, maintenance-only; no bounded post-clutter cutout was available,
so its arms are explicitly missing, not fabricated. Only two retained SWA CPIs
have valid+0.186/+0.686s truth offsets; it is not an admitted scored positive.

Selected disjoint2-CPI target/local-training contrasts, in dB:

|Development window|Fixed2 baseline|Existing fractional path envelope|Diagonal|Shrinkage0.75|
|---|---:|---:|---:|---:|
|MXY first|1.504|7.505|7.234|7.219|
|MXY second|0.204|4.937|6.170|6.274|
|MXY third|0.578|−2.957|−6.947|−7.150|
|AAL first|9.123|8.978|9.466|9.472|
|AAL second|3.573|6.093|6.360|6.174|
|AAL third|0.017|2.057|4.282|4.272|

This is not a universal whitening win. Eight-CPI contrast remains negative for
both passes. Median per-CPI channel-power spread over the selected cutouts was
5.699dB(MXY)/5.922dB(AAL); mean pairwise squared coherence medians were
0.146758/0.139923. These cutouts contain echoes/clutter and are not a noise-only
covariance estimate. Fixed shrinkage is close to diagonal here; these selected
windows do not justify a correlated-noise recovery or tracking-gain claim.

The diagnostic path envelope lacks native beam-compatibility/detector-selection
replay. The retained v1 cutouts lack applied production weights and committed
sample-lineage receipts, and their8 frames leave no evaluation CPI after the
frozen8-CPI warmup. Therefore the numbers are **not CFAR margins, live SNR gains,
measured PFA, recovered aircraft or false-track deltas**. The prior quoted
MXY−5.82/−0.43 and AAL+1.58/−5.32dB margins are likewise forensic reconstructions,
not verified production-equivalent margins. No nominal-PFA correction was removed.

Display2 immutable receipt:
`/var/tmp/vectorwarp-lineage-test/build/hp-weak-comparison-20261112-v2.json`
SHA256 `4f57d7981a1b15de767e143a6026bc350d8f2063ed550e7df2003536ee835129`.
ManifestSHA256 `4d905faab30dd04bba5fada5bb9599837c734ebb2bf28050db32fb7f8d5a5446`;
policySHA256 `0204b0b602470a5efa2ea06045105bcbcf8f79dc2e30e6b999455c4dee1a3530`.
The earlier v1 receipt is preserved, not overwritten. `go=false`,
`false_track_delta=null`, `null_tail_pfa=null` are deliberate outcomes.

## Validation and exact scope

All new build/replay work ran on Display2 in bounded2CPU/4GiB containers,
or1CPU/1GiB Node22 containers, never live radar/Strix. Passed:

- Resident wire209 checks/110 rejection cases, including fixed trace size,
  config-identity binding, phase visibility and ordinary/experimental versions.
- Five serializer groups; exact time beyond2^53, provenance, seed wrap linkage,
  bounded prefixes; actual C++ newline fixture accepted unchanged by Node22.
- Node collector exactness/backward rejection/truth rejection/seed-link,
  queue/file/epoch safety tests; best-effort socket newline/oversize/closed-peer/
  bounded-poll tests, including ASan/UBSan with leak detection.
- CPU zero-acceleration identical-hit confirmation and strict-boundary negatives.
- Eight calibrated-replay tests, five comparison tests using actual C++ bootstrap,
  30 GLSL variants and2,616 production lineage integer cases under ASan/UBSan.
- Full software-Vulkan tracker harness, including the final seed-link addition:
  thresholds−5/5/12, guided-shadow off/on, exclusive admission, actual RPA5731
  confirmation, sparse negative controls,4/6 lifetime/deletion,151 clutter cases,
  mixed-window/impulse/reset/restore/gap/wrap lineage;120CPI diagnostics-on/off
  checkpoint-byte parity at maxAcc0/8 and strict zero-span first associations.
- Latest shared main object and Vulkan resident module strict compile/link,
  including final seed-link telemetry. Final full shader rerun completed with
  exit0 at approximately23:21UTC. Log on Display2:
  `/var/tmp/vectorwarp-lineage-test/build/causal-shader-seed-final.log`.
- Final `git diff --check` passes. No test/build job remains active from this
  workstream. The full tracker fixture has its existing nonfatal range-loop-copy
  warning; main and resident module compile with strict warnings as errors.

Task-owned paths: `api/causal-funnel-{contract,producer,recorder}.js`,
`api/causal-funnel-recorder.test.js`, causal status in`api/server.js`;
`core/GpuTracker.private.h`; `src/process/tracker/CausalTrace{Layout,Json}.h`;
`src/process/utility/BestEffortSocket.h`; causal wiring in`src/blah2.cpp`;
`src/process/ambiguity/GpuResident{Backend.h,Wire.cpp,Module.cpp}` and ABI message
in`GpuWorker.cpp`; focused`test/gpu/Test{GpuResultJson,GpuResidentWire,
GpuTrackerCorrectness,BestEffortSocket,TrackerZeroAcceleration}.cpp`;
`cmake/GpuProcessTests.cmake`; `script/analyze-calibrated-complex-replay.py`,
`script/compare-weak-evidence.py` and corresponding Python tests;
`test/recording/WeakEvidenceBootstrap.cpp`, frozen development manifest and docs.
Some files also contain preexisting/other-owner changes; this list is not a claim
of exclusive ownership of their entire git diff.

Snapshot hashes: tracker`2a6149bf7689fc5ea9cd435dd707b572c571a368f3b7eae6121410c1f2fa32d1`;
main`de5db91ec3372a879e7325b1d5530fcc6b20e95dfe9266db1c11cca13fec5188`;
analyzer`8de0fe596ee78ec4d4750e125cc84a2523ebd4815a42e775f8994f72ae963120`;
comparison`a482b5cb0b415ffc7869260289fc067f1de3ca29a21b9431cf9b01aa8c778154`.

## Verdict and smallest next evidence

**NO-GO for promotion or a claimed HP tracking repair by changed statistics.**
Safe source/instrumentation defects are repaired; available evidence still does
not establish that below-noise echoes dominate over sparse continuity, noisy
association or evaluation exclusions. No gate widening is supported. The known
allocator header fix is independently software-qualified but HP's active
stationary-clutter configuration already allocated the full header; it is not
the proven explanation for HP's observed tracking deficit.

Next use the existing `ResidentReplaySnapshot` covariance/applied-fusion-weight
blocks on identical IQ, retaining exact sample/generation lineage and≥8 warmup
CPIs plus an evaluation interval. The three4-second clips alone cannot satisfy
that context. Obtain independently qualified sample-/whole-pass-disjoint
calibration, heldout and target-free/nuisance negatives; aircraft zero-hit clips
are not automatically target-free. Replay actual candidate/track decisions at
fixed production thresholds/PFA and measure null tails, continuity, maintenance,
and false tracks before choosing a new statistic or error model. Keep the clean
HomePortal two-channel directional baseline unchanged. Software-Vulkan/source
checks do not replace target physical-GPU driver, cold/warm cadence, sustained
diagnostic-load, SDR/platform or RF qualification and explicit live authorization.
