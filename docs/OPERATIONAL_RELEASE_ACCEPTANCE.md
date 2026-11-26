# Bounded operational release acceptance

Stable publication uses `tools/vectorwarp_release_regression.py` with policy
schema `vectorwarp-operational-release-policy-v1`. Its small operational module
uses the same retained hash archive, atomic revocation/publication, disk floors,
binding and signing chain as the existing evaluator. It does not change DSP,
tracking, receiver configuration or API defaults. The paired recorded-IQ
[research evaluator](RELEASE_REGRESSION_EVALUATOR.md) remains available, but its
unapproved research strata are not stable-publication prerequisites.

The default remains `frozen-baseline-prospective-operational-v1`. An explicitly
authorized single release may instead use `bounded-functional-v1`, described
below. The two modes are different claims, never interchangeable receipts.

This is a bounded non-regression check, not statistical significance, causal RF
improvement, long-duration reliability, whole-pass continuity, or an operational
false-alarm-rate estimate. The legacy `measurement-backed-track-continuity`
receipt slot means finalized tracked-after-detection yield in operational mode;
`false-track-controls` means the **unmatched/independent-track nonincrease proxy**.
Unassociated radar tracks are not automatically false tracks.

## Freeze before installation

Enable the new observational API code on the existing baseline without changing
its native radar/configuration. Retain its exact installed-tree/source/config
identity and deployment record; a baseline development tree may have
`packageSha256: null` if no package exists. A candidate may never omit its exact
downloadable package digest. After at least 600 seconds of uninterrupted tracker
frame observation, retain a complete baseline federation snapshot. API restart,
frame gap over three seconds, clock regression, or track retention eviction
inside the window makes coverage incomplete. Persisted historical tracks cannot
manufacture post-restart coverage.

Before candidate installation, freeze the baseline bytes and this explicit policy
in the hash-addressed evidence archive. The release maintainer reviews the chosen
operational limits and records the actual review; no scientific scorer or missing
recorded-IQ study is implied. Recommended strict limits are three eligible
finalized passes, zero yield tolerance, maximum backlog 500 ms, and at most
60 seconds between snapshots. Tolerance is an explicit absolute fraction and
must not exceed 0.05. Do not tune it after seeing candidate outcomes.

Policy fields (all required): `schema`, `status: reviewed-before-install`,
`reviewedAtEpoch`, `reviewEvidenceSha256`, positive `minimumFreeBytes`,
`baselineSha256`, `baselineFrozenAtEpoch`, `baselineIdentities`,
`minimumEligiblePasses`, `yieldTolerance`, `maximumBacklogMs`, and
`maximumSampleGapSeconds`. Each baseline identity has `sourceCommit`,
`artifactSha256`, `configSha256`, and `packageSha256`. Node names and unchanged
configuration digests must match the candidate binding. Pin the policy digest
in the existing binding's `regressionPolicySha256`.

## Capture exact snapshots

Retain each raw `/api/federation/statistics-summary` response without rewriting
its fields. The bounded federation pass transport must preserve each pass's
actual policy, contract, timing and sensitivity provenance. Its appended v1
columns dictionary-encode repeated identities; older readers retain scoring
compatibility. Any malformed augmented row refuses the whole batch; it cannot
silently disappear from the eligible denominator. Legacy display-only defaults
never supply acceptance provenance. Older producers omit this evidence and cannot qualify acceptance:
never fill missing pass provenance from a channel's current policy. Install the
reviewed observational code and collect fresh eligible exposure instead.
Wrap the response in a hash-addressed JSON document containing exactly:

```json
{
  "schema": "vectorwarp-operational-release-snapshot-v1",
  "epoch": 0,
  "nodes": {"NODE": {
    "sourceCommit": "FULL_COMMIT", "artifactSha256": "INSTALLED_TREE_SHA256",
    "configSha256": "CONFIG_SHA256", "packageSha256": "PACKAGE_SHA256"
  }},
  "statistics": {}
}
```

Replace the illustrative zero/placeholders with measured values. The `nodes`
identities come from the existing identity probe/deployment record, not an API
claim. The monitor independently rechecks running executables, modules, source,
config and installed-tree hashes. Retain the probe/deployment bytes for approval.
Collect candidate snapshots from a fresh post-install start, with no missing
interval over the reviewed gap. Both nodes must remain available, receiving,
fresh and config-matched; captures must have zero drops and retired CPI/samples.
The existing monitor additionally checks receiver discontinuities, restarts,
discard counters and immutable process identity. Native cumulative maximum
completed-cycle duration must stay <=500 ms, and maximum completion interval
must meet the binding's reviewed cadence bound (at most600 ms). These maxima
retain failures between polls. Backlog must remain bounded.
Retired CPI/sample counts come from the canonical native cycle evidence's latest
`captureAfter`, using exact available counters and a fault-free evidence schema.
The existing cycle contract validates bounded retention, exact anchors and positive
contiguous sequences, ordered non-overlapping times, and monotonic capture counters
before selecting the latest record. Reordering cannot hide a newer retirement.
Missing, faulted or inexact values remain unknown and fail acceptance; absent
legacy flat telemetry fields are never evidence of zero retired work.

Each channel exports its canonical 600-second track window, coverage and policy
identity. The comparison uses only complete fresh candidate windows and unchanged
channel/evaluation policy sets. Tracks overlapping the window remain counted,
including long-lived unmatched tracks born before its moving start. It rejects
any increase in unmatched tracks per channel relative to the equal-duration
baseline. No retention overflow, reset,
missing value or unknown metric becomes zero.

