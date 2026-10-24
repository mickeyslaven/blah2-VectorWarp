# SDR/OS pipeline adversarial audit — 2026-10-23

## Scope and disposition

This was a read-only audit of the production path from SDR ingress through IQ
conditioning, reference/clutter processing, ambiguity/detection, tracking, JSON
publication, ADS-B event-time association, and OPRS recording/scoring. The live
configuration and production code were not changed. Existing unrelated working
tree changes were not touched.

Review order and status:

| Order | Surface | Status |
| --- | --- | --- |
| 1 | Kraken | **Complete** |
| 2 | RSPduo | **Complete** |
| 3 | USRP | **Complete** |
| 4 | dual HackRF | **Complete** |
| 5 | Fedora/Linux native package and systemd path | **Complete** |
| 6 | Debian/Ubuntu/Raspberry Pi package and systemd path | **Complete** |
| 7 | macOS Homebrew/standalone lifecycle path | **Complete** |

Severity means: **Critical** invalidates the evaluation or can silently assign
RF data to the wrong truth event; **High** can silently corrupt coherent IQ or
select the wrong RF samples; **Medium** breaks provenance, reproducibility, or a
supported deployment mode without necessarily corrupting every frame.

## Executive findings

| ID | Severity | Affected SDR / OS | Summary |
| --- | --- | --- | --- |
| F1 | **Critical** | all SDRs; all OSs; especially replay/OPRS | Published `timestamp` is host dequeue/extraction time, not RF sample event time, yet ADS-B and OPRS treat it as exact CPI event time. Replay also publishes current wall time rather than a preserved/synthetic signal epoch. |
| F2 | **High** | RSPduo; strongest on the documented Pi profile; all OSs | AGC gain-change callbacks are counted but do not invalidate the CPI, segment the recording, or preserve the new per-tuner gain. The supplied Pi profile enables AGC and unequal tuner gain. |
| F3 | **High** | USRP; all OSs | UHD `rx_metadata_t.time_spec` continuity is ignored. `ERROR_CODE_NONE` blocks with a time jump can be concatenated, and no device time reaches publication. |
| F4 | **High** | dual HackRF; all OSs | Independent callbacks feed independent dropping FIFOs with no cross-device sequence/timestamp check. A one-sided queue overflow or USB shortfall silently changes reference/surveillance alignment. |
| F5 | **High** | Kraken; all OSs | Clip absolute time is anchored from host TCP-arrival time. Network/queue latency therefore shifts ADS-B-requested clip windows relative to RF samples, but the sidecar reports exact requested times without timing uncertainty. |
| F6 | **Medium** | USRP topologies beyond a single coherent two-channel motherboard; all OSs | Per-channel tuning/readback is checked, but clock/time source, motherboard topology, and synchronization are not. A two-channel UHD mapping is accepted without proving a common coherent clock/time domain. |
| F7 | **Medium** | `.blah2iq` from all live SDRs; all OSs | The portable recording header stores only channel count/rate/frequency/sample count. It omits receiver identity, channel gains, event-time epoch, discontinuity/sample-position provenance, antenna/subdevice, and clock source, preventing full live/replay parity. |
| F8 | **Medium** | Fedora, Debian, Ubuntu, Pi systemd installs | Config validation accepts arbitrary absolute output paths and arbitrary replay strings, while the processor unit can write only `/var/lib/vectorwarp` and hides home directories. A config valid on macOS/source execution can fail under packaged Linux. |

## Cross-pipeline finding

### F1 — published host-processing time is mislabelled as RF event time (**Critical**)

**Evidence.** Live FIFO processing takes `current_time_us()` only after a complete
CPI is found and drained; the paired RSPduo queue likewise timestamps acquisition
when the processing thread borrows the ready CPI
(`src/blah2.cpp:1145-1151`, `src/blah2.cpp:1199-1235`). The clock is POSIX
`system_clock`, not a device/sample clock (`src/blah2.cpp:1761-1772`). Kraken is
the only backend exposing an epoch/sample position, and that position is attached
only to timing telemetry (`src/capture/Capture.cpp:30-35`,
`src/blah2.cpp:1211-1224`, `src/blah2.cpp:1600-1603`). It is not used as the
frame timestamp.

Replay does compute deterministic signal time for its internal tracker
(`src/blah2.cpp:1492-1494`), but every published spectrum/map/detection/track uses
`time[0]/1000`, i.e. current host playback time (`src/blah2.cpp:1531-1560`), and
the timestamp socket publishes the same value (`src/blah2.cpp:1637-1639`). The
replay file itself contains no event epoch (F7).

