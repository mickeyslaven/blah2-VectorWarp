#include "process/fusion/BearingRefinement.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
using Oracle = blah2::fusion::BearingRefinement;
using Complex = std::complex<double>;
constexpr double twoPi = 6.283185307179586476925286766559;
std::array<double, 8> unity() { std::array<double, 8> scales{}; scales.fill(1); return scales; }
Oracle::Config config(uint32_t channels = 4, uint32_t beams = 4) {
  Oracle::Config result;
  result.channels = channels; result.beams = beams;
  result.frequencyHz = 539e6; result.radiusM = .2;
  result.elementStepRad = -twoPi / channels;
  result.referenceAnchor = 1;
  result.guardRadius = 0; result.trainingRadius = 1;
  return result;
}
std::vector<Complex> reference(uint32_t channels) {
  return std::vector<Complex>(channels, Complex(1 / std::sqrt(double(channels)), 0));
}
struct Maps {
  std::vector<Map<Complex>> storage;
  std::vector<Map<Complex>*> maps;
  Maps(uint32_t channels, uint32_t rows = 5, uint32_t columns = 5) {
    storage.reserve(channels);
    for (uint32_t channel = 0; channel < channels; ++channel) storage.emplace_back(rows, columns);
    for (auto& map : storage) {
      for (auto& row : map.data) std::fill(row.begin(), row.end(), Complex{});
      maps.push_back(&map);
    }
  }
  void cell(uint32_t row, uint32_t column, const std::vector<Complex>& values) {
    for (uint32_t channel = 0; channel < storage.size(); ++channel)
      storage[channel].data[row][column] = values.at(channel);
  }
};
void fourier4(Oracle::Config& c) {
  c.measuredResponses = {{1,0},{1,0},{1,0},{1,0},
    {1,0},{0,1},{-1,0},{0,-1}, {1,0},{-1,0},{1,0},{-1,0},
    {1,0},{0,-1},{-1,0},{0,1}};
}
uint32_t packed(uint32_t first, uint32_t second, uint32_t third, uint32_t ratio2, uint32_t ratio3) {
  return 0x80000000u | first | (second << 7) | (third << 14) | (ratio2 << 21) | (ratio3 << 26);
}
// Closed-form two-channel inverse, deliberately independent of the oracle's
// pivoted elimination. Used only on finite positive loaded covariance fixtures.
std::array<Complex, 4> inverse2(double a, Complex b, double d) {
  const double determinant = a * d - std::norm(b);
  return {d / determinant, -b / determinant, -std::conj(b) / determinant, a / determinant};
}
std::vector<double> analyticScores(const Oracle& oracle, const std::array<Complex, 4>& inverse,
    const std::array<Complex, 2>& sample) {
  std::array<Complex, 2> filtered{inverse[0] * sample[0] + inverse[1] * sample[1],
    inverse[2] * sample[0] + inverse[3] * sample[1]};
  std::vector<double> scores;
  const auto& weights = oracle.calibratedWeights();
  for (uint32_t beam = 0; beam < weights.size() / 2; ++beam) {
    const Complex a(weights[beam * 2]), b(weights[beam * 2 + 1]);
    const Complex numerator = a * filtered[0] + b * filtered[1];
    const Complex denominator = a * (inverse[0] * std::conj(a) + inverse[1] * std::conj(b)) +
      b * (inverse[2] * std::conj(a) + inverse[3] * std::conj(b));
    scores.push_back(std::norm(numerator) / std::max(std::abs(denominator.real()), 1e-12));
  }
  return scores;
}
}

TEST_CASE("finite bearing oracle returns exact independent three-peak evidence") {
  auto c = config(); fourier4(c);
  Oracle oracle(c, reference(4), unity(), false);
  Maps maps(4);
  maps.cell(2, 2, {{6,0},{2,2},{2,0},{2,-2}}); // 3*q0 + 2*q1 + q2.
  const auto result = oracle.refineAt(maps.maps, 2, 2);
  REQUIRE(result.outcome == Oracle::Outcome::valid);
  REQUIRE(result.beams == std::array<uint32_t,3>{1,2,3});
  REQUIRE(result.powers == std::array<float,3>{36,16,4});
  REQUIRE(result.spatial == packed(1,2,3,14,3));
  REQUIRE_FALSE(result.adaptiveUsed);
  REQUIRE_FALSE(result.singularFallback);
  maps.cell(2, 2, {{6e18,0},{2e18,2e18},{2e18,0},{2e18,-2e18}});
  REQUIRE(oracle.refineAt(maps.maps, 2, 2).spatial == result.spatial);
  // Origin-cell selection is independent of any later interpolation/dedup key.
  maps.cell(2, 3, {{1,0},{0,-1},{-1,0},{0,1}});
  REQUIRE(oracle.refineAt(maps.maps, 2, 3).spatial == packed(4,0,0,0,0));
}

