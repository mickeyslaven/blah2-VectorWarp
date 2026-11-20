# Weak-evidence implementation status — 2026-11-10

## Implemented

- The existing GPU-resident 2/4/8-CPI motion-path detector remains unchanged.
- `PostClutterBootstrap` defines the portable CPU numerical contract for a
  manifold-independent diagonal or shrinkage covariance statistic, including
  bounded training, trace normalization, relative conditioning checks,
  diagonal loading, and explicit statistic-family/invalid-cell reporting. It
  never mixes raw power into the normalized statistic.
- `TemporalEvidence` implements the unconditioned 2/4/8-CPI path-mean contract
  for the portable replay lane and retains separate baseline/2/4/8 banks. It
  does not implement the production GPU mode's optional beam-history/angle
  compatibility gate and is not described as a full GPU-engine replacement.
- `ConfirmationLedger` is a bounded, replay-safe primitive that credits only
  fully disjoint CPI windows. It is unit tested but has no tracker/evaluator
  consumer yet, so end-to-end false-confirmation protection is not complete.
- One multi-illuminator temporal shadow lane consumes the unthresholded fused
  or post-clutter-bootstrap map before CFAR. It is disabled by default and is
  rejected outside offline replay because its current synchronous execution is
  not isolated from live cadence.
- The replay manifest validator requires identical IQ bytes for all arms,
  warmup, sample-disjoint calibration/holdout/negative-control/miss splits,
  recording geometry and hardware-epoch identity, arm build/config hashes,
  bootstrap covariance, temporal lineage, and (when requested) successful
  execution receipts plus byte-verified output artifacts.
- The replay lane writes a bounded, exclusively-created `VWTBD01` binary
  artifact containing full-precision per-CPI input power, per-bank power,
  compact exact window/motion lineage, statistic-family masks and capture
  sample ranges. Monitor/API maps remain quantized presentation products; the
  experiment-sized arrays are not sent through the 16-MiB UI transport.
- The artifact reader and execution ledger validate numerical values, geometry,
  epoch/sequence/window history, arm/build/config/source identity, and declared
  split warmup/evaluation coverage. No completed exact-IQ arm run or result is
  claimed by this source milestone.
- Channel-health diagnostics are connected read-only to receiver headroom and
  optional matching recorded anchor evidence. They report missing/invalid and
  all-zero/dropout states, identity mismatches, clipping, and anchor outliers
  without changing weights or quarantine state.

## Validation completed

- Fedora CPU build of the complete `blah2` processor on a separate build host.
- Focused C++ suite: 27 cases, 2,345 assertions.
- Replay-ledger Python suite: 6 cases.
- Configuration manager and configuration server suites.
- Array-channel-health and bounded causal-funnel recorder suites.
- `git diff --check`.

## Promotion gates still required

- Exact-IQ baseline/bootstrap/temporal comparisons on independent splits.
- A split-aware arm runner/evaluator must consume the artifacts and produce
  independently verifiable receipts. The schema, artifact, reader and ledger
  are implemented; this orchestration and the actual results remain deferred.
- An isolated bounded live shadow implementation and run after replay
  acceptance; the current candidate deliberately cannot run live.
- Oppositional portability and numerical re-review.
- The operational soak and explicit release permission.

No deployment, promotion, release, commit, push, or pull-request mutation is
part of this milestone.

## 2026-11-12 exact-IQ and live-shadow follow-up

- HP is running the production GPU 2/4/8-CPI motion banks with fractional
  delay alignment enabled. The live tracker-guided lane remains shadow-only;
  promotion is disabled.
- Exact-IQ replay of retained HP clip
  `kraken-clip-r13-1794516877206988625-7.mchq` found a real sub-threshold case:
  the best ordinary two-CPI margin was -5.82 dB, while the fractional
  motion-aligned two-CPI margin was -0.43 dB. Fractional alignment recovered
  3.37 dB over the integer path even though no robust single-CPI peak existed.
  Audit output is
  `/var/tmp/vectorwarp-hp-fractional-replay/mxy1663-fractional-tbd-audit.json`
  on Display2.
- A second exact-IQ control (`aal743-fractional-tbd-audit.json`) passed the
  ordinary two-CPI arm by +1.58 dB while its moving arm was -5.32 dB. This
  rejects an unconditional preference for a moving or longer-integration bank.
- `analyze-calibrated-complex-replay.py` now implements the production Q16
  fractional-delay coordinate rule. Its five focused tests pass.
- The production guided-promotion path previously had no disjoint-window
  lineage gate. A source-only candidate now records guided credits in private
  checkpoint history bit 3, counts detector or guided credits toward the same
  M-of-N engine, and rejects a two-CPI guided credit when the immediately prior
  CPI already contains credited evidence. Public tracker/bearing JSON masks
  the private bit and remains byte-compatible; guided predictions are excluded
  from bearing support because they carry no new spatial measurement.
  Promotion remains disabled pending replay/false-alarm acceptance.
- Display2 validation completed: public serializer compatibility passes and
  all 30 tracker GLSL variants compile with guided mode off/on. The complete
  llvmpipe correctness executable corrupts allocator state before finishing;
  that harness/runtime failure is not counted as behavioral acceptance.
- Oppositional Astra Ultra review is still required. The mandatory Strix
  subagent resource gate repeatedly blocked at 17.9 GiB available versus the
  24 GiB floor, so no review is claimed.

## 2026-11-12 subsequent oppositional correction

