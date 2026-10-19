# Measured common gain on settings Apply

For live Kraken reception, set `capture.device.heimdall.gain_control: measured`.
Every **Apply and restart** then checks the delivered IQ stream on every capture
channel, even when the edited setting does not change frequency. The selected
numeric `heimdall.gain` is saved with the rest of the configuration. Gain remains
fixed during normal processing. This does not enable independent tuner AGC.

Existing configurations without this field retain fixed-gain behavior. Settings
offers **Automatic headroom check** and **Fixed gain**. Fixed mode is necessary
for matched illuminator experiments with a preselected common gain. Save for
later and replay never run a gain search. The automatic workflow requires the
installed Linux restart helper and refuses to interrupt a recording. A generic
restart command is insufficient: automatic mode requires the helper's root-owned
completion receipt. Automatic mode is unsupported on macOS until a trusted
restart receipt provider is implemented.

The controller first verifies the old receiver tuple and records its actual
fixed gain. After applying settings it first waits for a fresh coherent MCHQ
stream. When a receiver setting command was sent, this requires a matching-tuple
phase transition from a non-ready phase to sustained phase 4 with noise off;
noise-on alone does not count. A missed transition fails closed. This bounded
15-second readiness check prevents Suite's gain-change calibration cleanup from
overlapping a subsequent full reset. It is distinct from headroom acceptance.
The controller then explicitly requests full lag and phase calibration,
requires an acknowledgement, observes recovery and verifies
completion with the requested frequency, rate, channel count and gain. Stock
Suite may omit optional control calibration flags; missing values remain unknown
in the receipt, while malformed or failed reported flags are rejected. It then
measures a bounded fresh 0.25-second MCHQ capture in a worker thread. Every
captured packet must report phase state 4, noise off and no retuning, supplying
the stream evidence that an omitted control flag cannot provide. A second
independent capture must pass at the final gain. Measurements use separate I/Q
histograms on all channels; the worst channel determines acceptance. No antenna
orientation or channel remapping is inferred from this check.

The conservative limits are zero observed rail samples, peak magnitude no more
than 95% of a component's full scale, and 99.9th-percentile magnitude no more
than 70%. Selection moves in 1 dB Suite requests, upward only while all channels
have clear headroom (peak below 80% and 99.9th percentile below 60%). It stops
near that headroom target or just below a measured unsafe setting. An unsafe
upward trial returns to the last measured safe gain. Requests are not guaranteed
physical tuner steps because Suite does not advertise a gain table. Clipping at
minimum gain fails qualification. Sixteen trials and a 15-minute selection
deadline bound the search; up to 315 further seconds are reserved for rollback.
These finite captures cannot guarantee the absence of later overload.

While processing runs, a bounded probe checks every minute. Two consecutive
unsafe captures under the same configuration trigger the normal managed Apply
and restart path with upward adjustments disabled. Safe readings, busy settings,
configuration changes and measurement errors reset the streak. Failed backoff
remains stopped and reported, without an automatic retry loop. Fixed-gain
experiments and replay are excluded. `GET /api/receiver/gain` exposes the latest
selection and monitor state. This checks calibrated wire overload, not hidden
analog compression.

The checked signal is the **calibrated uint8 wire stream**, after Suite's
compensation and requantization. This does not independently verify analog/ADC
headroom, reference CNR, cancellation, aircraft yield or bearing accuracy. MCHQ
does not carry sample rate; the rate is verified separately using control status.
Status gains are Suite's requested common dB values, not an independent
measurement of each tuner's physical gain.

Until verification and restart, the API suppresses radar publication, external
tracker input and manifold recording. The in-process frequency shadow is paused.
The processor may still be running during the check; its transient output is not
accepted. Restart discards its old tracking/filter state. The durable receiver
journal keeps an interrupted or failed transaction interlocked. A successful
check also saves a pending-restart marker. The replacement API releases that
marker only after the helper reports completion for the saved configuration,
fresh processor status reports the matching live Kraken frequency and rate, and
a current timestamp connection delivers a frame after completion. The original
API process cannot release the marker, even for an unchanged configuration.
Clearing the marker must be saved successfully before publication resumes. An
operator must reconcile a failed check; it does not silently resume with
unverified settings.

Before saving, failures attempt to restore the previous receiver tuple and fully
recalibrate. A persistence failure triggers hardware restore only if the config
file still has its starting revision: concurrent edits and ambiguous post-rename
failures require reconciliation. A successful restore is recorded distinctly
from verified headroom. No retry races a command still in flight. Each normal
sync is bounded by 75 seconds, each full calibration by 150 seconds and each
readiness check and capture by 15 seconds. A new operation starts only if its complete timeout fits
the remaining selection budget. The browser allows 20 minutes 15 seconds plus transport
time for selection and rollback; ordinary settings transactions keep their
existing shorter timeout.

Changing the receiver endpoint or beginning from continuously varying Suite AGC
requires a separate fixed-gain apply first. Other receiver backends retain their
existing gain controls; no automatic measured-headroom capability is claimed for
RSPduo, USRP or HackRF until their measurement/readback adapters are implemented.

Tests use simulated Suite control and fragmented MCHQ streams, including real
worker-thread parsing through the API. They cover clipping, worst-channel
selection, calibration failure, stale/mismatched frames, rollback, persistence
ordering and publication interlocks. Tests do not open radio hardware.
