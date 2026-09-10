// Standalone, dependency-light regression of the production interpolation code.
// All expectations use the existing 10*log10(abs(z)) map display convention.
#include "data/Detection.h"
#include "data/Map.h"
#include "process/detection/Interpolate.h"
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>

namespace {
void near(double actual, double expected) {
  if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-10)
    throw std::runtime_error("actual=" + std::to_string(actual) +
                             " expected=" + std::to_string(expected));
}
Map<std::complex<double>> makeCross(bool transpose = false) {
  Map<std::complex<double>> map(3, 3);
  map.delay = {0, 1, 2}; map.doppler = {-2, 0, 2}; map.noisePower = 3;
  const double level[3][3] = {{5, 17, 5}, {10, 20, 18}, {5, 19, 5}};
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c)
    map.data[r][c] = {std::pow(10., level[transpose ? c : r][transpose ? r : c] / 10.), 0};
  return map;
}
void check(Map<std::complex<double>>& map, bool delay, bool doppler,
           double expectedSnr, double expectedDelay, double expectedDoppler) {
  const double center = 10 * std::log10(std::abs(map.data[1][1])) - map.noisePower;
  Detection input(1., 0., center);
  Interpolate interpolate(delay, doppler);
  const auto output = interpolate.process(&input, &map);
  if (output->get_nDetections() != 1) throw std::runtime_error("lost detection");
  near(output->get_snr()[0], expectedSnr);
  near(output->get_delay()[0], expectedDelay);
  near(output->get_doppler()[0], expectedDoppler);
}
}
int main() {
  try {
    // Delay peak 20+2/3 is higher than Doppler peak 20+1/8.
    // Upstream's Doppler assignment bug produces 17.125 instead of 17+2/3.
    auto map = makeCross();
    check(map, true, true, 17. + 2./3., 1. + 1./3., .5);
    check(map, true, false, 17. + 2./3., 1. + 1./3., 0);
    check(map, false, true, 17.125, 1, .5);
    check(map, false, false, 17, 1, 0);
    auto transposed = makeCross(true);
    check(transposed, true, true, 17. + 2./3., 1.25, 2./3.);
    // Noise translation changes the reported peak, not either position.
    map.noisePower += 7;
    check(map, true, true, 10. + 2./3., 1. + 1./3., .5);
    // A common complex gain changes mean-map noise and peak equally.
    map.set_metrics(); const double snr = 20. + 2./3. - map.noisePower;
    check(map, true, true, snr, 1. + 1./3., .5);
    for (auto& row : map.data) for (auto& z : row) z *= std::complex<double>(0, 100);
    map.set_metrics();
    check(map, true, true, snr, 1. + 1./3., .5);
    std::cout << "PASS: 8 interpolation SNR cases\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
  }
}
