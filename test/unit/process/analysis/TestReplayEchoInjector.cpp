#include "process/analysis/ReplayEchoInjector.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>
#include <cmath>
#include <limits>

TEST_CASE("replay echo injector adds a delayed Doppler-shifted copy")
{
  blah2::analysis::ReplayEchoInjector::Config config;
  config.enabled = true; config.amplitudeDb = -20;
  config.delayKm = 299792.458 / 1000; config.dopplerHz = 0;
  IqData iq(4); auto& data = iq.resize_for_write(4);
  data = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
  blah2::analysis::ReplayEchoInjector injector(config, 1000, 100000000, 0,
    {{{0, 0, 0}}}, true);
  injector.apply({7, 0, 4, true}, {&iq});
  REQUIRE(iq.view_data()[0].real() == Catch::Approx(1));
  REQUIRE(iq.view_data()[1].real() == Catch::Approx(2.1));
  REQUIRE(iq.view_data()[2].real() == Catch::Approx(3.2));
  REQUIRE(iq.view_data()[3].real() == Catch::Approx(4.3));
}

TEST_CASE("replay echo is independent of CPI partition with signed Doppler and raw history")
{
  for (double doppler : {-125., 125., 83.3}) {
    blah2::analysis::ReplayEchoInjector::Config config;
    config.enabled = true; config.amplitudeDb = -20;
    config.delayKm = 3 * 299792.458 / 1000; config.dopplerHz = doppler;
    const std::vector<std::array<double, 3>> geometry{{{0, 0, 0}}};
    blah2::analysis::ReplayEchoInjector whole(config, 1000, 100000000, 0, geometry, true);
    blah2::analysis::ReplayEchoInjector chunked(config, 1000, 100000000, 0, geometry, true);
    IqData input(12); auto& values = input.resize_for_write(12);
    for (unsigned i = 0; i < 12; ++i) values[i] = {double(i + 1), double(i % 3)};
    const auto raw = values;
    whole.apply({7, 100, 112, true}, {&input});
    unsigned first = 0;
    for (unsigned count : {2u, 3u, 1u, 6u}) {
      IqData block(count); auto& samples = block.resize_for_write(count);
      for (unsigned i = 0; i < count; ++i) samples[i] = raw[first + i];
      chunked.apply({7, 100 + first, 100 + first + count, true}, {&block});
      for (unsigned i = 0; i < count; ++i) {
        const unsigned at = first + i;
        auto expected = raw[at];
        if (at >= 3) expected += .1 * raw[at - 3] *
          std::polar(1., 2 * std::acos(-1.) * doppler * (100 + at) / 1000);
        REQUIRE(std::abs(block.view_data()[i] - expected) < 1e-12);
        REQUIRE(std::abs(block.view_data()[i] - input.view_data()[at]) < 1e-12);
      }
      first += count;
    }
  }
}

TEST_CASE("replay echo preserves declared relative array phase across CPI boundaries")
{
  blah2::analysis::ReplayEchoInjector::Config config;
  config.enabled = true; config.amplitudeDb = -20; config.dopplerHz = 125;
  config.delayKm = 299792.458 / 1000; config.targetAzimuthRad = std::acos(-1.) / 2;
  // One quarter wavelength along +x: direct-to-target phase contrast is +pi/2.
  blah2::analysis::ReplayEchoInjector injector(config, 1000, 100000000, 0,
    {{{0, 0, 0}}, {{299792458. / 100000000 / 4, 0, 0}}}, true);
  IqData left(2), right(2);
  for (uint64_t first : {0u, 2u}) {
    left.resize_for_write(2) = {{1, 0}, {1, 0}};
    right.resize_for_write(2) = {{1, 0}, {1, 0}};
    injector.apply({7, first, first + 2, true}, {&left, &right});
    for (unsigned index = 0; index < 2; ++index) {
      if (!first && !index) continue;
      const auto leftEcho = left.view_data()[index] - std::complex<double>(1, 0);
      const auto rightEcho = right.view_data()[index] - std::complex<double>(1, 0);
      REQUIRE(std::abs(leftEcho) == Catch::Approx(.1));
      REQUIRE(std::abs(rightEcho - leftEcho * std::complex<double>(0, 1)) < 1e-12);
    }
  }
}

TEST_CASE("muted replay warmup preserves raw delay history and exact signed onset")
{
  for (double doppler : {-125., 125.}) {
    blah2::analysis::ReplayEchoInjector::Config config;
    config.enabled = true; config.amplitudeDb = -20; config.dopplerHz = doppler;
    config.delayKm = 3 * 299792.458 / 1000;
    blah2::analysis::ReplayEchoInjector injector(config, 1000, 100000000, 0,
      {{{0, 0, 0}}}, true);
    IqData iq(4);
    for (uint64_t first : {0u, 4u, 8u}) {
      auto& raw = iq.resize_for_write(4);
      for (unsigned i = 0; i < 4; ++i) raw[i] = {double(first + i + 1), 0};
      const auto input = iq.get_data();
      injector.apply({7, first, first + 4, true}, {&iq}, first >= 4);
      for (unsigned i = 0; i < 4; ++i) {
        auto expected = input[i];
        if (first >= 4) expected += .1 * double(first + i - 2) *
          std::polar(1., 2 * std::acos(-1.) * doppler * double(first + i) / 1000);
        REQUIRE(std::abs(iq.view_data()[i] - expected) < 1e-12);
      }
    }
  }
}