The API recorder copies this field verbatim into `event_time_ms`
(`api/oprs-event-recorder.js:40-58`, `api/oprs-event-recorder.js:77-95`). Trial
construction then requires exact nanosecond equality with a predeclared plan,
otherwise it silently assigns `unplanned:<timestamp>`
(`oprs/events.py:79-90`, `oprs/events.py:98-114`). ADS-B `getAt` also projects
truth to this exact supplied CPI time (`api/adsb-source.js:219-233`). Thus queue
depth, DSP load, OS scheduling, TCP latency, and replay start time all change the
claimed event time without changing the RF samples.

The use of unsigned wall-clock deltas is an additional failure mode if NTP or an
administrator steps the clock backward: `timing_helper` subtracts two `uint64_t`
POSIX readings (`src/blah2.cpp:1768-1781`), while the tracker refuses reversed or
duplicate times (`src/process/tracker/Tracker.cpp:40-45`,
`src/process/tracker/Tracker.cpp:65-74`). Neither Linux unit ordering nor the
macOS launcher establishes a sampled clock epoch contract.

**Impact.** OPRS opportunity membership and ADS-B projection are not defensible
as event-time joins. The same recording can receive different timestamps and
truth associations on two replays or two OSs. The published tracker timestamp
can also disagree with the time used internally by replay tracking.

**Reproduction proposal.** Record one fixed `.blah2iq`; replay it twice with a
controlled 750 ms consumer stall before CPI 3. Assert that the map/detection JSON
timestamps and OPRS CPI IDs remain identical. They currently move with the stall.
Also freeze a plan with known CPI times, run normal replay, and assert that no
observation becomes `unplanned:*`; the exact-time lookup exposes the mismatch.
Add an injectable realtime clock test that steps backward while a monotonic
processing clock continues.

**Fix.** Define one frame-time contract (prefer CPI midpoint) as
`{epoch_id, first_sample, sample_count, event_time_ns, uncertainty_ns, source}`.
Have every live backend provide a device or explicitly qualified epoch anchor;
carry it through capture, tracker, all JSON, ADS-B lookup, and the OPRS event log.
Store it in recordings and reproduce it in replay. Use a monotonic clock only
for durations. Reject scoring when absolute RF time is unavailable rather than
substituting host dequeue time.

## Backend review 1 — Kraken (**Complete**)

Kraken validates channel count and per-channel frequency, rejects unsynchronized
or retuning data, clears buffers across retune/synchronization loss, and appends
all channels under one lock set (`src/capture/kraken/Kraken.cpp:113-178`). The
decoder preserves and validates per-channel frequency/gain, equalizes every
channel to the least-amplified channel, removes per-frame DC, and requires the
calibrated synchronized state (`src/capture/kraken/HeimdallFrame.cpp:63-81`,
`src/capture/kraken/HeimdallFrame.cpp:94-151`). MCHQ replay applies the same
decoder and refuses tuning/gain changes (`src/capture/Recording.cpp:169-182`,
`src/capture/Recording.cpp:192-195`). These are sound backend-specific guards.

### F5 — Kraken clip epoch is host TCP-arrival time (**High**)

On the first accepted Heimdall frame, clip recording defines RF sample zero as
`system_clock::now() - frame_duration` (`src/capture/kraken/KrakenClipRecorder.cpp:262-281`,
`src/capture/kraken/KrakenClipRecorder.cpp:317-357`). Requested wall-clock windows
are then converted directly to samples using that anchor
(`src/capture/kraken/KrakenClipRecorder.cpp:380-410`,
`src/capture/kraken/KrakenClipRecorder.cpp:425-443`). The sidecar publishes the
requested times and precise epoch sample range, but no arrival-latency or epoch
uncertainty (`src/capture/kraken/KrakenClipRecorder.cpp:584-619`). A TCP frame
received 40 ms late moves the selected IQ by about 96,000 samples at 2.4 MS/s.

**Reproduction proposal.** Feed identical numbered Heimdall frames through the
Kraken socket fixture twice, inserting a known 100 ms delay before the first
frame in one run while keeping the clip request wall time fixed. Compare
`firstEpochSample` and selected payload. They shift despite identical RF frame
content.

