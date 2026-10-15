#include "process/fusion/CoherentBeamBank.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace
{
constexpr double C = 299792458.0;
constexpr double PI = 3.1415926535897932384626433832795;
}

TEST_CASE("coherent beam bank recovers array gain in a steered direction")
{
  CoherentBeamBank::Config config;
  config.frequencyHz = 539000000;
  config.beams = 12;
  const double radius = .193548;
  for (int channel = 0; channel < 5; ++channel)
  {
    const double angle = -2 * PI * channel / 5;
    config.positionsM.push_back({radius * std::cos(angle), radius * std::sin(angle), 0});
  }

  std::vector<Map<std::complex<double>>> storage;
  std::vector<Map<std::complex<double>>*> maps;
  const double targetAzimuth = 2 * PI * 3 / config.beams;
  const double waveNumber = 2 * PI * config.frequencyHz / C;
  for (const auto& position : config.positionsM)
  {
    storage.emplace_back(1, 1);
    const double phase = -waveNumber * (position[0] * std::cos(targetAzimuth) +
      position[1] * std::sin(targetAzimuth));
    storage.back().data[0][0] = std::polar(1.0, phase);
  }
  for (auto& map : storage) maps.push_back(&map);

  CoherentBeamBank fusion(config);
  const auto result = fusion.process(maps);
  REQUIRE(std::abs(result->data[0][0]) == Catch::Approx(std::sqrt(5.0)).epsilon(1e-12));
}

TEST_CASE("coherent beam bank rejects mismatched map geometry")
{
  CoherentBeamBank::Config config{539000000, 12, {{0, 0, 0}, {.1, 0, 0}}};
  CoherentBeamBank fusion(config);
  Map<std::complex<double>> only(1, 1);
  REQUIRE_THROWS_AS(fusion.process({&only}), std::invalid_argument);
}
