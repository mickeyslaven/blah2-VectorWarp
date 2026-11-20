# ATSC 3.0 exact-grid reference reconstructor

`Atsc3ExactGridReferenceReconstructor` accepts complex samples already on the
exact 6.912 MS/s grid plus a waveform profile produced by a successful ATSC
A/322 bootstrap/L1/PLP decoder. The profile must enumerate every FFT bin for
every symbol, including the known pilot values, PLP data cells and null cells.
It also carries the decoder's residual carrier phase and phase increment.

The reconstructor de-rotates tracked carrier phase, FFTs the useful symbol,
estimates the complex channel from known pilots, validates interpolation on
held-out pilots, equalizes and slices configured uniform QAM PLP cells, applies
the pilot-derived channel to the decided waveform, inverse FFTs, creates the
standard guard interval from the useful-symbol tail, and restores carrier
phase on the original sample clock. It commits the candidate only when every
symbol and whole-block confidence gate succeeds. A discontinuity, malformed
profile, unsupported mode, weak pilot validation, or ambiguous decisions
returns the original `complex<float>` vector unchanged.

Supported modes are the A/322 8K, 16K and 32K FFT sizes, their permitted GI-1
through GI-12 sample counts, the standard's uniform QPSK payload mode, and
16-NUC and 256-NUC at code rate 9/15 from A/322:2024-09 Annex C. The minimum
distance gates for uncorrected 16- and 256-NUC are 0.20 and 0.00074. A caller
may supply FEC-validated constellation points through `decodedData`; it must
establish validation provenance itself. Those points still pass the
per-symbol pilot, guard and aggregate data-EVM gates.
Higher-order uniform QAM slicers remain synthetic test scaffolding, not A/322
payload modes. The component does not decode bootstrap/L1, derive pilot
patterns, track carrier, or resample. Other A/322 signalling-dependent
non-uniform constellations are rejected. MISO,
time-frequency slicing, channel bonding and layered-division multiplexing are
not independently decoded or validated here. The caller must establish that a
single-waveform model applies; the interface cannot verify that claim. It also
cannot validate active constellation extension or extended guard intervals
from time-aligned frames. The startup parser accepts externally supplied L1 bits without binding
them cryptographically to the input samples. The hash is format-checked, not
computed or verified. This remains an offline experiment, not a validated
complete receiver or a live reference source.

The official A/322:2024-09 Annex C NUC position vectors can be extracted from
the exact pinned PDF with `python3 script/import-atsc3-nuc-tables.py A322.pdf
vectors.json`. The extractor checks the document digest, all row counts and
indices, and the examples in sections 6.3.4.2.1 and 6.3.4.3.1. It can generate
`Atsc3NucTables.generated.h`, including bounded 64K 9/15 LDPC
and group-permutation tables, with `--cpp-header PATH`. Vectors alone do not
establish the PLP code rate, carrier allocation, bit labels or profile validity.

Offline acquisition tools support the bounded profile-50 first preamble and
legacy A/322:2022-11 L1-Basic version 0 and L1-Detail version 1. Hard-decided
L1 bits are emitted only after their CRCs pass; no L1 FEC correction is
claimed. The payload prototype supports the observed non-inter-subframe HTI
layouts, 64K rate-9/15 Type B LDPC, and corresponding 16/256-NUC mappings. It
uses bounded normalized min-sum, followed on failure by bounded sum-product,
and requires LDPC syndrome and BCH verification for every block. BCH is
verified but not corrected. A whole PLP fails closed if any block fails.

End-to-end PLP1 recovery has not succeeded on the examined real frames: 141 of
156 and 152 of 156 blocks passed on two frames. The final 24-worker workload
took about 638 ms for a roughly 248 ms frame before any radar processing. The
offline three-symbol PLP0 hybrid is not a full reconstructed frame: preamble,
boundary, mixed-PLP and PLP1 samples remain raw. Do not use it as a live
reference. It also reduced a synthetic echo by about one percent and produced
mixed clutter-cell changes, so it is not evidence of radar benefit. Full-frame
recovery, clock tracking, provenance binding and production radar validation
remain outstanding. CMake registers focused C++ tests; Python tests for table
import and preamble decoding are in `test/script`.

