# Owl external-reference validation

## Outcome

The stock Owl OS recording toggle is useful for bounded validation but is not a
continuous external-reference transport. The trial left no persistent Owl
configuration or software change.

- Owl remained at 551 MHz (WTAE), 2 MS/s and 0.5 s configured CPI.
- Original Owl configuration SHA-256 before and after every trial:
  `a900508d525276c4b13b9719b330eae1b70c4ec97dad168e7463fc562f983539`.
- The initial 30/90-CPI timing sample has been superseded by matched, warmed
  100-second profiles described in `CPI_TAIL_PROFILE.md`.
- A corrected trial kept the stock API available through a temporary loopback
  proxy while its built-in recorder ran. Across 90 consecutive CPIs, median
  processing time was 470.04 ms, p95 507.95 ms and maximum 519.93 ms; 5/90
  reached 500 ms. Relative to the pre-trial sample, the observed median cost was
  **+2.79 ms** and the p95 cost was **+11.92 ms**. This is measurement noise plus
  synchronous two-channel disk writing, so it is a conservative upper bound for
  a one-channel asynchronous network exporter.
- An earlier harness attempt stopped `blah2-api` while replacing `/capture`.
  That removed all of the processor's output sockets and caused the observed
  restart loop; it was a harness error, not evidence that IQ recording crashes
  Owl. The corrected proxy trial preserved every output socket.
- The trial was stopped. Stock `blah2`, `blah2-api`, tracker and telemetry were
  restored with ports 3000/4001 listening again.

## CPI-tail profile

The matched warmed cohorts show that Owl's ordinary processing already runs
close to its configured 500 ms CPI budget. Normal processing exceeded 500 ms
on 7/200 CPIs (3.50%); synchronous stock two-channel recording exceeded it on
16/201 CPIs (7.96%). Recording raised median processing time by 2.84 ms and p95
by 10.30 ms. In both modes, the main cause of a CPI crossing 500 ms was a spike
in ambiguity processing, not a fixed recorder stall. See
`CPI_TAIL_PROFILE.md` for component-level results and limitations.

## Synchronized recording evidence

For feasibility only, HP VectorWarp was temporarily tuned to Owl's unchanged
551 MHz and then restored to its exact 509 MHz configuration. The synchronized
artifacts are:

- `owl-reference-validation-551mhz-sync.npy`, SHA-256
  `a0994f087134d5588acfa4f5cf42d6e9485dd4959ef25a61a2deab85f9aa87bb`
- `20261027-013636.rspduo.iq`, SHA-256
  `4441bf3d0ea7c8c409fcedd5142a642cbe701aed089cdaaca60874bd7f2acb79`
- `20261027-013647.rspduo.iq`, SHA-256
  `a30e5532d737a1d623ab86be16e4c961663ec0031fc15f3c8fe58ba27b3d2ba4`
- `owl-reference-validation-551mhz-sync.json` records the HP tune/capture and
  exact restoration timestamps.

A coarse nominal-rate comparison found a short-window normalized correlation
near 0.39, but only 0.063 maximum 10 ms coherence after a single CFO correction.
This is evidence that the recordings share broadcast content, not that they can
be substituted directly. A production external reference must continuously
estimate lag, sample-rate offset and frequency offset for each CPI.

## Network and implementation boundary

Owl-to-HP transfer used the Ford Street LAN route, not Tailscale. Measured file
copy throughput was 12.0--14.0 MB/s. One complex-int16 reference channel at
2 MS/s is 8 MB/s, so one channel is feasible with bounded buffering; exporting
both RSPduo channels (16 MB/s before framing) is not justified by this test.

The acceptable implementation is an optional, asynchronous, single-channel
export from the already assembled coherent RSPduo CPI buffer. It must never do
network or disk I/O on the SDRplay callback, must drop only the export copy on
backpressure, and must leave native Owl processing authoritative. It requires a
source change and a before/after CPI/drop benchmark; the stock recording toggle
cannot satisfy those constraints.

## Implemented candidate (not deployed)

The source candidate now implements that boundary:

- `AsyncReferenceExporter` is enabled only by
  `VECTORWARP_EXTERNAL_REFERENCE_TCP=IPv4:port` and only on the RSPduo paired-CPI
  path. With the variable absent, the path is not constructed.
- Submission occurs after a coherent paired CPI is published and before its
  queue loan is released. It copies only the configured reference channel into
  one of three preallocated buffers. It performs no network or disk I/O on the
  SDRplay callback.
- Submission uses a try-lock and fails immediately if all buffers are occupied.
  Backpressure therefore increments the exporter drop counter and never drops,
  waits for, or mutates Owl's native CPI.
- Each TCP frame carries source epoch, first-sample position, RF, sample rate,
  channel and format. `script/receive-external-reference.py` validates that
  contract and writes a hash/receipt ledger without pretending LAN arrival time
  is RF synchronization.
- A loopback contract test verifies exact channel selection, framing, source
  sample identity and payload bytes.

The ambiguity range stage also now uses a shared four-row work queue instead of
a fixed 50/50 split between the two range workers. A deterministic regression
test compares every output cell against the one-worker implementation. On the
local test geometry, caller time fell from 0.332 ms to 0.197 ms and join wait
fell effectively to zero; that is only a functional microbenchmark, not the
required Pi acceptance result.

Nothing has been installed on Owl. Acceptance still requires a separate
candidate image and a matched warmed profile on the Pi showing unchanged native
maps, no additional native dropped CPIs, and no meaningful increase in the
>=500 ms tail before enabling transfer to HP.

## HP-side alignment result

Re-analysis of the synchronized 551 MHz recordings found a real coarse match
that the original short-window search obscured. After reducing both radios to
25 kHz power-envelope samples, all five HP channels against Owl tuner A chose
the same lag (2.81484 seconds) and held that lag across five separated windows.
The best envelope correlation was 0.454. This establishes shared broadcast
content and a stable coarse time mapping; LAN arrival timestamps are not needed
for that step.

It still did not establish usable complex coherence. The HP samples in this
trial had only about 0.87--2.40 ADC-count standard deviation by channel, and a
bounded complex delay/CFO search remained at noise-like coherence around 0.05.
Those recordings therefore cannot qualify Owl as a drop-in reference, but they
also do not reject it: they were captured with the low-gain Kraken configuration
that motivated the external-reference experiment. The next trial must pair the
accepted asynchronous Owl stream with a deliberately higher-gain Kraken
surveillance epoch, estimate affine sample mapping and CFO per CPI, and score
CAF target recovery against the native reference at a fixed false-alarm burden.

