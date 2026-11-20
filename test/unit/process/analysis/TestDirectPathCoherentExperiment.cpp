#include "process/integration/DirectPathCoherentExperiment.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>

namespace {
using Experiment = blah2::integration::DirectPathCoherentExperiment;
using Complex = Experiment::Complex;
Experiment::Config experimentConfig() {
  Experiment::Config config;
  config.channelIds = {7, 2}; config.anchorChannelId = 7;
  config.spatialCoefficients = {1 / std::sqrt(2.0), Complex{0, -1 / std::sqrt(2.0)}};
  config.temporal.delayMotionBins = config.temporal.dopplerMotionBins = 0;
  config.temporal.minimumCoherence = .8;
  return config;
}
Experiment::Frame lineage(uint64_t sequence) {
  return {3, sequence, (sequence - 1) * 8, sequence * 8, 527000000, 9, 80, true, true};
}
Experiment::ComplexMap oneCell(double doppler = 0) {
  Experiment::ComplexMap map(1, 1); map.delay = {0}; map.doppler = {doppler}; return map;
}
std::optional<Experiment::Pilot> pilot(const Experiment::Frame& frame, double phase) {
  std::vector<Complex> reference(8, std::polar(1.0, -phase)), anchor(8, Complex{1, 0});
  return Experiment::estimatePilot(reference.data(), anchor.data(), anchor.size(), frame, 7);
}
}

TEST_CASE("direct-path experiment preserves spatial and temporal coherent gain", "[direct-path-experiment]") {
  Experiment engine(experimentConfig(), true);
  auto first = oneCell(2.5), second = oneCell(2.5), fallback = oneCell(2.5);
  fallback.data[0][0] = {13, 0};
  Experiment::Result result;
  constexpr double tau = 6.2831853071795864769;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    const double referenceGauge = .37 * double(sequence * sequence);
    const auto signal = std::polar(1.0, tau * 2.5 * .1 * double(sequence - 1) + referenceGauge);
    first.data[0][0] = signal; second.data[0][0] = Complex{0, 1} * signal;
    const auto frame = lineage(sequence);
    result = engine.process({&first, &second}, {7, 2}, frame, pilot(frame, referenceGauge), fallback);
  }
  REQUIRE(result.powerFallback == &fallback);
  REQUIRE(fallback.data[0][0] == Complex{13, 0});
  REQUIRE_FALSE(result.nullCalibrated);
  REQUIRE(result.declaredPerCellTrials == 3);
  for (std::size_t bank = 0; bank < 3; ++bank) {
    REQUIRE_FALSE(result.usePowerFallback(bank, 0));
    REQUIRE(result.banks[bank].coherence[0] == Catch::Approx(1).margin(1e-12));
    REQUIRE(std::norm(result.banks[bank].map->data[0][0]) ==
      Catch::Approx(2.0 * double(2u << bank)).margin(1e-10));
  }
  REQUIRE(result.banks[2].lineageAt(0).supportSequences() ==
    std::vector<uint64_t>{8, 7, 6, 5, 4, 3, 2, 1});
}

TEST_CASE("direct-path pilot measures reference gauge independently of eigenweights", "[direct-path-experiment]") {
  const auto frame = lineage(1);
  const auto first = pilot(frame, .2), rotated = pilot(frame, 1.1);
  REQUIRE(first); REQUIRE(rotated);
  REQUIRE(std::arg(rotated->normalizedCorrelation / first->normalizedCorrelation) ==
    Catch::Approx(.9).margin(1e-12));
  REQUIRE(rotated->coherence == Catch::Approx(1));
  auto invalidFrame = frame; invalidFrame.endSample++;
  std::vector<Complex> values(8, Complex{1, 0});
  REQUIRE_FALSE(Experiment::estimatePilot(values.data(), values.data(), 8, invalidFrame, 7));
  values[0] = {std::numeric_limits<double>::quiet_NaN(), 0};
  REQUIRE_FALSE(Experiment::estimatePilot(values.data(), values.data(), 8, frame, 7));
}

