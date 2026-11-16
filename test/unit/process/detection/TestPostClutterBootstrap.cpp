#include "process/detection/PostClutterBootstrap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <limits>

namespace
{
Map<std::complex<double>> bootstrapMap(uint32_t columns)
{
  Map<std::complex<double>> result(1, columns);
  result.delay = {0, int(columns) - 1};
  result.doppler = {0};
  return result;
}
}

TEST_CASE("robust post-clutter covariance clips a bounded contaminated training cell")
{
  auto first = bootstrapMap(65), second = bootstrapMap(65);
  for (uint32_t column = 0; column < 65; ++column)
  {
    first.data[0][column] = {1, 0};
    second.data[0][column] = {0, 1};
  }
  // This belongs to the CUT's training ring but is not representative clutter.
  first.data[0][20] = {100, 0};
  second.data[0][20] = {0, 100};
  first.data[0][32] = {2, 0};
  second.data[0][32] = {0, 2};
  std::vector<Map<std::complex<double>>*> maps{&first, &second};

  auto fixedConfig = blah2::detection::PostClutterBootstrap::Config{};
  fixedConfig.covariance =
    blah2::detection::PostClutterBootstrap::Covariance::shrinkage;
  const auto fixed = blah2::detection::PostClutterBootstrap(fixedConfig).process(maps);
  auto robustConfig = fixedConfig;
  robustConfig.covariance =
    blah2::detection::PostClutterBootstrap::Covariance::robustShrinkage;
  const auto robust = blah2::detection::PostClutterBootstrap(robustConfig).process(maps);

  REQUIRE(fixed.covariance == "shrinkage");
  REQUIRE(robust.covariance == "robust_shrinkage");
  REQUIRE(robust.statisticFamily[32] == 1);
  REQUIRE(std::norm(robust.map->data[0][32]) >
    3 * std::norm(fixed.map->data[0][32]));
}

TEST_CASE("robust post-clutter covariance rejects invalid robust parameters")
{
  auto config = blah2::detection::PostClutterBootstrap::Config{};
  config.covariance = blah2::detection::PostClutterBootstrap::Covariance::robustShrinkage;
  config.robustClip = .99;
  REQUIRE_THROWS_AS(blah2::detection::PostClutterBootstrap(config), std::invalid_argument);
  config.robustClip = 4;
  config.robustShrinkageFloor = .9;
  config.robustShrinkageCeiling = .1;
  REQUIRE_THROWS_AS(blah2::detection::PostClutterBootstrap(config), std::invalid_argument);
}

TEST_CASE("bootstrap covariance arms exclude guards and preserve global IQ scale")
{
  using Bootstrap = blah2::detection::PostClutterBootstrap;
  for (const auto mode : {Bootstrap::Covariance::diagonal,
      Bootstrap::Covariance::shrinkage, Bootstrap::Covariance::robustShrinkage})
  {
    auto first = bootstrapMap(65), second = bootstrapMap(65);
    for (uint32_t column = 0; column < 65; ++column)
    {
      first.data[0][column] = {1, 1};
      second.data[0][column] = {2, -3};
    }
    first.data[0][32] *= 2;
    second.data[0][32] *= 2;
    std::vector<Map<std::complex<double>>*> maps{&first, &second};
    auto config = Bootstrap::Config{}; config.covariance = mode;
    const Bootstrap bootstrap(config);
    const auto baseline = bootstrap.process(maps);
    const auto power = std::norm(baseline.map->data[0][32]);
    REQUIRE(baseline.statisticFamily[32] == 1);
    // Strong neighboring targets inside the guard cannot contaminate this
    // CUT's covariance estimate in any of the three comparison arms.
    first.data[0][31] *= 1000;
    second.data[0][33] *= 1000;
    for (auto* channel : maps)
      for (auto& value : channel->data[0]) value *= std::complex<double>{0, 1e-8};
    const auto scaled = bootstrap.process(maps);
    REQUIRE(scaled.statisticFamily[32] == 1);
    REQUIRE(std::norm(scaled.map->data[0][32]) == Catch::Approx(power).epsilon(1e-10));
    first.data[0][32] = {std::numeric_limits<double>::quiet_NaN(), 0};
    const auto invalid = bootstrap.process(maps);
    REQUIRE(invalid.statisticFamily[32] == 0);
    REQUIRE(std::norm(invalid.map->data[0][32]) == 0);
  }
}

TEST_CASE("single-channel diagonal fast path preserves the scalar covariance contract")
{
  using Bootstrap = blah2::detection::PostClutterBootstrap;
  auto map = bootstrapMap(65);
  for (uint32_t column = 0; column < 65; ++column)
    map.data[0][column] = {1.0 + .013 * column, -.4 + .007 * column};
  const auto config = Bootstrap::Config{};
  const auto result = Bootstrap(config).process({&map});
  REQUIRE(result.channels == 1);
  REQUIRE(result.trainedCells == 65);
  REQUIRE(result.fallbackCells == 0);
  REQUIRE(result.invalidCells == 0);
  for (uint32_t column = 0; column < 65; ++column)
  {
    std::complex<double> covariance{};
    uint32_t samples = 0;
    const int first = std::max<int>(0, int(column) -
      int(config.guardCells + config.trainingCells));
    const int last = std::min<int>(64, int(column) +
      int(config.guardCells + config.trainingCells));
    for (int training = first; training <= last; ++training)
      if (std::abs(training - int(column)) > int(config.guardCells))
      {
        const auto value = map.data[0][training];
        covariance += value * std::conj(value); ++samples;
      }
    covariance /= double(samples);
    const double expected = std::norm(map.data[0][column] /
      std::sqrt(covariance.real())) / (1.0 + config.diagonalLoading);
    REQUIRE(result.statisticFamily[column] == 1);
    REQUIRE(std::norm(result.map->data[0][column]) ==
      Catch::Approx(expected).epsilon(1e-14));
  }
}
