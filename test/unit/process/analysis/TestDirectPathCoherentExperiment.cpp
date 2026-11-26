#include "process/integration/DirectPathCoherentExperiment.h"
#include "process/integration/CoherentFamilySelector.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <random>
#include <iostream>

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
    const auto hybrid = result.hybridAt(bank, 0);
    REQUIRE(hybrid.valid); REQUIRE(hybrid.coherent);
    REQUIRE(hybrid.amplitude == result.banks[bank].map->data[0][0]);
    REQUIRE(result.noncoherent.banks[bank + 1].detectorPower[0] == 13);
  }
  REQUIRE(result.sourceCount == 8);
  REQUIRE(result.sources[0].firstSample == 56);
  REQUIRE(result.sources[7].endSample == 8);
  REQUIRE(result.banks[2].sampleRateHz == 80);
  REQUIRE(result.banks[2].firstSamples == std::array<uint64_t, 8>{56, 48, 40, 32, 24, 16, 8, 0});
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
  REQUIRE(result.hybridAt(0, 0).valid);
  REQUIRE_FALSE(result.hybridAt(0, 0).coherent);
  REQUIRE(result.hybridAt(0, 0).power == 7);
  REQUIRE(result.hybridAt(0, 0).lineage.supportSequences() == std::vector<uint64_t>{3, 2});
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

TEST_CASE("hybrid phase fallback preserves exact canonical power and source CPI deduplication",
  "[direct-path-experiment]") {
  Experiment engine(experimentConfig(), true);
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  blah2::integration::ConfirmationLedger ledger;
  Experiment::Result result;
  for (uint64_t sequence = 1; sequence <= 16; ++sequence) {
    const double phase = sequence <= 8 || sequence % 2 == 0 ? 0 : 3.14159265358979323846;
    first.data[0][0] = std::polar(1.0, phase);
    second.data[0][0] = Complex{0, 1} * first.data[0][0];
    // This exact power is lost by sqrt/norm rounding; every bank must keep it.
    const double power = std::nextafter(1.0, 2.0);
    fallback.data[0][0] = power;
    result = engine.process({&first, &second}, {7, 2}, lineage(sequence),
      pilot(lineage(sequence), 0), fallback);
    if (sequence == 8) {
      REQUIRE(result.hybridAt(2, 0).coherent);
      REQUIRE(ledger.credit("target", result.hybridAt(2, 0).lineage));
      REQUIRE_FALSE(ledger.credit("target", result.hybridAt(0, 0).lineage));
    }
    if (sequence == 9) {
      REQUIRE_FALSE(result.hybridAt(2, 0).coherent);
      REQUIRE_FALSE(ledger.credit("target", result.hybridAt(2, 0).lineage));
    }
  }
  for (std::size_t bank = 0; bank < 3; ++bank) {
    const auto cell = result.hybridAt(bank, 0);
    REQUIRE(cell.valid); REQUIRE_FALSE(cell.coherent);
    REQUIRE(cell.power == std::nextafter(1.0, 2.0));
    REQUIRE(cell.power == result.noncoherent.banks[bank + 1].detectorPower[0]);
    REQUIRE(cell.amplitude == result.noncoherent.banks[bank + 1].map->data[0][0]);
    REQUIRE(cell.lineage.depth == (2u << bank));
  }
  REQUIRE(ledger.credit("target", result.hybridAt(2, 0).lineage));
  REQUIRE_FALSE(result.nullCalibrated);
  REQUIRE_FALSE(result.hybridAt(3, 0).valid);
  REQUIRE_FALSE(result.hybridAt(0, 1).valid);
}