`Atsc3PilotGridProjector` is a separate offline matched-reference experiment.
It accepts an acquired profile and externally established absolute symbol epoch
on the 6.912 MS/s grid, then removes the measured pilot-bin waveform from a copy
of an ordinary OFDM symbol. It currently accepts only the bounded 16K/GI-768,
SP16_4, zero-boost, unreduced, symbol-aligned SISO profile. The A/322 pilot
sequence and common, scattered, additional and edge pilot locations are
generated once; boundaries are left raw. GI, held-out pilot interpolation and
removed-power gates apply to every symbol. Discontinuity or an uncertain symbol
index invalidates the instance and returns byte-identical input. The caller
must bind the externally acquired epoch, profile and IQ provenance and must
continue to use the physical raw reference for clutter cancellation. No live
path is wired to this projector; a production matched-filter A/B, target and
false-cell preservation, and full-CPI cadence are required before selection.
The offline projector supports both all-pilot and common-pilot-only scopes,
plus bounded retention of the measured pilot waveform. The batch API checks
the complete epoch and continuity before scheduling independent symbols and
returns the entire input unchanged if any symbol fails.
Each instance has mutable continuity state and requires serialized external
`process()` calls; its immutable FFT plan and internal batch workers are safe
to share. Individual symbol calls may return a mixture of projected and raw
symbols, while a batch deliberately commits all-or-raw. The current live lane
is sampled at 6.144 MS/s, whereas this prototype requires 6.912 MS/s: live use
would require continuous 9/8 conversion and an 8/9 return conversion (or an
equivalent rational channelizer), with group delay, sample identity and
surveillance alignment verified across CPIs. Conversion and radar contention
must be included in the cadence gate.

Two bounded real-frame offline tests selected raw after correcting the Wiener
span to 1,419 taps, injecting echoes before cancellation and scoring background
relative to the injected target response. Earlier apparent pilot gains used a
different span and post-cancellation injection and are superseded. The tests
are not native production CPI or tracking results. The per-window raw p99.9
crossing count is relative to a recalibrated threshold, not an absolute false
alarm measurement. The diagnostic hybrid replaces only caller-attested
BCH-verified payload bins; its manifest hashes the inputs but cannot establish
the truth of the caller's source, epoch or BCH assertions. It is offline only.
For pilot frame probing, a bounded 100-symbol geometry, L1 bits and source
correlation in every symbol are checked before projection; this is a signal
consistency check, not cryptographic provenance. Live selection remains raw.

`atsc3-reciprocal-reference.py` implements the distinct OFDM experiment that
pilot deletion did not: on known pilots and independently BCH-verified payload
cells it constructs a bounded Tikhonov reciprocal matched reference with
carrier weights `S/(|S|^2 + lambda)`. The physical raw reference still supplies
the Wiener clutter canceller. Unknown cells are zero rather than guessed,
unsupported symbols remain byte-identical raw, inverse gain is capped, and the
result is RMS-normalized per symbol. Three phase policies are explicit:
measured per-carrier phase, a pilot-fitted bulk linear phase, and common scalar
phase. The scalar policy avoids carrying reference-path frequency selectivity
into the synthesized transmitted waveform.

The corresponding qualifier uses disjoint selection and holdout intervals,
1,419-tap exact-grid raw cancellation, independently synthesized decoded target
waveforms passed through three fixed target-channel FIRs, deliberately off-bin
Dopplers, global p99/maximum and CFAR counts, and the 13 nonzero predicted
SP16_4 delay-Doppler lattice cells. Selection never substitutes a runner-up;
any held-out regression makes the effective result raw.

The initial apparent qualification was rejected by opposition and is
superseded. It demodulated injected off-bin targets at their exact Doppler while
adding them to background evaluated at the nearest 40 Hz row, and the earliest
candidate also carried the wrong carrier phase across its cyclic prefix. Both
are corrected. The inverse-gain cap now bounds per-bin gain relative to its
median before RMS normalization, and the CLI binds IQ, L1, tables and labels to
an explicit caller attestation.