**Fix.** Source an epoch from the capture producer/device, or perform and record
a bounded clock/transport calibration. Until then mark clip time as host-arrival
derived with uncertainty at least the observed transport/buffering bound, and do
not use it for exact ADS-B association.

## Backend review 2 — RSPduo (**Complete**)

The A/B callbacks do useful fail-closed work: A sample counters are checked,
B must match A's first sample and count, malformed/reset/rate-change events clear
pending state, and only a matched pair is published
(`src/capture/rspduo/RspDuo.cpp:519-568`,
`src/capture/rspduo/RspDuo.cpp:596-676`). The Pi image installs the paired-CPI
queue/counter-scale profile (`contrib/systemd/pi4-rspduo-performance.conf:1-10`,
`packaging/pi/build-pi4-image.sh:210-215`).

### F2 — AGC gain changes cross CPI and recording boundaries silently (**High**)

Both tuners receive separate requested gains and the selected AGC mode
(`src/capture/rspduo/RspDuo.cpp:487-509`). However, a vendor `GainChange` event
only increments a counter (`src/capture/rspduo/RspDuo.cpp:702-711`); it does not
clear pending samples, invalidate the in-progress CPI, create a recording epoch,
or retain the new per-tuner gain. The startup receipt explicitly says independent
readback is unavailable and reports only requested gain
(`src/capture/rspduo/RspDuo.cpp:810-838`). The documented Pi configuration enables
5 Hz AGC and begins with unequal tuner gain reductions `[50, 45]`
(`config/config-pi4-rspduo.yml:1-15`). Portable recording then stores only the IQ,
not the gain event (F7).

**Impact.** A gain step inside one CPI violates stationary amplitude assumptions
in reference/clutter/CAF processing and makes live versus replay interpretation
ambiguous. A tuner-specific AGC change also changes reference/surveillance scale
without any sample-aligned metadata.

**Reproduction proposal.** Extend the SDK fixture to emit `GainChange` for tuner
A halfway through a 0.5 s CPI with a deterministic amplitude step. Assert that
the current code publishes the CPI and completes a recording with no epoch/gain
annotation. The corrected behavior should either split/drop the CPI at the exact
sample boundary or disable AGC for scored coherent capture.

**Fix.** For scored/coherent operation, require fixed gain on both tuners. If AGC
remains supported, capture the event's effective per-tuner gain at a sample
counter, terminate the current CPI/recording epoch, clear paired queues, and
resume only from a fully post-change CPI. Carry per-channel gain metadata into
recording and publication.

## Backend review 3 — USRP (**Complete**)

USRP applies and independently reads back channel rate, frequency, gain,
antenna, and subdevice mapping (`src/capture/usrp/UsrpSettings.h:8-28`,
`src/capture/usrp/UsrpReadback.h:9-31`). It schedules a timed two-channel stream
and fails on UHD errors, out-of-sequence metadata, oversized blocks, or a full
paired ingress queue (`src/capture/usrp/Usrp.cpp:42-98`,
`src/capture/usrp/UsrpStream.h:6-20`, `src/capture/usrp/UsrpIngress.h:38-63`).

### F3 — UHD device timestamps are ignored (**High**)

`verify_usrp_receive` checks only count, `error_code`, and `out_of_sequence`
(`src/capture/usrp/UsrpStream.h:11-20`). It never requires
`metadata.has_time_spec`, checks expected `time_spec + n/fs`, or exports that
time. `Usrp::process` receives metadata and immediately appends IQ after this
limited gate (`src/capture/usrp/Usrp.cpp:80-107`). Therefore two otherwise
successful blocks with a discontinuous device time are accepted as contiguous,
and F1 substitutes host extraction time for the device's timing evidence.

**Reproduction proposal.** Extend the fake UHD metadata with `has_time_spec` and
`time_spec`. Submit consecutive `ERROR_CODE_NONE`, `out_of_sequence=false`
blocks whose second time is one sample late. The present verifier accepts both;
the corrected verifier must mark a discontinuity before append/record.

**Fix.** Require device time metadata, track exact next tick with a rational rate,
fail or start a new epoch on any jump, and publish/record the device epoch and
per-CPI sample interval.

