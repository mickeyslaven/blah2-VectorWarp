#include "process/ambiguity/BearingSteering.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>

namespace
{
constexpr double C = 299792458.0;
constexpr double PI = 3.1415926535897932384626433832795;
}

TEST_CASE("bearing refinement follows the ambiguity-map phase convention")
{
  constexpr uint32_t channels = 5;
  constexpr uint32_t beams = 60;
  const double radius = .193548;
  const double waveNumber = 2 * PI * 539000000.0 / C;
  const double direct = 51 * PI / 180;
  const uint32_t target = 7;
  std::array<std::complex<double>, channels> reference{}, response{};
  for (uint32_t channel = 0; channel < channels; ++channel)
  {
    const double element = -2 * PI * channel / channels;
    reference[channel] = std::polar(1.0 / std::sqrt(double(channels)), .37 * channel);
    const double targetAngle = 2 * PI * target / beams;
    const double relative = waveNumber * radius *
      (std::cos(element - targetAngle) - std::cos(element - direct));
    response[channel] = reference[channel] * std::polar(1.0, relative);
  }

  uint32_t winner = 0;
  double best = -1;
  for (uint32_t beam = 0; beam < beams; ++beam)
  {
    std::complex<double> sum{};
    const double angle = 2 * PI * beam / beams;
    for (uint32_t channel = 0; channel < channels; ++channel)
    {
      const double element = -2 * PI * channel / channels;
      const double relative = waveNumber * radius *
        (std::cos(element - angle) - std::cos(element - direct));
      sum += blah2::bearing_steering::mapCoefficient(reference[channel], relative) *
        response[channel];
    }
    if (std::norm(sum) > best) { best = std::norm(sum); winner = beam; }
  }
  REQUIRE(winner == target);
}

TEST_CASE("bearing refinement preserves an explicit physical channel mapping")
{
  const std::vector<uint32_t> mapping{2, 1, 0, 4, 3};
  for (uint32_t channel = 0; channel < mapping.size(); ++channel)
    REQUIRE(blah2::bearing_steering::physicalElement(channel, mapping) == mapping[channel]);
  REQUIRE(blah2::bearing_steering::physicalElement(3, {}) == 3);
}
