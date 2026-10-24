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
  std::vector<std::complex<double>> direct;
  for (const auto& position : config.positionsM)
    direct.push_back(std::polar(1.0 / std::sqrt(5.0),
      -waveNumber * position[0]));
  fusion.calibrate(direct);
  const auto result = fusion.process(maps);
  REQUIRE(std::abs(result->data[0][0]) == Catch::Approx(std::sqrt(5.0)).epsilon(1e-12));
  REQUIRE(result->data[0][0].imag() == 0.0);
}

TEST_CASE("coherent beam bank rejects mismatched map geometry")
{
  CoherentBeamBank::Config config{539000000, 12, {{0, 0, 0}, {.1, 0, 0}}};
  CoherentBeamBank fusion(config);
  Map<std::complex<double>> only(1, 1);
  REQUIRE_THROWS_AS(fusion.process({&only}), std::invalid_argument);
}

TEST_CASE("coherent beam bank removes receiver chain phase")
{
  CoherentBeamBank::Config config;
  config.frequencyHz = 539000000;
  config.beams = 8;
  config.directPathAzimuthRad = 0;
  const double radius = .193548;
  for (int channel = 0; channel < 5; ++channel)
  {
    const double angle = -2 * PI * channel / 5;
    config.positionsM.push_back({radius * std::cos(angle), radius * std::sin(angle), 0});
  }
  const double waveNumber = 2 * PI * config.frequencyHz / C;
  const double targetAzimuth = 2 * PI * 2 / config.beams;
  const std::array<double, 5> chainPhase{{0, .7, -1.1, 2.0, -.4}};
  std::vector<std::complex<double>> direct;
  std::vector<Map<std::complex<double>>> storage;
  std::vector<Map<std::complex<double>>*> maps;
  for (std::size_t channel = 0; channel < config.positionsM.size(); ++channel)
  {
    const auto& position = config.positionsM[channel];
    const double directPhase = -waveNumber * position[0] + chainPhase[channel];
    direct.push_back(std::polar(1.0 / std::sqrt(5.0), directPhase));
    storage.emplace_back(1, 1);
    const double targetPhase = -waveNumber * (position[0] * std::cos(targetAzimuth) +
      position[1] * std::sin(targetAzimuth)) + chainPhase[channel];
    storage.back().data[0][0] = std::polar(1.0, targetPhase);
  }
  for (auto& map : storage) maps.push_back(&map);

  CoherentBeamBank fusion(config);
  fusion.calibrate(direct);
  const auto result = fusion.process(maps);
  REQUIRE(std::abs(result->data[0][0]) == Catch::Approx(std::sqrt(5.0)).epsilon(1e-12));
}

TEST_CASE("coherent beam bank applies a calibrated soft direct-path null")
{
  CoherentBeamBank::Config baseline;
  baseline.frequencyHz = 539000000;
  baseline.beams = 12;
  baseline.directPathAzimuthRad = 0;
  const double radius = .193548;
  const double waveNumber = 2 * PI * baseline.frequencyHz / C;
  std::vector<std::complex<double>> direct;
  std::vector<Map<std::complex<double>>> storage;
  std::vector<Map<std::complex<double>>*> maps;
  for (int channel = 0; channel < 5; ++channel)
  {
    const double angle = -2 * PI * channel / 5;
    baseline.positionsM.push_back({radius * std::cos(angle), radius * std::sin(angle), 0});
    const double phase = -waveNumber * baseline.positionsM.back()[0];
    direct.push_back(std::polar(1.0 / std::sqrt(5.0), phase));
    storage.emplace_back(1, 1);
    storage.back().data[0][0] = std::polar(1.0, phase);
  }
  for (auto& map : storage) maps.push_back(&map);
  CoherentBeamBank unnulled(baseline);
  unnulled.calibrate(direct);
  baseline.directPathNullDb = 20;
  CoherentBeamBank nulled(baseline);
  nulled.calibrate(direct);
  REQUIRE(std::abs(nulled.process(maps)->data[0][0]) ==
    Catch::Approx(std::abs(unnulled.process(maps)->data[0][0]) * .1).epsilon(1e-11));
}

