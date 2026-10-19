# Ford Street bearing and reference survey — 2026-10-19

## Result

The physical CH0/+X heading remains **80 degrees true**. Nothing in this survey
supports a large physical rotation or a reversed channel order. The two
quality-gated known-angle sources favor the configured forward/clockwise channel
family and an identity-mapping fitted phase axis of **168 degrees true**. This
fitted phase axis is a model/calibration result, not the physical CH0 heading.

The current bearing error is principally a processing/installed-response problem:
target-cell spatial vectors are dominated by a repeatable illuminator-related
component, and lightly loaded MVDR turns that component into a stable null/alias.
It is not evidence that the antenna moved by 90–170 degrees.

## Known-angle survey

Every retune used the receiver's supported full lag-and-phase calibration. The
survey tool independently gates each source at 10 updates, median coherence at
least 0.8, and median inter-channel amplitude span no greater than 6 dB.

| Source | RF center | True bearing | Updates | Coherence | Amplitude span | Gate |
|---|---:|---:|---:|---:|---:|---|
| WPXI RF23 | 527 MHz | 61.958° | 21 | 0.963 | 1.48 dB | pass |
| WINP RF16 | 485 MHz | 86.616° | 14 | 0.919 | 1.82 dB | pass |
| WTAE RF27 | 551 MHz | 129.814° | 17 | 0.659 | 15.57 dB | reject |
| WPGH RF20 | 509 MHz | 36.726° | 17 | 0.941 | 9.94 dB | reject |
| WPCB RF28 | 557 MHz | 103.134° | 15 | 0.983 | 15.52 dB | reject |

WPXI and WINP jointly produce:

- configured identity mapping / forward family: fitted phase axis 168°, median
  source-balanced phase residual 29.79°;
- best reverse family: fitted phase axis 200°, residual 68.72°;
- cyclic forward mappings: exactly equivalent solutions separated by the
  five-element spacing of 72°. Physical channel identity selects forward-0.

The rejected sources are still informative: WPGH and KDKA both arrive from the
north-northeast and show large channel imbalance, while WPCB and WTAE show large
imbalance farther east/southeast. That pattern is directional and installed-site
dependent; it must not be flattened into one global per-channel gain correction.

Station parameters came from current FCC-derived records and FCC station data:
[Pittsburgh facilities](https://www.fccinfo.com/CMDProFacLookup.php?sCity=PITTSBURGH&sCurrentService=TV&sState=PA&tabSearchType=City),
[WPGH FCC profile](https://publicfiles.fcc.gov/tv-profile/wpgh-tv), and
[WPCB facility record](https://fcc.report/LMS/25076f916a6a0ecc016a7e6d71612d7a).

## Before/after roof move control

The preserved pre-move archive contains 4,266 labelled target samples from 80
aircraft in all eight azimuth sectors. With the configured mapping it had 53.83°
median bearing error; the best heading-only replay was 32.48° at +33°. This was
already not calibrated well enough for bearing publication.

The post-move RF25 snapshot contains 1,284 samples from 31 aircraft in five
sectors. Its configured-mapping median error is 159.16°. A heading-only replay
can make the same contaminated spatial output appear good (11.88° at -169°), but
that is not a physical heading estimate and must not be applied.

KDKA itself fails the RF gate before and after the move:

| KDKA period | Coherence | Amplitude span | Identity fitted phase axis |
|---|---:|---:|---:|
| pre-move | 0.759 | 11.00 dB | 316° |
| post-move | 0.838 | 10.33 dB | 289° |

Raising a rigid array should not rotate its ideal inter-element phase pattern,
yet the observed KDKA phases changed by roughly 40–100° on several channels and
the amplitude pattern also changed. This is direct evidence of changed roof/site
propagation and explains why KDKA cannot be the sole absolute reference.

## Calibration disposition

- Receiver lag/phase calibration: completed and converged at every surveyed
  frequency and again at RF25 after restoration.
- Physical geometry: retain CH0/+X = 80° true and identity channel mapping.
- Handedness: forward/clockwise family supported; reverse family rejected by the
  quality-gated multi-source fit.
- Global gain correction: do not apply; imbalance is direction-dependent.
- Global heading correction: do not apply; the fitted phase axis is not the
  physical CH0 heading and varies for rejected multipath/shadowed sources.
- Measured bearing manifold: do not promote from current target samples. The
  recorded target response is illuminator-contaminated and would learn the
  failure instead of calibrating it.
- Next calibration layer: learn a direction- and frequency-dependent manifold
  only from independently clean spatial samples, with the known-angle survey as
  an orientation/handedness acceptance gate.

## Restoration proof

After the survey, RF25 at 539 MHz, gain 14, all geometry confirmations, and the
original configuration were restored. `/etc/vectorwarp/config.yml` exactly
matches the pre-survey SHA-256 and config revision
`894f868ed0b0d1abd89a7202e935ea3c47dbf39a6c391a89af4bb31aaa936f0f`.
Heimdall, the API, and the processor are active, and GPU-resident processing is
qualified and receiving fresh frames.
