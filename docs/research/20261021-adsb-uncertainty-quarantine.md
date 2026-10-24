# ADS-B uncertainty and replay quarantine audit

This audit is replay-only. It did not change live RF/configuration, delete a recording, or launch a wider historical replay.

## Outcome

All 251 reconciled rows (243 unique MCHQ recordings) are quarantined under the fresh-only acceptance policy in `results/20261021-adsb-replay-quarantine-v1.json`.

- 201 rows contain at least one ADS-B point older than 3 seconds or a legacy fallback.
- 116 rows are Doppler-unbounded because the reconciled paired-source record retains position disagreement but no paired velocity disagreement.
- All 251 rows hold observations rather than projecting each source from event time to the CPI midpoint.
- 228 rows lack two independent sources at every CPI.
- All 251 rows exceed the historical ±4-delay-bin/±20 Hz candidate window. Among the 134 conditionally bounded rows, the required radii have medians of 36 delay bins and 232.1 Hz; maxima are 132 bins and 840.0 Hz. The least-uncertain bounded row still needs 10 bins and 56.5 Hz.
- 169 rows are outside the requested offline processing box of 0–64 km and ±800 Hz (or are unbounded). This is a processing/cutout exclusion, not proof that the RF samples omit the echo.
- 153 rows use at least one source observation whose event time predates the recording window.

The read-only raw-host check on 2026-10-21 found all 243 authoritative MCHQ paths present: 202 files were 240,105,552 bytes and 41 were 240,105,480 bytes. Each manifest declares 24,000,000 samples at 2.4 MS/s (10 seconds).

The 527 MHz spatial manifests declare cutouts for 175 rows. Only 10 conditionally bounded tubes fit those requested extents; 165 do not. These are requested extents, not retained complex samples: the runner deletes replay JSON after scoring and keeps only result summaries. No local complex cutout artifact can establish wider actual coverage. There is no corresponding declared spatial-cutout manifest for the 76 current 563 MHz rows.

## Bounds

Delay-bin width is `c / 2.4e6 = 0.124913524 km`; Doppler-bin width at a 0.5-second CPI is 2 Hz. The row audit uses a declared 350 m/s speed ceiling and the fact that bistatic excess path is 2-Lipschitz in target position. It uses a declared 10 m/s² acceleration ceiling for age-dependent Doppler error. Position-only inter-source disagreement cannot bound velocity disagreement, so those frames are marked unbounded rather than assigned a guessed Doppler radius.

## Fresh-only acceptance

A future capture is eligible only when every CPI retains event-time observations from at least two independent sources; projects each source's position and velocity to the CPI midpoint; records the projection method, covariance, source IDs, event times, and raw ages; has raw age no greater than 3 seconds; has pairwise projected horizontal disagreement no greater than 0.5 km and velocity disagreement no greater than 10 m/s; and has a predeclared uncertainty tube that fits the retained cutout on every CPI.

Every tube cell must retain its rank. Tube maximization must be applied identically to targets and translated controls, with the tube-size trial factor charged only during validation.

## Existing candidate evidence

There is no accepted recovery candidate. The strongest prior ADS-B sparse result is `r277`, whose top cell is delay offset 0 / Doppler offset +10 Hz, but it ranks only 25th among 85 target/control paths (`p = 0.2941`) and uses a legacy event-prediction fallback. It is quarantined and cannot support a recovery claim.
