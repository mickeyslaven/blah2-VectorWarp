# Array geometry correction

## Finding

The installed Arrow/Kraken array uses the 440 MHz element set in the physical 600 MHz position, hole 9. Arrow's installation table gives hole 9 a radius of **7.62 cm**, or **0.0762 m**. The locked VectorWarp configuration instead says "7.62 inches" and uses **0.193548 m**. That is a centimetre-to-inch interpretation error and makes the modeled radius 2.54 times too large.

Primary sources:

- <https://www.arrowantennas.com/inst/KrakenAntenna.pdf>, page 5: hole 9, 600 MHz, radius 7.62 cm.
- <https://www.arrowantennas.com/arrowii/krsdr.html>: small-array construction and element-set description.

The photographs are consistent with the elements being close to the hub, and are inconsistent with a 19.35 cm radius. They independently support the manufacturer dimension but are not used to invent hidden feed or bonding details.

## Verified scope

- Read-only inspection of `/etc/vectorwarp/config.yml` on the locked live host found radius-0.193548 coordinates for all five channels and an operator note that explicitly mislabels the manufacturer value as inches.
- The live file was not modified. The 72-hour data lock remains intact.
- The radius feeds ideal phase steering, bearing/manifold identity, frequency-resolved analysis and coherent beamforming. It does not alter raw Kraken samples or noncoherent summation, but it can materially reduce coherent gain and bias bearing results.
- Existing 0.193548 m Sionna/openEMS array results are retained as invalidated comparison evidence. The dipole element's standalone elevation-pattern result remains useful; array coherence, coupling, steering and site-height conclusions must be rerun at 0.0762 m.

## Analytic steering impact

The 527 MHz phase-center comparison must be read in two different ways:

- Forcing the old manifold to use the same labelled direction as truth gives a
  10.60 dB median loss. This is useful for diagnosing the phase error, but it
  overstates production detector loss because the live beam bank selects the
  strongest of 12 azimuth beams.
- In the exact production-grid strongest-of-12 comparison, the old radius gives
  1.57 dB median loss, 288/1,296 cells worse than 3 dB and 65 cells worse than
  6 dB. Its more universal failure is beam identity: median bearing error is
  65 degrees, the 90th percentile is 150 degrees, and 960/1,296 cells exceed
  30 degrees.
- The corrected-radius bank gives 0.14 dB median loss, no cells worse than 3
  dB, 10 degrees median bearing error and no cells over 30 degrees in the same
  ideal phase-center test. The grid is the deployed local 0°,30°,… grid mapped
  to true bearing by `x_axis - local`, with production first-winner tie handling.

These are analytic bounds, not measured live gains.

## Frozen measured-IQ A/B

The same physical-channel complex CUTs were rescored with only the nominal
radius changed. The cohort contains three labeled positive captures (60
frames, two aircraft/pass identities), thirteen labeled miss captures (260
frames), and the target-free cells surrounding every truth-selected CUT.
An additional end-to-end processor replay was possible for all three positive
captures and four miss labels whose original raw recordings remain present.

The corrected free-space radius did **not** improve this historical 563 MHz
cohort:

- The analytic CUT rescore reduced the median positive best-frame margin by
  4.01 dB and the median positive per-frame margin by 2.07 dB.
- In the exact processor replay, truth-matched positive frames fell from 25 to
  18 and median matched SNR fell 1.27 dB.
- Total reported detections rose from 141 to 592 across the positive captures.
  In the four available miss replays they rose from 207 to 774, with zero
  truth matches under either geometry. This is an interference/false-alarm
  regression, not recovered aircraft energy.
- Across all thirteen miss CUTs, the median best-frame margin change was only
  +0.04 dB while the median per-frame change was -1.80 dB.

This does not make the 0.193548 m dimension physically correct. It shows that
the ideal free-space manifold is not the installed effective manifold: the old
radius appears to have acted as an accidental spatial distortion/filter in the
favorable positive sector. Coupling, mast/boom currents, roof multipath,
polarization and per-channel hardware phase can dominate a literal
phase-center substitution. Therefore the corrected dimension belongs in the
configuration identity, but it must not be promoted as a detector improvement
without a measured installed manifold or a qualified held-out A/B.

The machine-readable paired result is
`frozen-array-geometry-rescore.json` (SHA-256
`914274940733b7bb944e507436e39281fb0df3b60266ae4ae255761391df6295`).

## Correct geometry at the array coordinate frame

The corrected radius is 0.0762 m, adjacent chord spacing is 0.0895785 m, and the physical channel order remains clockwise in 72-degree steps. `installed-array-geometry.json` is the machine-readable source for offline work.

## Post-lock acceptance gate

Before changing production:

1. Complete corrected-radius Sionna and openEMS baselines and compare them with the old-radius hypothesis.
2. Treat the frozen 563 MHz A/B as a failed direct-promotion gate. Repeat on
   the locked 527 MHz epoch when enough pass-disjoint positives exist, using a
   measured installed manifold rather than substituting ideal coordinates.
3. Regenerate manifold identity rather than mixing samples from the two geometries.
4. Apply the corrected positions through the normal configuration path, confirm the saved revision and runtime readback, then start a new scientific epoch.
5. Do not claim a detection improvement until held-out replay or a controlled live epoch demonstrates it.