With corrected scoring, the original frame still improves only when the target
is the supplied ideal decoded waveform: the 75% clean/reciprocal blend improves
held-out target-normalized p99 about 1.6 dB, but both raw and candidate miss one
of three off-bin CFAR probes. When the raw reference or other physical receiver
channel is used as a target-path sensitivity control, selection falls back to
raw. The independent fresh frame also falls back to raw. Lambda-30 and the
lambda-infinity clean remodulation control correlate above 0.9995, so the data
does not isolate a benefit from reciprocal weighting; it mainly tests a
regenerated clean matched reference. The offline implementation is retained,
but no candidate is qualified for live use. A frozen design must be validated
prospectively on new recordings with transmitter-truth or independently
defensible target-channel controls before native CPI/tracker, 6.144/6.912 MS/s
conversion, and real-time cadence work is justified.

## Symbol-native passive-radar experiment

`atsc3-symbol-native-radar.py` corrects the architectural limitation of feeding
a reciprocal synthetic waveform into the ordinary long-CPI CAF. It removes the
cyclic prefix, operates on corresponding reference/surveillance OFDM carriers,
uses absolute-sample Doppler de-rotation (so hypotheses outside the symbol-rate
Nyquist interval are not silently aliased), and performs per-symbol range
compression before coherent integration. Raw matched, decoded matched,
reference-channel-restored, hybrid and loaded-reciprocal filters remain separate.
The measured reference supplies one common frame CFO and optional per-symbol
phase; it never supplies an independently normalized phase that could erase
target Doppler. Channel restoration uses a bounded cyclic impulse response,
not direct complex-frequency smoothing across rotating path phase. Unknown
decoded cells retain raw carrier values in hybrid candidates, and frame-wide
normalization prevents sparse pilot-only symbols from receiving full-symbol
weight.

On the two available independent exact-grid frames, no method qualified. The
well-decoded frame had 91.4% selection and 89.2% holdout verified-carrier
coverage. Its best bounded hybrid (64 channel taps, 25% blend) reduced the
absolute held-out maximum about 1.43%, but after preserving the worst target
control its held-out p99 was 0.072 dB worse than raw. The independent partial
frame had only 35.3%/16.4% coverage and is fail-closed by the 75% coverage gate;
its best hybrid was effectively tied with raw, not a repeatable improvement.
Loaded reciprocal sweeps from 0.1 to 100 approached raw as loading increased but
never beat it after target normalization. The captured L1 profile signals no
PAPR reduction, so TR/ACE are not the cause for this channel. Pilot EVM of the
decoded capture is about 2.15%, which also means the measured reference is
already clean enough that little reference-noise gain is available.

These results supersede the reciprocal time-waveform experiment as the correct
OFDM-domain test. They validate the processor and explain the earlier target
loss, but do not authorize live ATSC 3.0 conditioning. The live selector remains
raw until a prospective independent frame clears coverage, target preservation,
background, maximum-cell and native cadence gates.

The public `automatic` mode exposes that decision directly in live frame
telemetry as `effective_mode: raw` and `reason: raw_control_not_beaten`. It does
not repeat rejected decoding work in the CPI hot path, and it cannot silently
promote a candidate merely because decoding locked.

## Corrected posterior/reference qualification

The later posterior experiment closes several gaps in the first symbol-native
test.  It fills the deterministic A/322 dummy BPSK cells, obtains posterior
constellation means only from LDPC-syndrome-valid and BCH-valid blocks, tracks
both common phase and fractional-delay phase slope from pilots, and measures
receiver noise from the cyclic-prefix duplicate.  Failed payload remains an
explicit zero-confidence erasure.  Every input IQ, label, posterior,
confidence and table artifact is size- and SHA-256-bound in the report.

Qualification now keeps one common 1,419-tap physical-reference Wiener
canceller for every candidate and uses the reconstructed waveform only as the
matched reference.  It computes linear rather than cyclic range correlation,
so delays beyond the 768-sample guard interval are valid.  Off-grid injected
targets are evaluated at their exact Doppler and are added before the common
canceller is refit.  The ideal decoded target is supplemented by a conservative
copy of the literal measured physical reference; the latter prevents failed
payload cells from being omitted in a way that favors reconstruction.  Three
delays (one beyond the guard interval) and three off-grid Dopplers must all
retain their CFAR detection.  The literal raw comparator, including its
measured cyclic prefix, remains byte-preserved.  Training, selection and
holdout use disjoint PLP1 HTI/FEC groups separated by guard symbols.  Candidate
choice uses selection only; holdout cannot choose a runner-up.  Maximum-cell
safety is normalized by the same worst target response as p99.

