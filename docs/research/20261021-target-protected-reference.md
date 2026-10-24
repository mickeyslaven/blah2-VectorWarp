# Target-protected array reference (replay-only)

The normal five-channel array reference is the principal covariance eigenbeam.
It is excellent for the direct path but can contain a moving target whose array
manifold is correlated with the direct-path mode; ECA can then learn part of
the target.  This experiment projects that reference weight vector onto the
orthogonal complement of one candidate target manifold *before* ECA/CAF.

The candidate manifold is the measured eigenbeam rotated by the calibrated
array geometry's direct-to-candidate relative phase.  Therefore it preserves
the measured direct-path amplitude/phase errors instead of substituting an
ideal steering vector.  A twelve-direction fixed azimuth bank is replayed
serially; the selected direction is not taken from ADS-B.  ADS-B is used only
afterwards to score the corresponding delay/Doppler cell.

This is a bounded LCMV-style null, not a claim of a full adaptive beamformer.
Its risk is loss of reference SNR or an unstable null near the illuminator;
acceptance consequently requires (a) r277 retained and (b) a miss at least
3 dB over the pooled p99 of every non-target cutout cell across all bank
members.  The pooled threshold charges the beam trials.  No live configuration
recognizes these keys: `target_protection_beams` and
`target_protection_beam_index` exist solely in replay configurations.

## 2026-10-21 result

The complete 12-member bank was run serially on r277 (ten CAF frames per
member).  The best truth-cell mean was **-1.962 dB** relative to pooled p99
over 287,040 non-target cells, so it fails both the +3 dB acceptance threshold
and positive-control retention.  Mean output was 198.75 detections per member
and the maximum tracker count reached 7,030, reinforcing the rejection.  The
bank was therefore not replayed on r12/r94/r620/r623: it cannot pass the
required conjunction while its positive control fails.  The compact result is
`results/20261021-target-protected-reference-r277-bank.json`; raw CASS replay
artifacts remain in `results/target-protected-ref-r277-20261021/` on CASS.