TEST_CASE("finite bearing oracle ties and circular exclusions are deterministic") {
  auto c = config(2, 8);
  c.measuredResponses.assign(16, {1,0});
  Maps maps(2); maps.cell(2, 2, {{1,0},{1,0}});
  Oracle tied(c, reference(2), unity(), false);
  REQUIRE(tied.refineAt(maps.maps, 2, 2).spatial == packed(1,2,3,31,31));
  c.exclusionBins = 1;
  Oracle separated(c, reference(2), unity(), false);
  REQUIRE(separated.refineAt(maps.maps, 2, 2).spatial == packed(1,3,5,31,31));
  c.exclusionBins = 4;
  Oracle primaryOnly(c, reference(2), unity(), false);
  REQUIRE(primaryOnly.refineAt(maps.maps, 2, 2).spatial == packed(1,0,0,0,0));
}

TEST_CASE("finite bearing oracle independently calibrates ideal and fixed-site anchors") {
  auto c = config(3, 24); c.channelMapping = {2,0,1};
  c.directPathAzimuthRad = .37;
  const uint32_t target = 7;
  const double targetAngle = twoPi * target / c.beams;
  const double waveNumber = twoPi * c.frequencyHz / 299792458.0;
  std::vector<Complex> learned{{.2,.4},{-.7,.1},{.3,-.2}};
  for (uint32_t anchor : {0u, 1u}) {
    c.referenceAnchor = anchor;
    Maps maps(3); std::vector<Complex> source;
    for (uint32_t channel = 0; channel < 3; ++channel) {
      const double element = c.element0Rad + c.channelMapping[channel] * c.elementStepRad;
      const double targetPhase = waveNumber * c.radiusM * std::cos(element - targetAngle);
      const double directPhase = waveNumber * c.radiusM * std::cos(element - c.directPathAzimuthRad);
      source.push_back(anchor ? std::polar(1.0, -targetPhase) :
        learned[channel] * std::polar(1.0, -(targetPhase - directPhase)));
    }
    maps.cell(2, 2, source);
    Oracle oracle(c, learned, unity(), false);
    REQUIRE(oracle.refineAt(maps.maps, 2, 2).beams[0] == target + 1);
    if (anchor) {
      auto rotated = learned;
      for (auto& value : rotated) value *= std::polar(3.0, 1.2);
      Oracle unchanged(c, rotated, unity(), false);
      REQUIRE(unchanged.calibratedWeights() == oracle.calibratedWeights());
    }
  }
}

TEST_CASE("finite bearing oracle measured anchors whitening nulls and projection are calibrated") {
  auto c = config(2, 4);
  c.measuredResponses = {{2,0},{0,1}, {1,0},{1,0}, {1,0},{-1,0}, {1,0},{0,-1}};
  std::vector<Complex> learned{{0,1},{2,0}};
  c.referenceAnchor = 0;
  Oracle measured(c, learned, unity(), false);
  // Beam1 target/direct * independently learned weights = [i/2,-2i].
  const double norm = std::sqrt(.25 + 4);
  REQUIRE(measured.calibratedWeights()[2] == std::complex<float>(0,float(-.5/norm)));
  REQUIRE(measured.calibratedWeights()[3] == std::complex<float>(0,float(2/norm)));
  c.referenceAnchor = 1;
  Oracle fixed(c, learned, unity(), false);
  REQUIRE(fixed.calibratedWeights()[2] == std::complex<float>(float(1/std::sqrt(2.0)),0));
  auto scales = unity(); scales[0] = 2; scales[1] = .5;
  Oracle whitened(c, learned, scales, true);
  REQUIRE(whitened.calibratedWeights()[2].real() == float(2/std::sqrt(4.25)));
  REQUIRE(whitened.calibratedWeights()[3].real() == float(.5/std::sqrt(4.25)));
  c.directPathNullDb = 20;
  Oracle nulled(c, learned, unity(), false);
  REQUIRE(std::abs(nulled.calibratedWeights()[0]) == Catch::Approx(std::abs(fixed.calibratedWeights()[0])*.1).epsilon(1e-6));
  c.directPathNullDb = 0; c.referenceAnchor = 2;
  Oracle projected(c, {{2,0},{0,1}}, unity(), false);
  REQUIRE(projected.calibratedWeights()[0] == std::complex<float>{});
  REQUIRE(projected.calibratedWeights()[1] == std::complex<float>{});
}

