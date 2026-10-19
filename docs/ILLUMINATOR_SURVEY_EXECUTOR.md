# Automated illuminator survey: execution and acceptance design

Status: implementation contract, not an enabled executor. The band plans,
shadow reference analysis and evidence evaluator exist. No endpoint currently
performs this complete survey, retunes automatically, or promotes a winner.

## Objective and outputs

Find the transmitter that provides usable radar tracks and trustworthy array
bearings at the installed site. Record four independent assessments: RF
reference quality, waveform/cancellation quality, opportunity-normalized
tracking performance, and held-out bearing safety. RF strength is a gate and
diagnostic; it is not the ranking objective.

The final report includes raw evidence references, units, acquisition times,
receiver/configuration fingerprints, missing measurements, rejection reasons,
per-sector results, uncertainty and a restore receipt. Distinguish RF-unusable,
reference-capable/bearing-unsafe, spatial-prescreen-only, radar-qualified with
insufficient bearing validation, and fully qualified. A recommendation never
changes the running receiver or bearing model.

## Preview and station input

`preview(request, currentConfig, adapterCapabilities, catalog)` is pure. It
returns a hash binding the exact starting revision, immutable candidate list,
protocol version, fixed settings, processing version and execution budgets.
Display exact frequencies, transmitter coordinates and provenance, every
receiver/service mutation, capture/dwell duration, and bounded restore time.
Execution requires explicit intent referencing that preview hash; an edited
configuration or changed catalog invalidates it. Saving a band plan is not
execution intent.

The catalog interface is `resolve({plan, receiverLocation, radiusKm, asOf})`.
Its bounded result contains station/facility IDs, center frequency, standard,
latitude, longitude, antenna-center altitude in metres AMSL, source URL/file,
retrieval date, coordinate uncertainty and verification status. Providers have
timeouts, candidate/result-size limits, caching and explicit failure results.
Accept operator-supplied catalog files through the same schema. Frequency-only
energy scans can find signals but cannot certify a transmitter direction.
Co-channel transmitters and SFNs remain distinct hypotheses; do not silently
attribute all energy to the nearest tower. Unknown altitude or identity remains
missing evidence. Receiver bandwidth limits the captured waveform; a 2.4 MSPS
capture is not evidence for the full 6 MHz ATSC channel.

## Transaction and recovery

State progression is PREVIEWED → LOCKED → BACKED_UP → PRESCREEN → SHORTLIST →
ALTERNATING_DWELL → RESTORING → VERIFIED → COMPLETE. Any failure, cancellation,
expired lease or deadline goes to RESTORING. Failed restoration ends in
RESTORE_REQUIRED, never COMPLETE and never a subsequent candidate.

Before the first receiver mutation:

1. Acquire one host/receiver survey lock, excluding configuration writers and
   other surveys. Compare the live revision with the preview and verify fresh
   processor status plus matched receiver readback.
2. Atomically save the complete original configuration and its hash, actual
   receiver tuple, service/timer active states, software/adapter versions,
   initial counters and an append-only transaction journal. Sync these to disk.
3. Arm an independent host-local recovery supervisor. Its lease and absolute
   deadline cover the worker, API restart, client disconnect and worker crash.
   Persist the restore intent before applying any candidate. A `finally` block
   alone cannot recover SIGKILL or process loss.

Each journal transition is durable before its side effect; resume is idempotent
and reconciles actual state before repeating a command. Use revision-checked
managed API transactions, not direct config-file edits. Separate mutating
receiver authority from the unprivileged analysis worker. The supervisor may
restore only the recorded configuration and prior service states.

Restore on success too: apply the exact original document, perform the required
receiver calibration/restart, check complete document equality and revision,
compare actual RF settings, require matching API loaded revision and fresh
processor frames, then restore timers/services to their prior states. A partial
receiver failure must not restart processing on an unverified configuration.
Unexpected external edits stop the run and are preserved for reconciliation;
never overwrite them silently under a stale revision. Physical disconnection,
host failure or unavailable hardware can prevent restoration: persist the
failure and retry on recovery rather than claiming a guarantee that hardware
cannot provide. Reboot, USB reset and hardware power cycling are not recovery
actions for this survey.

