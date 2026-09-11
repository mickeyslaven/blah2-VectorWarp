# Unified native receiver architecture and acceptance

One processor executable retains built-in Kraken and recording replay. Optional
USRP, dual-HackRF and RSPduo adapters are separate modules in the **same package**;
missing vendor runtimes do not prevent another receiver or replay from starting.
Only an explicitly selected live receiver is constructed. No alternate receiver
is selected following an adapter failure.

## Cohort and packaging contract

- CMake: `BLAH2_ENABLE_USRP`, `BLAH2_ENABLE_HACKRF`, `BLAH2_ENABLE_RSPDUO` select
  source-build modules. Unified packages enable all three. Kraken is always built.
- `blah2` depends on enabled targets `blah2ReceiverUsrp`, `blah2ReceiverHackrf`,
  `blah2ReceiverRspduo`. `blah2CaptureCore` is always built, even Kraken-only.
- Ship adjacent `blah2`, `libblah2-capture-core.so.1.0.0` and its `.so.1` / `.so`
  symlinks, and `blah2-receiver-{usrp,hackrf,rspduo}.so`.
- Core/main have no vendor SDK dependency. Modules use only fixed executable-
  adjacent paths, `RTLD_LOCAL|RTLD_NOW`, factory ABI1 and a compiler/interface-
  fingerprint check. This is a private same-package ABI, not a user plugin API.
- Main/common/module runtime paths are `$ORIGIN` only. If an RSPduo module cannot
  load normally, its loader can open only the exact root-owned, non-group/world-
  writable regular file `/usr/local/lib/libsdrplay_api.so.3.15`; symlinks and
  special files are refused. The checked inode and SDK handle stay bound through
  adapter destruction. No environment path, global loader configuration or
  temporary build-SDK path is added to published binaries.
- The module handle outlives the derived deleting destructor. No proxy Source is
  used: shared recording state and IQ buffers remain the actual capture object.

`blah2 --receiver-status` emits schema1 JSON, `hardwareProbed:false`, and an array
`receivers`. Each item has `receiver` (Kraken/Usrp/HackRF/RspDuo), `builtIn`,
`compiled`, `moduleLoadable` booleans and `error` (printable ASCII, <=240 chars).
This checks module/runtime/ABI loadability only. It does not construct a receiver,
open hardware, start a service, apply settings or assert physical readiness.

## SDK and setting boundaries

Native Ubuntu22.04 UHD4.1.0.5 headers compile the actual capture source with
GCC11, C++17, `-Wall -Werror`; native Fedora44 UHD4.9 also builds the complete
module. UHD4.1 is therefore the minimum source/package-adapter floor, not4.8.
This is compile/API compatibility, not a hardware or timing qualification.

The user explicitly approved **build-only** use of the existing SDRplay SDK.
Only our adapter is packaged: no SDK installer, header, API library or service
redistribution, and no automatic license acceptance on an end-user machine.
The vendor API is installed locally by its owner. The tracked archive is pinned
at SHA256 `3a97ca764263bbe76fb0f2220e6408942357e8864c19e1408a6d6987af382fe3`.
Extracting its known gzip/tar payload is not executing its installer.

| Adapter | Startup SDK application | Verification / limitation |
| --- | --- | --- |
| USRP/B210 | address to UHD device creation; subdevice mapping; both channels' rate, frequency, gain and antenna | Independent getters gate streaming. UHD clock/time-source defaults remain; no external UHD settings application is needed. |
| Dual HackRF | two distinct serials; both channels' rate/frequency/LNA/VGA/amp; existing fixed synchronization/CLKOUT roles | Every setter return is checked; startup failures release partially opened devices. No equivalent universal RF getter/coherence proof is claimed. Physical synchronization wiring remains required. |
| RSPduo | exact optional serial; dual-tuner mode; both tuners' frequency, gain reduction, LNA state, AGC mode/setpoint, IF/bandwidth/decimation and RF/DAB notches | Multiple unselected units refuse startup. Init/Update acceptance and local parameter records are not independent physical readback. Output rates are62.5k–2MS/s in the existing dual-tuner mode, not6MS/s. |
| Kraken | Native receives the configured Heimdall endpoint/channel mapping | Upstream software's supported live ACK/readback contract is managed separately by the web/API layer; other software installation/startup-only settings remain explicitly distinguished. |

USRP and HackRF setter mocks include2,6,20MS/s; there is no6MS/s hard cap in
their capture rate forwarding. UHD buffers use bounded SDK packet/chunk sizes,
not Doppler width or entire CPI length. CPU/GPU throughput, USB/network headroom,
coherence and long-duration receiver reliability require actual hardware tests.

The upstream project's reported B210 timeout/crash after5–10minutes remains
**unverified**, not declared fixed. The receiver now reports timeout/overflow/
out-of-sequence/oversized-block faults as structured stopped-input errors before
accepting that block, marks recordings discontinuous and stops the UHD stream.
There is no silent concatenation across IQ loss or automatic gap retry.

## Offline acceptance

Tests run only in the existing0.5CPU, CPUs8/24,8GiB containers. No hardware,
services, host installations or firmware are touched.

- `receiverModuleIsolation`: relocated valid/missing/malformed/wrong-ABI/wrong-
  cohort/wrong-receiver/missing-runtime/factory-error modules; move/destruction
  lifetime, no eager sibling load, bounded read-only status and shared IQ state.
- `usrpAppliedReadback`: setter tuples at2/6/20MS/s; both-channel coercion,
  nonfinite values, mapping/antenna errors and receive/capacity faults.
- `hackrfAppliedSettings`: all exposed setters and paired callbacks at2/6/20MS/s,
  18 API/start failures, partial cleanup and malformed callback refusal.
- `rspduoStructuredFailures`:24 setting tuples (six rates × four AGC modes),
  explicit serial selection, ambiguous/missing serial refusal, unsupported6MS/s,
  15 API failure stages and idempotent cleanup.
- APT/DNF failure-injection suites retain transaction/signature/approval/locking
  protections, with the independently qualified UHD4.1 minimum.

Final combined packaging, full processor replay and UI acceptance are owned by
the integration task. These native checks do not replace that final acceptance.
