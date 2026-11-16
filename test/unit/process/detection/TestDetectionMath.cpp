/// @file TestDetectionMath.cpp
/// @brief Regression tests for centroid and interpolation boundary handling.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "data/Detection.h"
#include "data/Map.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/IntegratedCfar.h"
#include "process/detection/DopplerRidgeScrubber.h"

#include <cmath>
#include <complex>
#include <limits>
#include <vector>

namespace {

Map<std::complex<double>> makeMap() {
  Map<std::complex<double>> map(3, 3);
  map.delay = {0, 1, 2};
  map.doppler = {-2.0, 0.0, 2.0};
  map.noisePower = 0.0;
  map.data.assign(3, std::vector<std::complex<double>>(3, {10.0, 0.0}));
  return map;
}

TEST_CASE("Integrated CFAR uses the Gamma power-mean null", "[detection]")
{
  constexpr double pfa = 1e-4;
  constexpr uint32_t depth = 4;
  constexpr uint32_t training = 4;
  const double alpha = blah2::detection::integrated_cfar::alpha(
    pfa, training, depth);
  CHECK(std::abs(blah2::detection::integrated_cfar::survival(
    alpha, training, depth) - pfa) < 1e-12);

  Map<std::complex<double>> map(1, 5);
  map.delay = {0, 1, 2, 3, 4}; map.doppler = {10}; map.noisePower = 0;
  CfarDetector1D detector(pfa, 0, 2, 0, 0, depth);
  for (const double power : {alpha * (1.0 - 1e-8),
      alpha * (1.0 + 1e-8)}) {
    map.data[0] = {{1, 0}, {1, 0}, {std::sqrt(power), 0}, {1, 0}, {1, 0}};
    CHECK(detector.process(&map)->get_nDetections() ==
      (power > alpha ? 1 : 0));
  }
  CHECK_THROWS_AS(CfarDetector1D(pfa, 0, 2, 0, 0, 3),
    std::invalid_argument);
}

TEST_CASE("CFAR bounds dense diagnostic output before candidate materialization",
  "[detection]")
{
  Map<std::complex<double>> map(1, 5);
  map.delay = {0, 1, 2, 3, 4}; map.doppler = {10}; map.noisePower = 0;
  map.data[0].assign(5, {1, 0});
  CfarDetector1D detector(.9, 0, 1, 0, 0);
  uint64_t truncated = 0;
  const auto result = detector.process(&map, nullptr, nullptr, 2, &truncated);
  REQUIRE(result->get_nDetections() == 2);
  REQUIRE(truncated == 3);
}

} // namespace

TEST_CASE("CFAR includes the first valid training bin and skips empty windows", "[detection]")
{
  Map<std::complex<double>> map(1, 3);
  map.delay = {0, 1, 2}; map.doppler = {10}; map.noisePower = 0;
  map.data[0] = {{10, 0}, {2, 0}, {0, 0}};
  CfarDetector1D detector(.5, 0, 1, 1, 0);
  CHECK(detector.process(&map)->get_nDetections() == 0);
  Map<std::complex<double>> single(1, 1);
  single.delay = {1}; single.doppler = {10}; single.noisePower = 0;
  CHECK(detector.process(&single)->get_nDetections() == 0);
  CHECK_THROWS_AS(CfarDetector1D(0, 0, 1, 1, 0), std::invalid_argument);
  CHECK_THROWS_AS(CfarDetector1D(.5, 0, 0, 1, 0), std::invalid_argument);
}

