# Dual-HackRF wiring for VectorWarp

This requires 2 HackRF units with a shared clock signal and a shared hardware trigger.

Install [VectorWarp](https://mickeyslaven.github.io/blah2-VectorWarp/#install), then follow
[Dual HackRF setup](../../../docs/SETUP.md#dual-hackrf).
Source builders can use `--backend hackrf`. The `--backend all` option includes
a locally buildable RSPduo source kit; it does not require SDRplay's SDK when
building VectorWarp.

## Wiring

- The 2 HackRF boards should be wired as per the diagram below.
- Note the [official guide](https://hackrf.readthedocs.io/en/latest/hardware_triggering.html) on setting up the shared clock and hardware trigger.
![Two HackRF units with shared clock and trigger](./hackrf-blah2.png "HackRF")
