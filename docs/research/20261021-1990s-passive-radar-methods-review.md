# 1990s passive/covert/bistatic radar methods for five-channel UHF-TV IQ

Date: 2026-10-21

## Scope and bottom line

This is a decade-specific review of primary 1990--1999 papers, theses,
standards, patents, and technical reports.  It is deliberately narrower than a
general passive-radar history.  The implementation target is the existing
phase-coherent five-channel, 2.4 Msps, 563 MHz MCHQ recording and its known
aircraft trajectory.  All proposals below are replay-only; none requires a
live receiver or configuration change.

The best next replay experiments are, in order:

1. ATSC pilot/field-sync-assisted reference reconstruction followed by the
   existing CAF;
2. joint-domain localized GLR (JDL-GLR) and a rank-selected eigencanceller;
3. a normalized adaptive coherence estimator (ACE) on the same target-excluded
   training cells;
4. long-dwell pilot Doppler/phase interferometry when range can be sacrificed;
5. blind fourth-order source separation as an exploratory reference builder.

Hough track-before-detect is historically important but lower priority here:
the current replay matrix already integrates along exact truth trajectories,
which is a stronger oracle than a blind Hough search.

## What the decade established

Howland's television radar used a two-channel FFT to measure echo Doppler and
inter-element phase, time- and frequency-CFAR, mutual-coupling correction, a
Kalman association layer, and an extended Kalman fit for Cartesian position.
It did not require transmitter synchronization and reported tracking to
260 km.  The method used a television vision or sound carrier rather than a
wideband range CAF [Howland 1999].

Sahr and Lind built a synchronized passive bistatic system around commercial
FM, explicitly correlating a clean transmitter observation with remote weak
scatter.  Their 1997 paper and 1998 AFOSR final report demonstrate the value of
long coherent records, a clean reference, and phase-synchronous multichannel
receivers; the report records aircraft detections to 240 km [Sahr and Lind
1997; Sahr 1998].  Ringer, Frazer, and Anderson then treated broadcast-signal
suitability through the measured ambiguity function, receiver bandwidth,
antenna pattern, and observation time rather than assuming every part of a
broadcast channel is equally radar-useful [DSTO-TR-0809, 1999].

The same decade supplied the statistical tools that make those systems useful
with a small array: the CFAR adaptive matched filter [Robey et al. 1992],
localized/JDL adaptive processing [Wang and Cai 1991, 1994], adaptive matched
subspace/ACE detectors [Scharf and Friedlander 1994; Asilomar 1996],
eigencancellation [Haimovich and Bar-Ness 1991; Haimovich 1996], blind
higher-order beamforming [Cardoso and Souloumiac 1993], and Hough
track-before-detect [Carlson, Evans, and Wilson 1994].

Finally, the original 1995 ATSC A/53 standard makes the present illuminator
less opaque than an arbitrary waveform.  It specifies a pilot 310 kHz from the
lower channel edge at 11.3 dB below average data power, a four-symbol `1001`
segment sync every 77.3 microseconds, and a field-sync segment every 24.2 ms
containing known PN511 and PN63 sequences.  Those structures can provide
frequency/phase tracking and a noise-free partial reference [ATSC A/53, 1995].

## Untried replay experiments

### 1. ATSC-aided clean reference and selective CAF

**Algorithm.** Downconvert the 6 MHz 8-VSB channel so the pilot is at baseband;
estimate residual carrier frequency/phase from the pilot independently in each
channel; locate the `1001` segment sync and PN511/PN63 field sync; estimate each
channel's short complex impulse response by regularized least squares on the
known field-sync symbols.  Construct two references: (a) the known sync symbols
passed through the estimated transmitter/receiver response and (b) a
decision-directed 8-VSB remodulation, retaining estimated carrier drift.  Run
the existing range-Doppler CAF against both and combine only after separate
empirical controls.  Also test ambiguity-selected subbands from the 1999 DSTO
procedure rather than always correlating the full captured band.

**Expected gain.** A 704-known-symbol field-sync correlation has 28.5 dB
processing gain relative to one symbol, but its low duty factor means that is
not a 28.5 dB gain over the present full-waveform CAF.  The plausible benefit
is a cleaner reference and lower deterministic ambiguity sidelobes.  Reference
noise alone imposes a 3 dB ceiling when reference and surveillance SNR are
equal; multipath/sidelobe suppression can be substantially larger.  Report the
measured control-floor change, not the 28.5 dB number, as recovery gain.

**Cost/calibration.** Moderate CPU, small memory, no additional RF calibration.
It needs confirmation that the captured station is ATSC 8-VSB and that 2.4
Msps retains enough of the pilot/sync structure; the capture does not span the
full 6 MHz channel, so full remodulation may be impossible.  Pilot-only and
known-sync variants remain feasible if their frequencies are in band.

