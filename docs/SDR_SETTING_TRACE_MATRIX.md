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

## Backend 2 — RSPduo (pending serial trace)

Settings enumerated from code: optional serial, centre frequency, output sample
rate, AGC set point, bandwidth enum, per-tuner gain reduction, shared LNA state,
DAB notch, RF notch, derived IF bandwidth/frequency/decimation, dual-tuner mode,
paired sample counter, gain-event epochs, and overload acknowledgement.

## Backend 3 — USRP (pending serial trace)

Settings enumerated from code: device address/identity, centre frequency, sample
rate, per-channel gain, per-channel antenna, subdevice mapping, motherboard
count/type, clock source, time source, two-channel stream mapping, timed start,
and UHD device-time continuity.

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
