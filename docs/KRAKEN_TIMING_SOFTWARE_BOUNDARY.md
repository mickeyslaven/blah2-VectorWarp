# Kraken timing software boundary

Last updated: 2026-10-24 (America/New_York)

## Measured boundary

Heimdall V2 frames carry coherent sample order and frame sample count, but no
device/RF timestamp. Live evidence in `CURRENT_RADAR_REMEDIATION_STATUS.md`
only confirms coherent sample ranges and `exact:false`,
`uncertaintyBounded:false`. The native timing receipt in
`docs/benchmarks/20260910/native-timing.json` is sample-rate-paced replay, not
live RF acquisition, and cannot infer USB latency.

The deterministic Kraken fixture proves the same sample block received 100 ms
later moves its host-derived estimate by 100 ms. That proves host receipt is not
an RF epoch; it is not a live-radio confidence interval. Current RF-event-to-UTC
uncertainty is therefore **unbounded/unknown**, not one second. The configured
±1 s is a provisional ADS-B association nuisance radius only.

## Implemented software mitigation

- Anchor each full Heimdall TCP-payload receipt to its coherent end-sample.
- Use a rolling 31-anchor Theil-Sen offset/drift fit. `eventTime.hostReceipt`
  publishes anchors, median/max residual and drift ppm.
- Reanchor the published nominal-sample-rate epoch at the latest fitted
  receipt; fitted host-delivery drift does not accumulate over the full
  sample index or replace the configured RF sample rate.
- A fit residual over 100 ms starts a fresh sample epoch, rejecting an
  ambiguous bridge. Retune, metadata change, socket loss, sync loss, recorder
  contention and clip gaps already do likewise.
- The diagnostic always declares `boundedRfUtc:false`; event time remains
  inexact and unbounded, so authoritative scoring remains fail-closed.
- Existing ADS-B projection projects observation time to CPI and expands only
  provisional association gates by measured delay/Doppler rates. It never
  steers detection, beamforming, cancellation or tracking.

## 527 MHz mapping

At 527 MHz, wavelength is 0.568866 m. For CPI time error `Δt`:

```
Δ(excess delay range) <= |dR_bi/dt| · |Δt|
Δ(bistatic Doppler)    <= |dfD/dt| · |Δt|
fD = -(dR_bi/dt) / 0.568866 m
```

At 2.4 MS/s, one delay cell is 124.9135 m excess path. One Hz Doppler equals
0.568866 m/s excess-path-rate change. Since `Δt` is unbounded, there is no
geometry-independent finite delay/Doppler error. The scorecard records actual
trajectory rates and gate expansion, never a hardware timing claim.

## Hardware blocker and deterministic checks

No receipt timestamp, regression, NTP discipline or ADS-B projection says when
an RTL2832 sample was converted at RF. Decision-grade UTC needs a qualified
device timestamp tied to PPS/UTC (or equivalent independent RF-time path).

`test/capture/KrakenClipRecorderFixture.cpp` covers stable fit and a 300 ms
synthetic receipt step; `api/adsb-geometry.test.js` and
`api/evidence-statistics.test.js` cover projected rates and rate-expanded
provisional gates. None open or change live RF/configuration.
