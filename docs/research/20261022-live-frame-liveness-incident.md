# Live frame-liveness incident, 2026-10-22 UTC

## Failure chain

- At 2026-10-22 02:32:08 UTC (22:32:08 EDT), an SSH hotfix replaced `/etc/vectorwarp/config.yml` and restarted only `vectorwarp-api.service`.
- At 02:32:10 UTC, the API shutdown reset Heimdall's BLAH2 TCP output. Heimdall logged `client disconnected (Broken pipe)` and BLAH2 logged `Radar input: write: Connection reset by peer` before calling its input-stop path.
- The BLAH2 process remained alive, so systemd continued to report the processor active although no frame timestamp advanced. The direct config replacement also changed the revision without a settled receiver journal, so a later ordinary processor start was correctly blocked.
- No fresh frames were published until the ordered Heimdall -> exact receiver synchronization -> processor recovery at 03:11:37--03:13:25 UTC, about 39 minutes later.

The configuration SHA-256 before and after recovery was
`9d690eeedacb9553c92140d9a8a8cf7de0f606f6b7f9076ff17ad7801c491197`.
Readback remained 563000000 Hz, 2400000 samples/s, common gain 2, and channel
gains `[19.7,25.4,20.7,25.4,22.9]`.

## Permanent controls

- The processor unit is now ordered after and `PartOf` the API. An API restart quiesces the socket consumer first and restarts it afterward.
- A config path guard compares the saved revision with the receiver journal, stops processing on an invalid/unsettled direct write, and writes `/var/lib/vectorwarp-api/config-guard.json`.
- A 15-second frame watchdog requires an advancing, fresh `lastFrameAt`. After startup grace and confirmation it performs a bounded Heimdall -> forced exact-config synchronization -> journal settlement -> processor recovery. It allows at most two attempts per 15 minutes with a two-minute cooldown, then emits `VECTORWARP_LIVENESS_ALERT restart-loop-blocked` and fails closed. State and bounded history are in `/var/lib/vectorwarp-api/health-watchdog.json`.
- Receiver recovery re-reads the config revision before synchronization and before declaring success and compares exact requested/read-back frequency, sample rate, channel count, common gain, and channel gains.
- ADS-B acquisition now starts before the configured four-CPI/two-second association wait. Fusion admits only raw observations within an absolute one second, constant-velocity projects each source to CPI time, clusters at 0.5 km, never blends points, and retains per-source event-time/projection/disagreement provenance.

## Live acceptance

The safeguards were installed and enabled on the ATC host. A deliberate API-only
restart changed the API PID `1092651 -> 1093414` and processor PID
`1092701 -> 1093464`; the processor resumed with a 1.277-second frame age.
Subsequent recorder module and ADS-B policy reloads used the same coupling and
also resumed fresh frames. The watchdog reports `healthy`, reason `advancing`.

The first policy-valid post-fix clip is a geometry-eligible miss for DAL2416
(`a3c246`), event/pass `6af931...b0c78`, clip revision 2. It contains 24,000,000
samples per channel over five channels at 563 MHz. Across its retained truth,
maximum absolute raw observation offset is 0.901000023 s and maximum projected
multi-source disagreement is 0.0261362 km. It has zero matched detection and
tracker frames, so it is not a positive control.

Native tracker state `ASSOCIATED` means association of radar detections across
CPIs; it is not an ADS-B association. Therefore an `ASSOCIATED` track with no
ADS-B object is expected and must not be admitted as a confirmed aircraft
positive. At acceptance there were 31 native tracks, 9 in native `ASSOCIATED`
state, but zero ADS-B-matched fresh positives. Replay/echo-margin claims remain
blocked until a provenance-qualified event has nonzero strict delay/Doppler
matched frames.
