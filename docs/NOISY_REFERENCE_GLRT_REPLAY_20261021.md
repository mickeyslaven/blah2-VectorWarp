# Noisy-reference GLRT replay (2026-10-21)

This bounded replay-only arm implements the deterministic-unknown-signal GLRT
from Cui, Liu, Li, and Himed, *Signal Processing* 108 (2015), equations 10 and
19. It compares the known/estimated-noise and unknown-noise statistics with a
normalized CAF computed from the same sufficient statistics at identical
ADS-B-centered cells and translated trajectory controls.

The run used three retained 563 MHz WWAT MCHQ clips, eight 0.5-second frames per
clip. Miss trajectories came from the corrected
`interpolated_position_samples` manifest. No receiver, RF, or live
configuration was read or changed by the analysis arm.

Every target and translated-control score was computed, retained, and ranked.
The +3 dB criterion was applied only afterward as a validation label. Compressed
per-clip artifacts contain 25,270--25,399 candidates each.

| Role | Revision | Known/estimated-noise GLRT | Unknown-noise GLRT | Normalized CAF | Unknown GLRT rank | +3 dB |
|---|---:|---:|---:|---:|---:|---|
| positive | 42 | -3.432 dB | -3.455 dB | -3.443 dB | 14,523 / 25,343 | no |
| miss | 10 | -1.501 dB | -1.549 dB | -1.517 dB | 3,147 / 25,399 | no |
| miss | 100 | -5.767 dB | -5.791 dB | -5.755 dB | 23,557 / 25,270 | no |

For equation 19, separation is reported on evidence above its 0.5
equal-energy/no-correlation floor; the raw statistic remains in every candidate
record and determines its rank. The known-noise arm uses a per-CPI robust noise
estimate: median periodogram power divided by `log(2)`.

The positive did not separate from controls, and neither miss passed +3 dB.
Revision 10 is the closest miss candidate but remains 1.55 dB below its matched
control p99. These results do not support promotion beyond replay-only study.
