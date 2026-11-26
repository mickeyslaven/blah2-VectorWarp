# Paired recorded-IQ research evaluator

The recorded-IQ mode of `tools/vectorwarp_release_regression.py` produces a
schema-3 research receipt. Stable publication instead requires the explicit
[operational baseline/prospective mode](OPERATIONAL_RELEASE_ACCEPTANCE.md);
research receipts cannot substitute for its live acceptance. The paired mode is an offline evaluator,
not an installed radar service or a second processor. It never activates a
release, starts capture, calls a live API, or feeds ADS-B into inference. Its
decisions use retained paired whole-pass measurements and a complete runtime
ledger. Raw federation summaries are insufficient input.

The checked-in
[`release-regression-policy.unapproved.json`](../config/release-regression-policy.unapproved.json)
is deliberately unusable. Test limits are not scientific approval. A reviewer
must supply a baseline, measurements, exposure requirements, limits, and retained
review before a cohort can qualify. The independent input exporter/scorer and
its method also require review; this evaluator does not extract truth
associations or radar tracks from IQ. Missing instrumentation is a blocker, not
permission to synthesize evidence.

## Policy and review

Complete the policy in a new file and retain its exact bytes. Schema
`vectorwarp-release-regression-policy-v1` accepts only the template's fields.
Set `status` to `reviewed-before-cohort` only after review. Both
`reviewedAtEpoch` and `tuningCutoffEpoch` must precede `evaluationStartEpoch`.
`truthUsage` is exactly `offline-evaluation-only`. `minimumFreeBytes` protects
the evidence filesystem; the binding's `monitorMinimumFreeBytes` separately
protects the receipt filesystem. Both reserves must be positive.

Retain the documents addressed by `reviewEvidenceSha256`,
`measurementMethodSha256`, and `selectionMethodSha256`. Review must establish:

- The appropriate baseline and exact artifact/configuration identities for
  paired baseline/candidate replay on the same recorded bytes.
- Selection of complete recording windows, whole passes, independent negative
  controls, and untouched prospective traffic before looking at outcomes.
  Record every eligible completed window, including failures. Audit population
  completeness against the acquisition catalog; do not select by detections,
  truth matches, receiver, or backend.
- Evaluation-only truth definitions for detection opportunities and negative
  adjudication, without truth entering processor features, tracking, or gating.
- Counting of detections, confirmed false tracks, and continuous measured
  tracking. Coasting/prediction, truth-only associations, repeated publication,
  and identity reassignment must not manufacture continuity.
  `continuousMeasurementBackedTrackSeconds` sums each adjudicated target's
  longest uninterrupted measurement-backed run within its whole pass. Both
  versions share the same eligible target-time denominator.
- Adequacy of each stratum's exposure and limits for the release claim. These
  are descriptive paired outcomes, not calculated confidence intervals, an
  operational PFA estimate, or a causal significance test. Claims requiring
  statistical inference need a separately reviewed retained analysis.

Policy `nodes` must exactly match the schema-3 binding. Every node fixes baseline
`sourceCommit` (full 40-character commit), `artifactSha256`, and `configSha256`.
Candidate identities come from the binding. Each stratum
(`retrospective-positive`, `retrospective-negative`, `prospective-positive`,
`prospective-negative`) requires positive `minimumPasses` and `minimumSeconds`.
Positive strata also require positive `minimumOpportunities`; negatives use zero.

Detection and continuity fractions use paired denominators and are checked
separately for retrospective and prospective positives. Minimum fractions must
be greater than zero and at most one. Minimum candidate-minus-baseline deltas
must be in `[0,1]`; zero permits equality and does not claim improvement. Require
a reviewed positive delta when the release claim requires improvement.
False-track counts must not increase over baseline in **any** stratum, including
positive traffic, and candidate false tracks/hour must meet the absolute limit.
`maximumCompletedCycleMs` must be positive and no greater than the binding's
reviewed `maximumCadenceMs`. Percentiles and averages cannot establish deadlines.

## Retained inputs

Use an existing evidence directory of regular files named exactly by lowercase
SHA-256, without extensions. All references must be present and hash correctly.
Opaque evidence, including whole-pass IQ, is hashed as a stream. Structured JSON
documents are bounded to 64 MiB each; split runtime traces into contiguous blocks.
Retain review/qualification documents, recordings, acquisition/adjudication
records, both executions and raw products, and full runtime source traces.
A URI or hash without local retained bytes is insufficient.

The input document has these exact fields:

