#include "LegacyCfar.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/IntegratedCfar.h"
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
// Deliberately retain the original complex-square operation and scalar,
// left-then-right training adds.  This independently checks the real-amplitude
// shortcut with the actual valid-cell count used by integrated CFAR.
std::unique_ptr<Detection> maskedReference(Map<std::complex<double>>& map,
    const std::vector<uint8_t>& validCells, unsigned cpis, int guard, int train,
    double pfa) {
  std::vector<double> delay, doppler, snr;
  std::vector<double> alphas(2 * train + 1,
    std::numeric_limits<double>::quiet_NaN());
  const int columns = map.get_nCols();
  for (unsigned row = 0; row < map.get_nRows(); ++row) {
    const auto valid = [&](int column) {
      return validCells.empty() || validCells[row * columns + column] != 0;
    };
    for (int column = 0; column < columns; ++column) {
      if (!valid(column)) continue;
      int count = 0;
      double sum = 0;
      for (int k = std::max(0, column - guard - train);
          k < std::max(0, column - guard); ++k) {
        if (!valid(k)) continue;
        sum += std::abs(map.data[row][k] * map.data[row][k]);
        ++count;
      }
      for (int k = std::min(columns, column + guard + 1);
          k < std::min(columns, column + guard + train + 1); ++k) {
        if (!valid(k)) continue;
        sum += std::abs(map.data[row][k] * map.data[row][k]);
        ++count;
      }
      if (!count) continue;
      double& alpha = alphas.at(count);
      if (!std::isfinite(alpha))
        alpha = blah2::detection::integrated_cfar::alpha(pfa, count, cpis);
      const auto value = map.data[row][column];
      if (std::abs(value * value) > alpha * (sum / count)) {
        delay.push_back(column + map.delay[0]);
        doppler.push_back(map.doppler[row]);
        snr.push_back(10.0 * std::log10(std::abs(value)) - map.noisePower);
      }
    }
  }
  return std::make_unique<Detection>(delay, doppler, snr);
}
size_t integratedRegression() {
  size_t cases = 0;
  for (unsigned cpis : {2, 4, 8}) for (unsigned columns : {1, 3, 33, 65}) {
    Map<std::complex<double>> map(3, columns);
    fill(map, 7419 + columns);
    const auto original = map.data;
    for (unsigned fixture = 0; fixture < 5; ++fixture) {
      map.data = original;
      for (unsigned row = 0; row < map.get_nRows(); ++row)
        for (unsigned column = 0; column < columns; ++column) {
          auto& value = map.data[row][column];
          if (fixture != 0) value = {value.real(), row % 2 ? -0.0 : 0.0};
          if (fixture == 2) {
            // Finite real inputs include underflow, overflow, and signed zero.
            const double values[] = {-0.0, 0.0, 1e-200, -1e-200,
              1e200, -1e200, 1.0, -3.0};
            value.real(values[column % 8]);
          } else if (fixture == 3 && column % 7 == 0) {
            value.real(column % 2 ? std::numeric_limits<double>::infinity() :
              std::numeric_limits<double>::quiet_NaN());
          } else if (fixture == 4) {
            value = {0.0, -0.0};
          }
        }
      for (unsigned mask = 0; mask < 4; ++mask) {
        std::vector<uint8_t> validCells;
        if (mask) validCells.resize(map.get_nRows() * columns, mask == 3 ? 0 : 1);
        if (mask == 2)
          for (size_t i = 0; i < validCells.size(); ++i)
            validCells[i] = (i % 3) != 0;
        for (const auto window : {std::pair<int, int>{0, 1}, {1, 3}, {7, 16}}) {
          CfarDetector1D detector(.001, window.first, window.second, -10, 0, cpis);
          auto expected = maskedReference(map, validCells, cpis,
            window.first, window.second, .001);
          auto actual = detector.process(&map, validCells);
          compare(*actual, *expected);
          ++cases;
        }
      }
    }
  }
  Map<std::complex<double>> threshold(1, 5);
  threshold.delay = {0, 1, 2, 3, 4}; threshold.doppler = {10};
  threshold.noisePower = 0;
  for (unsigned cpis : {2, 4, 8}) {
    for (const std::vector<uint8_t>& validCells : {
        std::vector<uint8_t>{}, {1, 1, 1, 1, 1}, {0, 1, 1, 1, 0}}) {
      const unsigned count = validCells.empty() || validCells[0] ? 4 : 2;
      const double center = std::sqrt(
        blah2::detection::integrated_cfar::alpha(.001, count, cpis));
      for (double value : {std::nextafter(center, 0.0), center,
          std::nextafter(center, std::numeric_limits<double>::infinity())}) {
        threshold.data[0] = {{1, 0}, {1, -0.0}, {value, 0}, {1, 0}, {1, 0}};
        CfarDetector1D detector(.001, 0, 2, 0, 0, cpis);
        auto expected = maskedReference(threshold, validCells, cpis, 0, 2, .001);
        auto actual = detector.process(&threshold, validCells);
        compare(*actual, *expected);
        ++cases;
      }
    }
  }
  CfarDetector1D detector(.001, 0, 2, 0, 0, 8);
  bool rejected = false;
  try { detector.process(&threshold, std::vector<uint8_t>{1}); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "CFAR accepted a malformed validity mask");
  return cases + 1;
}
void observationRegression() {
  Map<std::complex<double>> map(3,5);
  map.delay={0,1,2,3,4};map.doppler={-10,0,10};map.noisePower=0;
  for(auto& row:map.data)row={{1,0},{1,0},{100,0},{1,0},{1,0}};
  CfarDetector1D detector(.001,0,1,1,5);
  CfarDetector1D::Trials trials;
  auto observed=detector.process(&map,nullptr,nullptr,
    std::numeric_limits<std::size_t>::max(),nullptr,&trials);
  auto ordinary=detector.process(&map);compare(*observed,*ordinary);
  require(trials.evaluated==8&&trials.accepted==2&&!trials.nonfinite,
    "Observed population ignored delay/Doppler gates or pre-centroid decisions");
  trials={};uint64_t truncated=0;
  auto bounded=detector.process(&map,nullptr,nullptr,1,&truncated,&trials);
  require(bounded->get_nDetections()==1&&truncated==1&&trials.accepted==2,
    "Observed false-alarm population used truncated published candidates");
  std::vector<uint8_t> valid(15,0);valid[2]=valid[12]=1;trials={};
  detector.process(&map,valid,nullptr,nullptr,
    std::numeric_limits<std::size_t>::max(),nullptr,&trials);
  require(!trials.evaluated&&!trials.accepted,
    "CUTs with no valid training cells invented evaluated exposure");
  valid[1]=valid[11]=1;trials={};
  detector.process(&map,valid,nullptr,nullptr,
    std::numeric_limits<std::size_t>::max(),nullptr,&trials);
  require(trials.evaluated==4&&trials.accepted==2,
    "Masked CUT/training population was not measured");
  map.data[0][2]={std::numeric_limits<double>::infinity(),0};trials={};
  detector.process(&map,nullptr,nullptr,
    std::numeric_limits<std::size_t>::max(),nullptr,&trials);
  require(trials.nonfinite>0,"Nonfinite trials were silently counted as finite negatives");
}
void regression() {
  observationRegression();
  size_t cases = 0;
  for (const unsigned columns : {1, 2, 3, 7, 32, 256}) {
    Map<std::complex<double>> map(7, columns);
    fill(map, 7301 + columns);
    const auto original = map.data;
    for (int fixture = 0; fixture < 5; ++fixture) {
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
      } else if (fixture == 4) {
        for (auto& row : map.data)
          for (size_t column = 0; column < row.size(); ++column)
            row[column] = {row[column].real(), column % 2 ? -0.0 : 0.0};
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
  std::cout << "CFAR exact-output fixtures passed: " << cases + 3 +
    integratedRegression() << '\n';
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