## Receiver adapter boundary

Adapters provide `snapshot`, `planApply`, `apply`, `awaitReady`, `readback` and
`restore`, with bounded operations and explicit evidence receipts.

| Receiver | Required apply/readiness evidence | Current boundary |
| --- | --- | --- |
| Kraken | Suite managed frequency change; full lag/phase recalibration after every retune; command acknowledgement, completed recovery and complete tuple readback | Suite settings readback is not independent hardware/serial verification; record calibration evidence separately from an `already-matched` receipt |
| RSPduo | Managed processor-owned restart; both tuners initialized; applied parameters and coherent readiness | Independent post-init tuner readback is currently unavailable; do not report it as implemented |
| USRP | Managed processor-owned restart; UHD getters verify actual frequency/rate/gain/antenna/subdevice; coherent timing verified | Fresh generic processor status alone is insufficient |
| Dual HackRF | Managed restart; both device serials/settings and synchronization verified | Current adapter lacks post-set frequency/rate/gain/clock/synchronization readback; automated qualification remains blocked until supported |

Only adapters with the required evidence may execute automatically. Supported
frequency plans do not imply a receiver supports every listed frequency, rate,
bandwidth or coherent topology. Two channels may qualify radar/reference use;
they cannot independently certify a multi-element bearing manifold.

## Acquisition protocol

Prescreen at fixed rate, mapping, geometry, reference algorithm and detector/
tracker settings. For every frequency, use the same measured procedure to find
the highest safe coherent common gain; do not force every transmitter to use the
gain required by the strongest direct path. Freeze that selected gain within all
accepted blocks for that candidate. Change only frequency, candidate gain and
the correct transmitter geometry. Verify actual settings and exclude warm-up/
calibration frames before each measurement. Freeze the prescreen thresholds
before looking at outcomes.
Capture repeated short records and reject clipping, weak CNR, instability,
severe imbalance, direction mismatch and excessive spectral phase curvature.
Retain at most a small predetermined shortlist (default two).

Alternate shortlist candidates in balanced blocks, for example ABBA then BAAB.
Every block has a fixed valid observation budget, a bounded settling budget and
an immutable epoch ID. Both candidates receive the same detector, cancellation,
tracking and matching rules. Exclude reset/startup transients consistently.
Stop immediately on clipping, receiver mismatch, calibration failure, stale
config/processor, CPI deadline violation or increasing drop counters; restore.
Capture counter baselines anew after each intentional restart and distinguish
startup discontinuities from losses during accepted measurement windows.

Measurements must include:

- RF: calibrated CNR procedure, per-channel clipping fraction from ADC-level
  data, usable occupied bandwidth and spatial/subband repeatability. Zero IQ
  display minima/maxima are not proof of no clipping. Relative sample power is
  not dBm. Record the selected common gain, peak fraction and input-referred
  noise for every candidate. If the input format cannot reveal clipping or the
  gain scale cannot support an input-referred comparison, record it as unavailable.
- Waveform: ambiguity peak sidelobe level with a fixed exclusion of the main
  lobe and fixed delay/Doppler search region; pre/post-cancellation power on
  matched samples, cancellation depth and residual clutter. Reference
  coherence does not substitute for these measurements.
- Radar: timestamped detections, their SNR, native unassisted track IDs and
  associations, duration, drop counters and CPI distribution. Assisted tracker
  outputs are separately labelled; ADS-B may score radar but must not create
  the detections being scored.
- Bearing: truth-blind selected bearing, per-channel target response/covariance,
  reference/model version, and subband target response when available. Current
  reference-only subband captures cannot replay frequency-resolved aircraft
  bearings. Missing target subbands prohibit that comparison.

## Opportunities, matching and held-out validation

Declare eligibility independently of radar success: fresh ADS-B positions and
motion, valid transmitter/receiver geometry, predicted delay/Doppler inside the
configured search window and masks, fixed range/elevation limits and a healthy
receiver epoch. Retain eligible misses. Use consistent units: the bearing-track
API uses sample delay, ADS-B delay is kilometres, and detection/manifold evidence
already uses kilometres. Convert sample delay once using that epoch's rate.

