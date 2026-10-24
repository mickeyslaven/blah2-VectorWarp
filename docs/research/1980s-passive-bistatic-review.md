# 1980s passive/bistatic radar: replay relevance

## Evidence base

The 1980s were a transition decade rather than the modern broadcast-PCL era.
The most relevant experimental line was UCL's early-1980s passive receiver
hitchhiking on Heathrow ATC radar at about 600 MHz.  It demonstrated real-time
synchronization, coherent MTI, pulse chasing, and a digital-beamforming array.
The historical account is **not** evidence that this system used continuous
television; its transmitter was pulsed ATC radar.  It is nevertheless a close
UHF, multichannel precursor to the present five-channel receiver.

* Griffiths, Baker et al., *An Introduction to Passive Radar*, historical
  overview, reports the UCL/Heathrow frequency and the four techniques:
  https://ftp.idu.ac.id/wp-content/uploads/ebook/tdg/ADNVANCED%20MILITARY%20PLATFORM%20DESIGN/An%20Introduction%20to%20Passive%20Radar.pdf
* US Air Force priority filing of 13 December 1989, later published as the
  narrowband passive differential tracking patent, explicitly names UHF/VHF TV
  illuminators and updates position/velocity from Doppler measurements:
  https://patents.justia.com/patent/6424290
* US Patent 4,644,356 (1987) documents a passive bistatic coherent receiver,
  external/noncooperative illumination, aircraft echoes, and terrain-clutter
  footprints: https://patents.justia.com/patent/4644356

## Mapping to the current replay matrix

| 1980s technique | Core algorithm | Current coverage | Replay decision |
|---|---|---|---|
| Coherent MTI | phase-stable slow-time differencing/filter bank after synchronization | spectral-interference variants, CPI Doppler processing, post-Doppler STAP | Covered; fixed MTI is dominated by adaptive STAP and risks slow-aircraft cancellation. |
| Digital beamformed pulse chasing | use direct-path timing to place receive/array processing on each illuminating pulse | coherent beam bank, gain equalization, spatial GLRT, sparse beam | Spatial part covered. Pulse chasing itself does not apply to continuous ATSC. |
| Real-time reference synchronization | estimate carrier/sample-clock/phase from direct path before coherent processing | partially covered by reference processing, but no explicit per-CPI phase-coherence quality gate is documented | Genuinely useful adaptation: pilot/direct-path phase tracking and complex CAF integration. |
| Two-Doppler differential tracking | update a target state from Dopplers from geographically separated illuminators | track-before-detect/stacking only if multi-illuminator measurements exist | Out of scope for the one-illuminator 563-MHz slice; retain as a future multi-station fusion method. |
| Terrain-clutter footprint emitter localization | correlate fixed clutter signature/geometry to infer transmitter position | not needed if transmitter position is already known; spatial maps partly overlap | Calibration/QA only, not a weak-echo detector. |

## Best candidate: pilot-locked complex path integration

Adapt the UCL synchronization/coherent-MTI premise to continuous ATSC instead
of pulsed beam chasing.  For each 0.5-s CPI, estimate the direct-path pilot
phase, carrier offset, and fractional sample-clock drift from the strongest
reference/array combination.  Correct each surveillance/reference block before
CAF formation.  Along an exact delay-Doppler trajectory, retain the complex CAF
value and form

`T = |sum_k w_k z_k exp(-j phi_path,k)|^2`.

`phi_path,k` is the predicted bistatic slow-time phase, with a fitted residual
phase only when held-out positives establish coherence.  Compare with identical
translated trajectories; include phase-model choices and all coherence gates in
the empirical parameter-trial control distribution.

This is distinct from incoherent CPI stacking and local-ratio/max-stat scoring.
It is cheap after CAF production (linear in frames), needs no new RF hardware,
and has a potentially material weak-echo gain if residual inter-CPI phase drift
currently forces power-only accumulation.  It requires stable common-clock
channels, direct-path SNR, transmitter-pilot observability, and a strict
coherence veto to avoid self-fitting controls.

## Ranking

1. Pilot-locked complex path integration: best fit and lowest implementation
   risk; test offline with existing raw IQ.
2. Constrained adaptive beam/reference phase tracker: augment the pilot tracker
   with the five-channel direct-path beam; this overlaps existing spatial work
   and should be attempted only if its calibration residual is not already
   removed by gain equalization/STAP.
3. Multi-illuminator two-Doppler track update: historically appropriate but
   deferred until retained captures contain independent transmitter measurements.