TEST_CASE("coherent beam bank uses measured target to direct ratios")
{
  CoherentBeamBank::Config config;
  config.frequencyHz = 563000000;
  config.beams = 12;
  config.directPathAzimuthRad = 0;
  const double radius = .193548;
  for (int channel = 0; channel < 5; ++channel)
  {
    const double angle = -2 * PI * channel / 5;
    config.positionsM.push_back({radius * std::cos(angle), radius * std::sin(angle), 0});
  }

  const std::size_t targetBeam = 4;
  const std::array<std::complex<double>, 5> directManifold{{
    {1.0, .2}, {.7, -.4}, {1.2, .5}, {.8, .1}, {.6, -.7}}};
  const std::array<std::complex<double>, 5> targetRatio{{
    std::polar(.7, -.4), std::polar(1.4, .8), std::polar(.9, -1.2),
    std::polar(1.1, 2.1), std::polar(.6, .3)}};
  const auto rowPhase = std::polar(1.0, 1.73);
  config.measuredResponses.resize(config.beams * config.positionsM.size());
  for (std::size_t beam = 0; beam < config.beams; ++beam)
    for (std::size_t channel = 0; channel < config.positionsM.size(); ++channel)
      config.measuredResponses[beam * config.positionsM.size() + channel] =
        directManifold[channel];
  for (std::size_t channel = 0; channel < config.positionsM.size(); ++channel)
    config.measuredResponses[targetBeam * config.positionsM.size() + channel] =
      rowPhase * directManifold[channel] * targetRatio[channel];

  std::vector<std::complex<double>> direct;
  std::vector<Map<std::complex<double>>> storage;
  std::vector<Map<std::complex<double>>*> maps;
  double rawTargetNorm = 0;
  for (std::size_t channel = 0; channel < config.positionsM.size(); ++channel)
  {
    direct.push_back(std::polar(1.0 / std::sqrt(5.0), .31 * channel));
    const auto target = direct.back() * targetRatio[channel];
    rawTargetNorm += std::norm(target);
    storage.emplace_back(1, 1);
    storage.back().data[0][0] = target;
  }
  const double targetScale = 1.0 / std::sqrt(rawTargetNorm);
  for (auto& map : storage)
  {
    map.data[0][0] *= targetScale * std::sqrt(5.0);
    maps.push_back(&map);
  }

  CoherentBeamBank fusion(config);
  fusion.calibrate(direct);
  REQUIRE(std::abs(fusion.process(maps)->data[0][0]) ==
    Catch::Approx(std::sqrt(5.0)).epsilon(1e-12));
}

TEST_CASE("coherent beam bank rejects a mismatched measured manifold")
{
  CoherentBeamBank::Config config{563000000, 12,
    {{0, 0, 0}, {.1, 0, 0}}, 0, 0, {{1, 0}}};
  REQUIRE_THROWS_AS(CoherentBeamBank(config), std::invalid_argument);
}

TEST_CASE("measured coherent beam bank preserves the soft direct-path null")
{
  CoherentBeamBank::Config config;
  config.frequencyHz = 563000000;
  config.beams = 12;
  config.directPathNullDb = 20;
  config.positionsM = {{0, 0, 0}, {.1, 0, 0}, {0, .1, 0}};
  const std::array<std::complex<double>, 3> measuredDirect{{
    {1, .2}, {.7, -.4}, {.8, .6}}};
  config.measuredResponses.resize(config.beams * config.positionsM.size());
  for (std::size_t beam = 0; beam < config.beams; ++beam)
    for (std::size_t channel = 0; channel < config.positionsM.size(); ++channel)
      config.measuredResponses[beam * config.positionsM.size() + channel] =
        std::polar(1.0, .17 * beam) * measuredDirect[channel];

  std::vector<std::complex<double>> direct;
  std::vector<Map<std::complex<double>>> storage;
  std::vector<Map<std::complex<double>>*> maps;
  for (std::size_t channel = 0; channel < config.positionsM.size(); ++channel)
  {
    direct.push_back(std::polar(1.0 / std::sqrt(3.0), .4 * channel));
    storage.emplace_back(1, 1);
    storage.back().data[0][0] = direct.back() * std::sqrt(3.0);
  }
  for (auto& map : storage) maps.push_back(&map);

  CoherentBeamBank fusion(config);
  fusion.calibrate(direct);
  REQUIRE(std::abs(fusion.process(maps)->data[0][0]) ==
    Catch::Approx(std::sqrt(3.0) * .1).epsilon(1e-11));
}