System-wide deduplicated pass rows must be finalized, eligible, wholly inside
the candidate window and unmixed across scoring epochs. A bounded 100-row
transport qualifies only if complete or its oldest row precedes the window.
Every node/channel's retained-since and ended-at bounds must cover the evaluated
interval, and the network common aligned start must not exclude its beginning.
The API's `complete` flag describes its default24-hour request; an explicitly
incomplete24-hour history may still prove the full600-second interval from
consistent retained bounds. Pass-list slicing completeness alone proves no
duration. Pending alignment is inconclusive.
The common retained end must be at least 600 seconds after the candidate start;
freshness allowances cannot shorten the evaluated exposure, even by one millisecond.
Detection yield and tracked-after-detection yield must be positive and not
worse than the frozen baseline beyond the explicit tolerance. No eligible
traffic or incomplete coverage is inconclusive, never accepted.

## Evaluate and publish

Input fields are exactly `schema: vectorwarp-operational-release-input-v1`,
`bindingSha256`, `policySha256`, `evaluationStartEpoch`, `evaluatedThroughEpoch`,
and `snapshots` (chronological retained snapshot digests). The first/last snapshot
must match the declared bounds. Use the existing CLI:

```sh
python3 tools/vectorwarp_release_regression.py --binding binding.json \
  --policy operational-policy.json --inputs operational-input.json \
  --evidence-dir /retained/sha256 --output /monitor/evaluator-latest.json
```

The resulting schema-3 receipt must explicitly carry
`evaluationMode: frozen-baseline-prospective-operational-v1`; research-mode
receipts cannot authorize stable publication. Inconclusive exposure resets the
monitor's clean clock, so initial coverage collection may precede the required
600 consecutive accepted seconds. Capture/deadline/regression faults remain
sticky. All exact-source CI, physical FP32/FP64 product/unified-corpus parity,
package install/upgrade, signed payload/provenance, and release approval gates
remain unchanged.

## Explicit single-release bounded functionality

For an operator-authorized release without a long soak, policy schema
`vectorwarp-functional-release-policy-v1` uses the same evaluator, archive,
monitor, publication verifier and signing chain. It does **not** evaluate
whole-pass detection/tracking yield nonregression: both receipt and retained
report say `passYieldNonRegression: not-evaluated`. Absence of eligible whole
passes is not relabeled as a passing whole-pass test. This mode proves bounded
sampled radar functionality, not whole-pass continuity or long-term reliability.

The binding must explicitly declare `evaluationMode: bounded-functional-v1`,
`functionalAuthorizationSha256`, stable `releaseVersion`, `candidateRunId` and
`candidateRunAttempt`. The retained authorization has exactly `schema:
vectorwarp-functional-release-authorization-v1`, `evaluationMode`, `sourceCommit`,
`releaseVersion`, `candidateRunId`, `candidateRunAttempt`, `nodes`,
`windowSeconds: 600`, `passYieldNonRegression: not-evaluated`,
`authorizedAtEpoch` and a nonempty `reason` recording the actual operator order.
Its nodes map pins both exact source/artifact/config/package identities. Its
timestamp must precede policy review; review and baseline freeze precede the
prospective window. It cannot authorize another source, package run or version.
Never fabricate authorization or measurements. Missing/unknown modes fail closed;
omitting the mode keeps the whole-pass default.

Functional policy retains all common policy fields above, replacing
`minimumEligiblePasses` and `yieldTolerance` with
`functionalAuthorizationSha256`. The frozen baseline still needs complete
600-second per-channel track-population coverage and the same configuration and
evaluation policy, but need not contain whole finalized passes. The input schema
is `vectorwarp-functional-release-input-v1`, otherwise using the same fields.
Each candidate snapshot additionally retains `nativeFrames`, keyed by both node
names, containing the unmodified raw `/api/tracker/native` JSON responses. These
native products describe the node's primary illuminator; proxy comparisons still
cover every channel. Capture snapshot time after the reads, with native frames
no more than three seconds old. Retain the raw bytes/probe records; do not fill
missing fields or derive radar evidence from ADS-B association.

Each node needs at least three fresh native frames with truth-blind detector
diagnostics reporting detections, and at least three ACTIVE detector-measured
observations of the same native track spanning at least one second. All credited
observations must lie in the trailing fixed600-second prospective window.
Native capture intervals and frame sequences must advance without reset; the
separate detector `history_epoch` must remain unchanged and exactly representable.
Measured tracks bind their evidence epoch to that history generation, not the
capture epoch, and require the current sequence and contiguous temporal support.
Predicted/coasting tracks, isolated identities, stale/repeated frames, missing
lineage or a missing node cannot provide continuity. No detections or continuity
is inconclusive, not success. This is sampled measurement-backed continuity,
not a claim that every intervening frame or an entire aircraft pass was tracked.

Both nodes must also pass the unchanged source/payload/config checks, cumulative
capture-loss/discontinuity/retirement checks, CPI/cadence/backlog limits and
complete600-second unmatched/independent-track nonincrease proxy. No false-alarm
rate is inferred. The monitor's separate600-consecutive-accepted-second clock
is unchanged: initial evidence collection plus monitor acceptance normally takes
about20minutes. Inconclusive samples reset that clock; actual faults remain
sticky. Exact-source hosted CI, physical FP32/FP64 product/unified-corpus parity,
package installation/upgrade and all signatures remain mandatory. Release notes
must disclose the selected mode and unevaluated whole-pass yield claim.
