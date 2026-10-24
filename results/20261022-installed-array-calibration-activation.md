# Installed-array calibration activation — 2026-10-22

## Decision

- The operator-confirmed physical channel mapping is authoritative: logical and physical channels are `[0, 1, 2, 3, 4]`.
- Geometry and orientation are confirmed. CH0 is 80 degrees true; CH1–CH4 proceed clockwise in 72-degree steps.
- RF remains 563 MHz at 2.4 Msps with gains `[19.7, 25.4, 20.7, 25.4, 22.9]`.
- Bearing publication remains disabled while the measured installed manifold is collected and held-out qualification is incomplete.
- Automatic manifold promotion remains disabled. This prevents sparse or biased calibration coverage from changing live bearing behavior.

## Activated measurements

- The live manifold recorder is enabled and writes labelled complex five-channel samples plus reference-drift observations.
- The frequency-resolved installed-array shadow is enabled with 24 subbands and periodic captures.
- The hourly recovery monitor retains headroom, frame-continuity, ADS-B provenance, and matrix checks.

## Verified live acceptance

- Release: `/opt/vectorwarp/releases/20261022T164054Z-unknown`
- Configuration revision: `ea5ddd10b6a717716ad1f44ad069fc1cb4f63172f8041f5ba247fd6f1783ff15`
- Two status reads five seconds apart showed advancing `lastFrameAt` values and processor state `live`.
- Manifold recorder status after activation: enabled, 20 evidence frames, 3 labelled aircraft samples, 7 reference samples, no recorder error.
- Stored evidence declares `covariance_mvdr`, `fixed_site`, 60 scan beams, and channel mapping `[0,1,2,3,4]`.
- Frequency-resolved shadow completed without error. Its latest median ideal-manifold correlation was approximately 0.579 while the median rank-one fraction was approximately 0.9984. This confirms a stable coherent direct-path response that differs materially from the ideal geometric manifold.
- The processor log contains no `Invalid manifold replay configuration` after the corrected release was activated.
- All four ADS-B sources remained configured.

## Receiver headroom result

Two independent ten-second raw-ADC observations found no sustained clipping or compression. P99.9 magnitudes were roughly 0.39–0.56 full scale and peaks roughly 0.55–0.75 full scale. Extremely rare historical rail events exist on CH0 and CH4, but their observed fractions are too small to explain the broad detection deficit. The evidence favors installed-site multipath/manifold mismatch and cancellation limitations over gross ADC overload.

## Software defect repaired

The resident GPU publication path attempted to emit manifold evidence while bearing output was disabled, but previously discarded the configured replay metadata in that state. This caused `Invalid manifold replay configuration` after physical confirmation enabled evidence collection. The processor now passes an explicit evidence-only bearing replay descriptor to the serializer without enabling live bearing inference or changing detection DSP.

## Qualification gate

The installed manifold is not eligible for use yet: current azimuth coverage is sparse (the automation initially reported 1/60 sufficiently populated bins). Collection continues. A candidate may be promoted only after pass-disjoint held-out evaluation demonstrates improvement in 15/30/45-degree accuracy, median angular error, pass-macro accuracy, and correct-lobe selection without detection regression.