TEST_CASE("finite bearing MVDR matches independent closed-form loaded covariance") {
  auto c = config(2, 4); c.method = 1; c.diagonalLoading = .1f;
  c.measuredResponses = {{1,0},{1,0}, {1,0},{-1,0}, {1,0},{0,1}, {1,0},{0,-1}};
  Maps maps(2);
  uint32_t sample = 0;
  for (uint32_t row = 1; row <= 3; ++row) for (uint32_t column = 1; column <= 3; ++column)
    if (row != 2 || column != 2) maps.cell(row, column, ++sample % 2 ?
      std::vector<Complex>{{1,0},{0,0}} : std::vector<Complex>{{0,0},{2,0}});
  maps.cell(2, 2, {{1,0},{1,0}});
  Oracle oracle(c, reference(2), unity(), false);
  const auto result = oracle.refineAt(maps.maps, 2, 2);
  REQUIRE(result.adaptiveUsed);
  REQUIRE(result.covarianceSamples == 8);
  REQUIRE(result.beams == std::array<uint32_t,3>{1,3,4});
  REQUIRE(result.spatial == packed(1,3,4,20,20));
  const auto expected = analyticScores(oracle, inverse2(.625,0,2.125), {{{1,0},{1,0}}});
  for (uint32_t peak = 0; peak < 3; ++peak)
    REQUIRE(result.powers[peak] == Catch::Approx(expected[result.beams[peak]-1]).epsilon(2e-6));
  // Raw SC16 CAFs are unnormalized sums. Scaling a well-conditioned scene
  // must not overflow squared pivots or change its dimensionless MVDR score.
  for (double scale : {1e10, 1e30}) {
    Maps scaled(2);
    for (uint32_t channel = 0; channel < 2; ++channel)
      for (uint32_t row = 0; row < 5; ++row)
        for (uint32_t column = 0; column < 5; ++column)
          scaled.storage[channel].data[row][column] = maps.storage[channel].data[row][column] * scale;
    const auto large = oracle.refineAt(scaled.maps, 2, 2);
    REQUIRE(large.outcome == Oracle::Outcome::valid);
    REQUIRE(large.adaptiveUsed);
    REQUIRE(large.spatial == result.spatial);
    for (uint32_t peak = 0; peak < 3; ++peak)
      REQUIRE(large.powers[peak] == Catch::Approx(result.powers[peak]).epsilon(2e-6));
  }
  // Rank-one training with loading exercises an actual off-diagonal row pivot.
  for (auto& map : maps.storage) for (auto& row : map.data) for (auto& value : row) value = {1,0};
  for (auto& row : maps.storage[1].data) for (auto& value : row) value = {2,0};
  maps.cell(2, 2, {{1,0},{1,0}}); c.diagonalLoading = .01f;
  Oracle pivoted(c, reference(2), unity(), false);
  const auto pivot = pivoted.refineAt(maps.maps, 2, 2);
  REQUIRE(pivot.adaptiveUsed); REQUIRE(pivot.pivotSwaps > 0);
  const auto rankOne = analyticScores(pivoted, inverse2(1.025,2,4.025), {{{1,0},{1,0}}});
  for (uint32_t peak = 0; peak < 3; ++peak)
    REQUIRE(pivot.powers[peak] == Catch::Approx(rankOne[pivot.beams[peak]-1]).epsilon(2e-5));
  for (auto& row : maps.storage[1].data) for (auto& value : row) value = {0,2};
  maps.cell(2, 2, {{1,1},{1,-1}});
  const auto complexPivot = pivoted.refineAt(maps.maps, 2, 2);
  const auto complexExpected = analyticScores(pivoted, inverse2(1.025,{0,-2},4.025), {{{1,1},{1,-1}}});
  REQUIRE(complexPivot.adaptiveUsed); REQUIRE(complexPivot.pivotSwaps > 0);
  REQUIRE(complexPivot.beams[0] == uint32_t(std::max_element(
    complexExpected.begin(), complexExpected.end()) - complexExpected.begin()) + 1);
  for (uint32_t peak = 0; peak < 3; ++peak)
    REQUIRE(complexPivot.powers[peak] == Catch::Approx(complexExpected[complexPivot.beams[peak]-1]).epsilon(2e-5));
}

