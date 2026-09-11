// Opt-in physical Vulkan smoke test: independent direct DFT/correlation oracle.
// No software-device override and no fallback path: a CPU fallback cannot pass.
#include "process/ambiguity/GpuProcess.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
int main() {
  try {
    using C = std::complex<double>;
    const blah2::GpuGeometry g{16, 9, 5, 1, -2};
    auto start = std::chrono::steady_clock::now();
    auto backend = blah2::createGpuProcess(g, "auto");
    std::cout << "device=" << backend->device().name << " startup_ms="
      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count() << std::endl;
    std::mt19937 gen(423); std::uniform_real_distribution<float> random(-1, 1);
    for (int frame = 0; frame < 3; ++frame) {
      std::vector<std::complex<float>> ref(g.range*g.doppler), surv(ref.size()), actual;
      for (auto& v : ref) v = {random(gen), random(gen)};
      for (auto& v : surv) v = {random(gen), random(gen)};
      backend->process(ref, surv, actual);
      if (actual.size() != g.delays*g.doppler) throw std::runtime_error("wrong output shape");
      double error = 0, signal = 0, peak = 0, maxError = 0;
      for (unsigned lagIndex = 0; lagIndex < g.delays; ++lagIndex) {
        int lag = int(lagIndex) + g.delayMin;
        for (unsigned k = 0; k < g.doppler; ++k) {
          C expected{};
          for (unsigned d = 0; d < g.doppler; ++d) {
            C correlation{};
            for (unsigned r = 0; r < g.range; ++r)
              correlation += C(surv[d*g.range + (r+g.range+lag)%g.range]) * std::conj(C(ref[d*g.range+r]));
            expected += correlation * std::polar(1., -2*std::acos(-1.)*k*d/g.doppler);
          }
          double e = std::norm(C(actual[lagIndex*g.doppler+k])-expected);
          error += e; signal += std::norm(expected);
          maxError = std::max(maxError, e); peak = std::max(peak, std::norm(expected));
        }
      }
      const double rms = std::sqrt(error/signal), relativePeak = std::sqrt(maxError/peak);
      if (!std::isfinite(rms) || rms > 1e-4 || relativePeak > 1e-4)
        throw std::runtime_error("GPU/DFT mismatch rms=" + std::to_string(rms));
      std::cout << "PASS gpu_frame=" << frame << " relative_rms=" << rms
                << " relative_peak=" << relativePeak << std::endl;
    }
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
