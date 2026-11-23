# Current architecture and retention contract

Every change must preserve receiver, recording-format, and supported module ABI
compatibility. Do not commit, push, deploy, or perform physical qualification
without the task's authorization.

## Simple implementation rule

Before adding a path, find the existing canonical owner and extend it. Do not
duplicate processing for an SDR, operating system, channelizer, GPU, or CPU;
supported devices differ only at ingress, hardware control, or an explicitly
qualified backend boundary. After implementing a change, remove the code,
configuration, tests, and documentation it supersedes. A change is not complete
while an obsolete path or temporary compatibility copy remains.

## Operational completion rule

- Activate one immutable cohort: executable, libraries, modules, API, UI, and
  configuration must share a verified package identity. Refuse mixed cohorts.
- Verify behavior rather than service state: require advancing input and output,
  fresh products, the configured backend or an explicit supported fallback,
  bounded queues, and zero unexplained loss. Radar acceptance also requires
  detections, track formation, continuity, association, and publication to
  behave correctly on representative positive and negative evidence; advancing
  frames or maps alone is never sufficient.
- Bind every soak to the exact package, configuration, and declared
  qualification set. Restart the soak after a restart, loss, identity drift, or
  validated detection/tracking regression.
- Accept release evidence only when CI, platform builds, backend qualification,
  hardware-path tests, and soak receipts identify the exact merged source and
  package. Release still requires explicit user approval.

## Evidence-based radar rule

Production behavior requires a traceable evidence ladder: numerically correct
mechanism, synthetic positive and adversarial-null tests, retrospective
whole-pass recorded-IQ evaluation, prospective traffic not used for tuning,
and replication across applicable receivers, backends, and operating systems.
Promotion requires an outcome improvement in true detection, track continuity,
localization, or resource use without exceeding the reviewed false-track and
cadence bounds. A promoted feature replaces the old production behavior; it
does not remain as a comparison lane.

Do not ship live shadow processors, recorders, promotion toggles, endpoints, or
UI controls. An inconclusive experiment stays in isolated offline replay tooling
and cannot consume live-radar resources. If retained operational data cannot
answer an experiment after its declared collection window, treat that as an
instrumentation or evaluation defect: identify and repair the missing execution,
lineage, population, or negative controls before rerunning, or remove the
experiment. Plausibility and enabled service time are not evidence of benefit.

Raw IQ, acquisition identity, and independently captured truth/negative windows
form immutable evidence cohorts. Hash and retain them; a software change
invalidates derived outputs from the changed implementation, not the source
cohort. Re-run the pinned baseline and candidate on the exact same bytes and
emit one paired receipt containing detection, track formation/continuity,
false-track, cadence, and lineage outcomes. Keep a separate untouched
prospective holdout. Do not reset the evidence clock or collect another live
shadow merely because code changed when the retained cohort can answer the
question.

These rules are topology-independent. Never encode a user's hostnames, node
count, SDR inventory, operating system, accelerator, or local power policy in
product behavior or general acceptance criteria.

## UI and settings acceptance

Oppositional review must visit every reachable page. A real-browser control
inventory must exercise every visible editable setting and action at least once
across representative capability fixtures; exhaustive DOM/native matrices own
the remaining operating-system, receiver, backend, and capability combinations.
Controls must work, be unavailable with a reason when unsupported, and map to
the same canonical configuration and behavior on every platform.
Review responsive layouts, navigation, terminology, visual hierarchy, text,
empty/error states, and consistency between related views. Remove redundant or
useless subtext, implementation commentary, placeholder prose, and generic
AI-style explanations. Keep only text that helps a user understand status,
make a decision, or safely operate the product.

Source and DOM review are not UI acceptance. The canonical real-browser CI
harness must click every supported setup, settings, migration, node, catalog,
save, and recovery flow at desktop and narrow viewports. It must fail on page
errors, missing assets, stale controls, ineffective actions, stuck overlays,
unreachable content, broken scrolling, and clipping/overflow. Use stable
interaction and geometry assertions; screenshots are review evidence, not a
pixel baseline that can be silently regenerated. Every refusal or error must
state what happened and the next safe action in ordinary user language, with
advanced detail available through progressive disclosure.

