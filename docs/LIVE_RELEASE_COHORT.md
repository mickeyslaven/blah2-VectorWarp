# Live package cohort and release soak

This is the operator contract consumed by the native/package startup gate,
`tools/vectorwarp_soak_identity.py`, `tools/vectorwarp_two_node_soak.py`, and
`script/verify-release-soak.py`. Receiver/backend choice never changes this
contract. A binary overlay is not a release package.

## Activation boundary

The package upgrade helper and both native service `ExecCondition` lists run
`vectorwarp-cohort-check --preflight`. It reads the effective, merged service
configuration, including drop-ins. The API entrypoint and working directory,
processor executable, config path, and any native library search path must
resolve to one selected release. An already active peer must use that same
cohort. Wrapper commands, foreign preload/search paths, and unreviewed
environment files fail closed with exit 78. Nothing in this check starts,
stops, or rewrites a service, configuration, or symlink. Explicit systemd
`BindPaths`, `BindReadOnlyPaths`, `RootDirectory`, and `RootImage` overrides are
refused. The running artifact and configuration are also measured through each
service's process-root namespace, so removing a drop-in without restarting the
service cannot conceal a surviving code overlay.

For the existing `VECTORWARP_NODE_ROLE=coordinator` role, only the API must run;
the local processor must remain inactive, including when masked. The complete
artifact still requires its declared native hashes, and the running API role
must match its service plan. Coordinator identity is not radar-behavior or
hardware qualification, and does not satisfy a radar node's soak requirements.

The runtime probe additionally hashes the entire installed tree, config, host
identity, actual running executables, mapped project DSOs, and native child
executables. It verifies mapped file device/inode identity against disk and
rejects foreign/deleted modules. When Btrfs reports its backing device rather
than the mounted subvolume device in `/proc/PID/maps`, the inode must still
match and the exact kernel `map_files` bytes must equal the release file. The package tree must predate both service
starts: Node's cached JavaScript cannot be re-attested by replacing files in
place and hashing their new bytes. Boot, service start and child-process identities
detect restarts. Project modules outside the selected package cannot qualify
this package soak; this is not a change to the supported local module ABI.

Before an approved activation:

1. Produce the complete candidate package from the reviewed source and retain
   the CI run ID/attempt, package filename and SHA-256. Include API/UI, processor,
   libraries, private Node runtime and integration helpers. Never fill a binary
   overlay with arbitrary older API files. Extract the package without running
   maintainer scripts and use `verify-release-soak.py`'s payload calculation to
   record its artifact digest.
2. Record both nodes' config hashes, `current` targets, effective unit files,
   drop-ins and intended active states. Preserve those as rollback evidence.
   Stage a separate immutable complete release, never overwrite a running tree.
3. Quiesce the affected API/processor through the supported upgrade boundary.
   Preserve receiver ownership and configuration. Reconcile only the old code
   selection overrides (`ExecStart`, API working directory, library path and
   stale startup-check paths); retain reviewed resource/device settings.
4. Select the complete candidate and reload units while the pair is stopped.
   Run `/usr/bin/python3 /opt/vectorwarp/libexec/vectorwarp-cohort-check
   --preflight`. Inspect `systemctl show vectorwarp-api vectorwarp-processor
   --property=ExecStart,Environment,EnvironmentFiles,WorkingDirectory`.
   Do not start the pair if the preflight fails or `current` resolves elsewhere.
5. Restore only the previously intended active services, API first; wait for
   API readiness, then start the processor. Run the full identity probe as root.
   Its artifact digest must equal the extracted package payload; compare the
   running Node/processor hashes with that package too. Verify live backend,
   config revisions, zero-loss counters and fresh advancing frames on both
   nodes. Any startup failure leaves this candidate unqualified.

These steps require separately authorized live activation. Do not retain prior
soak time across this transition. An administrator can remove an ExecCondition;
release acceptance still requires the independent measured runtime probe.

## Frozen binding (schema 3)

Create a reviewed JSON binding with `schema: 3`, full `sourceCommit`,
`candidateRunId`, `candidateRunAttempt`, `regressionPolicySha256`, and a positive
`monitorMinimumFreeBytes` reserve for the monitor's state filesystem. Its
`nodes` object contains one or more operator-named nodes, each with:

- resolved `release`, `artifactSha256`, `configSha256`, `hostIdSha256`;
- `packageFilename`, `packageSha256`, explicit `acceleration` and
  `clutterAcceleration` expectations, and the reviewed `maximumCadenceMs`
  (at most 600 ms; select the actual approved cadence bound, not this ceiling);
- `runtimeExecutables` and `runtimeModules` copied from that node's successful
  probe, with both API and processor service keys;
- `endpoint`, an HTTP(S) origin, and `ssh`, either null for a local measured
  identity probe or an authenticated SSH destination;
- `disk: {"path": "/var/lib", "minimumFreeBytes": 200000000000}` using the
  reviewed absolute filesystem path and reserve for that host;
- `qualifications`, a map of qualification names to exact retained evidence
  SHA-256 values. Every node requires `radar-behavior`; any non-CPU ambiguity or
  clutter stage additionally requires `physical-gpu`. Other declared hardware
  or platform qualifications remain in this same map.

