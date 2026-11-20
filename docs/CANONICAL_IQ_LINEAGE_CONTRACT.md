# Canonical IQ lineage contract

`src/data/CanonicalIqLineage.h` defines the receiver/OS-independent provenance
value for future per-lane resident adapters. It is an in-process contract;
it does not change the resident wire ABI or enable a new production route.

The producer supplies a globally unique acquisition-session `captureId`,
capture/configuration epochs, lane identity, reset generation, immutable
reference/calibration identities, ordered physical channel identities, RF
frequencies, and input/output rates. Epoch/generation counters must be explicitly
present; counter zero is a valid initial value, distinct from missing metadata.
Missing provenance must not be replaced by guessed timestamps or defaults.
An uncalibrated stream must carry an explicit uncalibrated snapshot identity.
Identities are nonempty, at most 128 bytes, and contain no control characters;
at most eight unique physical channels are supported.

`[sourceFirst, sourceEnd)` is the exact raw input block consumed, not the FIR's
full dependency interval: causal filter history can precede `sourceFirst`.
Output anchors satisfy the declared decimation phase in this interval. The
validator checks their exact count and first anchor using integer arithmetic,
including empty outputs and source positions above 2^53 or near UINT64_MAX.
FIR group delay remains separate in input-sample units; subtracting it from an
unsigned anchor would lose valid early-frame/fractional timing. The initial
contract supports integer decimation, not rational resampling.

`canonicalIqContinues` permits streaming history only for adjacent raw input
intervals with every identity, transform, channel ordering and reset generation
unchanged. It is not an overlapping-CPI eligibility predicate. Producers must
advance the reset generation whenever adaptive/FIR state resets; the contract
does not discover hidden resets. Publication treats validated lineage as
immutable. A resident ingress implementation must still bind it to the exact
IQ payload and validate transaction/reset ownership.

`src/process/conditioning/CanonicalChannelizedIq.h` provides an opt-in adapter
from existing `IlluminatorOutput`. Callers supply the explicit provenance and
raw input interval. The adapter checks lane identity, RF frequency, ordered
channel count, equal channel lengths, decimation, group delay and sample
schedule before returning a validated lineage value. Existing channelizer
outputs and worker structures retain their layout and behavior; no live caller
uses the adapter yet. The legacy channelizer's phase is zero.

`test/unit/data/TestCanonicalIqLineage.cpp` is a standalone C++17 test with no
receiver, FFT or GPU dependencies. CTest registers it as
`canonicalIqLineageContract` when `BUILD_TESTING` is enabled. It covers geometry,
overflow, provenance bounds, discontinuity/reset/identity changes, coherent
channel shape, and adapter failure cases. DSP/resident replay and prospective
qualification remain required before integrating this contract into a live
route.
