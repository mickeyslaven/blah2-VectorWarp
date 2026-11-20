# Physical replay fault evidence

These hooks exercise the production portable/resident worker and its existing
recovery policy. They are not device qualification by themselves. Host contract
tests and worker doubles must never supply physical qualification receipts.

The isolated replay and physical qualification runners pin the resolved processor,
actual sibling worker and available module files before launch. Each spawned worker
must identify itself, then emit a `VECTORWARP_GPU_ARTIFACT` module-load receipt with
path, SHA-256, ABI, requested device and enumerated driver identities before backend
construction/warmup. Parent spawn records account for every worker, including failed
ones. Missing, late, mismatched or changed-artifact evidence rejects acceptance.
Only attempted modules enter the runtime cohort; unused resident modules are not
claimed as loaded. Artifact receipts use a private connected UNIX datagram channel,
separate from normal stdout/stderr. The runner installs `capture.replay.artifacts`
with `enabled: true`, confirmation `offline-gpu-artifact-binding-v1`, its explicitly
passed descriptor and a fresh launch nonce. Live capture rejects that configuration;
inherited artifact environment variables alone never activate it. Each worker spawn
strips inherited artifact pins/recipes and reconstructs only the authorized launch
state. Worker descriptor 6 carries receipts; protocol descriptors 3/4 and optional
qualification control descriptor 5 retain their existing roles.

Every artifact send uses `MSG_DONTWAIT`, at most 4096 bytes and 512 records per
process. The runner retains at most 1024 datagrams and rejects malformed, truncated,
missing or excess evidence. A full/stalled receipt channel fails the authorized
offline run promptly. Normal logging remains unchanged. Artifact binding preserves
module/numerical ABIs and does not enable replay fault injection. The isolated
runner retains automatic recording-format detection; physical qualification still
requires the explicit native formats described below.

Explicit opt-in, inside the replay configuration:

```json
{
  "state": true,
  "format": "blah2",
  "qualification": {
    "enabled": true,
    "confirmation": "offline-physical-gpu-qualification-v1",
    "fault": {
      "kind": "commit",
      "route": "resident",
      "lane": 0,
      "frame": 12
    }
  }
}
```

`fault` is optional for diagnostics-only runs. An existing `qualification` key
with missing/false enablement, wrong confirmation, non-native format, live
capture, unknown fault, out-of-range lane/frame, or incompatible route is an
error. Lanes are zero-based in the enabled configuration order. Runtime frame
numbers are one-based *captured CPI ordinals*, monotonic across recording loops,
not worker request IDs or published frame counts. Qualification admits at most
256 captured CPIs. Fault targets must actually be reached; absence of a matching
worker event is a failed qualification, never a skip or successful refusal.

| Kind | Route | Frame | Injection boundary |
| --- | --- | --- | --- |
| `startup` | `portable`, `resident` | 0 | Initialized real worker IPC, before module factory |
| `allocation` | `portable`, `resident` | 0 | Real factory with a one-byte allocation ceiling |
| `prepare` | `resident` | 1–256 | Reference completed, before reference-ready acknowledgement |
| `commit` | `resident` | 1–256 | Actual backend commit completed, before acknowledgement |
| `worker-exit` | `portable`, `resident` | 1–256 | Frame request validated, before execution; worker exits 86 |
| `timeout` | `portable`, `resident` | 1–256 | Same boundary; worker blocks until existing parent timeout kills it |
| `numeric-rejection` | `portable`, `resident` | 1–256 | Actual computation completed, output made nonfinite before acknowledgement |
| `input-gap` | `capture` (lane 0) | 1–256 | One extracted CPI discarded; next real sample range invokes existing full reset |
| `cpu-oracle-reset` | `host` | 1–256 | Exact lane adaptive-state reset before processing its unchanged canonical CPI; explicit CPU multi-lane replay only |

No environment variable enables parent qualification. Valid configuration
creates a private unlinked control file. Only explicit worker spawns inherit its
read-only descriptor 5 and a nonce-bound recipe; arbitrary inherited recipes
are stripped. Replacement worker executables are prohibited. The default worker
descriptor/environment policy is unchanged. No worker protocol or module ABI is
changed. Synthetic warmup sees capture frame 0 and cannot consume a runtime
fault. Each worker injects its requested fault once.

Each event is an individual bounded stderr write prefixed with
`VECTORWARP_GPU_QUALIFICATION ` followed by schema-1 JSON:

```json
{"schema":1,"boundary":"commit","route":"resident","lane":0,"pid":120,
 "workerPid":120,"frame":12,"captureEpoch":7,"captureFirstEpochSample":22000,
 "captureEndEpochSample":24000,"logicalFrame":20,"consumedSequence":20,
 "fault":"commit","device":"0:2:0"}
```

Worker events identify the requested fault and successful `frame-computed`
(portable), `frame-prepared` and `frame-committed` (resident) boundaries. Parent
events identify `worker-spawned`, `worker-reaped`, `capture-owned`,
`input-gap-discarded`, `history-reset`, `cpu-recovery-complete` (runtime resident
lane fallback), and `publication-complete`. A reaping event is emitted only after
`waitpid` observes that child. Parent reaping records bind through `workerPid` to
the original spawn; their route is `parent` and their lane is not a lane claim.

Allocation cases additionally require `allocation-budget-rejected`, emitted at
the actual native allocator's failed reservation, with `allocationBytes` greater
than its `allocationLimit` of one byte. These fields are zero on other events.
This second event prevents a module-load or driver failure from being counted
as successful allocation-fault coverage.

`publication-complete` means all existing local writes completed; it is not an
application acknowledgement or a claim that numerical products were emitted.
The runner must independently collect every sink, bind products/timing/lane
records by their sample identities, count each publication, and compare actual
CPU recovery with an oracle reset at the same boundary. Fail-closed cases must
have no partial/stale map, detection, track or bearing products for the failed
frame. A fault marker alone proves neither recovery nor publication integrity.

The `cpu-oracle-reset` control emits a frame-bound `history-reset` but no GPU
execution evidence. It resets only the selected lane's reference, temporal and
tracker state, retaining capture/channelizer history exactly as resident
fallback does. It cannot be enabled by live capture, an environment variable,
single-lane processing, `auto`, or a GPU mode. Dropping the preceding CPI is not
an equivalent CPU control because that also resets the channelizer.

Single-lane resident failures stop the route. Multi-lane resident power can
recover on the exact unchanged canonical host CPI after resetting host adaptive
state; resident-only semantics stop. Portable fallback retains its existing
stage-specific policy. Multi-lane `auto` may select resident on an FP64 device,
so a portable fault must not be claimed on that route. Qualification does not
override backend selection to manufacture coverage.

Complex/reference/bearing diagnostics are separately bounded under
`qualification.observer`; see the supplemental runner and its observer schema.

## Real recording acquisition identity

The existing confirmation gate also accepts explicit native `format: mchq`
(MCHQ/MCHT). Exact acquisition admission is opt-in:

```yaml
qualification:
  enabled: true
  confirmation: offline-physical-gpu-qualification-v1
  acquisition:
    require_exact: true
    recording_sha256: <separately trusted SHA-256 of the complete IQ artifact>
    acquisition_sha256: <separately trusted acquisition JSON SHA-256>
```

Portable event clips embed `vectorwarp-recording-acquisition-v1` in BLAH2IQ v2;
their trusted complete-file hash also authenticates the embedded metadata.
Manual recordings and native Kraken clips publish an exclusive, atomic adjacent
`<IQ file>.acquisition.json` manifest bound to the complete finalized IQ SHA-256.
Adjacent manifests require **both** external identities. A manifest's own
digest is an integrity check, not an authentication root. Producer hardware
signatures, calibrated RF accuracy and exact UTC are not claimed.

The contract retains receiver details, requested settings, applied gain strength,
ordered reference/surveillance roles, coherence declaration, confirmed geometry
and its content hash, active manifold hashes, original configuration hash and
recording-producer source hash. Epoch/sample/frame counters are decimal strings
to preserve values above 2^53. Every frame records its original source interval
and selected slice. Native Kraken clips also retain original per-channel ADC
I/Q means: replay conditions trimmed edge samples exactly as the live full-frame
decoder did, including after seeks. Legacy native clips without this contract
retain their original playback behavior and cannot claim exact acquisition.

USRP applied getters are admitted only during a sample-aligned qualified epoch.
Kraken gains are explicitly `sample-aligned-receiver-header`, not independently
measured RF gain; Heimdall does not expose serial identity, which remains
explicitly unavailable. Exact acquisition is sample/metadata reproducibility,
not receiver-serial attestation. RSPduo and dual HackRF retain requested settings; their current
backends cannot prove sample-aligned applied gain, so exact acquisition admission
fails. This capability limitation does not select different radar algorithms.
The exact gate checks the explicitly configured replay channel roles, coherence,
geometry and manifold identities against the recorded contract. Full YAML/code
hashes identify the producer; replay necessarily has a different input/qualification
configuration, so these are not silently substituted into processing settings.

