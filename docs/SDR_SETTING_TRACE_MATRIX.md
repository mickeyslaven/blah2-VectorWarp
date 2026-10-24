# SDR setting trace matrix

This ledger enumerates live receiver backends from the executable source of
truth, not from documentation assumptions. `Capture::VALID_TYPE` and the
receiver loader expose exactly four live backends: `Kraken`, `RspDuo`, `Usrp`,
and `HackRF` (`src/capture/Capture.cpp`, `src/capture/ReceiverLoader.cpp`).

The trace for each setting is:

`config schema/default/UI -> native parser/factory -> device/upstream apply -> independent readback/attestation -> DSP conditioning -> status -> recording -> replay -> regression`

State abbreviations are `A` applied, `R` independently read back or attested,
`P` preserved in the portable recording contract, `X` validated/consumed by
replay, and `T` covered by a deterministic regression. `provisional` means the
path is usable but cannot claim exact RF time or physical coherence.

## Backend 1 — Kraken (complete)

| Setting / contract | End-to-end trace | State | Residual / operational meaning |
| --- | --- | --- | --- |
| Receiver type; Heimdall host/data/control ports | `api/config-manager.js` profile and validation -> `api/receiver-sync.js` control endpoint -> `Capture::factory_source` -> `Kraken::connect_heimdall` -> recording `heimdall` object | A P T | Host/ports identify the transport, not the physical radio. Heimdall V2 exposes no stable Kraken serial; provenance says identity unavailable rather than inventing it. |
| Centre frequency | `capture.fc` -> acknowledged Suite `set_frequency` plus status readback -> per-channel Heimdall frame metadata -> native tolerance/mid-stream change gate -> sidecar observed frequencies and binary header -> replay header/config validation | A R P X T | Suite status is upstream readback; every accepted live frame also carries per-channel observed frequency. |
| Sample rate | fixed profile/schema value `2,400,000` -> Suite status mismatch block -> native constructor invariant -> binary header/sidecar -> replay validation | A R P X T | Heimdall frames do not carry a sample-rate field; the qualified contract is the fixed Suite V2 rate plus upstream status. |
| Active channel count and channel index identity | `channel_count` -> acknowledged `set_num_elements` plus status readback -> native buffer/frame count equality -> ordered channel vectors/sidecar -> replay channel-count validation | A R P X T | Active prefix indexes are preserved, but physical tuner serial/order is not exposed by Heimdall and remains provisional. |
| Common gain, per-channel gains, automatic-gain policy | validated `heimdall.gain`, `channel_gains`, `gain_control` -> Suite control transaction/readback -> per-frame gain metadata -> attenuation to minimum reported channel gain -> epoch boundary on any gain-vector change -> sidecar observed gains and conditioning contract -> replay consumes already-conditioned IQ | A R P X T | Actual frame gains, not requested values, are the RF evidence. Gain policy drives upstream automation but replay does not rerun it because recorded float IQ is already equalized. |
| Synchronization/calibration state | Heimdall phase/noise/retune fields -> `is_synchronized_data` gate -> FIFO reset and recording epoch rollover on loss -> observed-frame provenance | A R P X T | No device RF/UTC timestamp exists. Coherent state is upstream-reported and usable provisionally; physical phase agreement still requires a common-signal test. |
| Native conditioning | Heimdall signed-byte payload -> per-frame/per-channel mean removal -> per-channel attenuation to minimum gain -> float32 complex IQ -> portable recording | A P X T | Sidecar now states `dcRemoval`, `gainEqualization`, input gain source, and that conditioning is already applied, preventing replay from silently applying a second correction. |
| Relative sample/time epoch | host-arrival estimate plus clip sample count -> CPI midpoint provenance -> sidecar epoch/source sample -> deterministic replay | A P X T | `exact:false`, `uncertaintyBounded:false`; exact ADS-B/OPRS scoring remains unavailable until the producer supplies an RF timestamp or bounded clock mapping. |

Kraken setting changes are transactional at two boundaries. The API saves
frequency/gain/channel-count changes only after Suite acknowledgement and status
readback. Independently, native ingress treats any observed frequency or gain
vector change as a sample epoch discontinuity even if the Suite frequency-change
counter did not advance. It clears queued old-epoch IQ, closes only the old
recording incomplete, and lazily records the next synchronized frame in a clean
provisional epoch.

