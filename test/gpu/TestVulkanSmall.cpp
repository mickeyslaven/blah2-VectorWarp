// Opt-in physical Vulkan smoke test: independent direct DFT/correlation oracle.
// No software-device override and no fallback path: a CPU fallback cannot pass.
#include "process/ambiguity/GpuProcess.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
void run(const blah2::GpuGeometry& g) {
    using C = std::complex<double>;
    auto start = std::chrono::steady_clock::now();
    auto backend = blah2::createGpuProcess(g, "auto");
    std::cout << "device=" << backend->device().name << " range_fft=" << g.range
      << " doppler_bins=" << g.doppler << " channels=" << g.channels << " startup_ms="
      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count() << std::endl;
    std::mt19937 gen(423); std::uniform_real_distribution<float> random(-1, 1);
    for (int frame = 0; frame < 12; ++frame) {
      std::vector<std::complex<float>> ref(g.range*g.doppler), surv(ref.size()*g.channels), actual;
      for (auto& v : ref) v = {random(gen), random(gen)};
      for (auto& v : surv) v = {random(gen), random(gen)};
      backend->process(ref, surv, actual);
      if (actual.size() != g.delays*g.doppler*g.channels) throw std::runtime_error("wrong output shape");
      double error = 0, signal = 0, peak = 0, maxError = 0;
      for (unsigned channel = 0; channel < g.channels; ++channel)
        for (unsigned lagIndex = 0; lagIndex < g.delays; ++lagIndex) {
          const int lag = int(lagIndex) + g.delayMin;
          std::vector<C> correlations(g.doppler);
          for (unsigned d = 0; d < g.doppler; ++d)
            for (unsigned r = 0; r < g.range; ++r)
              correlations[d] += C(surv[channel*ref.size()+d*g.range+(r+g.range+lag)%g.range]) *
                std::conj(C(ref[d*g.range+r]));
          for (unsigned k = 0; k < g.doppler; ++k) {
            C expected{};
            for (unsigned d = 0; d < g.doppler; ++d)
              expected += correlations[d] * std::polar(1., -2*std::acos(-1.)*k*d/g.doppler);
            const auto index = (channel*g.delays+lagIndex)*g.doppler+k;
            const double e = std::norm(C(actual[index])-expected);
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
}
int main() {
  try {
    for (const auto& geometry : {blah2::GpuGeometry{16, 9, 5, 1, -2},
        blah2::GpuGeometry{29, 107, 7, 2, -2}, blah2::GpuGeometry{17, 521, 3, 1, -1}})
      run(geometry);
    return 0;
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