Acquisition metadata is bounded to 1 MiB, 4,096 frame slices, 64 KiB each for
receiver details and geometry, and 32 manifold identities. Portable clips fail
if these bounds/settings continuity cannot be retained; manual/native recordings
can finish with an explicit incomplete manifest when frame lineage exceeds its
bound. Ordinary BLAH2IQ v1, legacy raw, MCHQ and MCHT playback remains available.
Missing, inconsistent or tampered contracts never satisfy exact admission.
`replayProvenance` exposes `acquisitionIntegrityValid`, `acquisitionHashTrusted`,
`exactAcquisition`, `gainProvenance`, `acquisitionReason` and both SHA-256 identities.

Geographic replay has a separate fail-closed capability. New acquisition contracts
retain `geographicSites` and their canonical `geographicSitesSha256`: bounded Rx,
primary Tx, and up to 16 illuminator IDs/carriers/sites. Coordinates and optional
site IDs/names are operator declarations made at acquisition, not surveyed or
hardware-attested locations. Different native/API lane declarations are rejected.
Legacy contracts without sites remain playable without geographic localization,
truth association or fusion. Missing, malformed, self-hashed-only, stale, or
wrong-recording site evidence never falls back to current `config.location`.

Ordinary replay can independently pin `capture.replay.recording_sha256` and,
for an adjacent acquisition manifest, `capture.replay.acquisition_sha256`.
Obtain those pins from the trusted acquisition ledger, not the file's own claims.
The existing exact-qualification acquisition pins also authenticate geography.
Applied-gain readback is not invented as a prerequisite for authenticating an
operator's recorded site declaration. Native site-dependent coherent processing
requires authenticated recorded sites; non-geographic replay remains available.
The API exposes capability/reason/sites through `replayProvenance` and
`/api/replay/status`, and uses one resolver for localization, truth projection,
observation bundles and fusion. Timeline delay/Doppler conveniences are reprojected
from raw evaluation truth with recorded sites. UI replay profiles no longer
overwrite live site settings. Switching replay files clears the old trust pins.

Synthetic physical-corpus fixtures now embed the same bounded site/lineage
contract and pin their generated IQ digest. They explicitly set
`metadataAvailable:false` and `fixtureProvenance:synthetic-generator-not-hardware`:
site authentication is testable without manufacturing an exact hardware
acquisition/readback claim. This is arithmetic/fixture evidence, not field-RF or
surveyed-site qualification.

Host fixtures cover real recorder/writer/reader paths, original-frame trim/seek
equality, all four receiver topology shapes, requested-only rejection, malformed
fields, payload/metadata tampering, external trust anchors and legacy playback.
They are not physical receiver or GPU qualification. Field evidence still needs
trusted corpus hashes, truthful applied-gain capability and the existing leakage,
false-alarm, route, numeric and bounded-resource release gates.
`test/recording/replay_experiment_ledger.py` accepts `blah2iq-v1`, `blah2iq-v2`
and native `mchq`. Its optional `recording.acquisition` controls are
`{"require_exact":true,"sha256":"<trusted acquisition SHA-256>"}`; the existing
`recording.sha256` remains the independently supplied complete-IQ identity.
Exact admission also binds the decimal acquisition epoch to `hardware_epoch`.
The acquisition hash can be omitted only for embedded metadata covered by the
trusted IQ hash. Self-declared adjacent hashes never establish exact trust.

Build `inspectRecordingProvenance` before verifying acquisition contracts. The
ledger invokes this bounded offline helper (30-second deadline, no shell,
receiver or GPU) to reuse the production parser and RapidJSON canonical hashing;
Python JSON float serialization is not a substitute. An operator can explicitly
select a trusted helper using `--recording-inspector`; recording metadata cannot
select executable paths. Missing helpers or failed admission are errors, never
successful skips. Legacy v1 without acquisition metadata retains its standalone
framing validation and is explicitly reported as non-exact.

## Immutable native-plan and recorded golden parity

`RadarPlan` serializes all 34 resolved detector/tracker policy fields once, in
fixed order with locale-independent round-trip numbers. The existing offline
observer emits this string as `resolved_product_plan`; live publication and
numerical policy are unchanged. CPU/portable/resident receipts require matching
SHA-256 identities. The composite identity includes configured processing
intent, canonical rate/carrier/decimation/axes, reference roles and synthesis,
geometry, and content-pinned manifolds. Backend selection is excluded. The
native direct/channelized regression compares actual emitted plan identities
alongside maps, products, disabled stages and temporal reset behavior.