TEST_CASE("hybrid capture refusal cannot bridge or double count reordered frames",
  "[direct-path-experiment]") {
  auto config = experimentConfig(); config.temporal.maximumCpis = 2;
  Experiment engine(config, true);
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  first.data[0][0] = 1; second.data[0][0] = Complex{0, 1}; fallback.data[0][0] = 1;
  const auto step = [&](Experiment::Frame frame) {
    return engine.process({&first, &second}, {7, 2}, frame, pilot(frame, 0), fallback);
  };
  step(lineage(1));
  REQUIRE(step(lineage(2)).hybridAt(0, 0).valid);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto duplicate = step(lineage(2));
    REQUIRE(duplicate.reason == "capture_reordered");
    REQUIRE_FALSE(duplicate.hybridAt(0, 0).valid);
    REQUIRE(duplicate.sourceCount == 0);
  }
  REQUIRE_FALSE(step(lineage(3)).hybridAt(0, 0).valid);
  const auto recovered = step(lineage(4));
  REQUIRE(recovered.hybridAt(0, 0).lineage.supportSequences() == std::vector<uint64_t>{4, 3});
  REQUIRE(recovered.declaredPerCellTrials == 1);
  REQUIRE_FALSE(recovered.hybridAt(1, 0).valid);
  auto changed = lineage(5); changed.captureEpoch++;
  REQUIRE_FALSE(step(changed).hybridAt(0, 0).valid);
  fallback.data[0][0] = {-1, 0};
  REQUIRE(step(lineage(6)).reason == "power_lineage_or_numeric_rejection");
}

TEST_CASE("hybrid fallback obeys canonical beam compatibility and phase loss",
  "[direct-path-experiment]") {
  auto config = experimentConfig(); config.fallbackBeamCount = 4; config.fallbackBeamTolerance = 0;
  Experiment engine(config, true);
  blah2::integration::TemporalEvidence oracle({8, 0, 0, 0, 4, 0});
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    fallback.data[0][0] = double(sequence);
    std::vector<uint32_t> beams{sequence < 3 ? 1u : 2u};
    const auto frame = lineage(sequence);
    const auto expected = oracle.process(fallback, frame.captureEpoch, sequence, nullptr,
      &beams, blah2::integration::TemporalEvidence::Input::realPower);
    // Missing pilot disables only coherence; noncoherent history continues.
    const auto result = engine.process({&first, &second}, {7, 2}, frame, {}, fallback, &beams);
    for (std::size_t bank = 0; bank < 3; ++bank) {
      const auto& baseline = expected.banks[bank + 1];
      const auto selected = result.hybridAt(bank, 0);
      REQUIRE(selected.valid == bool(baseline.lineageAt(0).depth));
      if (selected.valid) {
        REQUIRE_FALSE(selected.coherent);
        REQUIRE(selected.power == baseline.detectorPower[0]);
        REQUIRE(selected.lineage.supportSequences() == baseline.lineageAt(0).supportSequences());
      }
    }
  }
}