TEST_CASE("Persistent broad Doppler power is learned without changing the map", "[detection]")
{
  Map<std::complex<double>> map(9, 101);
  for (int column = 0; column < 101; ++column) map.delay.push_back(column);
  for (int row = 0; row < 9; ++row) map.doppler.push_back(row - 4);
  map.data.assign(9, std::vector<std::complex<double>>(101, {1, 0}));
  const double ridgeAmplitude = std::sqrt(10.0);
  std::fill(map.data[4].begin(), map.data[4].end(),
    std::complex<double>(ridgeAmplitude, 0));
  map.data[4][30] = {std::sqrt(50.0), 0};

  DopplerRidgeScrubber scrubber({"shadow", 2, 3.0, 12.0, 0.0, 3, 2});
  CHECK(scrubber.process(&map, 1)[4][20] == 0);
  scrubber.process(&map, 1);
  map.data[4][60] = {std::sqrt(50.0), 0};
  const auto original = map.data;
  const auto& background = scrubber.process(&map, 1);
  CHECK(background[4][20] > 8.5);
  CHECK(scrubber.metrics().qualifiedRows == 1);
  CHECK(map.data == original);

  CfarDetector1D detector(.1, 0, 2, 1, 0);
  CHECK(detector.process(&map)->get_nDetections() == 2);
  const auto detections = detector.process(&map, &background);
  REQUIRE(detections->get_nDetections() == 1);
  CHECK(detections->get_delay().front() == 60);
}

TEST_CASE("Centroid suppresses a stronger neighbour across delay zero", "[detection]")
{
  Centroid centroid(2, 1, 1.0);
  Detection input({0.0, 1.0}, {0.0, 0.0}, {1.0, 2.0});

  const auto output = centroid.process(&input);

  REQUIRE(output->get_nDetections() == 1);
  CHECK(std::isfinite(output->get_delay().front()));
  CHECK(std::isfinite(output->get_doppler().front()));
  CHECK(std::isfinite(output->get_snr().front()));
  CHECK(output->get_delay().front() == 1.0);
  CHECK(output->get_snr().front() == 2.0);
}

TEST_CASE("Interpolation retains flat, edge, and unknown map detections", "[detection]")
{
  auto map = makeMap();
  Interpolate interpolate(true, true);
  Detection input({1.0, 0.0, 99.0}, {0.0, -2.0, 99.0}, {7.0, 6.0, 5.0});

  const auto output = interpolate.process(&input, &map);
  const auto delay = output->get_delay();
  const auto doppler = output->get_doppler();
  const auto snr = output->get_snr();

  REQUIRE(output->get_nDetections() == 3);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(delay[i] == input.get_delay()[i]);
    CHECK(doppler[i] == input.get_doppler()[i]);
    CHECK(snr[i] == input.get_snr()[i]);
  }
}

TEST_CASE("Doppler interpolation keeps a signed sub-bin peak and its SNR", "[detection]")
{
  auto map = makeMap();
  // The centre is the peak; unequal shoulders produce a positive sub-bin shift.
  map.data[0][1] = {10.0, 0.0};
  map.data[1][1] = {100.0, 0.0};
  map.data[2][1] = {40.0, 0.0};
  Interpolate interpolate(false, true);
  Detection input(1.0, 0.0, 0.0);

  const auto output = interpolate.process(&input, &map);

  REQUIRE(output->get_nDetections() == 1);
  CHECK(output->get_delay().front() == 1.0);
  CHECK(output->get_doppler().front() > 0.0);
  CHECK_THAT(output->get_doppler().front(), Catch::Matchers::WithinAbs(0.4307, 0.001));
  CHECK(output->get_snr().front() > 20.0);
}

TEST_CASE("Interpolation rejects a resolved non-peak but retains unusable samples", "[detection]")
{
  auto map = makeMap();
  Interpolate interpolate(true, false);
  Detection input(1.0, 0.0, 7.0);
  // A valid neighbourhood whose centre is lower than a shoulder is not a detection.
  map.data[1] = {{100.0, 0.0}, {10.0, 0.0}, {100.0, 0.0}};
  CHECK(interpolate.process(&input, &map)->get_nDetections() == 0);

  // An invalid neighbourhood cannot justify rejection or produce a NaN result.
  map.data[1][0] = {std::numeric_limits<double>::quiet_NaN(), 0.0};
  const auto output = interpolate.process(&input, &map);
  REQUIRE(output->get_nDetections() == 1);
  CHECK(std::isfinite(output->get_delay().front()));
  CHECK(std::isfinite(output->get_doppler().front()));
  CHECK(std::isfinite(output->get_snr().front()));
  CHECK(output->get_delay().front() == 1.0);
  CHECK(output->get_doppler().front() == 0.0);
  CHECK(output->get_snr().front() == 7.0);
}
