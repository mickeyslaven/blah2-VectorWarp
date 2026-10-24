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

At startup VectorWarp resolves each configured suffix uniquely against one
libhackrf enumeration, preserves the full serial-to-role mapping, and checks all
frequency, sample-rate, per-radio LNA/VGA/amplifier, clock-output, hardware-sync
and stream-start return codes. libhackrf's sample-rate setter automatically
selects a baseband filter at most 0.75 times the rate; that derived policy is
recorded, but the exact filter and the other RF settings have no independent
readback.

VectorWarp stages callbacks from both radios and publishes IQ only after an
equal-sized reference/surveillance callback pair has arrived. A short callback,
two callbacks from one radio before its mate, unequal pair size, a FIFO
drop-counter change, or insufficient space on either FIFO invalidates the common
epoch, clears both FIFOs, and closes an active recording epoch incomplete.
Streaming then drains one callback from each radio and requires two matched
callback pairs before atomically publishing the new relative epoch. Another
fault restarts qualification; nine consecutive faults exhaust the declared
eight-retry budget and stop RX callbacks.

Status and recording sidecars preserve the epoch, per-radio callback/sample and
discard counts, drain/qualification state, discontinuity/requalification counts,
retry budget, configured gains/serials, and the last reason. This automatic
recovery is provisional: without a hardware sample counter it cannot prove that
the two post-fault callback streams have the same RF sample origin. HackRF also
provides no RF/UTC timestamp, so published event time is explicitly inexact and
exact-time ADS-B/OPRS scoring remains unavailable.
- Note the [official guide](https://hackrf.readthedocs.io/en/latest/hardware_triggering.html) on setting up the shared clock and hardware trigger.
![Two HackRF units with shared clock and trigger](./hackrf-blah2.png "HackRF")