TEST_CASE("hybrid prefers a valid fixed-spatial coherent path over an incompatible fallback beam",
  "[direct-path-experiment]") {
  auto config = experimentConfig(); config.temporal.maximumCpis = 2;
  config.fallbackBeamCount = 4; config.fallbackBeamTolerance = 0;
  Experiment engine(config, true);
  auto first = oneCell(), second = oneCell(), fallback = oneCell();
  Experiment::Result result;
  for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
    first.data[0][0] = 1; second.data[0][0] = Complex{0, 1}; fallback.data[0][0] = 3;
    // The canonical power bank must reject this beam switch, while the fixed
    // spatial phasor remains a complete coherent path.
    std::vector<uint32_t> beams{uint32_t(sequence)};
    result = engine.process({&first, &second}, {7, 2}, lineage(sequence),
      pilot(lineage(sequence), 0), fallback, &beams);
  }
  const auto cell = result.hybridAt(0, 0);
  REQUIRE(cell.valid); REQUIRE(cell.coherent);
  REQUIRE(cell.lineage.firstSamples == std::array<uint64_t, 8>{8, 0, 0, 0, 0, 0, 0, 0});
  REQUIRE(cell.lineage.endSamples == std::array<uint64_t, 8>{16, 8, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("two-CPI beam-gated fallback remains a valid declared replay plan",
  "[direct-path-experiment]") {
  auto config = experimentConfig(); config.temporal.maximumCpis = 2;
  config.fallbackBeamCount = 4; config.fallbackBeamTolerance = 0;
  REQUIRE_NOTHROW(Experiment(config, true));
}

TEST_CASE("hybrid fractional delay motion shares canonical path geometry",
  "[direct-path-experiment]") {
  auto config = experimentConfig(); config.temporal.delayDriftPerHz = -.5;
  Experiment engine(config, true);
  Experiment::ComplexMap first(1, 8), second(1, 8), fallback(1, 8);
  for (auto* map : {&first, &second, &fallback}) {
    map->delay = {0, 1, 2, 3, 4, 5, 6, 7}; map->doppler = {2};
  }
  Experiment::Result result;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    for (auto* map : {&first, &second, &fallback})
      std::fill(map->data[0].begin(), map->data[0].end(), Complex{});
    const auto column = 8 - sequence;
    first.data[0][column] = std::polar(1.0, 6.2831853071795864769 * .2 * double(sequence - 1));
    second.data[0][column] = Complex{0, 1} * first.data[0][column];
    fallback.data[0][column] = 2;
    const auto frame = lineage(sequence);
    result = engine.process({&first, &second}, {7, 2}, frame, pilot(frame, 0), fallback);
  }
  const auto selected = result.hybridAt(2, 0);
  REQUIRE(selected.valid); REQUIRE(selected.coherent);
  REQUIRE(selected.power == Catch::Approx(16).margin(1e-10));
  REQUIRE(selected.lineage.delayMotion == 0);
  REQUIRE(result.noncoherent.banks[3].detectorPower[0] == 2);
  REQUIRE_FALSE(result.hybridAt(2, 7).valid); // No complete path, not measured zero.
}

namespace {
using Selector = blah2::integration::CoherentFamilySelector;
Selector::Contract selectorContract(uint32_t fit = 128, uint32_t joint = 128, uint32_t evaluation = 256) {
  Selector::Contract c; c.experiment = experimentConfig();
  c.experiment.temporal.delayMotionBins = c.experiment.temporal.dopplerMotionBins = 1;
  c.experiment.temporal.minimumCoherence = 0; c.experiment.temporal.refineDopplerPhase = true;
  c.experiment.temporal.sourceDomain = {true, 0, 0}; c.experiment.temporal.cpiHopSeconds = .1;
  c.delay = {0, 1, 2, 3, 4}; c.doppler = {0}; c.column = 2;
  c.sampleRateHz = 80; c.cpiSamples = c.hopSamples = 8; c.rates = {0, -4, 4};
  c.fitWindows = fit; c.jointWindows = joint; c.evaluationWindows = evaluation;
  c.planSha256 = std::string(64, 'a'); c.generatorSha256 = std::string(64, 'b');
  c.fitSplitSha256 = std::string(64, 'c'); c.jointSplitSha256 = std::string(64, 'd');
  return c;
}
Selector::Observation scoreObservation(const Selector& selector, uint64_t id,
  const std::array<double, 18>& ratios) {
  Selector::Observation o; o.contractSha256 = selector.contractSha256();
  o.window = {17, id * 64, (id + 1) * 64};
  for (uint32_t i = 0; i < 18; ++i) {
    auto& f = o.families[i]; f.raw.valid = f.available = true;
    f.raw.coherent = i >= 9; f.raw.power = f.ratio = ratios[i]; f.rowMedian = 1;
    f.validCount = 5; f.rateIndex = (i % 9) / 3; f.bankIndex = i % 3;
    f.rate = std::array<double, 3>{0, -4, 4}[f.rateIndex]; f.column = 2; f.delay = 2;
    auto& l = f.raw.lineage; l.depth = uint8_t(2u << (i % 3)); l.epoch = 17; l.newestSequence = 8;
    for (uint32_t age = 0; age < l.depth; ++age) {
      l.sequences[age] = 8 - age; l.firstSamples[age] = id * 64 + (7 - age) * 8;
      l.endSamples[age] = l.firstSamples[age] + 8;
    }
  }
  return o;
}
std::array<double, 18> constantScores(double value) {
  std::array<double, 18> scores; scores.fill(value); return scores;
}
void fitSimple(Selector& selector) {
  for (uint32_t i = 0; i < 128; ++i) selector.addFit(scoreObservation(selector, i, constantScores(i + 1)));
  selector.freezeFit();
  for (uint32_t i = 0; i < 128; ++i) selector.addJoint(scoreObservation(selector, 128 + i, constantScores(i + 1)));
  selector.freezeJoint();
}
}

TEST_CASE("selector retains independent family denominators and exact canonical gain", "[coherent-selector]") {
  auto c = selectorContract();
  c.delay.clear(); for (int i = 0; i < 33; ++i) c.delay.push_back(i);
  c.doppler = {-4, -3, -2, -1, 0, 1, 2, 3, 4}; c.row = 4; c.column = 16;
  Selector selector(c);
  std::array<Experiment::Result, 3> results;
  std::array<Selector::RateInput, 3> inputs;
  std::mt19937_64 rng(0x248001);
  for (uint32_t r = 0; r < 3; ++r) {
    Experiment engine(c.experiment, true);
    Experiment::ComplexMap first(9, 33), second(9, 33), fallback(9, 33);
    for (auto* map : {&first, &second, &fallback}) { map->delay = c.delay; map->doppler = c.doppler; }
    for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
      for (uint32_t row = 0; row < 9; ++row) for (uint32_t column = 0; column < 33; ++column) {
        const double angle = double(rng() >> 11) * 0x1p-53 * 6.2831853071795864769;
        first.data[row][column] = std::polar(1 / std::sqrt(2.0), angle);
        second.data[row][column] = Complex{0, 1} * first.data[row][column]; fallback.data[row][column] = 1;
      }
      first.data[4][16] = std::sqrt(32.0); second.data[4][16] = Complex{0, 1} * std::sqrt(32.0);
      fallback.data[4][16] = 64;
      results[r] = engine.process({&first, &second}, {7, 2}, lineage(sequence), pilot(lineage(sequence), 0), fallback);
    }
    inputs[r] = {r, c.rates[r], &results[r]};
  }
  auto o = selector.readout(inputs, {3, 0, 64}, selector.contractSha256());
  for (uint32_t bank = 0; bank < 3; ++bank) {
    const auto& nc = o.families[bank]; const auto& coherent = o.families[9 + bank];
    REQUIRE(nc.available); REQUIRE(coherent.available);
    REQUIRE(nc.raw.power == 64); REQUIRE(nc.rowMedian == 1); REQUIRE(nc.ratio == 64);
    REQUIRE(coherent.raw.power / nc.raw.power == Catch::Approx(2u << bank).margin(1e-12));
    REQUIRE(coherent.rowMedian > nc.rowMedian);
    REQUIRE(coherent.ratio == coherent.raw.power / coherent.rowMedian);
    REQUIRE(coherent.raw.lineage.firstSamples[0] == 56);
    // Removing coherent availability cannot alter the independent NC scale.
    results[0].banks[bank].valid[4 * 33 + 16] = 0;
  }
  const auto missing = selector.readout(inputs, {3, 0, 64}, selector.contractSha256());
  for (uint32_t bank = 0; bank < 3; ++bank) {
    REQUIRE_FALSE(missing.families[9 + bank].available);
    REQUIRE(missing.families[bank].ratio == o.families[bank].ratio);
    REQUIRE(missing.families[bank].rowMedian == o.families[bank].rowMedian);
  }
  std::swap(inputs[0], inputs[2]);
  REQUIRE_THROWS(selector.readout(inputs, {3, 0, 64}, selector.contractSha256()));
  std::swap(inputs[0], inputs[2]); inputs[0].rate = 4;
  REQUIRE_THROWS(selector.readout(inputs, {3, 0, 64}, selector.contractSha256()));
  inputs[0].rate = 0;
  auto changed = c.experiment; changed.minimumPilotCoherence = .5;
  results[0].configurationSha256 = Experiment::configurationSha256(changed);
  REQUIRE_THROWS(selector.readout(inputs, {3, 0, 64}, selector.contractSha256()));
  results[0].configurationSha256 = Experiment::configurationSha256(c.experiment);
  ++results[1].sources[0].gainCalibrationEpoch;
  REQUIRE_THROWS(selector.readout(inputs, {3, 0, 64}, selector.contractSha256()));
  --results[1].sources[0].gainCalibrationEpoch;
  std::fill(results[0].noncoherent.banks[1].detectorPower.begin(),
    results[0].noncoherent.banks[1].detectorPower.end(), 0);
  const auto zero = selector.readout(inputs, {3, 0, 64}, selector.contractSha256());
  REQUIRE_FALSE(zero.families[0].available); REQUIRE(zero.families[0].rowMedian == 0);
}