```json
{
  "schema": "vectorwarp-release-regression-input-v1",
  "bindingSha256": "EXACT_BINDING_FILE_DIGEST",
  "policySha256": "EXACT_POLICY_FILE_DIGEST",
  "cohortStartEpoch": 0,
  "evaluationStartEpoch": 0,
  "evaluatedThroughEpoch": 0,
  "nodes": {
    "REVIEWED_NODE_NAME": {
      "qualifications": {"radar-behavior": "EXACT_QUALIFICATION_DIGEST"},
      "populationSha256": "WHOLE_PASS_POPULATION_DIGEST",
      "runtimeBlocks": ["COMPLETE_RUNTIME_BLOCK_DIGEST"]
    }
  }
}
```

Replace placeholders with actual UTC epoch seconds and digests. Evaluation must
begin no later than the declared cohort start and cover it through a time no
more than 180 seconds old, never in the future. The soak independently checks
coverage of its actual start time. Qualify pre-cohort exposure first so the first
sampler sees passing evidence; do not erase an initial failure to recover startup
acceptance. Node names and complete qualification maps must exactly equal the
binding; every qualification object is hash checked.

Each population object has schema `vectorwarp-release-population-v1`, pinned
`selectionMethodSha256`, `completeFromEpoch` no later than evaluation start,
`completeThroughEpoch` exactly equal to evaluated-through time, and `passes`, an
array of whole-pass measurement digests. All entries are evaluated. Duplicate
pass IDs, reused recording hashes, and overlapping recording windows within a
node are refused. Represent simultaneous targets in one recording window;
overlapping aircraft passes cannot multiply the same exposure.

Each whole-pass object uses schema `vectorwarp-release-whole-pass-v1` and these
remaining fields:

| Fields | Contract |
| --- | --- |
| `passId`, `sourceKind` | Unique nonempty ID, and exactly `recorded-iq`. Synthetic fixtures cannot qualify a release. |
| `split`, `kind` | `retrospective` or `prospective`; `positive` or `negative`. |
| `startEpoch`, `endEpoch` | Whole recording interval with positive duration. Retrospective ends at/before tuning cutoff; prospective starts after cutoff and inside the evaluation interval. |
| `wholePass`, `usedForTuning`, `truthUsage` | Exactly `true`, `false`, `offline-evaluation-only`. |
| `measurementMethodSha256` | Exact policy-pinned scoring method. |
| `recordingSha256`, `acquisitionSha256`, `adjudicationSha256` | Retained source bytes, acquisition/whole-pass boundaries, and independent truth/negative adjudication. |
| `opportunities`, `eligibleTrackSeconds` | Integer detection opportunities and finite target-time denominator; both positive for positives, both zero for negatives. |
| `baseline`, `candidate` | Each has the three identity fields, retained `executionSha256` and `productsSha256`, integer `detectedOpportunities`, finite `continuousMeasurementBackedTrackSeconds`, and integer `falseTracks`. |

Both executions must share the recording, acquisition, adjudication, opportunity
definition, and denominators. Detections cannot exceed opportunities; continuous
track time cannot exceed eligible target time. Execution evidence must permit
review of those statements and confirm truth use only after inference. The
evaluator recomputes stratified metrics from scorer measurements; hashes do not
authenticate the scorer or prove its measurements scientifically correct.

Each runtime block uses schema `vectorwarp-release-runtime-v1`, exact candidate
identity fields, `startEpoch`, `endEpoch`, `captureStart`, `captureEnd`,
`completedCpisBefore`, `completedCpisThrough`, `cycles`, and
`rawEvidenceSha256`. Blocks tile the evaluation interval exactly. Shared capture
boundaries and CPI counters must agree exactly.

`cycles` lists **every** completed processing cycle, with integer `sequence`,
`startedEpoch`, and `completedEpoch`. Array length equals the completed counter
delta; sequences are consecutive. Completion times strictly advance within the
block. Every cycle's complete duration, every interval between completions, and
leading/trailing interval edges are checked. Export actual completed
multi-consumer cycles; ingress timestamps, frame publication counts, sampled
percentiles, and rounded averages cannot substitute.

Each capture snapshot has a nonempty string `epoch` and exact unsigned 64-bit
integer counters: `acceptedSamples`, `droppedSamples`, `discardedSamples`,
`discontinuities`, `requalifications`, `retiredCpis`, `retiredSamples`. Counters
must be monotonic and accepted samples increase in every block. Epoch changes,
preexisting/new loss, and any retirement fail. The exporter must include every
ingress lane and receiver recovery counter without rebasing; raw traces retain
per-lane detail. This offline format requires exact JSON integers, not rounded
JavaScript values or substituted zeros. A reset cannot repair a failed cohort.

