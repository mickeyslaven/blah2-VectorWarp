# GPU clutter path

The accelerated path preserves the existing Wiener-Hopf filter rather than
bypassing conditioning. For one CPI it shifts the common reference using the
configured signed minimum lag, computes its FFT/autocorrelation once, and
batches every surveillance channel through cross-correlation and filtering.
The configured lag interval remains half-open: `B = delayMax - delayMin`.
Circular correlations still use exactly `N` samples. The final linear
convolution may use any FFT length `L >= N + B - 1` and publishes its first `N`
samples after division by the selected `L`. The portable default chooses the
next 2/3/5-smooth length to avoid an unnecessary Bluestein transform (the
production `N=480000,B=210` case changes from the awkward `480211` exact length
to `486000`). `BLAH2_GPU_CLUTTER_PADDING=exact` retains the prior `N+B+1` path
for qualification and A/B diagnosis. This does not change the `N`-sample
circular correlation semantics.

Clutter FFT plans use VkFFT twiddle lookup tables on every device. VkFFT 1.3.4
otherwise chooses calculated FP32 twiddles on some backends and tables on
others, which is an avoidable device-dependent precision difference for deep
cancellation. `BLAH2_GPU_CLUTTER_TWIDDLES=native` restores the library default
for controlled A/B diagnosis; `lut` is the portable default. The initial memory
estimate includes an allowance for the seven plan tables. Every actual Vulkan
allocation is additionally checked against the aggregate and per-heap budgets,
including VkFFT's own LUTs, reorder/Bluestein scratch and transient uploads.

Vulkan/VkFFT performs the reference and surveillance FFTs, correlation
products, inverse correlations, padded filter FFTs, multiplication, inverse
filtering, and estimate normalization. The small complex Hermitian Cholesky factorization
and per-channel triangular solves remain FP64 CPU work between two bounded
queue submissions. Benchmark status therefore reports
`vulkan_fft+cpu_solve`, not a fully GPU-resident solve. The shared reference
factorization is reused by all channels.

Only the `B` live coefficients per channel cross the host boundary. The device
zeros the `L`-sample weight arrays before copying those compact vectors into
their prefixes. Full FP32 surveillance IQ still enters the worker, using directly
mapped Vulkan compute buffers on qualified integrated devices and staging elsewhere.
A full FP32 clutter estimate returns to the parent. After the complete estimate
and input shapes pass validation, qualified frames subtract it in the owned
FP64 surveillance block without allocating another CPI. Startup qualification
retains separate candidate/oracle storage. This avoids an unnecessary
deep-cancellation rounding step in the shader. Eliminating the
remaining input/readback process and device round trip requires a larger capture/conditioning
ownership change and is not claimed here.

Safety and selection are per stage. The first three frames compare every
filtered sample on every channel with the existing FP64 CPU filter at the same
1e-4 amplitude scale required by the complex-map contract. The next five frames
exercise the composed GPU-clutter to GPU-ambiguity path and compare its complete
complex maps against CPU-clutter plus CPU-ambiguity results. This is essential:
qualifying each stage only on CPU-owned intermediate data does not qualify their
composition. These eight startup frames qualify this instance's device and
geometry; accepted steady-state frames do not repeat CPU clutter or composed-map
oracles. The startup comparison includes residual-relative and peak limits so
strong-direct-signal cancellation cannot hide weak-channel error. Startup
accuracy rejection disables only GPU clutter and keeps the CPU-owned frame.
Rank deficiency, ill-conditioning and an unstable solve are still guarded on
every frame and select CPU clutter when GPU precision is unsuitable. Non-finite
output or a worker, protocol, or device fault disables both GPU stages; a
separate ambiguity accuracy or speed decision does not discard healthy GPU clutter.

The benchmark exports parent preparation, worker dispatch, and output
acceptance/conversion separately. `dispatch` includes shared-memory transfer,
both Vulkan submissions, and the FP64 CPU solve inside the worker; it is not a
kernel-only time. It also records whether GPU clutter and the CPU oracle ran,
plus the selected clutter backend/state, so mixed GPU-clutter/CPU-ambiguity
operation remains visible. The small FP64 solve remains normal GPU-clutter-path
CPU work, not an accuracy oracle. AUTO's sustained GPU cost selection also
remains; it compares GPU timing against the startup CPU measurement without
recomputing CPU maps. Changing signal conditions are not continuously checked
against a full CPU reference after startup.

Offline acceptance covers signed negative/zero/positive lags, `B = 1`, `B = N`,
multi-channel shared-reference equivalence, strong cancellation, changed input
after qualification, rank deficiency, invalid output/shape, explicit numerical
rejection, worker fault recovery, and atomic CPU fallback. Long steady-state
fixtures verify that neither GPU stage repeats a CPU oracle after qualification,
including when CPU ambiguity is independently selected. Physical acceptance
must still run the same FP64 oracle on every intended Vulkan device and then use
matched-IQ timing to qualify the complete path. No performance claim follows
from the implementation or mock tests alone.

The latest [physical results](benchmarks/20260911-efficiency/README.md) cover
Radeon 8060S, RTX 4050 Laptop, Intel HD 630 and AMD Polaris 12. Both clutter and
delay–Doppler ran on GPU for all 24 measured steady CPIs in every current GPU
group. The full report separates processing time, missed deadlines and stage
costs; the combined speedup does not mean every individual stage became faster.