A pass is one continuous eligible visit by an aircraft under a documented gap
rule; duplicate frames and fragmented tracker IDs do not create new passes.
Assign sectors with a frozen rule (for example the first eligible azimuth),
and retain the full angle/elevation history. A persistent pass requires a fixed
duration and actual associated updates; coasting alone does not qualify.
Store matched detection frames separately from matched/persistent pass counts.
Use one-to-one delay/Doppler matching with ambiguity rejection and fresh truth;
never use bearing agreement to decide the truth label for bearing scoring.

Report matched_passes / eligible_passes and persistent_passes / eligible_passes.
Use equal predefined sector weights on common support, with minimum opportunity
counts in every compared sector. Include pooled rates only as diagnostics.
Different traffic mixes, survey protocols, sites, masks, firmware or processing
versions cannot be silently pooled. Preserve block-level rates and per-pass
duration/SNR distributions; estimate uncertainty by resampling aircraft/pass
groups, not correlated frames. Inconclusive overlap warrants more evidence,
not a claim that an arbitrary numeric lead established superiority.

Freeze candidate algorithms before test data are opened. Keep whole aircraft
and passes out of training; for temporal validation also reserve later blocks.
Compare full five-channel ideal, the configured sparse baseline, and a frozen
frequency-resolved candidate on the same accepted target observations. Record
failures to produce a bearing instead of dropping them from accuracy statistics.
Report per-aircraft/pass-balanced circular error, tails, coverage and every
sector. The best lobe chosen using ADS-B is an oracle diagnostic, not operational
accuracy. A transmitter's measured vector at one direction cannot certify a
learned manifold at other aircraft directions.

Hard safety gates precede ranking. Rank only compatible fully qualified
candidates by sector-standardized persistent-pass yield, then matched-pass
yield, safely available receiver gain, input-referred noise and held-out bearing
error. Actual radar yield always outranks gain. No measured sector may regress against the
frozen paired baseline. The evaluator's recommendation is conditional on its
supplied evidence; it cannot authenticate measurements or establish statistical
superiority without acquisition provenance and uncertainty analysis.

## Ford Street next run and acceptance

Keep the raised array, 80° true CH0 heading, identity mapping, 2.4 MSPS and all
five channels. The next radar-sensitivity candidate should be chosen from a
measured safe-gain prescreen rather than the old fixed-gain WPXI/WINP proposal.
The exact original configuration remains the restore authority; do not replace
catalog geometry without verification.

A proposed eight-block pilot uses ten valid minutes per block: A B B A B A A B.
This is 80 valid minutes, up to 16 minutes of bounded transitions and a separate
five-minute restore reserve (101-minute ceiling). This budget is a proposal,
not an active schedule. A preview must replace these budgets if adapter
capabilities cannot meet them. No minimum duration guarantees enough aircraft.

Before running that pilot, require correct delay-unit tests, a live eligible
opportunity recorder, replay-verified per-channel target recording, measured
clipping/cancellation/ambiguity telemetry, and restore fault-injection tests.
Zero matches for A is a valid radar outcome and must not prevent measuring B.
Acquire verified target matches during the pilot before claiming bearing
qualification, then collect the multi-aircraft/multi-sector held-out comparison. Missing
frequency-resolved target evidence leaves that candidate unqualified. No live
fixed reflection null, mapping/heading change, calibrated-manifold promotion or
second-node geolocation integration is part of this experiment.

Executor acceptance tests must cover success, API timeout, rejected apply,
calibration failure, stale status, wrong gain/rate/channel readback, clipping,
drops/CPI regression, cancellation, killed worker, lease expiry, disk-full
journal failure, external config conflict, and failed restore. Verify exact
restoration and prior service states, including an originally stopped service.
Synthetic scoring tests must cover eligible misses, duplicate frames, tracker
fragmentation, unit mismatch, stale truth, ambiguous matches, aircraft leakage,
missing sector support and a stronger RF candidate with inferior track yield.