TEST_CASE("direct-path experiment falls back without bridging invalid input", "[direct-path-experiment]") {
  Experiment engine(experimentConfig(), true);
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  first.data[0][0] = 1; second.data[0][0] = Complex{0, 1}; fallback.data[0][0] = 7;
  const auto step = [&](Experiment::Frame frame, std::optional<Experiment::Pilot> anchor) {
    return engine.process({&first, &second}, {7, 2}, frame, anchor, fallback);
  };
  for (uint64_t sequence = 1; sequence <= 2; ++sequence) step(lineage(sequence), pilot(lineage(sequence), 0));
  auto weak = pilot(lineage(3), 0); weak->normalizedCorrelation = .1; weak->coherence = .01;
  auto result = step(lineage(3), weak);
  REQUIRE(result.reason == "pilot_quality"); REQUIRE(result.usePowerFallback(0, 0));
  REQUIRE(result.powerFallback == &fallback);
  REQUIRE(step(lineage(4), pilot(lineage(4), 0)).reason == "warmup");
  REQUIRE_FALSE(step(lineage(5), pilot(lineage(5), 0)).usePowerFallback(0, 0));
  auto changed = lineage(6); changed.gainCalibrationEpoch++;
  result = step(changed, pilot(changed, 0));
  REQUIRE(result.reason == "lineage_reset"); REQUIRE(result.usePowerFallback(0, 0));
  result = step(lineage(7), pilot(lineage(6), 0));
  REQUIRE(result.reason == "pilot_identity_unavailable");
  REQUIRE(result.usePowerFallback(0, 0)); REQUIRE(fallback.data[0][0] == Complex{7, 0});
}

TEST_CASE("direct-path experiment rejects changed hops and incoherent paths", "[direct-path-experiment]") {
  Experiment engine(experimentConfig(), true);
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  const auto step = [&](Experiment::Frame frame, double phase) {
    first.data[0][0] = std::polar(1.0, phase); second.data[0][0] = Complex{0, 1} * first.data[0][0];
    return engine.process({&first, &second}, {7, 2}, frame, pilot(frame, 0), fallback);
  };
  step(lineage(1), 0);
  auto result = step(lineage(2), 3.14159265358979323846);
  REQUIRE(result.banks[0].ready); REQUIRE(result.usePowerFallback(0, 0));
  auto gap = lineage(3); gap.firstSample += 8; gap.endSample += 8;
  result = step(gap, 0);
  REQUIRE(result.reason == "lineage_reset"); REQUIRE(result.usePowerFallback(0, 0));
  auto unavailable = lineage(4); unavailable.samplePositionAvailable = false;
  result = step(unavailable, 0);
  REQUIRE(result.reason == "capture_lineage_unavailable");
}

TEST_CASE("direct-path experiment bounds and trial count are explicit", "[direct-path-experiment]") {
  auto config = experimentConfig();
  REQUIRE_THROWS(Experiment(config, false));
  config.spatialCoefficients[0] = 1;
  REQUIRE_THROWS(Experiment(config, true));
  config = experimentConfig(); config.temporal.delayMotionBins = 2;
  config.temporal.dopplerMotionBins = 4; config.declaredSpatialTrials = 12;
  Experiment engine(config, true);
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  auto frame = lineage(1);
  auto result = engine.process({&first, &second}, {2, 7}, frame, pilot(frame, 0), fallback);
  REQUIRE(result.reason == "channel_identity");
  REQUIRE(result.declaredPerCellTrials == 12 * 3 * 5 * 9);
  REQUIRE_FALSE(result.nullCalibrated);
  first.data[0][0] = {std::numeric_limits<double>::infinity(), 0};
  result = engine.process({&first, &second}, {7, 2}, frame, pilot(frame, 0), fallback);
  REQUIRE(result.reason == "map_invalid");
}

