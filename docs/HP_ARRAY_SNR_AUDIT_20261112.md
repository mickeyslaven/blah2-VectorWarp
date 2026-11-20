# HP array versus HomePortal: mechanism, not a promised gain

Read-only HP verification at 2026-11-12 approximately22:50UTC: current release
`20261112T200825Z-a375991a0b81-proof1`; five channels at509MHz/2.4MS/s;
gain settings8.7/3.7/16.6/3.7/3.7dB; eigenbeam reference; noise-whitened12-beam
fusion/effective trials12; fractional2/4/8-CPI TBD135 motion trials;5dB birth,
adaptive4/6,maxAcc10,delete3; guided shadow on/promote=false; bearing off.
No live setting changed. HomePortal's directional two-antenna reference/
surveillance architecture is the comparison contract, not a measured equal-site
antenna or noise-temperature calibration.

For equal elements, independent noise and perfect steering, five-channel
coherent surveillance gain is10log10(5)=6.99dB over ONE identical element.
Two HomePortal antennas do not provide an additional3dB surveillance-array gain:
one supplies reference and the other surveillance. With identical illumination,
bistatic geometry, bandwidth and CPI, a useful bookkeeping approximation is

`HP−Yagi = 6.99 + Gdipole−GYagi + 10log10(ηcoherent)
           + 10log10(Tsystem,Yagi/Tsystem,HP) + reference-quality differences`.

Detection-threshold/search penalties must be compared separately. If one merely
assumes2.15dBi per dipole and10dBi for the surveillance Yagi, the ideal received
array advantage is already−0.86dB, before losses; these antenna values are an
illustration, NOT measurements of either installation. Gain pattern at the
actual target elevation/azimuth, polarization, cable/mismatch loss and transmitter/
receiver geometry are unknown. The five planar elements cannot create the same
elevation/directivity discrimination as an arbitrary directional installation.

For a signal steering vector `a`, noise covariance `R`, and selected weights `w`,
output SNR is proportional to `|wᴴa|²/(wᴴRw)`. Ideal covariance-aware weights give
`aᴴR⁻¹a`; their mismatch efficiency is the ratio of those two quantities and is
at most1. Equal positive phase-aligned pairwise noise correlationρ changes the
ideal five-element gain to `5/(1+4ρ)`:3.98dB atρ=.25 and2.22dB atρ=.5.
Independent Gaussian element phase errors with RMSσ yield expected finite-array
coherent efficiency `(1+4exp(−σ²))/5`: about−0.93dB at30° and−3.31dB at60°.
These are models, not estimates of HP's current correlation or phase error.

In an independent-noise, weak-echo cross-correlation model, finite reference
SNRγr contributes approximately `γr/(1+γr)` efficiency. A synthesized reference
does not supply another unconditional7dB surveillance gain. Shared-channel
reference/surveillance noise, residual direct path and cancellation leakage can
violate the independent-noise model and require the retained joint covariance.

At the configured12 CFAR training cells andPFA1e−5, the implementation's Gamma
threshold formula gives the following threshold-power penalties (not RF loss):

| Integration depth |12-beam versus1-beam penalty|additional135 motion trials|
|---|---:|---:|
|2 CPI|1.001dB|1.678dB|
|4 CPI|0.763dB|1.263dB|
|8 CPI|0.589dB|0.974dB|

The 135 trials are correlated, so these are configured algebraic corrections,
not a measured global null-tail guarantee. Baseline and moving bank currently
each receive a PFA budget; their union is not proven to share one globalPFA.
Do not remove the correction or lower thresholds to claim recovered SNR.
Noncoherent power integration is not coherent voltage integration:8CPI cannot
automatically be advertised as9dB SNR gain. Improvement depends on continuity,
noise distribution, path mismatch and threshold/trial costs.

## Capability inventory and genuinely missing work

|Method|Verified status|What is genuinely new or still missing|
|---|---|---|
|Five-channel coherent beam bank/eigenbeam reference|Live active|No new feature claim; characterize mismatch and shared-reference effects.|
|Residual-noise weighting|Live active|`GpuResidentFrame.private.h` derives diagonal scales from reference covariance, bounded[.5,2]. It is not local post-clutter full covariance.|
|Fractional2/4/8 motion-path TBD|Live active|No new feature claim; compare on identical data and preserve ordinary2CPI arm.|
|Per-channel gain settings/channel-health source|Gain settings active; bounded health source exists|Tuner gains are not proof of equal noise or complex calibration. Quantify post-clutter power spread/coherence and clipping.|
|Measured detection manifold tooling|Source implemented; live automation configured, no manifold_file in audited config|Qualification/application is missing, not a new manifold algorithm. Avoid training on scored passes.|
|Frequency-resolved bearing work|Configured shadow; bearing itself disabled|Not current detection/tracking gain.|
|Guided recovery|Live shadow only; safer disjoint-credit source implemented|Promotion remains off;HP4/6 is infeasible for the experimental independent2CPI credit policy.|
|Local diagonal/fixed.75-shrinkage post-clutter statistic|Portable source/offline-only|New paired executable comparison, null-tail calibration and future backend qualification are required. Existing raw-power Gamma thresholds do not apply automatically.|
|Exact causal native funnel|Source implementation in this milestone, opt-in/unqualified for live load|This supplies missing admission/gate/transition evidence; it does not itself add SNR.|

Priority is evidence fidelity, then a frozen local-statistic comparison. Expected
benefit has no defensible numeric dB promise yet: diagonal normalization can
recover loss from unequal channel/background variances; fixed shrinkage can
suppress correlated nuisance modes but may suppress real echoes in the same
subspace. Validate target/training contrast, per-CPI continuity and complete
null/false-track exposure at fixed thresholds/PFA. Keep HomePortal's clean
directional path unchanged. Only after these tests can any new production
statistic/backend or measured-noise gate model be justified.
