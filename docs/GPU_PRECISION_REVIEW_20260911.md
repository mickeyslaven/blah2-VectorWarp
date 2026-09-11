# GPU precision and allocation review — 2026-09-11

Source worktree: `/tmp/vectorwarp-gpu-memory-20260910.09iKbV`.
No commit, promotion, service restart or physical GPU execution was performed
by the source reviewer. Hardware acceptance is owned by the coordinating task.

## Verified precision diagnosis

VkFFT 1.3.4's Vulkan initialization sets FP32 `useLUT=-1` for NVIDIA and AMD,
and `useLUT=1` for Intel. Its explicit configuration override is applied later.
The FP32 LUT generator computes twiddles on the host with double-precision
angles before conversion to float. The seven clutter plans now explicitly
select LUTs. Ambiguity retains its existing configuration. No FP64 GPU feature
is required, and no numerical tolerance or qualification period was relaxed.

The v6 frozen source is
`/var/tmp/vectorwarp-combined-freeze-v6-20260911.bGrhgq/source.tar.gz`, SHA-256
`4de77e6f9b7ec8afa490e30c287f356fbe6f669d5b770f7d85c3071c37abcd08`.
The coordinating task reported the RTX 4050 Laptop's twenty recorded production
frames pass the unchanged `1e-4` complex-map gate with actual GPU clutter
selected. The full NVIDIA matched campaign also passed all twenty benchmark
rows. Standard two-repeat DSP means were 240.118 ms for actual upstream blah2,
239.479 ms for the fork CPU and 127.770 ms for AUTO. These are v6 measurements;
v7 still needs physical confirmation. Hardware artifacts remain under the
coordinating task's control; this report does not claim AMD/Intel acceptance.

## v7 changes after precision acceptance

The initial buffer estimate did not bound VkFFT-owned reorder scratch,
multi-upload LUTs, Bluestein data and transient upload allocations. VkFFT's
header-only Vulkan allocations now pass through the same actual-allocation
budget as application buffers. Each allocation is checked before calling the
driver. The existing aggregate cap and per-heap quarter/512-MiB caps remain.
Memory types aliasing one heap share its budget; frees return their reservation.

An unavailable clutter pipeline now returns the existing stage rejection
instead of throwing a worker failure. This preserves the usable ambiguity
pipeline when clutter allocation or construction fails. GPU ABI 3, process
protocol `0x42475003`, FFT configuration, shader arithmetic, and all accuracy
gates remain unchanged from v6.

All three v7 offline CTest suites pass (`cpuFallback`, `gpuProcessIsolation`,
`gpuClutterCpuOracle`; 1.04 seconds total). `git diff --check` also passes.
The pure memory-policy suite covers aggregate and shared-heap limits,
reservation release, overflow and invalid requests. The existing process suite
checks that a clutter rejection preserves subsequent ambiguity processing.
Source builds and tests use the existing `blah2-gpu-ubuntu` container limited to
0.5 CPU on CPUs 8/24 and 8 GiB memory, without GPU device access.

## Remaining acceptance

The v8 source follow-up scales the exact allocation ceiling to
`min(2 GiB, largest device-local heap / 4)` and applies the same quarter/2-GiB
rule to each heap. A 2-GiB GPU retains its former 512-MiB allowance. The separate
512-MiB IPC frame ceiling remains unchanged. All three offline suites pass,
including added small/large-device capacity and exact-ceiling tests. No FFT
configuration, arithmetic, numerical gate or ABI changed.

Confirm v7 on a physical device with actual GPU clutter and ambiguity selected,
all per-frame maps passing `1e-4`, and allocation diagnostics within the cap.
For a larger geometry that rejects clutter capacity, confirm GPU ambiguity
continues and the rejection is reported as fallback, not acceleration success.
Do not infer zero-copy, fully GPU-resident solving, or universal device speedup:
the small solve remains FP64 CPU work and process/device transfers remain.
