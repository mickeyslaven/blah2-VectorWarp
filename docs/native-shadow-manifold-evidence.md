# Native shadow-manifold evidence contract

`vectorwarp-native-truth-export-v1` is an authenticated native receipt.  Its
canonical SHA-256 (`export_sha256`) covers every field except authentication and `export_sha256`,
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

## Native-only fit and A/B lane

The adapter no longer depends on a detected-live-cell corpus. The automation
polls signed queue/result inputs and its derived ledger, including before that
ledger exists; bounded archived receipt/job pairs are included after queue
compaction. Invalid signatures, semantic evidence, or chain continuity stop
the evaluation; invalid outer receipts and unavailable per-frame evidence are
counted separately. A frame is bound to its enclosing job, snapshot and exact
physical CUT, not merely to a reusable schema label.

The dedicated fitter uses normalized native physical complex CAF vectors only.
Its fixed azimuth/elevation grid requires at least two independent signed pass
identities per supported cell; each pass has equal fit weight. Common complex
phase is aligned before averaging. The bounded shrinkage prior is the recorded
native truth-nearest fusion beam, **not** the direct-path reference and not an
invented ideal response. Native fusion coefficients are already conjugated;
projection uses the squared magnitude of the coherent channel sum. Orthogonal
alignment, sparse support and unobserved elevations remain unsupported.

The latest observed contiguous hardware/receiver collection epoch is isolated
from earlier epochs, including A/B/A gain returns. Intentional dwell/config
variants remain separate lanes; the configured CPI is preferred if available,
otherwise a deterministic configuration identity is selected without looking
at outcomes or support. All detected, subthreshold and miss labels are retained.
Pass IDs determine the frozen 80/20 hash split, shared across epochs and dwells.
Held-out response values do not participate in candidate construction/versioning.

Native candidates are content-versioned, non-runtime immutable artifacts. Each
evaluation appends a separate A/B receipt before replacing the shadow manifest.
The published ledger prefix is anchored in the manifest and receipts so a
truncated old signed prefix cannot silently reset evidence on a subsequent run.
Only dimensionless unit-normalized projection change is reported. Nearby
physical controls may contain target energy; they are not certified target-free
CFAR training or evidence of nuisance-detection rates. Noncoherent exports
without an authentic complex fusion prior remain generic native diagnostics,
not fabricated training rows. Sparse candidates can exist with zero supported
cells; the API reports support and pass counts explicitly.

Reads, worker memory/wall time, spool entries, rows, cells and ledger bytes are
bounded. Writes reject overlap with live manifold files, input corpora or the
receipt key (including resolved/hard-link aliases). Candidate writes are
exclusive; existing versions must match byte-for-byte. No runtime conversion or
automatic promotion exists.

## Residual physical gates

CPU fixtures establish encoding, validation, time mapping, authentication,
native-only import, phase math, epoch/pass isolation and artifact behavior—not
hardware qualification. Before any production consideration, Strix must run the
exact reviewed revision under its resource/lease guard: physical resident replay
for 1/2/4/8-CPI integration with whitening off/on, then a bounded genuine admitted
IQ job verifying source/sample-time identity, full native artifact coverage and
ordinary-output parity with instrumentation disabled. Replay reference/covariance
state is freshly initialized from the clip, not claimed to reproduce unknown
pre-clip live state. A separately frozen physical cohort and target-free control
regions are still needed for qualified paired full-bank production A/B. HMAC
authenticates the designated exporter; it is not a hardware attestation. All
current native fits remain hardware-unqualified, non-decision-grade and
promotion-disabled.

## Review validation (macOS, synthetic fixtures)

Passed: `npm run test:manifold --prefix api`, `npm run test:statistics --prefix
api`, configuration manager/settings-matrix/resident-config tests, calibration
portable/rich UI smoke tests, the `testResidentReplaySnapshot` and
`blah2ReplayVerify` build targets, both `residentReplay` CTests, integration
sensitivity shadow and replay-fusion tracker diagnostic Python regressions,
packaging shell syntax, and `git diff --check`. Native fitter regressions include
coherent cancellation and conjugation, common-phase invariance, pass/holdout
isolation, two-dimensional sparse support, interleaved dwell isolation, gain
epoch returns, signed semantic corruption, first import without live rows,
archive-before-import, immutable A/B artifacts, ledger-prefix truncation,
protected output aliases and evidence arriving during a fit. No Linux package
installation, physical GPU run, receiver operation, deployment, or live
configuration mutation was performed by this review.