Receiver setup is capability-driven. Never encode a user's station list,
frequency plan, serial, channel width, or regional broadcast assumption.
Direct and channelized setup must expose and validate receiver channel/rate
limits, waveform standard and version, explicit occupied bandwidth, reference
method capability, extraction passband, and physical channel mapping. Catalog
selection is an optional, provenance-bearing convenience; manual and offline
normalized import remain supported. A preset or standard change may not
silently overwrite an existing extraction plan.

## Canonical ownership

Apply the same single-owner rule to every concern, including settings, API
transactions, and UI data lanes. One declarative contract supplies shared field
identity, capabilities, and persistence mapping; one controller owns each data
lane's fetch, alignment, freshness, and state lifecycle. Pages, channels, nodes,
operating systems, and receivers use thin descriptors/adapters, not parallel
implementations. Keep genuinely distinct renderer mathematics or ingress/backend
semantics only when outcome-level tests establish the necessary boundary.
Acceptance must compare observable settings refusal/persistence, page visuals and
interactions, and radar timestamp/alignment/detection/track/fusion outcomes.
Internal source spelling is not a compatibility promise. Consolidation must
delete superseded ownership and reduce total code, not merely move it.

1. Capture/replay adapters in `src/capture/` own device I/O, acquisition identity,
   recorded topology, and conversion to canonical coherent samples.
   `src/process/conditioning/` owns input/reference conditioning and channelization.
   Channelization ends at `src/data/CanonicalSignal.h`; it is not a radar route.
   A separate immutable frontend plan binds capture/lane rates and frequencies,
   occupied bandwidth, waveform/version, filter/extraction parameters, physical
   channel roles, and lineage through live, replay, CPU, and GPU publication.
   Do not fold this identity into the downstream radar-plan hash or admit a
   runtime whose configured and published frontend identities differ.
2. `src/process/pipeline/` owns production stage factories, ordering, state
   lifetime, reset boundaries, and canonical signal-bank scheduling. Direct and
   channelized products use its shared clutter → ambiguity → fusion →
   temporal integration → detection → tracking stages. Configured disabled
   stages remain disabled. Never create another route-owned detector/tracker.
3. Numerical implementations live only in the corresponding
   `src/process/{clutter,ambiguity,conditioning,fusion,detection,integration,tracker,meta,utility}/`
   directory. `src/blah2.cpp` owns ingress wiring, runtime scheduling, and
   publication, not numerical algorithms or duplicate product orchestration.
   `src/process/analysis/` owns contracts, health, diagnostics, recording, and
   observation adapters, not an alternative production processor.
4. Receiver identity, SDR name, OS, and channelizer choice must not select a
   processing graph after canonical ingress. No receiver-, SDR-, OS-, or
   channelizer-named production processing path may be added there.
   Physical geometry, configured statistic, enabled stages, numerical
   representation, and explicitly qualified backend capability may select
   implementations of the same normalized contract.
5. An exception needs an explicit reviewed capability boundary, documented
   admission/refusal behavior, canonical configuration mapping, parity tests,
   and lifecycle/reset/failure tests. Existing boundaries are:
   - `src/process/ambiguity/` portable GPU and resident transport/qualification;
     private `core/` resident numerical kernels implement the supported ABI,
     with strict CPU-oracle and physical-device qualification.
   - Offline replay experiments may reuse canonical numerical owners, but they
     remain outside installed runtime entry points and cannot allocate live
     workers, alter products, or expose production configuration.
   - Float/borrowed workspaces are representation optimizations of canonical
     clutter/ambiguity, never receiver-specific algorithms.

The compatibility header `process/analysis/MultiIlluminatorRadar.h` preserves
existing bench/test/application source consumers with aliases/forwarding only.
Existing CMake target names preserve downstream linkage;
their sole implementation is registered in `cmake/ProcessingLibraries.cmake`.
Channelizer input/output aliases preserve existing source aggregate initializers;
their storage is canonical. Do not introduce speculative compatibility aliases.

## Every file needs a current purpose

