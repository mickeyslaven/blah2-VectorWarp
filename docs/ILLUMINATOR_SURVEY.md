# Illuminator survey and selection

VectorWarp separates finding a receivable transmitter from qualifying it as a
passive-radar illuminator. Direct-path strength is never the selection rule.

`api/illuminator-survey.js` supplies frequency plans for FM, DAB Band III,
US ATSC UHF, and generic 8 MHz DVB-T/T2 UHF. It also reports the assessment
available from the configured coherent receiver. Kraken arrays with at least
three positioned elements can perform the multi-element spatial checks. The
RSPduo, a two-channel USRP configuration, and synchronized dual HackRF can rank
reference and radar performance, but cannot independently certify a
reflection-safe multi-element bearing manifold.

Selection is staged:

1. RF prescreen measures carrier-to-noise, clipping, occupied bandwidth,
   repeatability, channel balance, and (for a positioned array) agreement with
   the surveyed transmitter direction and phase-versus-frequency curvature.
2. Radar qualification measures the waveform ambiguity sidelobes and actual
   clutter/direct-path cancellation rather than inferring them from strength.
3. Operational qualification uses eligible ADS-B opportunities, matched
   detections, persistent tracks, multiple aircraft and sectors, and held-out
   bearing error.

Clipping, unstable spatial response, poor direct-path agreement, severe
coherent channel imbalance, or severe frequency-dependent phase curvature is
a hard bearing-safety rejection. A rejected signal may still be an excellent
communications signal or dedicated reference; it is not automatically safe
for array bearing. No candidate is selected while required radar or held-out
evidence is missing.

The band plans and evaluator are receiver- and site-agnostic. A survey executor
must use the receiver's managed apply/readback path, preserve gain and sample
rate during comparisons, partition evidence by frequency/transmitter/site,
and restore the exact starting configuration after success or failure.

`GET /api/illuminator-survey` returns the built-in plans and the current
receiver's honest assessment limits. `POST /api/illuminator-survey/evaluate`
accepts candidate evidence and returns per-candidate rejection/missing-evidence
reasons. The evaluator never promotes a candidate merely because it is the
best of a bad set.