TEST_CASE("selector refuses impossible joint tails including the 128-fit top atom", "[coherent-selector]") {
  Selector selector(selectorContract());
  for (uint32_t i = 0; i < 128; ++i) selector.addFit(scoreObservation(selector, i, constantScores(i + 1)));
  selector.freezeFit();
  for (uint32_t i = 0; i < 128; ++i)
    selector.addJoint(scoreObservation(selector, 128 + i, constantScores(i < 17 ? 1000 : 1)));
  // A=128 and 18 independent families have 1-(128/129)^18 ~=13.1%
  // top-U atom in expectation. Here 17 B endpoints force p_min=18/129,
  // even though nominal B resolution 1/129 is below alpha=.05.
  REQUIRE(1 - std::pow(128.0 / 129, 18) == Catch::Approx(.1307).margin(.001));
  REQUIRE_THROWS_WITH(selector.freezeJoint(),
    "Selector requested alpha unattainable with frozen marginal/joint rank ties");
  REQUIRE_THROWS(selector.evaluate(scoreObservation(selector, 256, constantScores(10000))));
  auto rare = selectorContract(); rare.alpha = .001; Selector noResolution(rare);
  REQUIRE_THROWS(fitSimple(noResolution));
  Selector finiteEndpoint(selectorContract());
  for (uint32_t i = 0; i < 128; ++i)
    finiteEndpoint.addFit(scoreObservation(finiteEndpoint, i, constantScores(std::numeric_limits<double>::max())));
  finiteEndpoint.freezeFit();
  for (uint32_t i = 0; i < 128; ++i)
    finiteEndpoint.addJoint(scoreObservation(finiteEndpoint, 128 + i, constantScores(std::numeric_limits<double>::max())));
  REQUIRE_THROWS(finiteEndpoint.freezeJoint()); // No finite score can outrank this atom.
}

