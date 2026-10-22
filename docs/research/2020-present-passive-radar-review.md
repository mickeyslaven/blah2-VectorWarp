# 2020-present passive-radar aircraft detection: replay relevance

## Scope and evidence

This review concerns algorithms usable from retained five-channel UHF-TV raw IQ,
not classifier training or live changes.  The sources below are primary papers or
authoritative institutional copies.

* Filip-Dhaubhadel and Shutin, 2020/21, *Long Coherent Integration in Passive
  Radar Systems Using Super-Resolution Sparse Bayesian Learning*, IEEE TAES,
  DOI 10.1109/TAES.2020.3026844: https://elib.dlr.de/136155/
* Martelli et al., 2020, long coherent integration with range/Doppler migration
  compensation on real multi-polarimetric passive-radar data, DOI
  10.1109/RadarConf2043947.2020.9266624:
  https://iris.uniroma1.it/handle/11573/1543727
* Wang et al., 2021, sparse-Bayesian direct/multipath clutter estimation with a
  contaminated reference, DOI 10.3390/s21206736:
  https://pmc.ncbi.nlm.nih.gov/articles/PMC8538174/
* Nikaein et al., 2021, joint sparse range-Doppler regression/detection (LAR),
  DOI 10.1109/JSEN.2020.3035630:
  https://doi.org/10.1109/JSEN.2020.3035630
* PC-12 single-transmitter DVB-T measurement campaign with UHF/VHF SDR and
  beamforming, 2022, DOI 10.3390/s22072766:
  https://pmc.ncbi.nlm.nih.gov/articles/PMC9003249/

## Mapping to the replay matrix

| Modern method | Exact algorithm | Existing coverage | Gap / cost / calibration |
|---|---|---|---|
| Motion-compensated long coherent integration | estimate range walk, Doppler rate and residual phase; resample/de-rotate each short-CPI complex CAF then coherently sum over a path/hypothesis | CPI stacking and track-before-detect exist, but only count as covered if they retain **complex** CAF phase and explicitly correct range/Doppler migration | **Untried if current stacking is power-only.** Highest weak-echo promise: coherent rather than square-law gain. Cost is a trajectory bank times frames; use FFT/CZT or local interpolation. Needs common clock, direct-path phase tracking, accurate geometry or a small rate grid. All trajectory/rate choices need shared controls. |
| SBL disturbance covariance / sparse clutter estimate | express clutter covariance or delay-Doppler atoms sparsely; alternate clutter/multipath estimate and suppression; then matched detection | loaded reciprocal, spectral rejection, ECA-like cancellation concepts, post-Doppler STAP | Likely untried, but overlaps STAP/cancellation. Cost is iterative matrix solves per batch and substantial hyperparameter trials. Needs target-protected training and carefully frozen sparsity prior. |
| Joint LASSO/LAR target+clutter decomposition | solve a sparse regression on a delay-Doppler dictionary, selecting target and direct/multipath atoms together rather than first nulling a prescribed clutter subspace | local ratios, max-stat and CAF scoring do not jointly model atoms; sparse beam is spatial, not delay-Doppler regression | Genuinely distinct but expensive and prone to selection bias. Use only local regions around a frozen path plus matched translated regions; calibrate regularization and dictionary coherence. |
| Multi-polarimetric autoregressive adaptive detection | multichannel AR disturbance model and adaptive detector across polarizations | no polarization channels are documented; spatial STAP/GLRT are nearest analogues | Not a first experiment. A five-element spatial-AR variant is possible but substantially overlaps post-Doppler STAP and requires better manifold/calibration validation. |
| Deep learning range-Doppler classifier/detector | supervised CNN/RNN/transformer over CAF cubes | no sufficiently large, independent labelled raw-IQ corpus; ADS-B is disallowed for deployment decisions | Do not pursue for blind recovery. High leakage/domain-shift risk and poor empirical false-alarm calibration. |

## Top candidate: complex migration-compensated track integration

For each retained replay, form short complex CAFs.  For a frozen candidate path
and a small predeclared acceleration/range-walk grid, interpolate the complex
CAF at each compensated delay/Doppler coordinate and compute

`T(theta) = | sum_k w_k C_k(tau_k(theta), nu_k(theta)) exp(-j phi_k(theta)) |^2`.

Use exactly the same `theta` bank at the target and every translated control,
then pool the maxima across all grid trials for the empirical p99 threshold.
Require direct-path phase-coherence diagnostics before attempting coherent gain;
fall back to present incoherent stacking otherwise.  This retains blind
deployment semantics: historical truth only defines offline evaluation paths,
while deployment searches the same frozen motion bank over the surveillance
volume.

This is the best next implementation because it reuses existing CAF products,
adds no array hardware assumptions, and directly targets weak echoes whose
energy smears across 0.5-s CPI bins.  It is more defensible than a learned
detector and lower-risk than an iterative sparse Bayesian cancellation method.