### 2. Joint-domain localized GLR (JDL-GLR)

**Algorithm.** For each target cell, transform the five-channel by adjacent-
Doppler cube into a small beam-Doppler region: three calibrated spatial beams
around the predicted bearing times three Doppler bins around predicted Doppler.
Estimate a 9-by-9 covariance from target-excluded neighboring delay cells,
shrink it toward its diagonal, and evaluate the GLR/AMF in this localized
coordinate system.  Freeze beam count, Doppler width, loading, and guard on
positives; apply the existing translated-trajectory controls and charge every
choice in the family maximum.

**Expected gain.** The primary 1994 result is data efficiency and faster
convergence, not a universal dB promise.  Reducing the adaptive dimension from
15 to 9 reduces the usual `2D` training heuristic from about 30 to 18
independent samples and should reduce covariance loss in this sample-starved
replay.  A realistic acceptance target is at least 3 dB over the empirical
familywise control threshold, not an assumed literature gain.

**Cost/calibration.** Low-to-moderate CPU; existing phase calibration and
bearing manifold suffice.  Beam mismatch is the main risk, so include one
adjacent beam on either side and never select a beam per target after looking at
the target score.

### 3. Rank-selected eigencanceller plus ACE

**Algorithm.** From target-excluded space-time training vectors, eigendecompose
the loaded covariance.  Choose clutter rank only from positive controls using
a predeclared grid such as `r in {1,2,3}`.  Form
`P_perp = I - U_r U_r^H`, project both observation and steering vector, then
score with adaptive coherence

`|s^H R^-1 x|^2 / ((s^H R^-1 s)(x^H R^-1 x))`.

The eigencanceller and ACE must be evaluated separately and together because
ACE's normalization can trade absolute sensitivity for CFAR robustness.

**Expected gain.** No defensible fixed dB value follows from the 1990s papers.
The opportunity is reduced estimation variance when a few direct-path/clutter
modes dominate; the cost is target self-nulling if the rank is too high or the
training guard leaks.  Require held-out positive retention and the same +3 dB
empirical margin used by the current matrix.

**Cost/calibration.** Trivial for 15 dimensions.  Existing phase calibration is
needed for the steering vector; eigenprojection itself needs none.  Use
leave-one-delay-out covariance and report eigenvalue gaps to expose unstable
rank choices.

### 4. ATSC-pilot Doppler/phase interferometry

**Algorithm.** Adapt Howland's carrier method to the ATSC pilot.  Mix the pilot
to zero, narrowband-filter each antenna, split the record into 2--10 s dwells,
FFT slow time, and estimate inter-channel phase for each Doppler peak.  Fit
bearing with the calibrated five-element manifold, use time/frequency CFAR,
and associate peaks across dwells with a constant-acceleration Kalman filter.
For replay evaluation, score the exact ADS-B Doppler/bearing trajectory and
translated Doppler/bearing controls before any blind tracker is allowed.

**Expected gain.** Extending coherent dwell from 0.5 to 10 s has an ideal 13 dB
coherent-integration advantage if oscillator/target phase stays coherent.
The ATSC pilot is 11.3 dB below average channel power, so the net comparison to
a full-band CAF is not that simple.  This experiment trades away bistatic range
and is most promising where the wideband reference is multipath-limited.

**Cost/calibration.** Very low CPU and memory.  It requires stable relative
channel phase, mutual-coupling correction, and residual common-frequency
tracking.  Bearing aliases and the near-zero-Doppler direct path are explicit
failure modes.

### 5. Blind fourth-order reference separation (JADE-style)

**Algorithm.** Whiten blocks of the five complex channels, estimate a small set
of fourth-order cumulant matrices, jointly diagonalize them using complex Jacobi
rotations, and reconstruct up to five statistically independent components.
Identify the direct broadcast component only by target-excluded evidence
(pilot/field-sync coherence and stable bearing), then use it as the CAF
reference.  Treat each retained component count as a charged trial.

**Expected gain.** The 1993 paper promises robustness to array-manifold errors,
not a fixed detection gain.  This can outperform a principal eigenbeam when
strong non-Gaussian cochannel interference or multipath occupies a different
independent component.  It will not separate coherent copies of the same ATSC
source reliably; failure to improve positive controls should terminate it.

**Cost/calibration.** Moderate-to-high CPU from fourth-order statistics but
small matrices.  No manifold calibration is required for separation; component
labeling and phase alignment are required before CAF use.

### 6. Blind Hough track-before-detect

**Algorithm.** Convert each calibrated CAF frame to empirical log-survival
evidence.  Accumulate it over quantized constant-velocity/acceleration curves
in delay-Doppler-time with a Hough transform, using leave-one-trajectory-out
controls and a family maximum over every kinematic bin.  Only after the blind
threshold is frozen should a resulting curve be compared with ADS-B truth.