TEST_CASE("direct-path experiment integrates moving complex paths", "[direct-path-experiment]") {
  auto config = experimentConfig();
  config.temporal.delayMotionBins = config.temporal.dopplerMotionBins = 1;
  Experiment engine(config, true);
  Experiment::ComplexMap first(9, 9), second(9, 9), fallback(9, 9);
  for (auto* map : {&first, &second, &fallback}) {
    for (int index = 0; index < 9; ++index) {
      map->delay.push_back(index); map->doppler.push_back(index);
    }
  }
  double phaseCycles = 0;
  Experiment::Result result;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    for (auto* map : {&first, &second})
      for (auto& row : map->data) std::fill(row.begin(), row.end(), Complex{});
    if (sequence > 1) phaseCycles += .1 * (double(sequence) - .5);
    const double gauge = -.43 * double(sequence);
    first.data[sequence][sequence] = std::polar(1.0, 6.2831853071795864769 * phaseCycles + gauge);
    second.data[sequence][sequence] = Complex{0, 1} * first.data[sequence][sequence];
    const auto frame = lineage(sequence);
    result = engine.process({&first, &second}, {7, 2}, frame, pilot(frame, gauge), fallback);
  }
  REQUIRE_FALSE(result.usePowerFallback(2, 80));
  REQUIRE(std::norm(result.banks[2].map->data[8][8]) == Catch::Approx(16).margin(1e-10));
  REQUIRE(result.banks[2].motion[80].delay == 1);
  REQUIRE(result.banks[2].motion[80].doppler == 1);
  REQUIRE(result.declaredPerCellTrials == 27);
}

TEST_CASE("fixed direct-path coherent statistic preserves independent noise scale", "[direct-path-experiment]") {
  auto config = experimentConfig(); config.temporal.minimumCoherence = 0;
  Experiment engine(config, true);
  std::mt19937 generator(9031);
  std::normal_distribution<double> normal(0, 1 / std::sqrt(2.0));
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  double total = 0;
  for (unsigned trial = 0; trial < 512; ++trial) {
    engine.reset();
    Experiment::Result result;
    for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
      first.data[0][0] = {normal(generator), normal(generator)};
      second.data[0][0] = {normal(generator), normal(generator)};
      const auto frame = lineage(sequence);
      result = engine.process({&first, &second}, {7, 2}, frame, pilot(frame, 0), fallback);
    }
    REQUIRE_FALSE(result.usePowerFallback(2, 0));
    total += std::norm(result.banks[2].map->data[0][0]);
  }
  // One fixed path and beam, independent complex Gaussian channels/CPIs.
  // This checks normalization only, not the searched detector's null tail.
  REQUIRE(total / 512 == Catch::Approx(1).margin(.15));
}

TEST_CASE("direct-path coherent accumulation scales large finite inputs", "[direct-path-experiment]") {
  auto config = experimentConfig();
  config.channelIds = {7}; config.spatialCoefficients = {1};
  Experiment engine(config, true);
  auto channel = oneCell(), fallback = oneCell();
  Experiment::Result result;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    channel.data[0][0] = {1e200, 0};
    const auto frame = lineage(sequence);
    result = engine.process({&channel}, {7}, frame, pilot(frame, 0), fallback);
  }
  REQUIRE_FALSE(result.usePowerFallback(2, 0));
  REQUIRE(std::isfinite(result.banks[2].map->data[0][0].real()));
  REQUIRE(result.banks[2].map->data[0][0].real() ==
    Catch::Approx(std::sqrt(8.0) * 1e200).epsilon(1e-12));
  REQUIRE(result.banks[2].coherence[0] == Catch::Approx(1));

  engine.reset();
  for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
    channel.data[0][0] = {1.7e308, 0};
    const auto frame = lineage(sequence);
    result = engine.process({&channel}, {7}, frame, pilot(frame, 0), fallback);
  }
  REQUIRE(result.usePowerFallback(0, 0));
  REQUIRE(result.banks[0].map->data[0][0] == Complex{});
}
