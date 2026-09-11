# Final v8 all-device confirmation

This is a functional confirmation of the final memory-cap build, not another
timing cohort. The final v8 source was commit `907e467`; the frozen source
archive SHA-256 is
`8ceb110d4b71854a653789696b17fd7a57a318ab58f6c27212f9899422b666c3`.

The recorded confirmation workload passed on all three physical Vulkan devices:

| Device | Vulkan diagnostic ID | Result |
| --- | --- | --- |
| NVIDIA RTX 4050 Laptop | `4318:10401:0` | Small and recorded checks passed |
| Intel HD Graphics 630 | `32902:22811:0` | Small and recorded checks passed |
| AMD Radeon 500 Series | `4098:27039:1` | Small and recorded checks passed |

The recorded checks exercised GPU clutter FFT/filtering with the small CPU FP64
coefficient solve, and GPU ambiguity/delay–Doppler frames. They confirmed that
the v8 allocation-cap change retained this path on all three devices. They do
not replace the separately reported NVIDIA, Pavilion and Strix timing cohorts,
or establish new timing figures.

The retained machine-local receipts are the NVIDIA and Pavilion confirmation
manifests. This public receipt intentionally omits private command paths and
host configuration.
