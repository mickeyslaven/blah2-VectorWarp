# Passive aircraft detection, 2010–2019: targeted review

## Scope and decision boundary

This is a separate 2010--2019 review for the Ford Street replay programme.  The
receiver is a synchronised five-channel, 2.4 MHz, 563 MHz UHF-TV raw-IQ
recording; it is not evidence for changing the live receiver.  Comparison is
against the methods already in the matrix: loaded-reciprocal and
spectral/interference references, coherent beam bank, fixed-beam spatial GLRT,
post-Doppler STAP, sparse beam, gain equalisation, CPI/method stacking, and
max-statistic trial correction.

"Untried" below means no equivalent replay arm in that matrix.  It does not
mean that a paper's reported improvement transfers to this site.  In
particular, WWAT-CD is not assumed to be DVB-T/OFDM: an OFDM-specific proposal
must first prove its modulation/timing assumptions on a retained direct-path
recording.

## Primary and authoritative period sources

* Yi, Wan, Li and Leung, [*Robust Clutter Rejection in Passive Radar via
  Generalized Subband Cancellation* (2018)](https://doi.org/10.1109/TAES.2018.2805228),
  develops a tailored subband clutter subspace for carrier-frequency offset,
  sampling-frequency offset, fractional delay, and hardware frequency
  response, and validates it with field data.
* Jishy and Lehmann, [*A Bayesian track-before-detect procedure for passive
  radars* (2013)](https://doi.org/10.1186/1687-6180-2013-45), gives a passive
  radar measurement model and a tractable Gaussian-mixture Bayesian recursion
  over delay/Doppler state rather than thresholding each CPI independently.
* Hack, Patton, Himed and Saville, [*Detection in Passive MIMO Radar Networks*
  (2014)](https://doi.org/10.1109/TSP.2014.2319776), derives the passive-MIMO
  GLRT and approximate statistic distributions while exploiting additional
  reference/surveillance correlations.
* Colone, Langellotti and Lombardo, [*DVB-T Signal Ambiguity Function Control
  for Passive Radars* (2014)](https://doi.org/10.1109/TAES.2013.120616), is the
  relevant period treatment of television-waveform ambiguity/sidelobe control.
* Wang, Wan, Yi and Yi, [*Improved scheme for clutter rejection and DOA
  estimation in passive radar* (2019)](https://doi.org/10.1049/joe.2019.0584),
  describes subcarrier-based spatial adaptive processing; its stated OFDM
  premise limits direct applicability here.
* Colone, Langellotti and Lombardo, [*DVB-T-based passive radar for target
  detection using multiple frequency channels* (2016)](https://pmc.ncbi.nlm.nih.gov/articles/PMC5087383/),
  is useful authoritative evidence for multi-frequency diversity, but needs
  simultaneous extra broadcast bandwidth not contained in these recordings.

## Candidates that remain genuinely distinct

| Candidate | Exact offline algorithm | Plausible benefit and acceptance hazard | Cost/calibration |
|---|---|---|---|
| **Generalized subband cancellation (GSC)** | Split the reference and each surveillance channel into fixed, critically sampled analysis bands.  Per band, estimate fractional delay/phase and a short direct-path/multipath basis from training samples outside an ADS-B path guard.  Concatenate the corrected band bases, solve a *fixed, predeclared* ridge least-squares cancellation, reconstruct the residual, then run the already frozen CAF/spatial scoring.  Choose no parameters on misses; use one frozen subband partition and loading selected on tracked controls. | This is not loaded reciprocal (which weights the reference spectrum) or reduced-rank ECA alone: it explicitly models band-dependent clock/phase/analogue response mismatch.  It can help only if those mismatches leave structured residual clutter; it has no defensible universal dB prediction.  Main risk is target subtraction and a large implicit trial family. | FFT/filter-bank plus one small ridge solve per CPI/channel; memory is bounded by the filter taps and training block.  Calibrate per-band delay/phase on direct path, reserve target/path guards, and include all band/layout variants in the frozen max-stat trial family. |
| **Gaussian-mixture Bayesian TBD** | Convert each unthresholded 0.5-s CAF/spatial likelihood surface to a calibrated log-likelihood ratio using matched off-trajectory controls.  Propagate a Gaussian mixture in Cartesian constant-velocity/turn-rate state; map components through known bistatic delay, Doppler, and bearing; update with the cell likelihood; prune/merge only by predeclared posterior-mass rules.  Report posterior odds for an ADS-B-gated offline path and a blind-control path. | Distinct from current CPI/method stacking: it preserves competing low-SNR states and applies motion geometry jointly, rather than summing a fixed known path.  It addresses intermittency but cannot create signal absent from the per-CPI maps.  Its key risk is data-dependent birth/pruning becoming an uncounted search. | No raw-IQ replay once compact complex maps exist.  Compute grows with components × CPIs × candidate cells; start with a bounded mixture and a single kinematic model.  Freeze clutter intensity, process noise, birth density, pruning, and posterior threshold on tracked positives plus matched controls; max-stat correct model variants and blind births. |
| **Constrained ambiguity-sidelobe (mismatched) filter** | Estimate the direct-path autocorrelation/ambiguity matrix from the actual reference.  Solve a regularised constrained filter: unit response at zero delay/Doppler, minimise integrated energy in a predeclared near-range/near-Doppler sidelobe region, and cap SNR loss.  Use its output as the reference for ECA/CAF; compare with the existing loaded-reciprocal reference with filter choices frozen. | More specific than a scalar-loaded reciprocal: it targets measured ambiguity leakage that can mask short-delay aircraft cells.  The DVB-T source is rationale, not proof for ATSC/8-VSB.  It is not a top choice until the measured direct-path ambiguity has a persistent, target-overlapping sidelobe ridge. | One Toeplitz/Gram solve per reference calibration block, then FIR convolution.  Requires a stable ambiguity estimate and careful gain/noise normalisation; count sidelobe-region and loss-cap alternatives as trials. |
| **Passive-MIMO reference/surveillance GLRT** | Form the joint multi-channel covariance for reference and five surveillance channels in a local delay/Doppler neighbourhood.  Evaluate the passive-MIMO likelihood ratio with covariance estimated from withheld local controls; optionally use shrinkage fixed from positives. | The paper's joint correlation statistic is broader than the current fixed-beam spatial GLRT, but overlaps it strongly.  It is therefore a verification/comparator, not a first new recovery arm. | A six-channel covariance inverse per local cell; sample-starved local covariance makes calibration and false alarms the dominant risk. |

## Explicit non-candidates for this recording

* **Multi-frequency DVB-T fusion** is untried but not feasible from a single
  2.4 MHz capture: it requires simultaneous independent illuminator bands, so
  it cannot be honestly reconstructed offline.
* **Subcarrier/OFDM spatial processing** is not an immediate candidate.  The
  source presumes CP-OFDM subcarriers; use only after a modulation/timing
  diagnostic verifies that premise.  It should not be smuggled in as generic
  spatial STAP.
* **Displaced-phase-centre / moving-platform STAP variants** solve platform
  motion geometry absent from this stationary receiver.

## Ranked recommendation

1. **Generalized subband cancellation is the top implementation candidate.**
   It is waveform-format agnostic when driven by the measured direct-path
   reference, operates on the present five-channel raw IQ, and directly tests
   a failure mode (band-dependent phase/clock/hardware mismatch) not isolated
   by the existing loaded reciprocal, spectral rejection, or post-Doppler
   spatial arms.  First run a diagnostic only: residual direct-path/clutter
   power and target-versus-matched-control CAF energy for one frozen band
   partition.  Proceed to the full replay matrix only if the diagnostic shows
   lower structured residual without a positive-control loss.
2. **Gaussian-mixture Bayesian TBD** is second.  It is the right follow-on if
   compact maps show intermittent but calibrated truth-near energy after the
   best waveform cancellation; otherwise it simply integrates clutter.
3. **Constrained ambiguity filtering** is third and conditional on an observed
   ambiguity ridge.  It is a smaller waveform experiment but less assured for
   the actual TV modulation than GSC.

For every candidate, acceptance remains unchanged: retain an independent
tracked positive, obtain a persistent ADS-B-consistent miss trajectory, and
exceed matched/whole-search controls by at least the predeclared,
trial-corrected +3 dB-equivalent margin.  No period source justifies relaxing
that criterion or claiming a site-specific gain before replay.
