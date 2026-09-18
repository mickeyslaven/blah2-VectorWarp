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
the paired IQ data.

The callbacks currently write each validated tuner pair directly into the two
existing `IqData` buffers. A bounded paired-block queue remains a possible
future throughput improvement, but it would change capture backpressure and
shutdown behavior and is deliberately outside this adapter-only update.