The canonical `Timing.cycleEvidence` observation retains eight consecutive
consumer cycles, including warmup, rejected, drain, and final outcomes. All
sequence, monotonic-nanosecond, UTC-anchor, and capture counters use decimal
strings on the native/API boundary. The UTC anchor is fixed for the process;
more than 100 ms of wall-clock drift invalidates evidence. Completed durations
include local destruction, and nested drains are retained as `drainBatches`.
The existing timing socket is best-effort: transport loss never waits on DSP,
and a gap exceeding the retained window invalidates the ledger.

Set `VECTORWARP_CYCLE_LEDGER_DIR` to a unique nonexistent directory before API
startup to retain observations. No collection is enabled by default. The API
keeps at most 32 pending records and 512 MiB of ledger data with a 1 GiB disk
reserve. It never resumes, rotates away, or rebases a failed cohort. Sequence,
clock, counter, queue, disk, or write failures are sticky. Every append revokes
the old checkpoint before writing; a replacement becomes eligible only after
data and checkpoint fsync. A clean shutdown seals the same stream. An exporter
pins one valid checkpoint, reads exactly its byte count, and verifies its hash
and consecutive sequence. Concurrent later appends cannot contaminate that
prefix; truncation, corruption, or an incomplete checkpoint is refused.

FIFO receiver evidence uses the existing coherent sample-position authority
for every physical lane, `processing_epoch()` for receiver discontinuities,
and each FIFO's exact drops plus consumer boundary/stop discards. Processing
retirements retain distinct CPI/sample totals; ingress losses are not relabeled
as processing retirements. Host `Acceleration` and `RadarBank` retain renewed
qualification attempts across input resets; channelizer replacement attempts
join that total. Initial private qualification is not a requalification.
The optional paired queue has no exact lifetime sample-counter capability in
its current module ABI and remains explicitly ineligible. No counter is
invented or silently set to zero to make an unsupported path eligible.

Export a retained checkpoint without contacting or stopping a processor:

```sh
python3 tools/vectorwarp_export_cycles.py \
  --ledger /retained/unique-cycle-ledger --binding /reviewed/immutable-binding.json \
  --node site-a --evidence-dir /retained/sha256 --output /retained/runtime-export.json
```

The output's `runtimeEvidence` list supplies the existing evaluator input's
`nodes[NAME].runtimeBlocks`; its bounds supply the evaluation interval. The
reviewed binding supplies candidate identity and must match the independently
measured live cohort. The retained raw blocks preserve exact nanoseconds and
per-lane counters; runtime-v1's UTC seconds retain its existing JSON-number
representation. This exporter supplies runtime evidence only, not scored radar
outcomes, physical qualification, or release authority.

## Running and publication

After separately reviewing and retaining policy and evidence:

```sh
python3 tools/vectorwarp_release_regression.py \
  --binding /reviewed/immutable-binding.json \
  --policy /reviewed/release-regression-policy.json \
  --inputs /retained/evaluation-input.json \
  --evidence-dir /retained/sha256 \
  --output /monitor/evaluator-latest.json
```

Point `VECTORWARP_SOAK_REGRESSION_RECEIPT` at the output and arrange repeated
evaluation with fresh inputs before each sampler. The command installs no
scheduling and changes no live services. Serialize evaluator then sampler:
reevaluation atomically revokes previous success first, so a concurrent sampler
can intentionally see unavailable evidence and reject the cohort. Concurrent
writers to the same output are refused by a lock.

Exit 0 means all five checks passed this evidence/policy. Exit 1 publishes a valid
schema-3 receipt reporting a rejected check or explicitly inconclusive exposure.
Each check carries `status` (`accepted`, `rejected`, or `inconclusive`) and a
matching `accepted` boolean. No eligible traffic is inconclusive, never a pass;
insufficient exposure alone does not claim a measured regression. An observed
capture or deadline failure still rejects the cohort. Exit 2 means
malformed, missing, stale, unreviewed, or inconsistent evidence; after acquiring
the output lock, the output is an invalid rejection marker. If the filesystem
cannot accept even that marker, callers must treat command failure as unavailable
evidence and stop sampling for acceptance. Freshness alone cannot prove a failed
write was revoked.

The evaluator fsyncs hash-addressed inputs, evaluator/shared binding-validator
source, five per-check results, and a complete report before atomic receipt
replacement and directory fsync. `evidenceSha256` addresses the report with its
retained dependency list and result; per-check hashes address individual
decisions and measurements. Preserve the archive after failure. Hashes establish
integrity, not a signature, independent population audit, or scientific approval.
A passing receipt does not authorize signing, activation, promotion, or release.
