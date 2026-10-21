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
   the surveyed transmitter direction and phase-versus-frequency curvature. It
   also finds each candidate's highest safe coherent common gain using the same
   measured headroom protocol and records input-referred noise.
2. Radar qualification measures the waveform ambiguity sidelobes and actual
   clutter/direct-path cancellation rather than inferring them from strength.
3. Operational qualification uses eligible ADS-B opportunities, matched
   detections, persistent tracks, multiple aircraft and sectors, and held-out
   bearing error.

## Rapid no-aircraft proxy sweep

`POST /api/illuminator-survey/fast-evaluate` ranks short, comparable captures
without waiting for an aircraft. It is intended for a managed executor that
retunes through a pinned local transmitter catalog and measures each candidate
at its own highest safe common gain. The primary proxy is a calibrated digital
echo-injection test: delayed and Doppler-shifted copies of the candidate's
measured reference are injected **before** the production canceller, ambiguity
processor and detector over a frozen delay/Doppler/amplitude grid. A binary
search records the weakest echo/direct-path ratio recovered on the required
fraction of trials.

The rapid gate also requires ADC-derived per-channel clipping and near-rail
fractions, CNR, occupied bandwidth, ambiguity PSLR and ISLR, direct-path and
clutter cancellation depth, channel-power span, post-cancellation residual-floor
excess, safe gain and off-target detection density. It does not invent live
tracker, dropped-frame, CPI-deadline or calibrated input-noise evidence while the
processor is stopped for the raw capture; those remain mandatory in the full
aircraft qualification. A strong direct path is not a score. Among candidates
passing every rapid gate, the evaluator ranks:

1. weakest reliably recovered injected echo relative to the direct path;
2. recovery fraction and usable dynamic range;
3. lowest post-cancellation residual floor;
4. highest safe receiver gain;
5. ambiguity sidelobes.

All candidates must share the same receiver, sample rate, processing build and
injection grid. The result is explicitly `provisional` and
`requires_aircraft_validation`: digital injection exercises the real received
waveform, interference, cancellation and detector, but it occurs after the ADC
and cannot synthesize an aircraft's propagation, RCS, polarization, antenna
pattern or pre-ADC intermodulation. The separate clipping/headroom gates cover
the most important pre-ADC failure mode. This is a fast, defensible way to pick
the first operating frequency; accumulated aircraft opportunities later
confirm or replace it using the full evaluator.

Clipping, unstable spatial response, poor direct-path agreement, severe
coherent channel imbalance, or severe frequency-dependent phase curvature is
a hard bearing-safety rejection. A rejected signal may still be an excellent
communications signal or dedicated reference; it is not automatically safe
for array bearing. No candidate is selected while required radar or held-out
evidence is missing.

The band plans and evaluator are receiver- and site-agnostic. A survey executor
must use the receiver's managed apply/readback path, preserve sample rate and
the gain-selection protocol during comparisons, freeze each candidate's selected
gain within its blocks, partition evidence by frequency/transmitter/site,
and restore the exact starting configuration after success or failure.

`GET /api/illuminator-survey` returns the built-in plans and the current
receiver's honest assessment limits. It also returns the regulator/operator
catalog source registry, normalized catalog schema, and the explicit fact that
a pinned snapshot is required: the request path never performs a live network
fetch. `api/illuminator-catalog.js` validates and fingerprints those snapshots,
resolves FM, DAB, ATSC or DVB-T/T2 transmitters by plan, time, location and
radius, and keeps co-channel/SFN sites as separate hypotheses.
`POST /api/illuminator-survey/evaluate`
accepts candidate evidence and returns per-candidate rejection/missing-evidence
reasons. The evaluator never promotes a candidate merely because it is the
best of a bad set.

## Evidence and ranking contract (v3)

Each candidate must explicitly set `bearing_required` to a boolean. Omitting
it cannot bypass bearing checks. The recommendation is conditional on supplied
evidence and is never a command to change the receiver or promote a model.
Capabilities expose `bearing_evidence_evaluation` separately from automatic
execution and selection, both of which remain false.

`comparison` contains `site_id`, `protocol_id`, `sector_definition_id`,
`common_support_id`, `baseline_id`, `sector_ids`, and `range_bin_ids` (at least
two unique strings for each support dimension).
These identify frozen definitions, not arbitrary labels for otherwise different
experiments. They must match across the survey. Frequency/transmitter identity
remain candidate variables. Mixed radar-only and bearing-required candidates
cannot be ranked in one survey.

`sensitivity` records the candidate-specific receiver operating point. Its
`gain_strategy` must be `measured-highest-safe-common`; `selected_gain_db`,
`peak_fraction`, and `input_referred_noise_db` on one documented common scale
are mandatory. All candidates
must use the same device gain scale and measured-selection protocol, identified
by the comparison protocol, while the selected numeric gain may differ by
frequency. A peak above the configured headroom limit rejects the candidate.

`tracking.eligible_passes` contains unique evaluation passes with `pass_id`,
`aircraft_id`, `sector_id`, `range_bin_id`, `matched` and `persistent`. The two latter fields are
booleans; persistence implies a match. Eligibility is determined independently
of detection success. Aggregate counts `adsb_opportunities`, `matched_passes`,
`persistent_passes`, `matched_aircraft` and `sectors` must agree with these
records. `matched_detections` counts matched frames and is **not** divided by
pass opportunities. `persistent_tracks` remains a separate evidence count.

`tracking.held_out.fit_passes` is an explicit array of `{pass_id, aircraft_id}`;
use `[]` for a predetermined model with no fit data. Training aircraft/passes
must be disjoint from the evaluation passes. `held_out.observations` contains
`observation_id`, `pass_id`, `aircraft_id` and `sector_id`, plus paired
`bearing_error_deg` and `baseline_bearing_error_deg` for bearing-required runs.
Each observation must refer to a matched evaluation pass. IDs are unique and
aircraft, sector and aggregate counts are cross-checked. Every declared sector
needs opportunities and held-out observations. Counts, fractions and circular
errors have explicit validity ranges.

Detection and persistence yields give equal weight to every declared sector-by-
range support cell, so nearby traffic cannot hide poor long-range sensitivity.
Bearing errors use equal pass weight within each sector, then equal sector
weight. `tracking.held_out_bearing_error_deg` must match that derived value.
Each sector must pass the absolute error limit and not regress from its paired
baseline. RF/spatial measurements are qualification gates only. Qualified
candidates rank by equal-sector-by-range persistent-pass yield, matched-pass yield,
safely available receiver gain, input-referred noise, then held-out bearing
error. Gain is only a tie-breaker after actual radar yield and cannot make a weak
or otherwise unqualified reference win. This avoids treating a stronger direct
path or more busy airspace as better radar performance. Confidence intervals and provenance still
belong in the acquisition report; the evaluator cannot authenticate caller data.

See [the executor design](ILLUMINATOR_SURVEY_EXECUTOR.md) for preview/intent,
catalog and receiver contracts, durable restore, alternating blocks, opportunity
definitions and failure tests. That document describes work still needed before
automatic end-to-end surveys can run.