All host digests must differ. The current identity adapter measures Linux
systemd/proc cohorts; other OS probes require a separately reviewed adapter,
not invented identity evidence. Package metadata is checked against downloaded
same-source CI artifacts by the signing gate; a claimed package hash alone is
not a package verification. Never revise a binding in place to follow drift.

Install the exact frozen monitor/probe source together and configure the
sampler with:

```ini
Environment=VECTORWARP_SOAK_STATE=/var/lib/vectorwarp-soak-UNIQUE-COHORT
Environment=VECTORWARP_SOAK_BINDING=/path/to/immutable-binding.json
Environment=VECTORWARP_SOAK_REGRESSION_RECEIPT=/path/to/evaluator-latest.json
TimeoutStartSec=150
```

Use a unique empty state directory after activation. Sample every 60 seconds,
with no gap over 180 seconds; `TimeoutStartSec` must permit both bounded identity
probes and telemetry calls for every declared node. Keep the entire first
24-hour window. Monitor/probe
or binding changes require another state directory. The ledger retains sticky
restart, capture-loss, binding and regression failures even when rows rotate.
Any unhealthy sample resets the clean clock; a sticky failure requires a new,
explicit cohort. Never rebase counters or edit a failure out of the ledger.

Schema-2 bindings and old receipts remain historical evidence and cannot qualify
this monitor. Review a schema-3 binding and start a fresh cohort. The historical
script filename, default state path, and `VECTORWARP_SOAK_HP_HOST` unbound
diagnostic CLI remain for deployed callers; unbound diagnostics never qualify a
release and never choose its node topology or backend requirements.

## Regression evaluator receipt

The soak does not invent statistical thresholds or equate aggregate counts with
an approved detector/tracker comparison. The external evaluator owns whole-pass
selection, baseline, minimum exposure, regression decision and measurement of
retired CPIs/samples. The reviewed policy bytes are pinned in the binding.
HomePortal's `/api/federation/statistics-summary` can supply observations, but
its raw response is not itself an acceptance decision. ADS-B information stays
evaluation-only and never enters processor inference.

The evaluator atomically publishes this schema on the monitor host. Its node
keys and each `qualifications` map must exactly match the reviewed binding:

```json
{
  "schema": 2,
  "bindingSha256": "SHA256_OF_EXACT_BINDING_FILE_BYTES",
  "policySha256": "SHA256_OF_REVIEWED_EVALUATION_POLICY",
  "evidenceSha256": "SHA256_OF_RETAINED_EVALUATOR_INPUTS_AND_RESULT",
  "evaluationStartEpoch": 0,
  "evaluatedThroughEpoch": 0,
  "nodes": {
    "site-a": {"retiredCpis": 0, "retiredSamples": 0,
      "checks": {
        "detections": {"accepted": true, "evidenceSha256": "SHA256_OF_DETECTION_RESULT"},
        "measurement-backed-track-continuity": {"accepted": true, "evidenceSha256": "SHA256_OF_CONTINUITY_RESULT"},
        "false-track-controls": {"accepted": true, "evidenceSha256": "SHA256_OF_NEGATIVE_CONTROL_RESULT"},
        "cpi-deadlines": {"accepted": true, "evidenceSha256": "SHA256_OF_COMPLETE_CPI_DEADLINE_RESULT"},
        "capture-health": {"accepted": true, "evidenceSha256": "SHA256_OF_CAPTURE_RESULT"}
      },
      "qualifications": {"radar-behavior": "SHA256_OF_REVIEWED_RADAR_EVIDENCE",
        "physical-gpu": "SHA256_OF_REVIEWED_PHYSICAL_GPU_EVIDENCE"}}
  },
  "regressionDetected": false
}
```

Use actual UTC epoch seconds: evaluation must begin no later than the cohort,
and its latest coverage must be no older than 180 seconds, never in the future.
Keep the hash-addressed evidence and policy for reviewer inspection; the digest
is an integrity binding, not an evaluator signature or independent proof that
the policy is scientifically valid. Missing/unavailable evidence, any nonzero
retirement, or a reported regression prevents release acceptance. A policy and
running evaluator must exist before starting the release cohort.
Each node must explicitly pass all five checks with retained, hash-bound results.
The reviewed policy defines their measurements, thresholds and required exposure;
inadequate positive or negative exposure is not a pass. CPI deadline evidence must
cover completed cycles throughout the interval, including between sampler polls;
sampled `cpi` percentiles or average cadence alone cannot establish that result.
Schema-1 evaluator receipts lack this coverage contract and cannot qualify a new
cohort. A false per-check result rejects the cohort even if the aggregate
`regressionDetected` value is false.

The executable producer is `tools/vectorwarp_release_regression.py`; its
[policy and retained-input contract](RELEASE_REGRESSION_EVALUATOR.md) describes
the reviewed baseline, separate positive/negative retrospective/prospective
exposure, complete CPI ledger, capture counters, and atomic publication.
`config/release-regression-policy.unapproved.json` is an intentionally
unapproved template and cannot qualify any cohort. A reviewed input scorer and
policy are prerequisites; synthetic test fixtures do not supply that review.

Release approval requires the full 86,400 clean seconds, both startup and steady
state acceptance, the fresh regression receipt, and successful same-package
verification by `script/verify-release-soak.py`. Signing may change the RPM
container signature only; it may not rebuild or modify the soaked payload.
