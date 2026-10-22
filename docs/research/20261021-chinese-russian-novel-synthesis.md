# Chinese/Russian passive-radar synthesis for WWAT replay

Date: 2026-10-21

## Constraints established

- Five phase-coherent UHF channels, 2.4 Msps, 0.5 s CPI.
- The retained 563 MHz slice is approximately 561.8--564.2 MHz. The ATSC
  pilot for a 563 MHz-centred 6 MHz channel is near 560.31 MHz, outside the
  recording. Standards-valid pilot locking or full ATSC reconstruction is
  therefore unavailable from these captures.
- Candidate discovery must retain continuous scores. The +3 dB corrected
  margin is a later validation label, not a prerequisite for scoring.

## Strongest imported methods

1. A noisy-reference GLRT that estimates the unknown illuminator and noise
   rather than treating one recorded reference as exact.
2. Low-rank plus sparse separation of persistent direct path/multipath and
   moving echo components before the existing detector.
3. ADS-B-windowed L1 delay--Doppler inversion, optionally with common support
   and independent complex amplitudes across channels.
4. Fractional-delay, range-migration and Doppler-phase compensated coherent
   integration along a bistatic trajectory.
5. Cyclostationary cross-coherence only after cyclic features are demonstrated
   in the actual narrow ATSC slice.

## System-specific synthesis: cross-fitted trajectory-tube consensus

Construct several references without privileging one antenna: for each held-out
channel, estimate a direct-path waveform from the other four channels using a
target-protected rank-one/low-rank model. Evaluate a noisy-reference GLRT or
narrow sparse inversion in the held-out channel. Marginalize the evidence over
an ADS-B uncertainty tube derived from source age and inter-source disagreement,
rather than maximizing over a flat grid. Couple delay and Doppler through the
physical bistatic invariant that Doppler is proportional (up to the calibrated
sign convention) to excess-path range rate. Accumulate only mutually
consistent evidence across time, subbands, and leave-one-channel-out views.

A real echo should recur at one physical trajectory despite reference choice;
reference self-noise, multipath ghosts and isolated noise peaks should be much
less stable. This is a new combination for this dataset, not a claim of a new
fundamental detector.

## Additional system-specific branches

- Treat the unequal channel gains as multiple HDR exposures using a
  heteroscedastic/censor-aware likelihood; do not merely amplify low-gain
  channels to match high-gain noise.
- Use the time-reversed or Doppler-sign-inconsistent trajectory as a paired
  counterfactual control. Rank the forward physical path against its paired
  impossible path before applying broader empirical controls.
- Use cross-subband consensus, not best-subband selection: an echo must retain
  compatible delay, Doppler and bearing across independently scored subbands.

## Replay order

1. Noisy-reference GLRT versus normalized CAF.
2. ADS-B-windowed L1/common-support inversion.
3. Cross-fitted reference and trajectory-tube consensus.
4. RPCA only with injection tests proving bounded target attenuation.
5. Cyclostationary processing only if the recorded slice shows stable cyclic
   peaks.