Every tracked file, including tests, scripts, docs, and fixtures, must have a
named current production, install, compatibility, documentation, or regression
purpose. A directory label is not evidence that an obsolete file is necessary.
When adding or retaining a file, state that purpose in its content or the change
description and identify its consumer. Remove superseded files, not just their
build references.

Historical copies, frozen implementations, abandoned experiments, generated
build/runtime artifacts, duplicate full configurations or processing pipelines,
and source-string tests protecting obsolete structure are forbidden.
Generated outputs belong outside the source tree. Unique numerical regression
oracles (for example `LegacyCfar.h` and legacy JSON reader fixtures) may remain
when they protect an actual compatibility/statistical contract and cannot enter
production. Physical qualification fixtures, evidence validators, and replay
provenance controls are necessary regression assets, not disposable examples.
Documentation of supported operations and reproducible evidence remains useful;
do not delete it merely because its filename contains a date.

## Keep the repository maintainable

Prefer consolidation over net-new structure. Extend an existing owner before
adding a file, dependency, configuration surface, abstraction, or compatibility
layer. Keep functions and files focused enough for an ordinary maintainer to
review without reconstructing the whole system. Generated output, vendored
copies, temporary migrations, and duplicated fixtures do not belong in source.

CI maintains a ratcheted repository budget for production file count and lines,
oversized files, dependencies, duplicate configurations, and forbidden parallel
pipeline structure. Existing debt may be reduced but must not silently grow.
An intentional budget increase requires an explicit reviewed baseline change
that identifies the owner, purpose, rejected reuse option, and cleanup plan.
Tests and necessary evidence remain first-class code; do not delete meaningful
coverage merely to satisfy a size metric.

For each changed responsibility, review `python3 script/check_repository_budget.py
--review-owner OWNER` with its changed regressions and actual ABI/behavior
contracts. Cross-owner work requires a packet for each affected owner. The
sealed budget measures exact first-party LOC, characters and lexical tokens,
plus direct foreign-file identities, sizes and computed-load counts. Lexical
tokens are not model tokens; packets omit transitive/external implementations
and do not prove semantic independence. Oversized existing packets are debt,
not a context-fit claim. Consolidate before splitting; file movement cannot
justify extra ownership or imports. Every reduction needs an exact downward
adjustment, and reviewed adjustment history must remain sealed. Never refresh
the original snapshots to regain headroom.

Responsibilities: `src/`, `core/`, `api/`, `html/` production;
`cmake/`, `packaging/`, `Formula/`, `services/`, `runtime/`, `config/`,
`oprs/`, `contrib/`, `lib/` build/install/integration dependencies;
`schemas/` machine-readable validation contracts consumed by those integrations;
`test/`, `bench/`, `vm-lab/`, `examples/` regression, qualification, and
executable examples; `script/`, `tools/` supported operation/validation;
`docs/` and top-level documentation document current contracts and evidence;
`.github/` owns CI. Each file still needs its specific current purpose.

## Required regression checks

Run `python3 -m unittest discover -s test/script -p test_canonical_processing_architecture.py`
for ownership/retention guards. The canonical direct-versus-channelized unit
test must compare maps, detections, tracking, temporal warm-up/reset, and
disabled stages. Keep receiver-topology and API stage-toggle parity tests.
Source checks supplement behavioral/ABI/physical tests; they do not qualify
hardware or replace numerical tests. Do not weaken a guard simply to retain
an obsolete path.
Cross-platform asynchronous tests must wait for an observable condition within
a bounded deadline. Do not encode sub-second scheduler or process-start timing
as product behavior with fixed sleeps.

Before an all-target native build, run `script/prebuild_acceptance.py` against
the configured build directory with a receipt path and every affected
`--native-target` and focused `--native-test`. The build must be configured with
`CMAKE_COMPILE_WARNING_AS_ERROR=ON`; the gate compiles only those targets after
diff hygiene, budget, architecture, and fast API/UI contract checks pass, then
runs the declared tests before allowing an all-target build. A native
source/CMake change without non-empty target and test declarations is refused.
Its source receipt is not radar, hardware, or operational-behavior evidence.
