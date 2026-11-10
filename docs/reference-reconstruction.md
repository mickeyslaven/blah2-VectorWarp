# Modulation-aware reference reconstruction

VectorWarp's universal reference path is raw coherent IQ. It is stable for any
receiver and any configured illuminator whose occupied bandwidth fits its lane.
Optional modulation-aware backends may replace that reference only after an
independent standards lock, continuous sample provenance, a held-out quality
test, and sustained qualification. Failure always selects the unchanged raw
reference for that CPI.

`automatic` is the default ATSC policy. It is a selector, not a promise that a
reconstructed candidate will be used: each RF channel is compared only with
its own raw control. ATSC 1.0 may select the qualified pilot projection. ATSC
3.0 currently selects raw because every completed target-normalized candidate
test lost to raw. The live frame publishes the requested mode, effective mode,
and rejection reason so fallback is visible rather than silent.

The framework is receiver-neutral. A backend consumes channelized complex IQ,
the actual sample rate and input-sample/group-delay provenance. It cannot own
the SDR, assume a particular processor, or alter surveillance samples. One
qualified reconstruction is shared by every surveillance channel observing the
same illuminator.

## Backend maturity

| Illuminator | Current backend | Maturity | Live selectable |
|---|---|---:|---:|
| Any / custom | Raw coherent IQ | Stable | Yes |
| FM | Demodulation-derived quality filtering; full modulation-aware regeneration next | Experimental | No |
| ATSC 1.0 8-VSB | Held-out-qualified offline pilot projection; full field-sync/exact-grid reconstruction remains a second experimental rung | Experimental offline | No |
| ATSC 3.0 OFDM | Bootstrap/L1 and bounded 9/15 PLP decoding; whole-PLP fail-closed because real PLP1 recovery is incomplete and slower than its frame cadence | Experimental rejected | No |
| DAB | OFDM null-symbol/PRS timing and decision-directed regeneration | Experimental, not implemented | No |
| DRM | OFDM reference-cell reconstruction | Experimental, not implemented | No |
| DVB-T/T2 | OFDM pilot/TPS-assisted reconstruction | Experimental, not implemented | No |
| ISDB-T / DTMB | Standards-specific pilot/training reconstruction | Experimental, not implemented | No |
| AM | Envelope/carrier-assisted regeneration | Experimental, not implemented | No |

“Experimental” is a capability label, not permission to use a backend in the
live radar. A backend becomes selectable only after synthetic standards vectors,
redistributable oracle comparison, real recorded IQ, identical held-out CAF and
clutter scoring, discontinuity/fallback tests, cadence benchmarks, and CPU/GPU
equivalence where applicable. The node capability endpoint publishes this
maturity explicitly so federated portals never infer support from receiver type.

ATSC 3.0 startup acquisition may cache a stable FFT/guard/L1 profile and avoid
repeating a blind search in the CPI hot path. It cannot cache the transmitted
payload: exact-grid OFDM symbol decisions and remodulation remain live work.
The runtime must cheaply monitor bootstrap/profile continuity and fall back to
raw immediately if the broadcaster changes parameters or lock is lost.

ATSC's detailed implementation and acceptance contract is documented in
`atsc-reference-reconstruction-design.md`. Other digital standards should use
the same carrier/timing/equalizer/remodulator interface and confidence policy;
only their standards-specific synchronization and symbol regeneration differ.

## ATSC 1.0 equal-coefficient bank gate

The experimental ATSC 1.0 path does not replace the raw reference. Its tested
candidate topology is a two-input canceller with 1,261 raw-reference
coefficients and 32 exact-grid reconstructed-reference coefficients, compared
against a 1,293-coefficient raw-only control. `Atsc1ReferenceBankQualifier`
enforces that equality, requires reconstruction confidence and continuous
sample provenance, and scores the two residuals only on samples excluded from
coefficient fitting. A win must persist across consecutive blocks; every
geometry, provenance, finite-value, confidence, or held-out-score failure
returns the raw-control residual byte-for-byte.

`Atsc1ReferenceBankProducer` now supplies the offline two-reference Wiener
candidate. It uses the same zero-loading solver policy for the raw control and
candidate, explicit signed delay origins for both branches, circular CPI
correlation matching the production Wiener convention, and the production
zero-padded linear FIR output convention.
Its output is structurally bound to the exact reconstruction object, sample
interval, tap budget, delay origins and loading policy consumed by the
qualifier. A direct dense-design oracle covers negative delay origins.

The producer is not qualified for live cadence. An earlier 3.072-million-sample
(500 ms at 6.144 MS/s) prototype measured 0.89--0.92 seconds per lane after its
initial cubic solver was replaced by bounded preconditioned CG. That timing
predates the final signed-origin, zero-padded linear-FIR correction and must not
be attributed to the current checkpoint. No configuration option enables it.

Startup acquisition must use the coherent wideband reference before ordinary
lane channelization. `Atsc1StartupAcquirer::acquireWideband` mixes the requested
lane, applies a 257-tap Kaiser 6 MHz isolation filter, validates pilot, segment
and field sync plus a bounded equalizer model-selection score, and returns cached
timing/carrier/equalizer state mapped to the production lane rate. Feeding the
ordinary 6.144 MS/s radar lane is intentionally not accepted when that lane no
longer retains enough acquisition quality; the Python oracle rejects the same
lane files. Two independent real wideband recordings locked on both tested
509 and 527 MHz references without relaxed gates. Those recordings establish
repeatable acquisition; the equalizer score itself is not an independent test
because it also selects tap count and decision delay.

The cache does **not** freeze payload symbols. ATSC payload decisions change
every CPI. A live implementation still needs a streaming rational resampler
and current-CPI slicer with an absolute, provenance-bound symbol epoch,
continuous residual-carrier phase, pulse-shaping history/lookahead, immediate
raw fallback, and persistent workspaces. The first per-CPI prototype was
removed after review found boundary-phase and hot-path planning defects.
Therefore the table above remains **Experimental offline / No** until that
stage plus full ambiguity-map, target-preservation, cadence and CPU/GPU
acceptance passes.