TEST_CASE("replay echo resets raw history on gap epoch rewind and repeated input")
{
  blah2::analysis::ReplayEchoInjector::Config config;
  config.enabled = true; config.amplitudeDb = -20;
  config.delayKm = 299792.458 / 1000; config.dopplerHz = 125;
  for (const auto frame : {blah2::analysis::ReplayEchoInjector::Frame{7, 5, 7, true},
                          {8, 2, 4, true}, {7, 0, 2, true}}) {
    blah2::analysis::ReplayEchoInjector injector(config, 1000, 100000000, 0, {{{0, 0, 0}}}, true);
    IqData iq(2); iq.resize_for_write(2) = {{100, 0}, {200, 0}};
    injector.apply({7, 0, 2, true}, {&iq});
    iq.resize_for_write(2) = {{1, 0}, {2, 0}};
    injector.apply(frame, {&iq});
    REQUIRE(iq.view_data()[0] == std::complex<double>(1, 0));
    const auto expected = std::complex<double>(2, 0) +
      .1 * std::polar(1., 2 * std::acos(-1.) * 125 * double(frame.first + 1) / 1000);
    REQUIRE(std::abs(iq.view_data()[1] - expected) < 1e-12);
  }
}

TEST_CASE("replay echo requires bounded aligned lineage and leaves null control unchanged")
{
  blah2::analysis::ReplayEchoInjector::Config config;
  IqData iq(2); iq.resize_for_write(2) = {{1, 2}, {3, 4}};
  const auto original = iq.get_data();
  blah2::analysis::ReplayEchoInjector disabled(config, 1000, 100000000, 0, {}, true);
  disabled.apply({}, {&iq}); REQUIRE(iq.get_data() == original);
  config.enabled = true; config.dopplerHz = 250; config.amplitudeDb = -20;
  blah2::analysis::ReplayEchoInjector injector(config, 1000, 100000000, 0, {{{0, 0, 0}}}, true);
  REQUIRE_THROWS_AS(injector.apply({1, 0, 2, false}, {&iq}), std::invalid_argument);
  REQUIRE_THROWS_AS(injector.apply({1, 0, 3, true}, {&iq}), std::invalid_argument);
  REQUIRE(iq.get_data() == original);
  // Exact sample phase remains testable beyond double's exact integer range.
  const uint64_t first = (uint64_t(1) << 63) + 3;
  injector.apply({1, first, first + 2, true}, {&iq});
  REQUIRE(std::abs(iq.view_data()[0] - original[0] * std::complex<double>(1, -.1)) < 1e-12);
  REQUIRE(std::abs(iq.view_data()[1] - original[1] * 1.1) < 1e-12);
  config.delayKm = std::numeric_limits<double>::max();
  REQUIRE_THROWS_AS(blah2::analysis::ReplayEchoInjector(config, 1000, 100000000, 0,
    {{{0, 0, 0}}}, true), std::invalid_argument);
}

TEST_CASE("replay echo injector rejects live input")
{
  blah2::analysis::ReplayEchoInjector::Config config; config.enabled = true;
  REQUIRE_THROWS_AS(blah2::analysis::ReplayEchoInjector(config, 1000,
    100000000, 0, {{{0, 0, 0}}}, false), std::invalid_argument);
}

TEST_CASE("replay fractional Doppler seed is exact beyond double sample precision")
{
  for (double doppler : {-.1, .1}) {
    blah2::analysis::ReplayEchoInjector::Config config;
    config.enabled = true; config.amplitudeDb = -20; config.dopplerHz = doppler;
    blah2::analysis::ReplayEchoInjector injector(config, 131070, 100000000, 0,
      {{{0, 0, 0}}}, true);
    IqData iq(1); iq.resize_for_write(1) = {{1, .25}};
    injector.apply({9, UINT64_MAX - 1, UINT64_MAX, true}, {&iq});
    // Independent exact rational oracle: Fraction.from_float(.1) * (2^64-2) / 131070.
    const double cycles = (doppler > 0 ? 1 : -1) * .45078049897001604;
    const auto expected = std::complex<double>(1, .25) *
      (1. + .1 * std::polar(1., 2 * std::acos(-1.) * cycles));
    REQUIRE(std::abs(iq.view_data()[0] - expected) < 1e-14);
  }
}
