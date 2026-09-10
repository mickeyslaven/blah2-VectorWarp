# Browser UI and configuration review

Scope and verification notes for the browser UI/configuration changes on the
2–8-channel Kraken base. Automated tests use temporary configuration files and
fixtures; they do not start a receiver or verify physical hardware.

## Implemented

- Shared browser/API scalar constraints plus processor-derived cross-field checks:
  FFT dimensions, integer-width overflow, spectrum minimum samples, capture buffer
  capacity, detector/tracker limits, distinct ports and receiver-specific fields.
- Typed controls, channel selectors, grouped MHz/kHz/Hz tuning, per-field errors,
  accessible labels, stale-validation protection and revision-checked atomic saves.
- Cross-field failures identify the affected controls by their readable labels.
  Known receiver-frequency/channel mismatches end restart waiting with a specific
  correction instead of waiting for the generic no-frame timeout.
- Newly selected API/data ports are bind-checked before validation/save accepts
  them. Existing owned listeners are released on restart; another process can
  still take a port after the check. Explicitly read-only config files/directories
  remain read-only even when the API runs as root.
- Receiver-first organization: Receiver, Sites, Radar, Evaluation, Recording,
  Connectivity. Kraken reference mode, reference input and synthesis controls live
  under Receiver → Kraken → Reference signal; advanced solver tuning is collapsed.
  YAML keys are unchanged and error badges follow the visible tab, not the key prefix.
- Routine memory estimates and generic hardware disclaimers are not UI warnings.
  Actionable advice is short and local to the affected field/category; actual
  configured memory-budget violations remain blocking errors.
- Sites uses Receiver/Transmitter dropdowns and Add/Edit location dialogs, without
  field-count badges. Assignments still save to `location.rx`/`location.tx` in YAML.
  Recent location choices are a convenience stored on that browser (labelled as
  such), never a substitute for the selected locations read from server config.
- Saved-file and API-startup config are separate endpoints. Saving does not relabel
  old radar frames with new tuning or site settings.
- Frame interval (`process.data.cpi`) is the sole timing control; buffer capacity
  is under Performance. All radar displays use the loaded interval, discover a
  restarted interval within the config check cadence (2 seconds plus requests),
  and await fetch/render completion without queuing missed ticks. Browser refresh
  is capped at approximately 60 Hz for very short intervals. Actual results still
  depend on processor throughput and network/render time.
- Histories consume arriving stream messages instead of independent 100 ms HTTP
  pollers. They deduplicate timestamps, reset incompatible grids/tuning, preserve
  empty detection updates, and do not mutate the current radar frame.
- Known unimplemented/legacy fields (overlap, smoothing, AIS, legacy IQ/timing
  flags and reserved config port) are hidden, including empty groups. Their
  existing YAML values survive browser edits/saves unchanged. Replay loop,
  portable/legacy format selection and the conditional legacy block length are
  editable and server-validated.
- Both Performance dropdowns support Auto (`0`) for every receiver backend.
  The processor now resolves FFT teams against workers, affinity and visible
  Linux v1/v2 cgroup CPU quotas, including parent quotas. Fully automatic choices
  leave one CPU of scheduling capacity when possible and cap each FFT team at four.
  Positive overrides and omitted-field legacy defaults remain unchanged. New
  setup/device presets opt into Auto; old valid files do not require setup merely
  because their optional performance section is absent.
- Missing/malformed/partial config recovery without automatically writing a file.
  Omitted Kraken optional fields retain processor defaults (dedicated reference,
  one worker), not new-device preset defaults.
- Manual-save fallback where no scoped restart helper is installed. Restart errors,
  occupied data ports and missing frames are reported without hiding settings.
- Passive, time/size-bounded, cached Suite V2 status observation using a separately
  configurable status port. Partial/unavailable telemetry never counts as matched.
- RSPduo, USRP, dual HackRF and Kraken controls. Restricted builds expose all
  profiles, marking backends absent from the compiled live binary as replay-only;
  those replay configurations remain editable and valid.
- Space or the recording button requests an IQ recording transition. The control
  shows processor acknowledgement, output file and writer errors rather than
  treating a request as a successful disk write. Recording is unavailable during
  replay and cannot start before live frames arrive.
