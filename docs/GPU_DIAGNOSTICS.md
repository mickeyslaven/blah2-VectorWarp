# GPU startup diagnostics

Set `BLAH2_GPU_DIAGNOSTICS=1` in an offline test process to log Vulkan
instance/device creation, buffer allocation, each VkFFT plan's geometry and
completion, custom-kernel construction, and backend readiness to stderr.
The timestamps are monotonic milliseconds, not wall-clock timestamps.
Subtract timestamps to find the startup stage that exceeded the deadline.

This flag does not change device selection, compiler options, timeouts, accuracy
checks, or safe CPU fallback. Discovery of a Vulkan device is not proof that a
particular radar geometry can start or run within its resource/deadline budget.
Startup timeout and frame-execution timeout messages identify their phase and
configured deadline separately. Loader failures include the loader's reason.

`testVulkanSmall` in the standalone `test/gpu` build is an opt-in physical-GPU
smoke test. It compares three small deterministic GPU frames against direct
CPU correlation/DFT calculations. It has no CPU fallback and cannot pass by
selecting a software Vulkan device. Passing this test does not validate larger
recorded-IQ geometries, real-time throughput, or speedup. Run it with an external
wall-time/resource/thermal guard appropriate to the host.

The standalone `test/detection` build runs `testInterpolationSnr` without
requiring the complete application dependencies. `DSP_ROOT` can select an
upstream source tree for a negative regression control. Both the standalone
and main application test builds compile the actual interpolation/data classes.

```sh
cmake -S test/detection -B build/detection-checks
cmake --build build/detection-checks --parallel 1
ctest --test-dir build/detection-checks --output-on-failure
```

After a GPU-enabled [standalone build](GPU_ACCELERATION.md#build-and-verify),
run `build/gpu-checks/bin/testVulkanSmall` only within the host's test budget.
It is deliberately not an automatic CTest test: normal CI does not have a
physical GPU. Keep the worker and Vulkan module beside the test executable.