For operational CPI sizes, the same observer accepts `summary_only: true`.
It omits IQ/CAF arrays but retains native plan, axes and detector-population
observations. This mode permits at most 256 frames, 16,777,216 samples/channel,
4,194,304 map cells and 256 MiB of observer output. It cannot request clutter
sample snapshots. The replay runner still retains every public map, detection,
track/history, bearing and exact publication interval; summary mode is not
counts-only qualification. Small synthetic fixtures retain full complex parity.

`test/gpu/recorded_iq_qualification.py --capture NAME` collects one `captureRuns`
entry from an independently pinned plan using the existing corpus lifecycle.
Each entry supplies pinned `config`, `binary`, `inspector`, `recording` references
(`path`, `sha256`), `deviceClass` (`cpu`, `fp32`, `fp64`), optional physical `device`,
`maximumFrames` and `timeoutSeconds` (at most 1800). The config must explicitly
select replay, non-looping native input, an execution backend and exact acquisition
pins. Only private peer ports/output paths and offline observer settings are
assigned by the harness; processing intent is preserved. The output is an
exclusive run JSON with acquisition, artifact, plan, execution and full products.
Legacy byte-pinned recordings lacking exact acquisition remain non-exact; do not
manufacture metadata or treat admission/timeline sidecars as hardware attestation.

`--golden-only` consumes pinned `runs` and `goldenPairs` in the existing
`vectorwarp-recorded-iq-qualification-plan-v1` plan. Each pair supplies `id`,
`baseline`, `candidate`, `planSha256ByLane`, and nonempty `requiredPlanFields`;
optional `requiredProcessFields` uses dotted process paths. `minimumCounts`
supports `frames`, `detections`, `operationalTrackFrames`; `requiredTrackStates`
requires actual public lifecycle exposure. A different baseline build must have
an explicit `baselineIdentity` containing its source/artifact pins. Every
`featureManifest` entry names `pairs` and concrete required fields, states or
minimum counts. Unmeasured coverage, plan drift, backend substitution, missing
frames or changed IDs/history/states fail acceptance. Exact temporal support and
sample lineage are retained. Detection delay is compared in km and Doppler in Hz;
tracker bins and associated histories are converted to km with the actual lane
rate. The existing two-decimal serialization envelope is .020001 plus 1e-7
relative; tracker delay tolerance is converted by the same physical scale.

The exclusive `recorded-golden-parity.json` receipt and content-addressed evidence
retain full paired runs and a publication lifecycle ledger. `passed: true` is
paired numerical parity only; `releaseQualified` remains false. Synthetic nulls,
first-publication events and track removal do not imply whole-pass RF efficacy,
native tentative birth identity, or adjudicated false-track exposure.

Depth-eight M2/N9/delete7, M3/N17/delete7 and M4/N25/delete7 are distinct valid
configuration cohorts, not interchangeable golden policies. Give each cohort its
own same-plan CPU/GPU baseline, and retain its exact motion/fractional, fusion,
reference, bearing and manifold policy. Choosing between cohorts needs independent
whole-pass/negative/holdout evaluation of confirmation latency, coverage,
continuity/coast and false tracks. Golden parity must never waive a changed M/N,
depth or deletion policy to select a winner.

## Paired recorded-IQ tracking-policy scoring

The same offline qualifier now has a separate `--policy-comparison` mode. It
consumes completed canonical replay runs; it does not launch replay, process IQ,
alter configuration, or select a policy. Invoke it with `--plan PLAN.json
--plan-sha256 AUTHORIZED_SHA256 --output NEW_DIRECTORY --policy-comparison`.
The plan's independently authorized SHA-256 is mandatory. Existing capture,
same-plan golden parity and release/pre-soak modes retain their contracts.

The `vectorwarp-recorded-policy-comparison-plan-v1` plan requires `sourceSha`,
`artifacts`, pinned `runs`, `runPolicies`, `policyPairs`, `cohortManifest`,
`phase` and positive `matchingGates` (`delayKm`, `dopplerHz`). A pinned reference
is `{ "path": "retained.json", "sha256": "..." }`; relative paths resolve
against the plan directory. Each pair declares `id`, `baseline`, `candidate`
and pinned `labels`. Each run declaration requires:

- `configSha256` for the captured input configuration and
  `resolvedConfigSha256` for canonical JSON of the actual harness configuration;
- `planSha256ByLane` for every actual composite native/configuration plan;
- `calibrations` matching the retained content pins (empty only without files);
- exact `harnessBindings` for `capture.replay.file`,
  `capture.replay.qualification.observer.directory`, `network.ports`, `save.path`;
- exact `fields` for the nine process-relative paths
  `detection.track_before_detect.{enable,max_cpis,motion_bins,delay_motion_bins,doppler_motion_bins,fractional_delay}`,
  `tracker.initiate.{M,N}` and `tracker.delete` (expand these to dotted keys).

These declared processing fields are the only allowed policy differences.
Pairs require a depth-one disabled-TBD control and depth-eight enabled-TBD arm,
both retaining `max_cpis: 8`, `integration_cpis: 1`, and
`integration_motion_bins: 0`; each pair shares M/N/delete, limited to 2/9/7,
3/17/7 or 4/25/7. Motion/fractional fields and native composite plans remain
independently pinned, never inferred from results. Every other configuration,
native plan, physical geometry, calibrated manifold, device/backend and source/
artifact identity must match, including across cohorts for the same recording.
The four harness path/port fields are removed only after exact binding checks.
Each pair uses identical contiguous, fixed-width CPI sample intervals and epoch.
Exact acquisition admission, finite native observations, explicit execution,
complete public products and zero unexplained replay loss remain mandatory.
Injected echoes/faults and truth used for inference are refused.

`cohortManifest` pins a reviewed `vectorwarp-policy-cohort-v1` ledger with
nonempty pinned `basis` inventories and `recordings` entries containing
`recordingSha256`, `source`, `partition`, `passIds`, `wholePass`. The trusted
qualification workflow must review the ledger against those inventories; a
hash proves identity, not independent truth or historical non-exposure.
Whole-pass IDs cannot cross partitions. Development accepts only `fit`,
`development_calibration` and `development_only`; lineage-review and reserved
data are refused. No source/partition pooling or overlapping exposure within
a recording/lane/M cohort is allowed.

Labels use `vectorwarp-policy-labels-v1`, binding `recordingSha256`,
`acquisitionSha256`, `captureEpoch`, `source`, `partition`, `timingQualified`.
Their `passes` specify `id`, `lane`, sample-aligned `entrySample`/`exitSample`
and `wholePass`. Their `frames` cover every published CPI exactly once with
`lane`, `firstSample`, `endSample`, `exposureValid`, `truthEligible`, and
`targets` (`passId`, `delayKm`, `dopplerHz`). Ineligible truth has no targets;
eligible CPIs must include every declared pass covering that interval. Labels
are joined only after production inference and cannot supply radar features.
Ambiguous multi-pass matches are reported and are neither credited matches nor
unmatched observations.

Holdout mode additionally requires pinned `selectionReceipt` and `holdoutAudit`.
The `vectorwarp-frozen-policy-selection-v1` receipt has `frozen: true`, exact
source/artifact pins, `policy` (`depth`, `M`, `N`, `delete`) and all nine exact
candidate `policyFields`. The `vectorwarp-holdout-exposure-audit-v1` receipt
requires `untouched: true`, the exact `cohortManifestSha256`, and audited
`passIds`. Only `reserved_holdout` entries are then admitted. This is an
independently reviewed exposure assertion, not something the scorer can certify
from replay outcomes. No policy search is permitted in that phase.

`policy-comparison.json` retains content-addressed plans, inventories, labels,
runs and audits. Per-pair/per-lane metrics separate admitted and truth-eligible
CPIs, exposure seconds, detection/ACTIVE/coasted coverage, pass detection and
confirmation, entry/evidence-to-confirmation latency, continuity gaps/splits,
concurrent identities, coast duration, disappearance versus configured delete
boundary, native inactivity/tentative-expiration counters, unmatched ACTIVE
first-publications and unmatched ACTIVE/coasted track-seconds per labelled hour.
Tracker bins are converted using each actual lane's sample rate. Initial ACTIVE
tracks are left-censored; ending tracks are right-censored. Unavailable exposure,
truth gaps and incomplete passes censor whole-pass outcomes. Full-depth
confirmation opportunity additionally requires a contiguous depth-times-M window;
this is not a claim that lower-bank confirmation cannot occur earlier.