TEST_CASE("selector preserves exact argmax lineage with conservative deterministic ties", "[coherent-selector]") {
  Selector selector(selectorContract()); fitSimple(selector);
  auto o = scoreObservation(selector, 256, constantScores(1));
  auto& best = o.families[14]; best.raw.power = best.ratio = 200;
  best.raw.lineage.delayMotion = -1; best.raw.lineage.dopplerMotion = 1; best.phaseOffsetHz = 1.25;
  const auto d = selector.evaluate(o);
  REQUIRE(d.candidateDetected); REQUIRE_FALSE(d.noncoherentDetected);
  REQUIRE(d.candidate.family.raw.coherent); REQUIRE(d.candidate.family.rateIndex == 1);
  REQUIRE(d.candidate.family.bankIndex == 2); REQUIRE(d.candidate.family.raw.lineage.depth == 8);
  REQUIRE(d.candidate.family.raw.lineage.firstSamples == best.raw.lineage.firstSamples);
  REQUIRE(d.candidate.family.raw.lineage.endSamples == best.raw.lineage.endSamples);
  REQUIRE(d.candidate.family.raw.lineage.delayMotion == -1); REQUIRE(d.candidate.family.raw.lineage.dopplerMotion == 1);
  REQUIRE(d.candidate.family.phaseOffsetHz == 1.25); REQUIRE(d.candidate.family.delay == 2);
  REQUIRE(d.candidate.family.validCount == 5); REQUIRE(d.candidate.family.rowMedian == 1);
  REQUIRE(d.declaredPerReadoutTrials == 837); REQUIRE(d.declaredNcTrials == 81);
  REQUIRE(d.availableFamilies == 18); REQUIRE(d.availableBankTrialUpperBound == 837);
  REQUIRE(d.calibrationSha256.size() == 64); REQUIRE(selector.fitSha256().size() == 64);
  const auto tied = selector.evaluate(scoreObservation(selector, 257, constantScores(200)));
  REQUIRE_FALSE(tied.candidate.family.raw.coherent); REQUIRE(tied.candidate.family.rateIndex == 0);
  REQUIRE(tied.candidate.family.bankIndex == 0); REQUIRE(tied.candidateP == tied.noncoherentP);
  // Equal-to-A-maximum retains the >= tie, giving 2/(128+1), not 1/129.
  const auto equal = selector.evaluate(scoreObservation(selector, 258, constantScores(128)));
  REQUIRE(equal.candidate.marginalP == Catch::Approx(2.0 / 129));
  auto ncWins = scoreObservation(selector, 260, constantScores(1));
  ncWins.families[2].ratio = ncWins.families[2].raw.power = 120;
  ncWins.families[9].ratio = 100; ncWins.families[9].raw.power = 10000; ncWins.families[9].rowMedian = 100;
  const auto strongNc = selector.evaluate(ncWins);
  REQUIRE_FALSE(strongNc.candidate.family.raw.coherent);
  REQUIRE(strongNc.candidate.family.bankIndex == 2);
  REQUIRE_THROWS(selector.evaluate(o));
  auto changed = scoreObservation(selector, 259, constantScores(1)); changed.contractSha256[0] = 'x';
  REQUIRE_THROWS(selector.evaluate(changed));
  changed = scoreObservation(selector, 259, constantScores(1)); changed.families[0].available = false;
  REQUIRE_THROWS(selector.evaluate(changed)); // Unseen availability regime.
  changed = scoreObservation(selector, 259, constantScores(1)); changed.families[0].rowMedian = 0;
  REQUIRE_THROWS(selector.evaluate(changed));
}