**Remediation.** Implemented fail-closed validation of each block plus bounded
best-effort recovery in `src/capture/usrp/UsrpDeviceTime.h` and
`src/capture/usrp/Usrp.cpp`. Every successful nonempty UHD block must carry the
exact next sample tick. A missing/regressing/jumping timestamp or UHD receive
discontinuity rejects that block, clears unpublished paired IQ, closes the
affected recording epoch incomplete, and enters `requalifying`. Exactly two
continuous device-timed blocks are buffered before their new epoch is published
atomically; recording then resumes lazily. Status and recording provenance carry
epoch, recovery state, discontinuity/requalification counts, discarded blocks
and samples, and the last cause. Nine consecutive discontinuities exceed the
bounded recovery budget and stop instead of spinning indefinitely. Because this
device clock has no qualified UTC mapping, recovered USRP event time remains
`exact:false` and authoritative ADS-B/OPRS scoring rejects it.

### F6 — two UHD channels do not prove a coherent topology (**Medium**)

Settings are applied to motherboard index 0 and checked per channel, but there is
no motherboard-count/topology restriction, `clock_source`/`time_source` setting
or readback, synchronization check, or LO/coherence qualification
(`src/capture/usrp/UsrpSettings.h:19-28`, `src/capture/usrp/Usrp.cpp:42-70`). The
class claims all USRP models (`src/capture/usrp/Usrp.h:3-8`), while setup correctly
admits that clock/time source is not verified (`docs/SETUP.md:119-133`). This also
conflicts with the illuminator executor's required “coherent timing verified”
claim (`docs/ILLUMINATOR_SURVEY_EXECUTOR.md:98-103`).

**Reproduction proposal.** Present a fake multi-motherboard device whose channels
read back identical RF settings but have independent time/clock domains, or use
two network USRPs in one UHD `multi_usrp`. The present gate accepts the mapping.

**Fix.** Restrict the supported topology to a qualified single-motherboard,
phase-coherent two-channel device (such as an explicitly identified B210) or add
clock/time source configuration, PPS/time synchronization, motherboard and
sensor readback, timed-start verification, and a physical common-signal phase/
sample-lag qualification.

**Remediation.** `src/capture/usrp/UsrpCoherence.h` now enforces the bounded
`single-motherboard-b210-v1` policy before streaming: one motherboard, B210
identity, at least two receive channels, two distinct applied subdevices, and
nonempty UHD clock/time-source readback. Other USRP models and multi-motherboard
topologies fail closed pending separate qualification. This attests topology;
it does not replace a hardware phase/LO calibration test.

## Backend review 4 — dual HackRF (**Complete**)

Serial suffixes are resolved uniquely, all setting/start statuses are checked,
the surveillance unit is armed for hardware triggering and started before the
reference/triggering unit (`src/capture/hackrf/HackRf.cpp:56-128`,
`src/capture/hackrf/HackRf.cpp:142-155`). Arming only the device to be triggered
is consistent with the vendor's documented `hackrf_transfer -H` procedure; this
was investigated and is **not** a defect. The fixture also intentionally asserts
this role order (`test/capture/HackRfSettingsFixture.cpp:90-121`).

### F4 — independent callback loss silently destroys pair alignment (**High**)

Each libhackrf callback independently pushes into one `IqData` FIFO
(`src/capture/hackrf/HackRf.cpp:158-189`). A full FIFO silently discards that
channel's oldest samples and increments only a counter
(`src/data/IqData.cpp:134-147`). The main loop reports per-channel dropped counts
but still drains the same `nSamples` from both channels; it neither requires
equal drop counts nor starts a new epoch (`src/blah2.cpp:1199-1227`). Callback
sample counts are not compared across devices, and no device counter or timestamp
exists. Shared trigger establishes the initial alignment only; it cannot preserve
alignment through an asymmetric USB shortfall. The setup documentation already
states that physical throughput/coherence is unverified (`docs/SETUP.md:135-148`).

**Reproduction proposal.** Invoke the two recorded callbacks with identical
initial frames, then overflow only surveillance by one callback before allowing
the processor to drain. Assert that a CPI is currently produced with different
drop counters and a known reference/surveillance impulse offset.

**Fix.** Treat any drop-count change or unequal callback/sample progression as a
pair discontinuity: clear both FIFOs, close the ambiguous recording epoch, and
refuse publication until bounded automatic pairing qualification succeeds.
Recording must be block-paired rather than independently assembled after a
silent FIFO drop.

