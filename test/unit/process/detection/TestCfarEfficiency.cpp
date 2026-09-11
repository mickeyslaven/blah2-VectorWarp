#include "LegacyCfar.h"
#include "process/detection/CfarDetector1D.h"
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
void equal(const std::vector<double>& a, const std::vector<double>& b) {
  require(a.size() == b.size(), "CFAR detection count changed");
  for (size_t i = 0; i < a.size(); ++i)
    require(std::memcmp(&a[i], &b[i], sizeof(double)) == 0,
      "CFAR detection value/order changed");
}
void compare(Detection& a, Detection& b) {
  equal(a.get_delay(), b.get_delay());
  equal(a.get_doppler(), b.get_doppler());
  equal(a.get_snr(), b.get_snr());
}
void fill(Map<std::complex<double>>& map, unsigned seed) {
  std::mt19937 random(seed);
  std::normal_distribution<double> noise;
  for (unsigned i = 0; i < map.get_nCols(); ++i) map.delay.push_back(int(i) - 10);
  for (unsigned i = 0; i < map.get_nRows(); ++i) {
    map.doppler.push_back((int(i) - int(map.get_nRows() / 2)) * 5.0);
    for (unsigned j = 0; j < map.get_nCols(); ++j) {
      const double scale = ((i * map.get_nCols() + j) % 41 == 0) ? 1e3 : 1;
      map.data[i][j] = scale * std::complex<double>(noise(random), noise(random));
    }
  }
  map.noisePower = 1.25;
}
void regression() {
  size_t cases = 0;
  for (const unsigned columns : {1, 2, 3, 7, 32, 256}) {
    Map<std::complex<double>> map(7, columns);
    fill(map, 7301 + columns);
    const auto original = map.data;
    for (int fixture = 0; fixture < 4; ++fixture) {
      map.data = original;
      if (fixture == 1) {
        for (auto& row : map.data) for (auto& sample : row) sample = {0, 0};
      } else if (fixture == 2) {
        for (auto& row : map.data) for (auto& sample : row) sample *= 1e-100;
      } else if (fixture == 3) {
        for (auto& row : map.data) {
          row[0] = {std::numeric_limits<double>::infinity(), 0};
          if (columns > 1) row[1] = {std::numeric_limits<double>::quiet_NaN(), 0};
        }
      }
      for (int guard : {0, 1, 8, 127}) for (int train : {1, 3, 127})
        for (double pfa : {1e-12, .001, .9}) for (int minDelay : {-10, 0, 100})
          for (double minDoppler : {0., 5., 20.}) {
            CfarDetector1D detector(pfa, guard, train, minDelay, minDoppler);
            auto expected = legacyCfar(map, pfa, guard, train, minDelay, minDoppler);
            auto actual = detector.process(&map);
            compare(*actual, *expected);
            ++cases;
          }
    }
  }
  // Values adjacent to the strict threshold must not change acceptance.
  Map<std::complex<double>> threshold(1, 5);
  threshold.delay = {0, 1, 2, 3, 4}; threshold.doppler = {10};
  threshold.noisePower = 0;
  const double center = std::sqrt(4.0 * (std::pow(.001, -1.0 / 4) - 1));
  for (double value : {std::nextafter(center, 0.), center,
      std::nextafter(center, std::numeric_limits<double>::infinity())}) {
    threshold.data[0] = {{1, 0}, {1, 0}, {value, 0}, {1, 0}, {1, 0}};
    CfarDetector1D detector(.001, 0, 2, 0, 0);
    auto expected = legacyCfar(threshold, .001, 0, 2, 0, 0);
    auto actual = detector.process(&threshold);
    compare(*actual, *expected);
  }
  std::cout << "CFAR exact-output fixtures passed: " << cases + 3 << '\n';
}
void benchmark() {
  using Clock = std::chrono::steady_clock;
  std::cout << "rows,delay_bins,legacy_ms,optimized_ms\n";
  for (unsigned rows : {321, 961, 1921}) {
    Map<std::complex<double>> map(rows, 256);
    fill(map, 4281);
    CfarDetector1D detector(.0001, 2, 8, -10, 0);
    double legacyMs = 0, optimizedMs = 0;
    for (unsigned repeat = 0; repeat < 8; ++repeat) {
      std::unique_ptr<Detection> legacy, optimized;
      for (unsigned order = 0; order < 2; ++order) {
        const bool baseline = ((repeat + order) % 2) == 0;
        const auto begin = Clock::now();
        if (baseline) legacy = legacyCfar(map, .0001, 2, 8, -10, 0);
        else optimized = detector.process(&map);
        const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
        if (baseline) legacyMs += elapsed;
        else optimizedMs += elapsed;
      }
      compare(*legacy, *optimized);
    }
    std::cout << rows << ",256," << legacyMs / 8 << ',' << optimizedMs / 8 << '\n';
  }
}
}
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--benchmark") benchmark();
    else regression();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
