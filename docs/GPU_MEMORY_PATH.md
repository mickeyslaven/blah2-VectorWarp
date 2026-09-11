# GPU frame memory path

The ambiguity processor keeps its worker-process fault boundary and its single
batched operation for all surveillance channels. The production producer now
writes FP32 conversion and zero padding directly into the worker's sealed,
bounded shared frame. A backend that implements the optional raw-buffer
interface can consume that mapping without allocating and copying temporary
worker vectors. The base `GpuBackend` virtual table retains its vector path;
the executable, worker protocol, geometry, and loadable module are all pinned
to GPU ABI 3 and safely reject a mismatched binary. ABI 3 also pins the clutter
output as an estimated signal for FP64 parent-side subtraction.

The Vulkan backend owns persistent allocations. Its default `auto` policy uses
host-visible device-local compute buffers only when every required buffer type
is compatible, the allocations fit the per-heap quarter/2-GiB budget, and the
device reports an integrated, largest-device-heap topology. This is a
conservative indication of unified memory, not a vendor or device-ID list and
not proof of performance. `BLAH2_GPU_MEMORY_PATH=staged` retains persistent
host staging plus device-local compute buffers. `direct` permits a qualified
host-visible device-local path on other device classes, with automatic staging
fallback if compatible allocations or their heap budget are unavailable.

Direct mode removes Vulkan transfer-buffer copies; it does not import the
process-shared `memfd` as Vulkan memory. One host copy into mapped Vulkan memory
and one result copy back across the process boundary remain. On a discrete GPU
those transfers can still cross PCIe, and host-visible device-local memory can
be slower than ordinary staging (for example through a BAR). Keep staged mode
available and qualify both modes on each physical GPU before changing its
deployment setting.

Mapped allocations are persistent. Coherent types need no explicit cache
operation. Noncoherent types flush input and invalidate output only after
rounding the mapped range to `nonCoherentAtomSize`; the result invalidate
follows the compute/transfer-to-host barrier and fence wait. Frame dimensions
are checked in the parent, worker, and Vulkan backend, and total shared memory
remains capped at 512 MiB.

The module also accounts for each actual Vulkan allocation, including memory
allocated inside VkFFT for tables, scratch and temporary uploads. The aggregate
cap is the smaller of 2 GiB or a quarter of the largest device-local heap;
each heap is independently limited to the smaller of 2 GiB or a quarter of
its size. Requests are checked before allocation, and releasing buffers returns
their allowance. An over-budget clutter plan falls back to CPU clutter while
preserving the already-created ambiguity backend. Diagnostic startup logs
include peak allocated bytes and rejected requests.
Thus a 2-GiB discrete GPU retains a 512-MiB allocation allowance, while larger
devices can use up to 2 GiB. The separate 512-MiB process-shared frame cap and
all dimension/overflow checks remain unchanged.

Offline tests cover raw and legacy workers, the direct shared-frame producer,
memory-type preference, unified/discrete AUTO selection, heap rejection, atom
alignment, worker faults, and exact-frame CPU fallback. Physical acceptance
still requires numerical qualification against the FP64 CPU implementation and
matched-IQ timing of both `auto/direct` and `staged`; this document makes no
zero-copy or speedup claim.

Primary synchronization and memory references:

- <https://registry.khronos.org/vulkan/specs/latest/man/html/VkMemoryPropertyFlagBits.html>
- <https://registry.khronos.org/vulkan/specs/latest/man/html/vkFlushMappedMemoryRanges.html>
- <https://registry.khronos.org/vulkan/specs/latest/man/html/vkInvalidateMappedMemoryRanges.html>
- <https://registry.khronos.org/vulkan/specs/latest/man/html/VkPhysicalDeviceLimits.html>