**Remediation.** `src/capture/hackrf/HackRf.cpp` now stages one callback per
radio and appends only an equal, progression-matched pair while holding both
FIFO locks. A short callback, duplicate one-sided callback, unequal callback or
sample progression, changed FIFO drop counter, asymmetric queue length, or
insufficient capacity clears both FIFOs, advances an unqualified epoch, and
closes only the affected recording epoch incomplete. While streaming remains
healthy, recovery discards one callback from each radio as a bounded drain
guard and buffers two equal post-fault pairs before atomically publishing a new
CPI epoch. A new discontinuity restarts that qualification; nine consecutive
faults exhaust the declared eight-retry budget and stop the callbacks.
Receiver health and recording sidecars expose `unavailable`, `requalifying`,
or `qualified`, plus per-radio callback/sample/discard counters, drain state,
epoch, discontinuities, requalifications, retry budget, gains, serials, and the
reason. The old recording stays replay-rejected as incomplete; the successor
file resumes lazily at source sample zero and replays provisionally. This is
automatic operational recovery, not proof of RF phase coherence: published
HackRF event time remains `exact:false` because libhackrf supplies no device
sample counter or RF/UTC timestamp. A physical common-signal lag/phase test is
still required for decision-grade coherent qualification.

## Recording/replay review

### F7 — portable recording lacks hardware and time provenance (**Medium**)

`RecordingMetadata` contains only channels, sample rate, frequency, samples, and
format (`src/capture/Recording.h:11-20`). `Source::open_file` supplies exactly
those values (`src/capture/Source.cpp:38-64`), and the 64-byte format writes only
channel count/rate/frequency plus completion/sample count
(`src/capture/Recording.cpp:64-104`). It cannot preserve per-channel gain,
receiver serial/type, antenna/subdevice, clock source, RF epoch, sample-gap
markers, or whether Kraken samples were gain-equalized. Replay correctly paces
and prevents FIFO overflow (`src/capture/Replay.cpp:18-73`), but starts a fresh
host clock and cannot recreate omitted context. MCHQ is better for Kraken because
it retains frame gain/frequency metadata, but its original absolute event epoch
is still absent.

**Reproduction proposal.** Record identical tones with two different USRP gains
or RSPduo AGC histories. Show that both resulting `.blah2iq` headers are
indistinguishable except for sample values and that replay cannot state which
gain/epoch produced either file. Replay the same file twice and compare output
timestamps (F1).

**Fix.** Version the portable format with a sidecar/header manifest containing
receiver/backend identity, stable device IDs, per-channel applied gain and RF
mapping, antenna/subdevice, clock/time source, conditioning already applied,
epoch/time uncertainty, and sample-continuity segments. Hash the manifest and
bind it to each sample segment. Refuse event-time scoring of legacy files that
lack an externally qualified epoch.

## Processing/fallback/tracker review

No separate concrete numerical bug was found in the shared conditioning,
clutter, ambiguity, detection, or tracker math during this pass. The most severe
failures enter these stages through incorrect amplitude/time/alignment semantics
above. GPU ambiguity and clutter paths qualify against CPU output and fall back
with reported state/reason (`src/process/ambiguity/Acceleration.cpp:22-79`,
`src/process/ambiguity/Acceleration.cpp:133-176`,
`src/process/ambiguity/Acceleration.cpp:470-503`). Replay loop boundaries reset
reference/resident/tracker state (`src/blah2.cpp:1236-1250`). Those hypotheses
were checked and rejected as current defects.

Fallback does not repair bad capture provenance: CPU and GPU receive the same
misaligned/gain-stepped samples, and the tracker receives either host event time
or an internal replay time that differs from publication (F1).

## OS review 1 — Fedora/Linux native (**Complete**)

The native RPM bundles a private Node runtime and rewrites both API and processor
unit invocations; it also rejects an accidental `/usr/bin/node` RPM dependency
(`script/package-native.sh:199-229`, `script/package-native.sh:320-322`). Fedora's
HackRF udev rule assigns the service group while retaining distro mode/ACL policy
(`contrib/systemd/72-vectorwarp-hackrf.rules:1-4`). No additional Fedora-only SDR
data-path bug was found.

F1–F7 still apply. In addition, F8 applies to the packaged service path below.

## OS review 2 — Debian/Ubuntu/Raspberry Pi (**Complete**)

Debian post-install creates system users/state and adds the processor account to
the distro `plugdev` group for HackRF (`packaging/deb/postinst:4-25`). The Pi image
actually installs and verifies the seven-variable RSPduo drop-in
(`packaging/pi/build-pi4-image.sh:210-215`,
`packaging/pi/build-pi4-image.sh:260-273`). No additional Debian/Ubuntu-only
sample-format bug was found.

