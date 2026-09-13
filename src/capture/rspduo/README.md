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