**Expected gain.** For 20 independent frames, noncoherent integration offers an
ideal order of 6.5 dB (`5 log10 N`); coherent phase-preserving integration could
approach 13 dB (`10 log10 N`).  The current truth-trajectory scorer already
performs the relevant oracle integration, so this is an acquisition method,
not a likely explanation for present truth-path misses.

**Cost/calibration.** Potentially high search cost and a large trials penalty;
coarse-to-fine Hough bins control both.  No new RF calibration.

## Methods not recommended as new experiments

- A conventional AMF/GLRT, full covariance whitening, full 15-dimensional
  post-Doppler STAP, sparse whitened beam recovery, loaded reciprocal/mismatched
  filtering, and spectral interference rejection are already represented in
  the current replay suite.
- The 1997 Thomson-CSF repeated multicarrier patent requires modifying the
  transmitter and therefore is not applicable to an illuminator of opportunity.
- Analog-TV line/frame methods do not transfer literally to ATSC.  Their useful
  descendant here is pilot/field-sync processing.
- A live array or gain retune is outside this review and would invalidate the
  frozen replay comparison.

## Primary sources

- P. E. Howland, “Target tracking using television-based bistatic radar,”
  *IEE Proceedings Radar, Sonar and Navigation*, 146(3), 166--174, 1999,
  [doi:10.1049/ip-rsn:19990322](https://doi.org/10.1049/ip-rsn:19990322).
- J. D. Sahr and F. D. Lind, “The Manastash Ridge radar,” *Radio Science*,
  32(6), 2345--2358, 1997,
  [doi:10.1029/97RS02454](https://doi.org/10.1029/97RS02454).
- J. D. Sahr, *A Passive Radar for Atmospheric Remote Sensing Using Commercial
  FM Broadcasts*, AFOSR final report F49620-96-1-0138, 1998.
- M. A. Ringer, G. J. Frazer, and S. J. Anderson, *Waveform Analysis of
  Transmitters of Opportunity for Passive Radar*, DSTO-TR-0809, 1999,
  [report](https://citeseerx.ist.psu.edu/document?doi=48b192ae5e8cc504ab774f8b21d9378a60330d7b&repid=rep1&type=pdf).
- F. C. Robey, D. R. Fuhrmann, E. J. Kelly, and R. Nitzberg, “A CFAR adaptive
  matched filter detector,” *IEEE TAES*, 28(1), 208--216, 1992,
  [doi:10.1109/7.135446](https://doi.org/10.1109/7.135446).
- H. Wang and L. Cai, “A localized adaptive MTD processor,” *IEEE TAES*,
  27(3), 532--539, 1991,
  [doi:10.1109/7.81435](https://doi.org/10.1109/7.81435).
- H. Wang and L. Cai, “On adaptive spatial-temporal processing for airborne
  surveillance radar systems,” *IEEE TAES*, 30(3), 660--670, 1994,
  [doi:10.1109/7.303737](https://doi.org/10.1109/7.303737).
- L. L. Scharf and B. Friedlander, “Matched subspace detectors,” *IEEE TSP*,
  42(8), 2146--2157, 1994,
  [doi:10.1109/78.301849](https://doi.org/10.1109/78.301849).
- A. M. Haimovich and Y. Bar-Ness, “An eigenanalysis interference canceler,”
  *IEEE TSP*, 39(1), 76--84, 1991,
  [doi:10.1109/78.80767](https://doi.org/10.1109/78.80767).
- A. M. Haimovich, “An eigencanceler: adaptive radar by eigenanalysis
  methods,” *IEEE TAES*, 32(2), 532--542, 1996,
  [doi:10.1109/7.489498](https://doi.org/10.1109/7.489498).
- J.-F. Cardoso and A. Souloumiac, “Blind beamforming for non-Gaussian
  signals,” *IEE Proceedings F*, 140(6), 362--370, 1993,
  [doi:10.1049/ip-f-2.1993.0054](https://doi.org/10.1049/ip-f-2.1993.0054).
- B. D. Carlson, E. D. Evans, and S. L. Wilson, “Search radar detection and
  track with the Hough transform. I,” *IEEE TAES*, 30(1), 102--108, 1994,
  [doi:10.1109/7.250410](https://doi.org/10.1109/7.250410).
- ATSC, *Digital Television Standard A/53, Annex D*, 16 September 1995,
  [archived standard](https://ptacts.uspto.gov/ptacts/public-informations/petitions/1468714/download-documents?artifactId=dJPHhyBWEANTG5HOc4pG0dgkVdwolr1yekBvK6ozY7eQqtovHF6AUgc).