- Header status uses frame-receipt freshness and reports stale, unavailable and
  restarting processing separately. ADS-B status reports the one raw tar1090
  feed and its aircraft/derived delay-Doppler products; disabled ADS-B and replay
  suppress live truth requests and overlays.
- ADS-B comes from the API's loaded raw tar1090 address, including host:port and
  explicit HTTP(S)/path-prefix forms. The API derives delay/Doppler internally;
  fetches are cached, time/size-bounded and freshness-checked, and motion warmup
  entries are filtered. ADS-B remains an evaluation overlay, not a radar input.
- The native API serves the browser, API, recording and history on one origin.
  HTTPS stays same-origin and no installation-specific gateway path is assumed.
- Unsupported boresight display and unused operator-tool styling are removed.

## Verification

Run with Node.js 22 or newer using the locked dependencies:

```sh
cd api
npm ci --ignore-scripts --no-audit --no-fund
npm test
npm run test:dom
```

The suite covers 271 setting leaves across four backends, Kraken counts 2–8,
invalid boundaries/combinations, file conflicts/backups/permissions, broken YAML,
occupied data ports, failed restart commands, receiver telemetry missing/mismatch/
timeout/size bounds, and browser field edits/save payloads/validation races.
ADS-B tests include actual local HTTP fixtures, source queries, empty/warming/
stale/disabled feeds, caching, body timeouts, size bounds and malformed responses.
Routing tests cover the native same-origin API/static server, alternate API ports,
hostnames, IPv6, HTTPS, freshness labels and non-retried writes.
The separate `test/ui/insights-smoke.js VIEW portable|rich` covers all eight new
views with sparse and populated fixtures, including optional ADS-B evaluation.
Both modes are fixture-only; they never contact a real ADS-B or radar service.
Run from the repository root with the API dependencies available:

```sh
for mode in portable rich; do
  for view in overview detections health activity site locations evaluation tracks; do
    node test/ui/insights-smoke.js "$view" "$mode"
  done
done
```

Frame tests additionally cover changed/invalid/very long intervals, pending-restart
saved config, slow requests, render retries, original plot first/subsequent frames,
late detection overlays and TCP-to-history delivery without a timestamp tick.
The standalone `testProcessingThreads` C++ target covers thread allocations for
1–256 CPUs and 1–8 paths, explicit overrides, missing CPU data, visible v1/v2 and
parent quotas, fractional limits, namespaces and mount-root/path handling. It was
compiled with warnings as errors and tested under a fractional CPU quota.

### Optional deployment acceptance test

`node test/ui/deployment.test.js` must run with port 3000 free and no live
configuration in its temporary directory. It starts its own temporary native API
and raw ADS-B fixture. The check exercises same-origin static/UI/API delivery,
configuration revision/save, recording status, ADS-B warmup, header status, JSON
error propagation and the scoped non-root unit templates. It removes its temporary
config and stops only the API/fixture processes it created.

## Remaining acceptance boundary

- Real radio/DAQ/driver/physical-recording and service-manager integration is
  outside these UI tests. Source-derived validation is not proof
  that every hardware model accepts a tuning/gain/antenna combination.
- Automatic threading is a conservative policy, not a throughput benchmark or
  runtime autotuner. Standalone policy tests and browser/API round trips are not
  a full radio build, deployment or throughput benchmark. Actual speed depends on workload,
  FFT sizes and other library threads (for example BLAS).
- API and processor filesystems may differ; recording/replay path permissions and
  free resources must be checked on the processing host. An optional configured
  memory budget rejects estimated allocations; it is not a total-memory guarantee.
- Firewall changes and initial driver/hardware permissions remain host
  administration. The supplied restart helper is limited to the two VectorWarp
  units and is not installed by UI tests.
- DOM and renderer smoke tests do not certify pixel layout on every browser.
- Config comments are serialized anew on save; the raw backup retains them.
  Revision checks prevent stale browser/API writes, but external editors must not
  race the tiny final filesystem-rename window or ignore the service boundary.

## Preview

`test/ui/preview-server.js` runs the real API against a temporary config, with no
restart helper, no upstream connection and no simulated live readings. The
preview serves the UI from the API port in its temporary configuration. Run it
only in an isolated test environment; restarting resets that temporary config.
Do not use the preview as evidence that a physical receiver was tested.
