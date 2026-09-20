# Pi 4 production AUTO soak — 19 September 2026

**PASS for the tested live workload.** The run completed 2400.010 measured
seconds, 4800 measured CPIs (4799 required), and 4802 observed CPIs, with zero
capture/paired-queue drops, no reported capture faults, zero final complete-CPI
backlog, and a clean receiver/process stop. The harness and wrapper exited 0;
the retained systemd journal confirms successful deactivation. The transient
unit has since been unloaded, so its current empty timestamps are not the
completion evidence.

The frozen run started at 16:23:56 UTC and finished at 17:04:07 UTC. It used
Raspberry Pi OS Lite 64-bit Bookworm on Pi 4B 8 GB, stock 1.8 GHz CPU, live
RSPduo dual channels at 2 MS/s / 551 MHz, 500 ms CPIs, the full 301×411 map,
±300 Hz Doppler, and detection and tracking enabled. Capture used the paired
queue, bulk USB, counter scale 3, and a two-CPI buffer. CPU settings were FFTW
measured plans, two clutter workers, and one OpenBLAS/OMP thread.

A later [mixed-worker repair](PI_MIXED_REPAIR_20260919.md) passed short live
comparisons with mixed selected. The binary and endurance results in this
report remain those of the earlier CPU-selected run.

## Processing time

All values are milliseconds and include the running diagnostics overhead.
Percentiles below use linear interpolation over the named population.

| Population | CPIs | Mean | p95 | p99 | Maximum | ≥500 / ≥750 / ≥1000 ms |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| All observed | 4802 | 375.924 | 391.738 | 398.919 | 871.52 | 3 / 1 / 0 |
| AUTO checking | 12 | 469.028 | 794.014 | 856.019 | 871.52 | 3 / 1 / 0 |
| AUTO ready | 4790 | 375.691 | 391.534 | 398.380 | 470.71 | 0 / 0 / 0 |

AUTO compared CPU 378.06 ms versus mixed 398.03 ms trial medians and selected
CPU. There were 4799 CPU-published frames and three mixed-published trial
frames, plus two GPU/CPU accuracy shadow comparisons. The final mixed trial
also carries `state=ready` because selection finishes on that frame; the 4789
CPU-only ready frames average 375.687 ms. No subsequent mixed activation or
fallback occurred. This qualifies production AUTO choosing CPU on this host;
it does not establish endurance for the experimental 337.377 ms in-process
mixed path or a performance benefit from the isolated worker.

The four successive 1200-CPI means were 375.827, 376.124, 375.404, and
376.332 ms. Every ready frame finished within the 500 ms CPI interval.

## Capture, spikes, and hardware

The three ≥500 ms frames were startup/qualification CPIs 1, 2, and 7:
500.99, 871.52, and 730.60 ms. CPI 2 triggered the sole diagnostic snapshot;
there were no timing-stall triggers. Its 704.69 ms clutter stage includes the
whole mixed accuracy shadow comparison. A bounded CPU profile around that
frame decoded 180 samples with no lost samples, showing parent/worker FFT,
copy, and SDRplay work. It is CPU evidence, not a GPU occupancy measurement.
No ≥750 ms ready-state event required a further profile investigation.

Maximum backlog at timing-frame checkpoints was 781260 samples per channel;
the higher-frequency asynchronous status observations reached 1353300 paired
samples during qualification. These are different observation points. The
maximum ready-frame checkpoint backlog was 63116 samples per channel, with
zero final complete-CPI backlog. The 565600-sample partial tail at normal stop
is recorded separately and is not a lost complete CPI.

Across 2390 system samples, temperature ranged 40.894–50.634°C and CPU
frequency remained 1800000 kHz. All 478 throttle readings were `0x0`.
Available memory stayed above 7142 MiB. This kernel does not expose the
`/proc/pressure` files; those fields are explicitly unavailable.

There were no kernel journal entries in the run window and no streaming SDK
timeout or overflow. Two startup SDK messages remain visible: device discovery
requested 1023 entries and was capped to 16, and the tuner-1 AM-notch operation
reported `Wrong tuner 3`. Startup was accepted and capture remained healthy;
these messages are not silently treated as resolved. The receiver reported
four gain events on A, six on B, and no overload/ACK events. This quiet run
therefore does not demonstrate that the earlier intermittent overload/ACK
storm has been fixed.

## Reproducibility and diagnostics correction

The Pi evidence directory is
`/var/tmp/vectorwarp-soaks/auto-headroom-soak-20260919/`; its manifest is
`hashes.sha256`. The application SHA-256 is
`0e642c66667c5eb84045ade4e16c0345026124962094ea3695e9acd115cdb7df`.
The frozen source/binaries and original harness are under
`/var/tmp/vectorwarp-auto-frozen-20260919T1630/`. Raw profiles remain on Pi.

The live diagnostic collector reported no errors. However, the original
wrapper's final journal exports failed because Bookworm `journalctl` rejected
its ISO timestamp. Completion review recovered both journals using explicit
epoch start/end bounds, alongside the systemd unit journal, into
`completion-review/`. The original failed exports are preserved. That
recovery closes the journal evidence gap for this run. The local wrapper was
subsequently corrected; its new version was not the one used during this soak.

The completion heartbeat was paused after review; no replacement soak was
started. Native correctness/failure tests and short live gates are summarized
in the [Pi guide](PI4_GUIDE.md). Image generation, clean-card boot and Wi-Fi
acceptance, release packaging, and Pi 5 qualification remain separate work.
