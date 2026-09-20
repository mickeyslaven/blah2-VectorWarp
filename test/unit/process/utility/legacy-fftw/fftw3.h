#pragma once

// FFTW 3.3.8's relevant public API: it deliberately has no planner getter.
#ifdef __cplusplus
extern "C" {
#endif
void fftw_plan_with_nthreads(int threads);
#ifdef __cplusplus
}
#endif
