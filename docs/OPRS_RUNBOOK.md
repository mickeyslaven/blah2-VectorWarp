# Running OPRS in another passive-radar project

OPRS requires Python 3.10+ and has no third-party runtime dependency. Run from
the repository root with `script/oprs`.

## 1. Declare the system

Copy one of the manifests in `examples/oprs/`. Select a capability profile from
the specification and declare physical sample units, channel roles, clock/time
class, and bearing capability. Validate it:

```sh
script/oprs validate manifest my-system.json
```

If the system cannot estimate bearing, set `bearing_capability` to false. The
scorecard will report bearing `N/A`, not zero.

## 2. Freeze evaluation before running it

Create an `oprs-evaluation-plan-v1` with a detector-independent physical
opportunity envelope, unique pass/CPI/event-time truth, required domains, and
mission thresholds. Hash the manifest with the canonical hash printed by
`validate`; insert that as `system_manifest_hash`. Compute `plan_hash` over the
canonical plan with the `plan_hash` member omitted. Commit or timestamp this
plan before evaluation.

Training, parameter selection, and evaluation backgrounds must be distinct
recording sessions. Never derive the opportunity envelope from candidate
detector bounds.

## 3. Run deterministic conformance and RF health

```sh
script/oprs generate-suite --profile REF_SURV_2CH --output suite.json
script/oprs make-fixture --output background.blah2iq
script/oprs analyze-iq --manifest my-system.json --iq background.blah2iq --output iq-health.json
```

For a scenario selected from `suite.json`, save it as its own JSON file and run:

```sh
script/oprs inject --manifest my-system.json --scenario scenario.json \
  --input background.blah2iq --output injected.blah2iq --receipt receipt.json
```

The injector is replay-only. In `REF_SURV_2CH`, it verifies that the reference
channel is bit-for-bit unchanged and injects only into surveillance. Array
profiles apply the declared geometry. VectorWarp's `BLAH2IQ` v1 is the initial
adapter; other projects can emit it or implement the same manifest-to-complex-
sample interface.

Replay each blank and injection through the unmodified system under test. Export
one `oprs-trial-output-v1` per independent run. Populate individual CPI rows;
do not submit only aggregates.

## 4. Score each evidence layer separately

```sh
script/oprs score --manifest my-system.json --plan frozen-plan.json \
  --trial trial-1.json --trial trial-2.json --output scorecard.json
```

Exit 0 means the evidence contract was valid; it does not imply qualification.
Exit 2 writes an `INVALID` scorecard and reports the contract violation. Inspect
domain statuses and the mission tuple. Do not compute an overall percentage.

Repeat with separate plans for pure synthetic, real-background injection, and
held-out real aircraft. Publish all three scorecards and their hashes.

## 5. Compare candidates

Prefer the same IQ, truth, plan, seed, and scenario bytes for baseline and
candidate. For live-only comparisons, predeclare interleaved time blocks and
closure/washout rules. Report raw results even when adjustment is used. If
common-support diagnostics, effective sample size, or cluster uncertainty fail
their frozen minimums, label the comparison `NOT_QUALIFIED`.

## VectorWarp and Owl OS adapters

VectorWarp's portable `BLAH2IQ` recording layout is supported directly. Its
constant-cell C++ replay injector remains useful for legacy checks, but OPRS's
reference injector supplies time-varying delay, Doppler, amplitude, azimuth,
blank controls, and multipath. Existing live `evidence-statistics.js` output is
not an OPRS trial because it lacks the frozen CPI contract and supported-track
measurement identities.

An Owl OS node can participate by exporting a completed `BLAH2IQ` recording and
read-only software/config hashes. Network reachability is availability evidence,
not radar-performance evidence, and failed access must be `NOT_QUALIFIED` rather
than scored as a miss.
