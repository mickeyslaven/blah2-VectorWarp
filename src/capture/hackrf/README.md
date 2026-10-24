# Dual-HackRF wiring for VectorWarp

This requires 2 HackRF units with a shared clock signal and a shared hardware trigger.

Install [VectorWarp](https://mickeyslaven.github.io/blah2-VectorWarp/#install), then follow
[Dual HackRF setup](../../../docs/SETUP.md#dual-hackrf).
Source builders can use `--backend hackrf`. The `--backend all` option includes
a locally buildable RSPduo source kit; it does not require SDRplay's SDK when
building VectorWarp.

## Wiring

- The 2 HackRF boards should be wired as per the diagram below.

## Pairing and discontinuity contract

VectorWarp stages callbacks from both radios and publishes IQ only after an
equal-sized reference/surveillance callback pair has arrived. A short callback,
two callbacks from one radio before its mate, unequal cumulative sample
progression, a FIFO drop-counter change, or insufficient space on either FIFO
invalidates the common epoch and clears both FIFOs. Capture does not attempt to
splice later USB data onto the old epoch: both radios must be stopped and
hardware-triggered again before a new epoch can qualify.

Status and recording sidecars preserve the epoch, per-radio callback/sample and
discard counts, discontinuity count, configured gains/serials, and the last
qualification reason. HackRF provides neither a hardware sample counter nor an
RF/UTC timestamp, so published event time is explicitly inexact and exact-time
ADS-B/OPRS scoring fails closed.
- Note the [official guide](https://hackrf.readthedocs.io/en/latest/hardware_triggering.html) on setting up the shared clock and hardware trigger.
![Two HackRF units with shared clock and trigger](./hackrf-blah2.png "HackRF")