The earlier apparent first-frame pass is invalid: both of its target controls
omitted real energy from failed payload cells, and one was derived from the
candidate itself.  With the independent physical control, literal raw prefix,
disjoint HTI partitions and an 8 Hz background grid, the frozen candidate
selects raw on both recordings.  Frame 1 regresses target-normalized p99 by
0.682/0.777 dB on selection/holdout.  On the fresh frame it improves p99 by
0.309/0.206 dB, but its selection maximum regresses by 1.260 dB, so the strict
gate rejects it.  Every injected target remains detected in these bounded
tests, but no reconstructed candidate is qualified.

The offline canceller is deliberately training-only and linear; production
uses a circular per-CPI fit.  The report states this limitation and binds the
input artifacts, scorer, canceller and run settings by size and SHA-256.  A
live candidate still requires an unopened prospective recording with defensible
transmitter/target truth, followed by native CPI residual, tracker and cadence
validation.  Automatic live ATSC 3.0 selection therefore remains raw.

Physical-MMSE candidates additionally estimate receiver noise only from a
validated trailing CP window.  Full-prefix averaging was removed because early
CP pairs can contain preceding-symbol multipath; synthetic native-geometry
tests showed errors above 20 dB for a strong long path.  An unstable or too
short clean suffix now removes those candidates rather than treating multipath
as noise.  This does not alter the frozen posterior-mean result above.

## OFDM follow-up probes

Three additional literature-derived methods were tested on both independent
paired-IQ frames.  They are bounded offline diagnostics and are not wired into
the live processor.

The first probe compared one fixed 1,419-tap raw Wiener canceller with a causal
32-symbol sliding fit updated every eight symbols.  Its corrected screen keeps
all eight slow-Doppler rows, combines each echo with that algorithm's complex
background before CFAR, and normalizes background by worst target response.
Target-normalized p99 worsened by 2.728 and 0.596 dB on the two frames; the
target-normalized maximum worsened by 2.345 dB on one.  Its worst response
relative to the fixed control across delays 128/512/767/769/1024 samples and
Dopplers +/-4/8/16/32 Hz was -2.54 dB on both frames.  Background CFAR cells
changed 1-to-0 and 1-to-2.  That exceeds the one-dB target-loss limit and is not
a repeatable background win, so sliding ECA is rejected.

The second probe fit a causal phase/fractional-delay clock state from alternating
pilot subsets.  Controlled +/-0.2 and +/-1 ppm drift is recovered, but real-IQ
held-out phase RMS changes by only -0.0016/+0.0002 dB on selection/holdout in
frame 1 and -0.0110/-0.0074 dB on the independent frame.  Those numerical ties
do not clear the 0.1 dB materiality gate, so continuous clock tracking is not
qualified for this capture set.  CFO and measurement-noise calibration use the
completed training interval only, and validation pilots cannot influence phase
unwrapping.

The third probe retained two separated channel-impulse clusters instead of one
contiguous 128-tap interval.  It improved held-out pilot RMS by 0.713 dB on the
first frame, but regressed by 0.006 dB on the independent frame after only a
0.061 dB selection win.  The result therefore fails repeatability and remains
raw/single-cluster.  An optional A/322 Annex N TxID scan was also attempted,
but the supplied decoded first-preamble waveform reproduces the physical
preamble.  Its host subtraction is exactly or numerically zero, so scanning
that residual would correlate floating-point noise rather than an independent
watermark.  The probe now fails closed as `host_subtraction_degenerate`; a
standards-derived host-only first-preamble reconstruction is required before
TxID/SFN separation can be evaluated honestly.

The final full reports are
`/private/tmp/vw-atsc3-frame1-ofdm-followups-reviewed-full.json` and
`/private/tmp/vw-atsc3-fresh-ofdm-followups-reviewed-full.json`; they bind every
input and the implementation by SHA-256.  All three
methods remain rejected for live use: slow-target preservation rejects sliding
ECA, real data gives no material clock-state gain, and the multi-cluster win is
not repeated.
