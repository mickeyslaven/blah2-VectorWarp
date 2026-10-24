# RSPduo adapter

Install [VectorWarp](https://mickeyslaven.github.io/blah2-VectorWarp/#install), then use the
[SDRplay setup guide](../../../docs/SDRPLAY_SETUP.md).

The adapter uses two tuners. `capture.device.gainReduction` contains exactly
two values: reference first, surveillance second. Current defaults and allowed
values are maintained in the [configuration profiles and validator](../../../api/config-manager.js)
and displayed in Settings; do not substitute older single-tuner defaults.

The [example configuration](../../../config/config.yml) shows the two-channel
format. SDRplay's API is an external, separately licensed dependency, not bundled
with VectorWarp.

`VECTORWARP_RSPDUO_USB_MODE=bulk` selects SDRplay bulk USB transport before
streaming; omit it (or use `isoch`) for the established isochronous default.
`VECTORWARP_RSPDUO_COUNTER_SCALE=3` is intentionally accepted only when the
SDK reports dual-tuner, 6 MHz ADC, 2 MS/s output, and decimation 1. It handles
the alternate divided sample counter at its 32-bit rollover without changing
the paired IQ data. `auto`, used by the Pi image profile, selects that guarded
ratio only for the matching SDK descriptors and otherwise falls back to the
normal ratio 1. Status and recordings preserve both the policy and the effective
ratio.

With `VECTORWARP_RSPDUO_CPI_QUEUE=1`, live dual-channel capture writes validated
tuner pairs into a bounded signed16 CPI queue. The processor borrows one ready
CPI at a time; paired queue capacity stays within the configured capture buffer.
This keeps packed input available to the isolated mixed worker while retaining
independent CPU recovery inputs. Without that opt-in, capture uses the existing
two `IqData` buffers. See the [Pi 4 setup profile](../../../docs/PI4_GUIDE.md)
for the tested service environment, workload and qualification limits.

Every SDRplay `GainChange` callback is a hard capture-epoch boundary. Pending
A/B pairs, FIFO samples, ready or partial paired CPIs, and a paired CPI borrowed
during the transition are discarded. Capture continues automatically: the next
matched A/B callback starts a qualified post-change CPI. If portable recording
was active, only the affected file epoch is closed as incomplete and the next
matched pair lazily starts a new file; consecutive gain events therefore do not
create empty successor files or stop the receiver. Receiver health and both
recording sidecars preserve the vendor event count, epoch, wall-clock arrival,
`gRdB`, `lnaGRdB`, and `currGain` separately for tuners A and B. The callback has
no sample number, so this provenance is explicitly `sampleAligned: false` and
remains provisional: a CPI already handed to processing before the serialized
callback boundary cannot be retroactively located against the hardware gain
transition.
