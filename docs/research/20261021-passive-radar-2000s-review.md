# Passive aircraft detection, 2000–2009: targeted review

## Scope and current baseline

This review is limited to methods published in 2000–2009 that are meaningfully
distinct from VectorWarp's replay methods: loaded reciprocal and spectral
interference references; five-channel coherent beam bank; spatial GLRT;
post-Doppler STAP; sparse beam; gain equalisation; CPI/method stacking; and
max-statistic trial correction.  It concerns a synchronized five-channel,
2.4 MHz UHF-TV raw-IQ receiver, rather than a change to the live receiver.

## Primary period sources

* Poullin, *Passive detection using digital broadcasters (DAB, DVB) with
  COFDM modulation*, IEE Proc. R&SN 152 (2005), 143–152,
  [DOI](https://doi.org/10.1049/ip-rsn:20045017). It establishes DVB/COFDM as
  a practical illuminator and makes waveform-aware reference construction,
  not merely a raw reference, historically grounded.
* Saini and Cherniakov, *DTV signal ambiguity function analysis for radar
  application*, IEE Proc. R&SN 152 (2005), 133–142,
  [DOI](https://doi.org/10.1049/ip-rsn:20045067). This is the relevant source
  for ambiguity sidelobe/range-Doppler structure and mismatched filtering.
* Howland, Griffiths and Baker, *Passive Bistatic Radar Systems* (2008),
  [DOI](https://doi.org/10.1002/9780470985755.ch7), documents the practical
  PBR chain and its sensitivity/coverage constraints.
* Tobias and Lanterman, *Techniques for birth-particle placement in the PHD
  particle filter applied to passive radar* (2008),
  [DOI](https://doi.org/10.1049/iet-rsn:20070051). This supplies a concrete
  passive-radar multi-target, low-SNR integration route rather than an
  independent per-CPI detector.
* Dersan, *Passive radar localization by TDOA measurements* (2001),
  [METU thesis record](https://hdl.handle.net/11511/11473), is an early
  authoritative treatment of bistatic measurement geometry.

## Genuinely untried candidates

| Method | Exact replay algorithm | Why distinct / likely gain | Cost and calibration risk |
|---|---|---|---|
| Decoded/reconstructed COFDM reference | Synchronize to ATSC/DVB pilot/OFDM structure, estimate channel, regenerate transmit symbols, then ECA/CAF against the regenerated low-noise reference. | Distinct from loaded reciprocal: attacks reference noise and broadcast-content variation before cancellation. Best chance where direct-path reference quality, not target energy, is limiting. | Requires a standards-specific demodulator, symbol/pilot lock and transmitter/channel knowledge; high implementation risk for ATSC 1.0 at 563 MHz. |
| Cyclostationary CAF | Form CAF only after cyclic-frequency filtering/correlation at known OFDM cyclic prefixes/pilots; use non-target cyclic bins as controls. | Rejects stationary non-cyclostationary interference and is not spectral masking or conventional CAF. | Moderate FFT cost; must estimate/correct sample-clock and carrier offset. UHF channel content/pilot parameters must be validated. |
| Geometry-constrained PHD/particle TBD | Propagate particles in Cartesian constant-velocity state; map each to bistatic delay/Doppler/bearing likelihood from CAF/GLRT cells; use clutter-density likelihood and posterior odds. | Unlike current fixed motion paths/CPI stacking, integrates a calibrated measurement likelihood and multiple hypotheses across maneuvers. | CPU proportional to particles × CPIs; needs measurement covariance and false-alarm intensity learned only from frozen positives/controls. |
| Subspace projection on a slow-time clutter basis | Estimate a low-rank temporal subspace from delay guard cells over several CPIs, project each range-bin slow-time vector out of it, then a steering-vector GLRT. | Different from post-Doppler STAP: explicitly learns temporal clutter modes shared across nearby ranges and preserves a target motion steering vector. | Moderate memory (range-neighborhood × 20 CPIs); vulnerable to target self-subtraction unless target guard/path is withheld. |

## Ranking

1. **Geometry-constrained PHD/particle TBD** is the best implementation
   candidate. It reuses compact existing per-CPI spatial/CAF evidence and
   ADS-B only for offline scoring, needs no waveform demodulator, and directly
   addresses intermittent sub-threshold trajectories. Freeze measurement
   likelihood, particle birth density, and posterior threshold on the same
   epoch's tracked positives and matched controls; evaluate a separate frozen
   miss cohort with max-stat correction for particle-count/model variants.
2. Slow-time clutter-subspace GLRT is a bounded second candidate if PHD shows
   no gain. Its main acceptance hazard is target leakage into the learned
   subspace.
3. Reconstructed COFDM reference is potentially higher upside but should wait
   for a verified standards lock and reference-SNR diagnostic; it is not a
   small change to the present raw-CAF pipeline.

No source supports claiming a gain for this site without replay controls.
Acceptance remains a persistent truth-near trajectory with the positive
control retained and trial-corrected unrelated-output control.
