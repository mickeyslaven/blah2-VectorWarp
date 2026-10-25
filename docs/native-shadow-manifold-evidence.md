# Native shadow-manifold evidence contract

`vectorwarp-native-truth-export-v1` is an authenticated native receipt.  Its
canonical SHA-256 (`export_sha256`) covers every field except authentication,
and `authentication` is an HMAC-SHA-256 over that same canonical payload.

The adapter accepts a receipt only when it names an admitted sample-clock range,
one RF epoch, a pass identity and ICAO, ADS-B truth (used only for truth and
validation), exact configuration fingerprint/RF/channel mapping/full applied
gain vector, source-snapshot hashes, covariance provenance, and a SHA-256 named
target physical-complex-IQ CUT.  The CUT's exact native fusion-weight vector is
copied without reconstruction.  Beam power maps, anchors, power-only responses,
unadmitted clocks, mixed epochs, and unsigned/tampered evidence are rejected.

Each derived `vectorwarp-native-shadow-manifold-row-v1` is canonically hashed,
HMAC authenticated, and chained to `previous_row_sha256`; append verifies the
entire existing ledger before writing.  Geometry-selected `detected`, `miss`,
and `subthreshold` outcomes are retained only in the shadow-training ledger;
they never authorize direct production promotion. Derived rows state
`adsb: truth-validation-only` and `shadow: training-only`. Reference-state covariance is provenance, not a claim
of local CFAR noise or production A/B replay evidence.
