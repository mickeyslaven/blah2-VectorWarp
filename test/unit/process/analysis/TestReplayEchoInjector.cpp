#include "process/analysis/ReplayEchoInjector.h"
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <complex>

TEST_CASE("replay echo injector adds a delayed Doppler-shifted copy")
{
  blah2::analysis::ReplayEchoInjector::Config config;
  config.enabled = true; config.amplitudeDb = -20;
  config.delayKm = 299792.458 / 1000; config.dopplerHz = 0;
  IqData iq(4); auto& data = iq.resize_for_write(4);
  data = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
  blah2::analysis::ReplayEchoInjector injector(config, 1000, 100000000, 0,
    {{{0, 0, 0}}}, true);
  injector.apply(0, {&iq});
  REQUIRE(iq.view_data()[0].real() == Catch::Approx(1));
  REQUIRE(iq.view_data()[1].real() == Catch::Approx(2.1));
  REQUIRE(iq.view_data()[2].real() == Catch::Approx(3.2));
  REQUIRE(iq.view_data()[3].real() == Catch::Approx(4.3));
}

TEST_CASE("replay echo injector rejects live input")
{
  blah2::analysis::ReplayEchoInjector::Config config; config.enabled = true;
  REQUIRE_THROWS_AS(blah2::analysis::ReplayEchoInjector(config, 1000,
    100000000, 0, {{{0, 0, 0}}}, false), std::invalid_argument);
}