## Backend 2 — RSPduo (complete)

| Setting / contract | End-to-end trace | State | Residual / operational meaning |
| --- | --- | --- | --- |
| Receiver type and optional serial | RSPduo profile/default/UI validation -> YAML parser -> receiver factory -> SDK enumeration and exact serial/model selection -> startup receipt -> recording details -> replay backend validation | A R P X T | The enumerated SDK serial, RSPduo model, device index, `Tuner_Both`, and `Dual_Tuner` mode are attested. An empty serial is accepted only when exactly one RSPduo matches. |
| Centre frequency | `capture.fc` -> native range/band-specific LNA validation -> both tuner `rfFreq.rfHz` records -> successful SDK initialization -> status/sidecar -> replay header/config validation | A P X T | SDRplay exposes the requested parameter record and API success, but no independent post-tune RF readback; `hardwareVerified:false`, `readbackAvailable:false`. |
| Output rate, IF, bandwidth and decimation | supported-rate map -> factory -> derived 1.620 MHz IF, 1536/600/300/200 kHz bandwidth and 1/2/4/8/16/32 decimation on both tuners -> SDK-reported ADC/descriptor clocks in status/sidecar -> binary header/replay validation | A P X T | The adapter preserves the SDK-reported effective ADC clock instead of overwriting or assuming 6 MHz. Those SDK descriptors are not an independent clock measurement. Counter scaling by three is allowed only for reported 6 MHz ADC, 2 MS/s output, IF 1.620 MHz, decimation 1 and dual-tuner mode. |
| Per-tuner gain and shared LNA | exactly two validated `gainReduction` values plus frequency-banded `lnaState` -> separate tuner parameter records -> per-tuner gain update calls -> startup/status/sidecar -> replay settings fingerprint | A P X T | Initial gain is request-plus-API-acceptance, not RF readback. Vendor GainChange callbacks preserve tuner-specific `gRdB`, `lnaGRdB`, `currGain`, arrival time and epoch. |
| AGC policy | `agcSetPoint` and 0/5/50/100 Hz bandwidth -> disabled/5/50/100 SDK enum and set point on both tuners -> startup/sidecar -> replay fingerprint | A P X T | Any vendor GainChange is a hard sample epoch boundary regardless of requested AGC mode. Its callback lacks a sample position, so `sampleAligned:false`. |
| DAB and RF notches | schema/default/UI -> factory -> each tuner RSPduo notch fields -> startup/sidecar -> replay fingerprint | A P X T | API acceptance only; no independent analog-filter readback. |
| USB transport, SDK debug and sample-counter scale | validated environment (`isoch|bulk`, `error|verbose`, `1|3`) captured once at construction -> device/API apply -> startup/sidecar | A P T | These deployment knobs are not YAML settings and replay does not reproduce USB transport or logging. It consumes their preserved provenance because IQ is already captured. |
| Paired A/B continuity and relative sample epoch | SDK first-sample counters -> tuner/order/length/reset/gap checks -> paired FIFO/CPI queue -> relative epoch/source sample in status and recording -> deterministic replay | A P X T | Time is `exact:false`, `uncertaintyBounded:false`, `sampleAligned:false`; no UTC clock mapping exists. Gain changes discard only ambiguous work and automatically publish the next matched pair in a new epoch. Other demonstrably corrupt callback spans remain rejected. |
| Overload state and acknowledgement | vendor per-tuner overload events -> serialized acknowledgement -> per-tuner active/count/time telemetry and acknowledgement failures | A P T | Diagnostic/control provenance is preserved in recording details; it is not an IQ transform replay can reproduce. |
| Native conditioning | paired signed-int16 callbacks -> direct complex conversion or packed paired-CPI queue -> portable `s16-interleaved` recording | A P X T | No hidden gain normalization or DC-removal stage exists in the RSPduo ingress path. Replay reads the recorded raw signed samples under the preserved receiver contract. |

RSPduo remains operationally provisional. Successful SDK selection,
initialization and gain-update calls prove that the vendor API accepted the
parameter records; they do not independently prove physical RF gain,
frequency, phase coherence or UTC timing. A gain event rolls only the affected
epoch incomplete and capture resumes automatically on the next matched pair.