TEST_CASE("selector freezes split identity and rejects repeated or undersized fit exposure", "[coherent-selector]") {
  auto c = selectorContract(); c.jointSplitSha256 = c.fitSplitSha256; REQUIRE_THROWS(Selector(c));
  c = selectorContract(); c.fitWindows = 31; REQUIRE_THROWS(Selector(c));
  Selector selector(selectorContract()); auto o = scoreObservation(selector, 0, constantScores(1));
  selector.addFit(o); REQUIRE_THROWS(selector.addFit(o)); REQUIRE_THROWS(selector.freezeFit());
  o = scoreObservation(selector, 1, constantScores(1)); o.families[0].ratio = std::numeric_limits<double>::quiet_NaN();
  REQUIRE_THROWS(selector.addFit(o));
  auto other = selectorContract(); other.experiment.minimumPilotCoherence = .5;
  REQUIRE(Selector(other).contractSha256() != selector.contractSha256());
  other = selectorContract(); other.delay[4] = 5; REQUIRE_THROWS(Selector(other));
}

TEST_CASE("selector handles declared missing families and identical or rescaled ranks", "[coherent-selector]") {
  Selector selector(selectorContract());
  const auto sample = [&](uint64_t id, double score, bool missing) {
    auto o = scoreObservation(selector, id, constantScores(score));
    if (missing) for (uint32_t i = 9; i < 18; ++i) o.families[i].available = false;
    return o;
  };
  for (uint32_t i = 0; i < 128; ++i) selector.addFit(sample(i, i + 1, i < 32));
  selector.freezeFit();
  for (uint32_t i = 0; i < 128; ++i) selector.addJoint(sample(128 + i, i + 1, i < 32));
  selector.freezeJoint();
  const auto missing = selector.evaluate(sample(256, 200, true));
  REQUIRE(missing.candidateDetected); REQUIRE(missing.noncoherentDetected);
  REQUIRE_FALSE(missing.candidate.family.raw.coherent);
  REQUIRE(missing.candidate.family.ratio == 200); REQUIRE(missing.candidate.family.rowMedian == 1);
  REQUIRE(missing.availableFamilies == 9); REQUIRE(missing.availableBankTrialUpperBound == 81);
  REQUIRE(missing.declaredPerReadoutTrials == 837);
  auto scaled = sample(257, 200, true);
  for (auto& f : scaled.families) { f.raw.power *= 8; f.rowMedian *= 8; }
  const auto invariant = selector.evaluate(scaled);
  REQUIRE(invariant.candidateP == missing.candidateP);
  REQUIRE(invariant.noncoherentP == missing.noncoherentP);
  Selector identical(selectorContract()); fitSimple(identical);
  uint32_t detections = 0;
  for (uint32_t i = 0; i < 256; ++i) {
    const auto d = identical.evaluate(scoreObservation(identical, 256 + i, constantScores(.5 + i / 2.0)));
    REQUIRE(d.candidateP == d.noncoherentP);
    REQUIRE(d.candidateDetected == d.noncoherentDetected);
    detections += d.candidateDetected;
  }
  std::cout << "selector-identical denominator=256 candidate=" << detections << " nc=" << detections << '\n';
}