TEST_CASE("finite bearing covariance ring boundaries singular fallback and invalid inputs fail closed") {
  auto c = config(); fourier4(c); c.method = 1;
  Maps maps(4); maps.cell(2, 2, {{6,0},{2,2},{2,0},{2,-2}});
  Oracle singular(c, reference(4), unity(), false);
  auto result = singular.refineAt(maps.maps, 2, 2);
  REQUIRE(result.singularFallback); REQUIRE_FALSE(result.adaptiveUsed);
  REQUIRE(result.spatial == packed(1,2,3,14,3));
  REQUIRE(singular.refineAt(maps.maps, 0, 0).covarianceSamples == 3);
  REQUIRE(singular.refineAt(maps.maps, 0, 2).covarianceSamples == 5);
  const auto nan = std::numeric_limits<double>::quiet_NaN();
  maps.storage[0].data[1][1] = {nan,0};
  result = singular.refineAt(maps.maps, 2, 2);
  REQUIRE(result.outcome == Oracle::Outcome::invalidInput); REQUIRE(result.spatial == 0);
  c.guardRadius = 1; c.trainingRadius = 2;
  Oracle guarded(c, reference(4), unity(), false);
  result = guarded.refineAt(maps.maps, 2, 2); // NaN is in the excluded guard square.
  REQUIRE(result.covarianceSamples == 16); REQUIRE(result.spatial == packed(1,2,3,14,3));
  maps.storage[0].data[2][2] = {std::numeric_limits<double>::infinity(),0};
  REQUIRE(guarded.refineAt(maps.maps, 2, 2).outcome == Oracle::Outcome::invalidInput);
  maps.storage[0].data[2][2] = {1e300,0};
  REQUIRE(guarded.refineAt(maps.maps, 2, 2).spatial == 0);
  REQUIRE_THROWS_AS(guarded.refineAt(maps.maps, 8, 0), std::out_of_range);
  REQUIRE_THROWS_AS(guarded.refineAt({}, 0, 0), std::invalid_argument);
  maps.storage[0].data[2][2] = {1,0};
  maps.storage[1].data[2].pop_back();
  REQUIRE_THROWS_AS(guarded.refineAt(maps.maps, 2, 2), std::invalid_argument);
}

TEST_CASE("finite bearing qualification refuses unbounded or malformed calibration") {
  auto c = config(); fourier4(c);
  auto bad = c; bad.channels = 9;
  REQUIRE_THROWS_AS(Oracle(bad, reference(9), unity(), false), std::invalid_argument);
  bad = c; bad.beams = 128;
  REQUIRE_THROWS_AS(Oracle(bad, reference(4), unity(), false), std::invalid_argument);
  bad = c; bad.trainingRadius = 7;
  REQUIRE_THROWS_AS(Oracle(bad, reference(4), unity(), false), std::invalid_argument);
  bad = c; bad.channelMapping = {0,1,1,3};
  REQUIRE_THROWS_AS(Oracle(bad, reference(4), unity(), false), std::invalid_argument);
  bad = c; bad.measuredResponses[0] = {std::numeric_limits<float>::quiet_NaN(),0};
  REQUIRE_THROWS_AS(Oracle(bad, reference(4), unity(), false), std::invalid_argument);
  REQUIRE_THROWS_AS(Oracle(c, std::vector<Complex>(4), unity(), false), std::invalid_argument);
  auto scales = unity(); scales[2] = std::numeric_limits<double>::infinity();
  REQUIRE_THROWS_AS(Oracle(c, reference(4), scales, true), std::invalid_argument);
}

TEST_CASE("finite bearing maximum dimensions retain bounded calibration and ring work") {
  auto c = config(8, 127); c.method = 1; c.guardRadius = 3; c.trainingRadius = 6;
  c.measuredResponses.assign(8 * 127, {1,0});
  Oracle oracle(c, reference(8), unity(), false);
  REQUIRE(oracle.calibratedWeights().size() == 8 * 127);
  Maps maps(8, 13, 13);
  maps.cell(6, 6, std::vector<Complex>(8, {1,0}));
  const auto result = oracle.refineAt(maps.maps, 6, 6);
  REQUIRE(result.covarianceSamples == 13 * 13 - 7 * 7);
  REQUIRE(result.singularFallback);
  REQUIRE(result.spatial == packed(1,2,3,31,31));
}