## Backend 3 — USRP (complete)

| Setting / contract | End-to-end trace | State | Residual / operational meaning |
| --- | --- | --- | --- |
| Device address and identity | USRP profile/default/UI -> YAML parser/factory -> `multi_usrp::make(address)` -> per-channel `get_usrp_rx_info` motherboard/RX identity -> status/UI/recording -> replay backend validation | A R P X T | Requested UHD device arguments and the enumerated motherboard serial/ID plus RX-chain descriptors are both preserved. Missing optional UHD identity fields remain explicitly incomplete; conflicting channel motherboard identities reject startup. |
| Subdevice/channel topology | validated two-value configuration -> motherboard-0 subdevice setter -> exact getter readback -> two-channel stream map `{0,1}` -> single-B210/distinct-subdevice attestation -> status/UI/sidecar | A R P T | This bounded policy supports one B210 motherboard only. Other coherent UHD topologies remain unsupported until separately qualified rather than being silently called coherent. |
| Centre frequency and sample rate | `capture.fc/fs` -> factory -> per-channel UHD setters -> per-channel getter tolerance gates (1 Hz and 0.5 Hz) -> startup API/UI/sidecar -> binary header/replay validation | A R P X T | UHD getter values are device-API configuration readback, not an RF measurement or frequency-standard calibration. 2, 6 and 20 MS/s setter/readback fixtures pass; sustained hardware throughput is not inferred. |
| Per-channel gain and antenna | exactly two finite gains and two nonempty ports -> separate channel setters -> getter gates (0.05 dB/exact string) -> startup API/UI/sidecar -> replay fingerprint | A R P X T | Unequal gains and antenna roles remain channel-specific. Replay consumes the recorded IQ and provenance; it does not rerun UHD setters. |
| Clock/time sources and motherboard | UHD getters -> one-motherboard B210 attestation -> startup status/UI/sidecar and per-CPI event-time provenance | R P X T | Sources are attested, not configured by VectorWarp. A nonempty source on a single motherboard supports provisional two-channel coherence, but device time is deliberately `utcMapped:false`; it cannot support exact ADS-B event-time scoring. |
| Timed stream start and two-channel receive | fixed device-time `now + 50 ms` start, explicit channel map, bounded SDK receive capacity -> paired queues | A T | Start policy is internal, not a user setting. The first accepted metadata timestamp establishes the recorded device-time epoch. |
| Device-time continuity and recovery | every block requires UHD `time_spec` -> integer sample-tick continuity -> fault rolls recording incomplete, clears unpublished IQ and stages two continuous blocks -> qualified relative epoch in health/recording/replay | A P X T | Missing/regressing/jumping time and receive discontinuities discard only corrupt/ambiguous spans. Eight consecutive faults are recoverable; the ninth terminates a persistently unhealthy stream. Epochs remain provisional and inexact. |
| Native conditioning | UHD `sc16` wire format converted by UHD to `fc32` -> paired complex-float queues/portable recording | A P X T | No backend gain equalization or DC-removal is applied. Frequency/gain/antenna readback and topology travel with the already captured samples. |

The API formerly discarded `hardwareVerified`, `readbackAvailable` and
`appliedReadback` even though native capture emitted them, while the receiver
acceptance UI claimed that radar status could not confirm the settings. The
validated status contract now preserves all per-channel getter values and the
Settings UI renders them with the physical-RF and exact-UTC limitations.

## Backend 4 — dual HackRF (pending serial trace)

Settings enumerated from code: two serial identities/roles, centre frequency,
sample rate, per-radio LNA gain, per-radio VGA gain, per-radio RF amplifier,
hardware-sync role, clock-output role, start order, paired callback/FIFO epoch,
bounded drain/requalification state, and retry budget.

## OS lifecycle passes (pending after all four backend traces)

1. Fedora/RPM native lifecycle, packages, paths, systemd sandbox and device permissions.
2. Debian/Ubuntu/DEB lifecycle, packages, paths, systemd sandbox and device permissions.
3. Raspberry Pi image/profile/service lifecycle and SDRplay permissions/performance overrides.
4. macOS Homebrew/standalone lifecycle, paths, launch ownership and receiver runtime loading.