`passed: true` means the pinned comparison was valid and scored, not that its
outcomes won: lifecycle-boundary violations remain visible, `policySelected`
is null and `releaseQualified` is false. Native tentative identities are not
published. An unmatched ACTIVE first-publication is not native birth and is
**not an adjudicated false track**; `falseTracks` stays null. No whole-pass
efficacy, false-track bound, untouched holdout, calibrated acquisition or policy
winner is manufactured from short historical clips or unit fixtures. The
November 19 readiness inventory still needs exact admitted candidate runs,
qualified timing/whole-pass labels, negative exposure and an independent
holdout exposure audit before a selection decision.

## Bounded immutable-clutter checkpoint qualification

`captureRuns.<name>.qualificationKind: clutter-echo-v1` is a separate first-path
qualification producer. Ordinary `golden-v1` capture remains unchanged, uninjected
and summary-only, and refuses a private clutter checkpoint. The bounded path
requires explicit `gpu` (portable) acceleration, exact completed native IQ,
single-lane clutter, authenticated executable/module receipts and a complex
clutter observer. CPU, resident, multilane, live input, looped input and fault
injection are refused. There is no live configuration, API or UI fixed-mode
setting and no general `GpuBackend` checkpoint control.

The independently pinned input config declares
`capture.replay.qualification.clutter_checkpoint` with:

- `mode`: `export`, `fixed` or `refit`; an absolute `path`; `source_sha`; and
  `sha256` for both apply modes (absent for export).
- `epoch`, `first_sample`, `warm_frame`, `warm_first_sample`, `warm_end_sample`,
  `injection_frame`, `injection_first_sample`, and boolean `injected`.
- Exactly one complete warm CPI boundary: `injection_frame = warm_frame + 1`
  and `injection_first_sample = warm_end_sample`. Frame numbers start at one;
  private startup/oracle frame zero is not recorded RF exposure.

Both paired arms configure the same enabled `inject_echo` and signed-Doppler
parameters. The private descriptor sets `injected: false` for the control and
`true` for its paired injected arm. The injector consumes unmodified samples
into its delay history throughout the muted prefix; injection begins only at
the declared boundary. Export is always uninjected and exclusively creates a
new checkpoint file. Retain and independently pin that file before authorizing
either paired apply run; missing, changed or stale bytes fail closed.

The checkpoint stores the actual initial FP64 coefficients and actual
post-cast FP32 correction coefficients separately for every physical
surveillance channel, including partial final batches. Both apply arms replay
the complete unchanged prefix and must reproduce its exact coefficients and
prefix digest at the checkpoint. The identity binds source and executed
artifacts, semantic configuration, calibration contents, exact acquisition,
sample/CPI boundaries, geometry/channel order, device/driver/capability and
portable FIR layout. Only harness paths/ports and the authenticated per-arm
qualification block are excluded from semantic config comparison; the retained
run still pins the complete resolved configuration.

After freezing, current-frame reference preparation and the existing two FIR
and merge stages remain unchanged. Covariance, factorization, both correlation
submissions and both coefficient solves are skipped; actual native stage counters
must remain zero. Parent FP64 surveillance subtraction and rounding order are
unchanged. Both startup and combined qualification must have completed before
real evidence begins; any fallback, reset, gap, worker restart or missing proof
invalidates the run. The observer derives `fixed-weights` from authenticated
worker execution proof, never from a requested observer label.

This path is limited to 4096 samples/channel and 64 CPIs, additionally constrained
by existing observer and retained-output byte limits. It is not operational-size
or release qualification. Paired evidence records
`qualificationScope: bounded-clutter-replay-v1`; the physical pre-soak gate
explicitly refuses that scope for release acceptance while preserving the
fixed/refit × FP32/FP64-device × positive/negative-Doppler requirement. Device
class denotes `shaderFloat64` capability, not changed clutter arithmetic: both
classes use FP64 host initial coefficients plus FP32 correction FIR. Injection
can change the adaptive array reference after onset, so this measures an
end-to-end fixed-clutter control, not an isolated linear-clutter attenuation.

`clutterReplayCheckpointCpuContract`, `gpuReplayQualificationOptIn`,
`testReplayEchoInjector`, `gpuClutterRefinementCpuOracle` and
`replayCoherentObserver` cover host/state contracts. The optional
`testClutterReplaySoftware --software` runs the production two-stage FIR on CPU
Vulkan only and rejects physical devices; it proves no physical GPU or release
qualification. The protected physical gate still needs separately admitted
recorded RF, operational exposure and approved measured policy bounds.
