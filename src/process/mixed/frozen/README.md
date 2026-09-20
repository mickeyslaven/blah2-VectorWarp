This child-only DSP snapshot comes from the recorded/live-validated
`build/pi-os/gpu-contiguous-combined-benchmark` ambiguity/Wiener sources and
`build/pi-os/gpu-blocked-full-benchmark` Vulkan correlation/FIR helpers.
It is compiled only into `blah2-mixed-worker`; the app does not link these
in-process Vulkan implementations. The worker fixes the validated
correlation68/FIR50/GPU75 shares and exact 1M/301/411/4096 geometry at startup.

MXP2 adds explicit packed `pairedFrameReference0` and
`pairedFrameReference1` operations. The shared allocation and output offset
are unchanged: the 32 MB input region still precedes the map/tail output.
For a live prepared transfer, however, only its first 8 MB contains paired
signed16 `IIQQ` samples instead of 32 MB of FP64 IQ. The worker calls
`process_borrowed_paired_i16` and decodes directly into its existing rotated-X
and straight-Y workspaces. The FP64 operation remains for ordinary replay.

The parent retains its original three-argument IQ decode independently for the
authoritative CPU fallback. `Process::prepare_paired_i16` is one-shot: a
prepared packed frame is consumed once and cannot be reused after a failed
request. Persistent worker workspaces remain, so this is not a zero-copy
claim. The rejected fused FP64-mirror method was removed; no `IqData` mirror
API remains. Child/IPC failure isolation is unchanged: the parent retains
authoritative input until it accepts a complete, finite response and can fall
back to CPU.

Before constructing `WienerHopf`, the worker requires FFTW planner threads=4.
That is required to obtain exactly two actual CPU slots (one persistent helper);
it rejects startup if the condition is not met. The reason is a confirmed
isolated-child mismatch: the old default of one silently clamped a requested
two slots to one, unlike the prototype's four-thread setup.

Keep kernel changes separate from AUTO policy work and requalify full frozen
complex maps, detector/tracker output, and short live timing after any change.
The shared `process/utility/FftwThreads.h` is an intentional exception to the
snapshot: it tracks calls to FFTW's thread setter so temporary planning scopes
can restore the configured count on FFTW 3.3.8, which has no public getter.
It changes no DSP math or class layout. Startup telemetry names that setting
`fftw_configured_threads`; the separate two-clutter-CPU-slot check still reads
the constructed worker count.
The evolving benchmark and validation record is the
[Pi mixed repair report](../../../../docs/PI_MIXED_REPAIR_20260919.md).