TEST_CASE("selector calibrates correlated vectors and matched NC exposure with frozen draws", "[coherent-selector]") {
  // Algebra qualification only: not IQ, physical target Pd or radar PFA.
  // Independent windows; A/B/evaluation seeds fixed before execution. One
  // terminal cell, no max over cells or readouts. Amplitude arms share draws.
  const auto c = selectorContract(2048, 512, 256);
  Selector selector(c);
  const auto draw = [](std::mt19937_64& rng) {
    const auto uniform = [&]() { return double(rng() >> 11) * 0x1p-53; };
    std::array<double, 18> scores{}; const double shared = uniform();
    for (uint32_t i = 0; i < 18; ++i) scores[i] = 1 + .5 * shared + .5 * uniform();
    return scores;
  };
  std::mt19937_64 fitRng(0x248a), jointRng(0x248b), evaluationRng(0x248c);
  for (uint32_t i = 0; i < c.fitWindows; ++i) selector.addFit(scoreObservation(selector, i, draw(fitRng)));
  selector.freezeFit();
  for (uint32_t i = 0; i < c.jointWindows; ++i) selector.addJoint(scoreObservation(selector, 2048 + i, draw(jointRng)));
  selector.freezeJoint();
  std::array<Selector, 5> arms{selector, selector, selector, selector, selector};
  const std::array<double, 5> amplitudes{0, .25, .5, 1, 2};
  std::array<uint32_t, 5> nc{}, candidate{}, ncOnly{}, candidateOnly{};
  for (uint32_t i = 0; i < 256; ++i) {
    const auto noise = draw(evaluationRng);
    for (uint32_t a = 0; a < 5; ++a) {
      auto scores = noise; scores[17] += amplitudes[a];
      const auto d = arms[a].evaluate(scoreObservation(arms[a], 2560 + i, scores));
      nc[a] += d.noncoherentDetected; candidate[a] += d.candidateDetected;
      ncOnly[a] += d.noncoherentDetected && !d.candidateDetected;
      candidateOnly[a] += d.candidateDetected && !d.noncoherentDetected;
    }
  }
  // Predeclared broad development bound; empirical counts/uncertainty, not a
  // claim of exact conditional 5% size from a single fitted reference table.
  REQUIRE(candidate[0] <= 26); REQUIRE(nc[0] <= 26);
  for (uint32_t a = 0; a < 5; ++a) {
    REQUIRE(nc[a] == nc[0]);
    if (a) REQUIRE(candidate[a] >= candidate[a - 1]);
    std::cout << "selector-vector amplitude=" << amplitudes[a] << " denominator=256 candidate=" << candidate[a]
      << " nc=" << nc[a] << " candidate_only=" << candidateOnly[a] << " nc_only=" << ncOnly[a] << '\n';
  }
  REQUIRE(candidate[4] == 256);
}
