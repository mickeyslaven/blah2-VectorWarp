# Native truth export bridge — implemented, physical acceptance pending

The integration shadow worker now consumes exact `ResidentReplaySnapshot` ABI1
files rather than estimating covariance from target CUTs or reconstructing an
ideal beam bank. `save.resident_snapshot` is default-off and accepted only for
resident file replay. It requires source/RF digests, actual gains, a source
epoch/sample interval, an explicit frame range, a per-frame byte limit, and a
total byte limit. The native host checks captured sample ranges and actual
gains before processing, calls the existing settled `Session::snapshot()`, and
exclusively creates each artifact. It adds no enabled observer to live configs.

The worker selects a representative 3–8-frame window for every admitted pass
and requested dwell from sample geometry alone. A window with fewer than three
CPIs, missing source timing, missing snapshots, exceeded bounds, or out-of-grid
truth fails the job; it cannot issue a completed receipt. Truth selection does
not inspect CFAR decisions or winning beams. Full native snapshots retain all
cells and complete native rolling state, including data outside the compact
nuisance neighborhood shown in the JSON result. The maximum is 256 MiB per
snapshot and 2 GiB per selected window. The job wall limit remains enforced.

`blah2-replay-verify`, built beside `blah2`, recovers the embedded configuration
and axes and invokes the production canonical C++ snapshot decoder. The worker
then checks file SHA256, native envelope/config/raw-IQ hashes and the requested
recording/RF/gain/frame identity before extracting cells. The signed result
binds full artifact paths/hashes, exact covariance storage and validity, actual
fusion weights, native per-beam powers, physical-channel complex CAF values,
CFAR statistics/flags, power/angle history and temporal banks. Native warmup
NaNs are represented as JSON null. Complete refinement weights, metadata,
training membership, raw/filtered IQ, reference and tracker state remain in the
hashed native artifacts. No ideal beam responses are substituted.

Production replay now preserves configured TBD, bearing, and track-guided
state. Baseline and diagnostic runs use the same configuration at each dwell.
Alternative/physical-channel arms retain their explicitly diagnostic policy;
physical leave-one-out arms use CPU replay because resident mode requires
all-capture channel identity. Arm scoring and truth CUT selection map native
source sample positions through admitted CPI midpoint/time anchors. Replay
wall-clock timestamps are retained only as native diagnostic identity.

The v2 truth export receipt rejects the old CUT-estimated covariance envelope.
The API additionally requires one export per queued pass/dwell. Status exposes
native export count, timing quality, hardware qualification and the remaining
qualification gate. Exact timing alone does not make a result decision-grade.
No automatic production promotion exists.

These are native **replay** states initialized at the admitted clip start. They
are not claimed to recover the original live covariance/tracker state preceding
the recording. The ABI also stores native beam powers and actual complex
weights, not a separately materialized complex beam sum. Physical channel CUTs
are complex; beam powers are exact native observer values.

Validation on Apple Silicon (2026-10-24): `blah2`, `blah2-replay-verify`,
`testResidentReplaySnapshot`, and `testGpuResidentProcess` compile. CTest
`residentProcessCpuBoundary`, `residentReplaySnapshotCpuContract`, and
`residentReplayEvidenceBridge` pass. The bridge test uses native encoded
synthetic fixtures with known covariance/weights/maps and CFAR rejects; it
checks warmup, artifact corruption, missing coverage, source identity, sample
clock mapping, and production temporal-policy preservation. Python integration
worker and Node queue regression suites pass.

Remaining acceptance: on Strix, with the existing resource/lease guard satisfied,
build the exact revision and run `gpuPhysicalResidentReplay` for 1/2/4/8 CPIs
and whitening off/on. Then run one bounded admitted-clip worker job and verify
source-sample identity, retained artifact hashes, receipt coverage and ordinary
output parity. No physical GPU test or admitted-clip GPU replay was run by this
change. Until those checks pass, results explicitly retain
`hardware_qualified=false`, `decision_grade=false`, and no promotion authority.
