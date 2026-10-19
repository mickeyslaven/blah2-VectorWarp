Bearing phase/projection regression (offline CPU, no radio or GPU)

The test is part of the repository's ordinary CMake/CTest build as
`bearingPhaseProjection`. The standalone commands below remain useful for a
focused local run:

From the repository root on macOS with Homebrew dependencies:
  cmake -S test/bearing -B /tmp/vectorwarp-bearing-phase-regression \
    -DCMAKE_PREFIX_PATH=/opt/homebrew -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/vectorwarp-bearing-phase-regression --parallel 2
  ctest --test-dir /tmp/vectorwarp-bearing-phase-regression --output-on-failure

Linux uses the same commands without CMAKE_PREFIX_PATH when dependencies are
installed in standard locations. DSP_ROOT may name another source checkout.
Requires Catch2 3, Armadillo, FFTW with threads and RapidJSON.

The test calls the production BearingSteering::mapCoefficient/normalizeModel
helpers and the actual ArrayReferenceSynthesizer, WienerHopf and Ambiguity CPU
implementations. Spaced complex pulses plus delayed +17 Hz copies have disjoint
direct/echo supports. Three predefined array directions are 0, 120, 240 degrees.
This makes the target correlation vector exact up to a common complex scalar:
  dedicated reference, cancellation off/on: a_minus
  array eigenbeam reference, cancellation off: a_minus
  array eigenbeam reference, one-tap cancellation on: P_w a_minus
where P_w = I - w w^H / (w^H w). A separate oracle checks these facts before any
bearing model assertion. Normalization/projection is then checked independently
of the phase helper, so a sign-only fix still fails the projection assertions.

The one-tap fixture deliberately isolates the proposed projection mechanism.
It does not claim that P_w is the complete response of general multitap,
multisignal or time-varying cancellation. Projection is an explicit candidate
mode; it must not become the default or apply to measured post-CAF manifolds.
Measured-response tests check unchanged complex direction through bypass only.

The beam-search check is a normalized model/data inner-product check, not the
GPU covariance/MVDR shader. GPU candidate validity, anchor selection, replay-only
configuration guards and pipeline output need their own integration validation.
The fixed-site anchor in this fixture is independently generated from the
negative physical IQ phase. Production anchor construction must use that same
sign; testing only mapCoefficient cannot prove its caller has the right anchor.

Pre-fix numerical evidence (before normalizeModel existed): with a temporary
test-only adapter for current unprojected normalization, the build ran 4 cases,
1,092 assertions: 1 case passed, 3 failed, 30 assertions failed. All independent
actual DSP oracle checks passed. The old phase helper selected beams 13/40/55
instead of 0/20/40 for all three unprojected reference/cancellation modes.
After real eigenbeam cancellation, unprojected sign-correct target vectors had
squared coherence 0.89805145, 0.99892607, 0.86140850 with actual CAF vectors;
projected oracle coherence exceeded 1-1e-10 in all three cases. The temporary
adapter was removed. Final tests require the shared production helper directly.

After the implementation owner's negative-phase correction and normalizeModel
addition, the final suite passed all 5 cases / 1,112 assertions on 2026-10-19.
CTest completed in approximately 0.8 seconds on the local macOS CPU.

No production sources, measured calibration files or replay recordings are
changed by this test. No physical-array or aircraft qualification is implied.
