# Pi soak spike diagnostics

The 19 September production AUTO [40-minute soak passed](PI_AUTO_SOAK_20260919.md),
including capture checks and a recovered final journal export. The measurement
starts after normal harness startup and can include AUTO qualification. The
optional diagnostics collect evidence for spikes and failed runs; they do not
change RF or processing settings or make a run pass.

Run the two harness modules from `bench/` on the Pi. Keep both
`live-rspduo-check.py` and its imported `pi_spike_diagnostics.py` beside each
other. The optional `run-pi-soak.sh` wrapper adds binary/harness hashes,
performance environment, start/finish timestamps, exit code, and kernel/SDK
journals in a new evidence directory. It converts its UTC start timestamp to
the `journalctl` `@SECONDS` form used by Debian Bookworm, records
`journal-status.txt`, and exits unsuccessfully if either final journal cannot
be collected; a successful harness code alone is then incomplete evidence. Use
it from a detached systemd unit with
`Restart=no` and a bounded runtime, so an SSH disconnect cannot end the soak.

```sh
sudo bash bench/run-pi-soak.sh /path/to/blah2 \
  /var/tmp/vectorwarp-soaks/pi40 auto 2400
```

The wrapper enables the qualified paired-CPI queue, bulk USB, sample-counter
scale 3, FFTW measured plans, two clutter workers and one OpenBLAS/OMP thread.
It records those values with the evidence. A direct Python harness invocation
does **not** supply them. Confirm the startup log reports the paired queue and
bulk USB before treating a new run as comparable. Use a matching receiver
module/capture-core build, including the diagnostic SDK event logging changes.

Diagnostics require Linux, root, and an installed `perf`; the harness refuses
to start capture if the flight recorder cannot start. Native preflight verified
the installed `perf`, the requested record flags, and `perf stat`. A native
overwrite-ring smoke check also passed: its triggered snapshot decoded with
CPU samples and DWARF stacks, with no collector errors. This verifies the
recorder mechanics, not its overhead during live processing.

The recorder is system-wide CPU sampling (`cpu-clock`, 49 Hz), not a GPU
occupancy measurement. It uses DWARF call graphs with a 2 KiB user-stack dump
and a 2 MiB overwrite ring per CPU. A 2 KiB stack may truncate a call chain.
The ring behaves as a flight recorder: old records are overwritten until a
SIGUSR2-triggered output switch preserves the current window. Linux documents
the relevant [`perf record` overwrite and output-switch behavior](https://raw.githubusercontent.com/torvalds/linux/v6.1/tools/perf/Documentation/perf-record.txt).

A snapshot is requested for a CPI at or above 750 ms or when no timing frame
arrives for 1.5 seconds. At most six snapshots are retained, with a 30-second
cooldown; a harness failure bypasses that cooldown but not the six-trigger cap.
The recorder also writes a final snapshot on exit and retains at most eight
timestamped `cpu.perf.*` files. The collector also writes
one-Hz system statistics and samples the Pi throttling flag every five seconds.
It records CPU/load, memory and pressure data, temperature, CPU frequency,
bounded process context, and recent kernel/SDRplay logs. Profiling overhead is
included in the observed CPI times.

A profiler exit, diagnostic write failure, or collector failure rejects the
soak rather than silently reporting an unprofiled pass. Six offline diagnostic
tests cover startup rejection before hardware access, recorder death, storage
errors, trigger behavior, snapshot limits, and normal-stop handling; the seven existing live-harness
test groups also pass.

Afterward, retain the evidence JSON and its `.diagnostics` directory. Report
all observed CPIs, the harness measurement interval, and post-qualification
CPIs separately. The measurement interval begins after the first accepted
timing/SDK receipt and can still include AUTO qualification; it is not itself
a steady-state filter. Report
the count of CPIs at or above 500 ms, capture drops/backlog and faults, and any
AUTO/mixed fallback state from the status records. A clean process exit,
installed `perf`, or a diagnostics directory alone is not a soak success.