The later source review supersedes the candidate description immediately
above: the previous-slot-only gate did not prevent detector-after-guided
overlap, and byte-identical public output hid guided measurement provenance.
The current source uses ordinary schema-3 measured-hit confirmation when
promotion is off and a distinct schema-4 disjoint-credit experiment when on.
The experiment rejects infeasible M/N settings instead of extending lifetime:
HP 4-of-6 must keep promotion disabled. Public output now explicitly separates
detector measurement, guided prediction and coast. See
`EXPERIMENTAL_GUIDED_CONFIRMATION_20261112.md` for the bounded contract,
restore/gap behavior, tests and remaining acceptance gates.

The review also identified a concrete source overrun behind the blocked
correctness investigation: the disabled stationary-clutter buffer allocated
16 bytes but initialized a 40-byte header. Its allocation and accounting are
corrected together. Earlier failed runs remain failed evidence; new behavioral
results must be recorded separately. No promotion or release follows from
this source correction.

The subsequent bounded Display2 CPU-Vulkan run now completes the entire
production-shader correctness suite, including ordinary 4-of-6 and mixed
experimental lineage cases, without allocator corruption. The isolated
confirmation and lineage modes pass too. Cold llvmpipe JIT still trips the
unchanged guided-work budget; only the warm bounded-cache run is behavioral
acceptance. Exact receipts and the unchanged live-promotion NO-GO are recorded
in `EXPERIMENTAL_GUIDED_CONFIRMATION_20261112.md`.

## 2026-11-12 applied-weight provenance correction

The numerical margins quoted above must now be read as **forensic unwhitened
coherent reconstructions from retained post-clutter cutouts**, not verified
live detector margins. HP already runs `noise_whitened_beam_bank`; the retained
v1 cutouts contain reference eigenweights but not the actual bounded residual-
noise scaling/applied fusion weights. The analyzer previously ignored that
distinction. It now requires a complete native applied-weight receipt before
exposing a production-equivalent margin. Otherwise it labels the reconstruction
forensic and leaves the production-margin field empty. Malformed, partial or
mode-mismatched applied weights cannot silently become production evidence.

The resident envelope also still rejected native checkpoint schema3/4 despite
separately passing serializer/shader tests. That integration defect is corrected
with ordinary-versus-experimental version checks. It is a candidate-source defect,
not proof that the separately deployed HP proof1 build has the same failure.

`HP_REPAIR_PLAN_20261112.md` freezes the next diagnostic comparison. The added
bounded portable bootstrap adapter uses the actual C++ implementation; the
comparison runner never labels selected-cutout controls as target-free exposure,
and reports missing warmup/applied weights/qualified holdout/negative data.
Guided promotion and live changes remain unauthorized.

## 2026-11-12 HP weak-evidence source completion

- The replay-only post-clutter bootstrap now offers diagonal, fixed shrinkage,
  and robust shrinkage covariance contracts through the validated portable
  configuration. The robust arm uses bounded-power training weights and
  sample-count-aware shrinkage; it is not selected automatically and cannot
  alter the live detector.
- The ordinary replay-only temporal arm accumulates unthresholded energy on
  separate 2/4/8-CPI motion paths. A standalone complex coherent-path contract
  uses exact first-sample lineage, locks the integer CPI hop, integrates phase
  over the path's Doppler trajectory, and rejects gaps, rate changes, variable
  hops, sequence discontinuities, and invalid frames transactionally. It is
  deliberately disconnected: the available fused/bootstrap maps are magnitude-
  only and therefore cannot support a truthful phase-coherence claim.
- Empirical-null calibration is fail-closed. Replay cells are never silently
  reused as their own controls; without an externally validated, sample-
  disjoint negative-control cohort the arm emits unresolved diagnostic scores
  and makes no global false-alarm claim.
- Channel/anchor identity now fingerprints the actual applied per-channel gain
  epoch in DAQ wire order. Complex anchor response and phase evidence are
  accepted only when capture generation and tuning identity match.
- The exact-IQ loss-budget tool binds the complete recording hash, recorded
  channel order and bounded sample ranges. Noise covariance remains unavailable
  unless the caller supplies an independently selected noise range, and direct-
  anchor phase evidence remains unavailable unless its range is supplied. Its
  ADC-domain metrics are explicitly not production-equivalent because the live
  decoder performs packet-wise DC removal and gain equalization.
- Partial common-mode subtraction and normalized-innovation gating exist only
  as explicit source contracts. Neither is wired into production because each
  still needs independent negative-control qualification.

Combined focused validation passes 2,503 multi-illuminator assertions, 9,728
Kraken/array assertions, 767 CPU tracker assertions, the resident-wire envelope
(209 accepted checks and 110 deliberate rejections), strict zero-acceleration
tracker boundaries, configuration validation, array-health/manifold identity
tests, and the Python replay/diagnostic suites. The source comparison remains
NO-GO for promotion because it lacks native applied weights, warmup and qualified
negative controls; it makes no live-CFAR, false-track, or recovered-track claim.
No deployment, promotion, release, commit, push, or pull-request mutation follows
from this source milestone.

## 2026-11-12 bounded live observer source connection

The separate, disabled-by-default `live_temporal_shadow` setting now connects
the existing post-clutter bootstrap and portable 2/4/8 temporal engine to
single-lane CPU and native resident CAFs. One bounded background worker uses
nonblocking offers, exact capture-generation/sample lineage, no work queue,
explicit memory/cell/time limits and failure latching. It has no detector or
tracker consumer. Backend arrays retain their native/portable precision;
presentation maps remain unchanged. The replay-only lane stays replay-only.

See `LIVE_TEMPORAL_SHADOW_20261112.md` for the exact data boundary and remaining
qualification gates. Source tests admit both current site geometries but do not
establish target copy/computation budgets, radar cadence or RF benefit. Enabling,
deployment, promotion and release remain outside this source milestone.
