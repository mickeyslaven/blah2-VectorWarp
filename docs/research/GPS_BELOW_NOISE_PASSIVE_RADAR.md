# GNSS below-noise recovery: what carries to ATSC passive radar

**Purpose.** This is a research note, not a claim that VectorWarp detects an
aircraft below the noise floor.  “Below noise” in high-sensitivity GNSS means
that individual samples/correlation intervals have negative SNR; a *known*,
phase-aligned waveform can nevertheless produce a detectable statistic after
integration.  The evidence and the limits below matter more than the slogan.

## What GNSS actually exploits

GPS is unusually favorable.  Its public interface defines continuous ranging
codes and navigation data on L1/L2 [IS-GPS-200N, §3.1](https://archive.gps.gov/technical/icwg/IS-GPS-200N.pdf), and PRN assignments are public and controlled
by the GPS PRN Coordination Office [GPS.gov](https://archive.gps.gov/technical/prn-codes/).
For each candidate satellite, delay and Doppler, the receiver correlates with a
local code/carrier replica.  This is a matched-filter/cross-ambiguity search:

`z(tau, f) = sum_n x[n] c*[n-tau] exp(-j 2 pi f n/Fs)`.

At the right cell, coherent signal amplitude grows as `N` while uncorrelated
noise RMS grows as `sqrt(N)` (processing gain).  Equivalently, the coherent
output SNR increases approximately as `C/N0 * T`; this does **not** make a
single RF sample observable.  FFT correlation computes all code phases more
cheaply, not more sensitively.

Acquisition is a coarse 2-D code-delay/carrier-Doppler search that selects the
CAF maximum; tracking then refines code and carrier with local replicas.  This
sequence and the signal model are explicitly stated in Gómez-Casco *et al.*
[IEEE TAES 2020, pp. 2302–03](https://spcomnav.uab.es/docs/journals/Gomez_TAES2020.pdf).
A DLL holds code delay and a PLL/FLL (often aided) holds carrier phase/frequency.
Their residual error limits coherent dwell: a frequency error attenuates the
output by `sinc(Delta-f*T)`, and code error attenuates it by the code
autocorrelation.  Longer dwells therefore require sufficiently good oscillator,
sample clock, Doppler/range-rate model, and dynamics model—not merely more RAM.

Navigation-bit changes reverse the correlation sign, and modern pilot/secondary
codes can do likewise.  Receivers either keep coherent dwell inside a known bit
interval, wipe a known/decoded bit or secondary code, hypothesize bit signs,
or use post-detection/noncoherent integration (sum `|z_k|^2`).  The latter can
extend elapsed integration but loses information relative to phase-coherent
summation.  The TAES study shows the trade explicitly: coherent dwell is limited
by residual frequency and data bits; nonlinear PDI tolerates them at lower
efficiency ([pp. 2302–05](https://spcomnav.uab.es/docs/journals/Gomez_TAES2020.pdf)).
Differential PDI trades a different robustness/loss profile; it is not magic.

Assisted GNSS reduces the *number of hypotheses*, rather than creating energy:
ephemeris/time/position narrows visible PRNs, code delay and Doppler.  A
peer-reviewed GLRT study reports conventional per-satellite delay/Doppler
search and shows a simulated/experimental joint assisted approach; its quoted
9 dB result is conditional on its 16-satellite, 1-ms setup and must not be
generalized ([Huang et al., 2014](https://link.springer.com/article/10.1186/1687-6180-2014-73)).

Vector tracking similarly uses a common state/Kalman filter to couple channels
through receiver position, velocity and clock; it can coast a weak channel from
the other satellites.  It is only valid when the shared geometry/clock model is
right.  It cannot recover arbitrary signals and can propagate a bad state.

Finally, a receiver controls false alarm across the whole search, not just one
cell.  A threshold must be set for desired `Pfa` with the number and correlation
of PRN-delay-Doppler-time trials accounted for, then verified by re-acquisition/
tracking consistency.  The weak-GNSS literature formulates this as `H0` complex
Gaussian noise vs. `H1` signal-plus-noise and compares a statistic to a
threshold ([TAES 2020, pp. 2303–04](https://spcomnav.uab.es/docs/journals/Gomez_TAES2020.pdf)).
Longer integration also shrinks Doppler resolution (~`1/T`), so a fixed
Doppler grid must become finer or suffer mismatch loss.

## Transfer map for an ATSC direct-reference bistatic radar

| GNSS idea | Valid transfer | Important non-transfer / required condition |
|---|---|---|
| Matched filter | Yes: cross-correlate each surveillance channel against the simultaneously received **direct-path reference**.  The measured reference is a waveform replica. | ATSC payload is not a per-aircraft known PRN.  Correlation peak is target-dependent bistatic delay/Doppler, not a transmitter code phase. |
| Coherent integration | Yes, within a CPI after reference phase, receiver channel phase, Doppler, and target migration are modeled.  Ideal gain remains proportional to dwell. | A fixed range-Doppler cell decorrelates when aircraft acceleration/range migration or oscillator/reference phase error is appreciable.  Do not infer GNSS-length coherent dwell. |
| Noncoherent/PDI | Yes: sum calibrated CAF **powers** across short CPIs, channels, or a bounded motion-path bank. | It sacrifices phase gain and does not repair a biased reference, direct-path leakage, or clutter.  Apply its actual degrees of freedom in the threshold. |
| Known code/data wipeoff | Partly: ATSC known bootstrap/preamble/pilots can be used for synchronization or reference quality checks if the waveform/version is decoded. | They are not a universal, continuous GNSS-like code that resolves arbitrary target delay.  Unknown/coded/scrambled payload is best handled by the direct reference, not an invented replica. |
| Assistance | Yes: known transmitter location, receiver position/time, ADS-B only as an **offline label**, or a pre-existing radar track can bound delay/Doppler/acceleration. | Assistance may reduce trial count but is not evidence of a target.  Feeding ADS-B into detection changes the experiment and creates confirmation bias. |
| Vector tracking | Conditional: a tracker can integrate along one predicted bistatic trajectory; calibrated five-channel arrays can form beams or fuse maps. | Separate aircraft do not share GNSS's common receiver clock/geometry state.  Do not jointly force unrelated targets into one vector state; target RCS scintillation and multipath are target-specific. |
| False-alarm calculation | Essential: budget spatial beams, delay, Doppler, CPI/motion paths and active tracks; calibrate empirically on target-free replay/live intervals. | Independent-cell Bonferroni is conservative but wrong if stated as exact: CAF cells, beams and overlapping CPIs are correlated. |

ATSC 3.0 is a flexible OFDM broadcast system specified as a suite of standards
([ATSC standards index](https://www.atsc.org/documents/atsc-3-0-standards/));
its bootstrap is a robust waveform entry point, not a GPS ranging signal
([ATSC A/300](https://www.atsc.org/wp-content/uploads/2017/10/A300-2020-ATSC-3-System-Standard.pdf)).
Thus a **recorded direct-path reference** is the correct transferable asset.  A
GNSS-style locally regenerated PRN search is not.

## Implementable, falsifiable VectorWarp experiments

These are offline/replay or shadow-only proposals; do not change live
configuration or authoritative detections.  The current documented system is
five coherent channels at 2.4 MS/s; the checked-in profile defaults to 0.5-s
CPI and noncoherent five-map fusion ([config](../../config/config-kraken.yml)).

1. **Integration curve and phase budget.** On the same 527-MHz five-channel
   recordings, form the normal reference and CAF at 50, 100, 200 and 500 ms;
   retain complex CAF for fixed, strong stationary direct/multipath peaks and
   for ADS-B-labelled aircraft only after detection.  Plot coherent magnitude
   and noncoherent power versus dwell.  Pass only if coherent scaling follows
   the expected trend until an explicitly measured break point; report phase
   drift, frequency offset and range/Doppler migration at that point.  This
   estimates, rather than assumes, the coherent limit.

2. **Motion-compensated long dwell, shadow-only.** Split 0.5-s CPIs into
   50/100-ms coherent partitions.  For each *already established* track,
   derotate predicted Doppler and interpolate predicted delay before complex
   summation; compare (a) same-cell noncoherent, (b) trajectory noncoherent,
   (c) compensated coherent.  Bound to one registered path per track, leave
   `promote: false`, and require held-out replay improvement plus unchanged
   normal output.  This is consistent with the repository's existing warning
   that long coherent integration requires migration/phase compensation
   ([TRACK_GUIDED_SHADOW](../TRACK_GUIDED_SHADOW.md)).

3. **Reference-quality gate (not signal reconstruction).** Decode/lock only
   enough ATSC bootstrap/preamble structure to timestamp direct-reference
   quality, then compare CAF sidelobes, reference coherence, and target-free
   false alarms for good vs. bad locks.  At 2.4 MS/s, log fractional delay and
   common-frequency estimates per CPI.  A success is lower/unchanged false
   alarm at equal detection; failure means retain raw-reference CAF.  Never
   substitute an incomplete ATSC replica for the direct reference.

4. **Trial-aware threshold calibration.** For target-free clips split before
   tuning/holdout, sweep `pfa` and measure empirical family-wise alarm rate for
   each dwell/fusion mode.  Record effective trials separately for delay bins,
   Doppler bins, 12 beams, CPI overlap, motion bins and active shadow tracks.
   Repeat after reference/array changes.  Acceptance requires holdout family
   rate at or below the declared bound and no hand-selected exclusion intervals.

5. **Five-channel fusion characterization.** Compare channel 0/1 physical
   pair, five-map noncoherent fusion, and calibrated coherent beam-bank on
   identical replay blocks.  Measure covariance/effective beam trials from
   target-free data; do not claim a five-fold independent gain.  Confirm known
   tracks using an external label *only after* all detector decisions.  Existing
   five-channel results already establish noncoherent fusion, not below-noise
   recovery or coherent-array gain ([capacity report](../ARRAY_CAPACITY_20260910.md)).

## Decision rule

Promote none of this from research until an experiment demonstrates: (1) a
pre-registered held-out gain over the ordinary detector, (2) calibrated
family-wise false-alarm control, (3) phase/migration residuals within the stated
coherent budget, (4) no impact on normal output in shadow mode, and (5) an
independent label used only for evaluation.  Otherwise call it a plausible
processing-gain hypothesis, not GNSS-derived proof.

## Sources and provenance

Primary signal/interface sources: [US Space Force IS-GPS-200N](https://archive.gps.gov/technical/icwg/IS-GPS-200N.pdf),
[GPS interface-document registry](https://www.gps.gov/interface-control-documents-icds-interface-specifications-iss),
and [ATSC standards](https://www.atsc.org/documents/atsc-3-0-standards/).
Peer-reviewed analytic sources: Gómez-Casco *et al.*, IEEE TAES 56(3), 2020,
[author-hosted article PDF](https://spcomnav.uab.es/docs/journals/Gomez_TAES2020.pdf),
and Huang *et al.*, *EURASIP Journal on Advances in Signal Processing* 2014,
[article](https://link.springer.com/article/10.1186/1687-6180-2014-73).
All quantitative performance claims above are constrained to the cited paper's
assumptions; no cited source validates VectorWarp's receiver, antenna, site, or
aircraft detection performance.