F1–F8 apply. F2 is especially exposed by the supplied Pi profile because AGC is
enabled by default. The paired-CPI queue improves A/B sample pairing but does not
make ignored gain changes safe.

### F8 — Linux config path contract disagrees with systemd sandbox (**Medium**)

The common config validator accepts any absolute `save.path` ending in `/` and
only requires replay file to be a string (`api/config-manager.js:250-258`,
`api/config-manager.js:1008-1010`). The processor systemd unit has
`ProtectSystem=strict`, `ProtectHome=yes`, and only
`ReadWritePaths=/var/lib/vectorwarp` (`contrib/systemd/vectorwarp-processor.service.in:10-30`).
Consequently `/srv/radar/` is valid configuration but unwritable in packaged
Fedora/Debian/Ubuntu/Pi operation, while a replay below a home directory is
hidden. The same paths can work in an interactive/source or macOS launch.

**Reproduction proposal.** In an installed-service fixture, validate a config
with `save.path: /srv/vectorwarp-recordings/`, create that user-writable
directory, and request recording. Compare direct processor execution with the
systemd unit; only the latter is denied. Repeat replay from the service user's
home.

**Fix.** Make deployment-aware validation constrain packaged Linux state to
approved paths and present a clear Settings error, or generate an administrator-
owned systemd drop-in granting the exact selected directory after ownership/
symlink validation. Do not broadly relax the sandbox.

## OS review 3 — macOS (**Complete**)

The macOS lifecycle creates per-user recordings/calibration directories and
rewrites the initial config to those paths (`script/vectorwarp-macos.py:332-351`).
Its supervisor tears down the partial stack and restarts the owned API/processor/
Kraken set after child failure (`script/vectorwarp-macos.py:548-580`). GPU
qualification explicitly falls back to CPU when it fails
(`docs/MACOS_STANDALONE.md:56-65`). No additional macOS-only correctness defect
was found in these paths.

F1–F7 apply unchanged. macOS does not have F8's systemd sandbox mismatch, which
is itself a portability difference. Physical macOS capture remains qualified
only for Kraken; the repository explicitly marks physical USRP/HackRF/RSPduo and
sleep/wake/endurance as unverified (`docs/MACOS_TEST_MATRIX.md:289-294`).

## Prioritized remediation

1. **P0 — event-time contract:** implement sample-derived CPI event time end to
   end and reject OPRS/ADS-B scoring without a qualified epoch (F1, F5, F7).
2. **P1 — fail closed on coherent-input changes:** RSPduo gain changes, USRP
   device-time jumps, and HackRF asymmetric loss must end the current CPI and
   recording epoch before any DSP/scoring (F2–F4).
3. **P1 — qualify USRP topology:** restrict to a proven coherent device or add
   explicit clock/time/topology and physical common-signal verification (F6).
4. **P2 — recording v2:** bind hardware, per-channel gain, conditioning,
   continuity, epoch, and uncertainty metadata to IQ segments (F7).
5. **P2 — deployment path contract:** align Linux validation/UI with systemd
   read/write/inaccessible paths and add installed-service tests (F8).

## Focused validation run

The following existing read-only tests passed on 2026-10-23:

```text
node api/oprs-event-recorder.test.js       PASS
node api/adsb-source.test.js               PASS
node api/adsb-fusion.test.js               PASS
python3 test/recording/test_loop_progress_predicate.py   6/6 PASS
PYTHONPATH=. python3 test/oprs/test_oprs.py              28/28 PASS
```

These passes establish current intended behavior; they do not cover the proposed
adversarial clock/gain/time-jump/asymmetric-drop scenarios. In particular, the
OPRS tests use already matching integer timestamps and therefore do not validate
that production capture timestamps represent RF event time.

## Investigated hypotheses not reported as defects

- HackRF needs hardware-sync mode on the device waiting for a trigger, not both;
  the current surveillance-first/reference-trigger role matches the official
  hardware-triggering procedure.
- RSPduo A/B callbacks do compare sample counter/count and publish matched pairs;
  the defect is gain-event semantics, not ordinary callback pairing.
- Replay applies shared backpressure and does not intentionally overflow one
  channel; the parity defect is missing time/hardware provenance.
- RPM/DEB release packages use their private Node runtime; source installs alone
  intentionally require system Node.
- GPU failure paths have CPU qualification/fallback and explicit telemetry; no
  silent OS-specific GPU result substitution was found.
