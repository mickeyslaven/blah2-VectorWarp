#include "process/detection/PostClutterBootstrap.h"
#include "process/detection/EmpiricalNullCalibrator.h"
#include "process/integration/CoherentPathEvidence.h"
#include "process/integration/TemporalEvidence.h"
#include "process/integration/TemporalShadowTracker.h"
#include "process/integration/TemporalEvidenceArtifact.h"
#include "process/tracker/NormalizedInnovationGate.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <complex>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

namespace
{

Map<std::complex<double>> map(uint32_t rows, uint32_t columns)
{
  Map<std::complex<double>> result(rows, columns);
  result.delay = {-1, int(columns) - 2};
  result.doppler = {-double(rows) / 2, double(rows) / 2};
  for (uint32_t row = 0; row < rows; ++row)
    for (uint32_t column = 0; column < columns; ++column)
      result.data[row][column] = {};
  return result;
}

TEST_CASE("two-CPI production bank retains only adjacent mean-power evidence")
{
  for (const uint32_t motion : {0u, 1u}) {
    blah2::integration::TemporalEvidence engine({2, motion, motion});
    auto previous = map(5, 5);
    previous.data[1][1] = {4, 0};
    auto first = engine.process(previous, 4, 10);
    REQUIRE(first.availableCpis == 1);
    for (const auto& bank : first.banks) REQUIRE_FALSE(bank.ready);
    auto current = map(5, 5);
    current.data[2][2] = {2, 0};
    auto second = engine.process(current, 4, 11);
    auto& bank = second.banks[motion ? 1 : 0];
    REQUIRE(bank.ready);
    REQUIRE(std::norm(bank.map->data[2][2]) == Catch::Approx(motion ? 10 : 2));
    REQUIRE(bank.lineageAt(12).supportSequences() == std::vector<uint64_t>{11, 10});
    REQUIRE(bank.lineageAt(12).delayMotion == int(motion));
    REQUIRE(bank.lineageAt(12).dopplerMotion == int(motion));
    REQUIRE_FALSE(second.banks[2].ready);
    REQUIRE_FALSE(second.banks[3].ready);
    REQUIRE_THROWS(engine.process(current, 4, 11));
    REQUIRE_THROWS(engine.process(current, 5, 12));
    auto background = map(5, 5);
    auto third = engine.process(background, 4, 12);
    REQUIRE(third.availableCpis == 2);
    REQUIRE(std::norm(third.banks[motion ? 1 : 0].map->data[2][2]) ==
      Catch::Approx(2));
    REQUIRE(third.banks[0].window.supportSequences() == std::vector<uint64_t>{12, 11});
    engine.reset();
    const auto fresh = engine.process(current, 5, 1);
    REQUIRE(fresh.availableCpis == 1);
    REQUIRE_FALSE(fresh.banks[0].ready);
  }
}

TEST_CASE("two-CPI detector uses the integrated null at the decision threshold")
{
  constexpr double pfa = 1e-4;
  const double threshold = blah2::detection::integrated_cfar::alpha(pfa, 4, 2);
  auto frame = map(1, 5);
  frame.delay = {0, 1, 2, 3, 4}; frame.doppler = {10};
  frame.data[0] = {{1, 0}, {1, 0},
    {std::sqrt(threshold * (1 + 1e-8)), 0}, {1, 0}, {1, 0}};
  frame.set_metrics();
  blah2::integration::TemporalEvidence engine({2, 0, 0});
  engine.process(frame, 1, 1);
  auto result = engine.process(frame, 1, 2);
  CfarDetector1D single(pfa, 0, 2, 0, 0, 1);
  CfarDetector1D integrated(pfa, 0, 2, 0, 0, 2);
  REQUIRE(single.process(result.banks[0].map.get())->get_nDetections() == 0);
  REQUIRE(integrated.process(result.banks[0].map.get())->get_nDetections() == 1);
}

TEST_CASE("temporal evidence follows weak motion paths")
{
  blah2::integration::TemporalEvidence engine;
  blah2::integration::TemporalEvidence::Result result;
  for (uint64_t sequence = 10; sequence < 18; ++sequence)
  {
    auto frame = map(17, 17);
    frame.data[4 + sequence - 10][5 + sequence - 10] = {2, 0};
    result = engine.process(frame, 7, sequence);
  }
  const auto cell = uint64_t(11) * 17 + 12;
  REQUIRE(result.availableCpis == 8);
  REQUIRE(result.map->data[11][12].real() == 2);
  const auto lineage = result.banks[3].lineageAt(cell);
  REQUIRE(lineage.depth == 8);
  REQUIRE(lineage.epoch == 7);
  REQUIRE(lineage.supportSequences() ==
    std::vector<uint64_t>{17, 16, 15, 14, 13, 12, 11, 10});
  REQUIRE(lineage.delayMotion == 1);
  REQUIRE(lineage.dopplerMotion == 1);
}

TEST_CASE("overlapping windows cannot repeatedly confirm one impulse")
{
  blah2::integration::TemporalEvidence engine;
  blah2::integration::ConfirmationLedger ledger;
  bool credited = false;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence)
  {
    auto frame = map(9, 9);
    if (sequence == 1) frame.data[4][4] = {8, 0};
    const auto result = engine.process(frame, 1, sequence);
    const double power = std::norm(result.map->data[4][4]);
    // Only scores still carrying the impulse are confirmation candidates.
    if (power > 1)
    {
      credited = ledger.credit("one", result.lineageAt(4 * 9 + 4));
      if (sequence > 1) REQUIRE_FALSE(credited);
    }
  }
}

TEST_CASE("confirmation ledger rejects replayed and malformed lineage")
{
  REQUIRE_THROWS(blah2::integration::ConfirmationLedger(10, 1));
  blah2::integration::ConfirmationLedger ledger;
  blah2::integration::EvidenceLineage first;
  first.epoch = 1; first.newestSequence = 10; first.depth = 2;
  first.sequences[0] = 10; first.sequences[1] = 9;
  REQUIRE(ledger.credit("track", first));
  auto later = first;
  later.newestSequence = 300;
  later.sequences[0] = 300; later.sequences[1] = 299;
  REQUIRE(ledger.credit("track", later));
  REQUIRE_FALSE(ledger.credit("track", first));
  auto nextEpoch = first;
  nextEpoch.epoch = 2;
  REQUIRE(ledger.credit("track", nextEpoch));
  REQUIRE_FALSE(ledger.credit("track", first));
  auto malformed = nextEpoch;
  malformed.newestSequence = 20;
  malformed.sequences[0] = 19;
  REQUIRE_FALSE(ledger.credit("other", malformed));
  malformed.sequences[0] = 20;
  malformed.sequences[1] = 17;
  REQUIRE_FALSE(ledger.credit("other", malformed));
}

TEST_CASE("temporal evidence matches mature GPU bank selection")
{
  blah2::integration::TemporalEvidence engine;
  blah2::integration::TemporalEvidence::Result result;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence)
  {
    auto frame = map(1, 1);
    frame.data[0][0] = {sequence == 8 ? 3.0 : 1.0, 0};
    result = engine.process(frame, 4, sequence);
  }
  REQUIRE(std::norm(result.map->data[0][0]) == Catch::Approx(5));
  REQUIRE(std::norm(result.banks[0].map->data[0][0]) == Catch::Approx(5));
  REQUIRE(std::norm(result.banks[1].map->data[0][0]) == Catch::Approx(5));
  REQUIRE(std::norm(result.banks[2].map->data[0][0]) == Catch::Approx(3));
  REQUIRE(std::norm(result.banks[3].map->data[0][0]) == Catch::Approx(2));
}

TEST_CASE("temporal evidence matches the unconditioned GPU reference contract")
{
  constexpr uint32_t rows = 5, columns = 6;
  blah2::integration::TemporalEvidence engine;
  std::vector<std::vector<double>> history;
  const auto reference = [&](uint32_t row, uint32_t column, uint32_t depth) {
    double best = -1; int bestDelay = 0, bestDoppler = 0;
    for (int dr = -4; dr <= 4; ++dr)
      for (int dc = -2; dc <= 2; ++dc)
      {
        double sum = 0; bool valid = true;
        for (uint32_t age = 0; age < depth; ++age)
        {
          const int rr = int(row) - dr * int(age);
          const int cc = int(column) - dc * int(age);
          if (rr < 0 || cc < 0 || rr >= int(rows) || cc >= int(columns))
          { valid = false; break; }
          sum += history[history.size() - 1 - age][uint64_t(rr) * columns + cc];
        }
        const double mean = valid ? sum / depth : -1;
        if (mean > best) { best = mean; bestDelay = dc; bestDoppler = dr; }
      }
    return std::tuple{best, bestDelay, bestDoppler};
  };
  for (uint64_t sequence = 1; sequence <= 8; ++sequence)
  {
    auto frame = map(rows, columns);
    history.emplace_back(uint64_t(rows) * columns);
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column)
      {
        const double amplitude = 1 + ((sequence * 17 + row * 7 + column * 3) % 11);
        frame.data[row][column] = {amplitude, 0};
        history.back()[uint64_t(row) * columns + column] = amplitude * amplitude;
      }
    const auto result = engine.process(frame, 11, sequence);
    REQUIRE(result.banks[0].ready == (sequence >= 2));
    REQUIRE(result.banks[1].ready == (sequence >= 2));
    REQUIRE(result.banks[2].ready == (sequence >= 4));
    REQUIRE(result.banks[3].ready == (sequence >= 8));
    for (std::size_t bank = 1; bank < result.banks.size(); ++bank)
    {
      const auto& output = result.banks[bank];
      if (!output.ready) continue;
      for (uint32_t row = 0; row < rows; ++row)
        for (uint32_t column = 0; column < columns; ++column)
        {
          const auto [power, delay, doppler] = reference(row, column, output.depth);
          const uint64_t cell = uint64_t(row) * columns + column;
          REQUIRE(std::norm(output.map->data[row][column]) == Catch::Approx(power));
          const auto lineage = output.lineageAt(cell);
          REQUIRE(lineage.delayMotion == delay);
          REQUIRE(lineage.dopplerMotion == doppler);
          REQUIRE(lineage.newestSequence == sequence);
        }
    }
  }
}

TEST_CASE("temporal evidence rejects gaps and changed axes")
{
  blah2::integration::TemporalEvidence engine;
  auto first = map(3, 3);
  engine.process(first, 9, 10);
  auto gap = map(3, 3);
  REQUIRE_THROWS(engine.process(gap, 9, 12));
  auto changed = map(3, 3);
  changed.delay = {0, 2};
  REQUIRE_THROWS(engine.process(changed, 9, 11));
  engine.reset();
  REQUIRE_NOTHROW(engine.process(changed, 10, 1));
}

TEST_CASE("post-clutter bootstrap normalizes unequal channel scales")
{
  auto first = map(3, 31), second = map(3, 31);
  for (uint32_t column = 0; column < 31; ++column)
  {
    first.data[1][column] = {1, 0};
    second.data[1][column] = {10, 0};
  }
  first.data[1][15] = {3, 0};
  second.data[1][15] = {30, 0};
  std::vector<Map<std::complex<double>>*> channels{&first, &second};
  blah2::detection::PostClutterBootstrap bootstrap;
  const auto result = bootstrap.process(channels);
  REQUIRE(result.trainedCells == 31);
  REQUIRE(result.fallbackCells == 0);
  REQUIRE(result.invalidCells == 62);
  REQUIRE(std::norm(result.map->data[1][15]) >
    5 * std::norm(result.map->data[1][14]));

  auto shrinkageConfig = blah2::detection::PostClutterBootstrap::Config{};
  shrinkageConfig.covariance =
    blah2::detection::PostClutterBootstrap::Covariance::shrinkage;
  const blah2::detection::PostClutterBootstrap shrinkage(shrinkageConfig);
  const auto shrunk = shrinkage.process(channels);
  REQUIRE(shrunk.covariance == "shrinkage");
  REQUIRE(shrunk.trainedCells == 31);
  REQUIRE(std::norm(shrunk.map->data[1][15]) >
    5 * std::norm(shrunk.map->data[1][14]));

  for (uint32_t column = 0; column < 31; ++column)
  {
    first.data[1][column] *= 1e-8;
    second.data[1][column] *= 1e-8;
  }
  const auto scaled = bootstrap.process(channels);
  REQUIRE(scaled.statisticFamily[31 + 15] == 1);
  REQUIRE(std::norm(scaled.map->data[1][15]) ==
    Catch::Approx(std::norm(result.map->data[1][15])).epsilon(1e-10));
}

TEST_CASE("post-clutter bootstrap falls back when no training exists")
{
  auto only = map(1, 1);
  only.data[0][0] = {2, 0};
  std::vector<Map<std::complex<double>>*> channels{&only};
  blah2::detection::PostClutterBootstrap bootstrap;
  const auto result = bootstrap.process(channels);
  REQUIRE(result.invalidCells == 1);
  REQUIRE(result.statisticFamily[0] == 0);
  REQUIRE(std::norm(result.map->data[0][0]) == 0);
}

TEST_CASE("empirical null calibration preserves statistic strata and resolution")
{
  blah2::detection::EmpiricalNullCalibrator calibration(100, 4);
  for (double score : {1., 2., 3., 4.}) calibration.add("diagonal/beam12/depth2", score);
  for (double score : {10., 20., 30., 40.}) calibration.add("shrinkage/beam12/depth2", score);
  calibration.freeze();
  REQUIRE(calibration.upperTailP("diagonal/beam12/depth2", 4.5) == Catch::Approx(.2));
  REQUIRE(calibration.upperTailP("diagonal/beam12/depth2", 3) == Catch::Approx(.6));
  REQUIRE(calibration.upperTailP("shrinkage/beam12/depth2", 3) == Catch::Approx(1));
  REQUIRE(calibration.minimumResolvableP("diagonal/beam12/depth2") == Catch::Approx(.2));
  REQUIRE_THROWS(calibration.add("diagonal/beam12/depth2", 5));
  REQUIRE_THROWS(calibration.upperTailP("unseen", 1));
}

TEST_CASE("coherent temporal evidence recovers phase-stable energy only")
{
  blah2::integration::CoherentPathEvidence::Config config;
  config.maximumCpis = 4; config.delayMotionBins = 0; config.dopplerMotionBins = 0;
  config.cpiHopSeconds = .1; config.minimumCoherence = .8;
  blah2::integration::CoherentPathEvidence engine(config);
  std::array<blah2::integration::CoherentPathEvidence::Bank, 3> result;
  constexpr double pi = 3.14159265358979323846;
  for (uint64_t sequence = 1; sequence <= 4; ++sequence)
  {
    auto frame = map(3, 1); frame.doppler = {-5, 5};
    frame.data[2][0] = std::polar(1.0, 2 * pi * 5 * .1 * double(sequence));
    result = engine.process(frame, 2, sequence);
  }
  REQUIRE(result[1].ready);
  const auto cell = uint64_t(2);
  REQUIRE(result[1].valid[cell] == 1);
  REQUIRE(result[1].coherence[cell] == Catch::Approx(1).epsilon(1e-12));
  REQUIRE(std::norm(result[1].map->data[2][0]) == Catch::Approx(4).epsilon(1e-12));
  REQUIRE(result[1].lineageAt(cell).supportSequences() ==
    std::vector<uint64_t>{4, 3, 2, 1});

  engine.reset();
  const std::array<double, 4> phases{0, 0, 0, pi};
  for (uint64_t sequence = 1; sequence <= 4; ++sequence)
  {
    auto frame = map(1, 1); frame.doppler = {0, 0};
    frame.data[0][0] = std::polar(1.0, phases[sequence - 1]);
    result = engine.process(frame, 3, sequence);
  }
  REQUIRE(result[1].valid[0] == 0);
  REQUIRE(std::norm(result[1].map->data[0][0]) == 0);
}

TEST_CASE("coherent temporal evidence locks its hop to capture lineage")
{
  blah2::integration::CoherentPathEvidence::Config config;
  config.maximumCpis = 4; config.delayMotionBins = 0;
  config.dopplerMotionBins = 0; config.cpiHopSeconds = 0;
  config.minimumCoherence = .8;
  blah2::integration::CoherentPathEvidence engine(config);
  constexpr double pi = 3.14159265358979323846;
  std::array<blah2::integration::CoherentPathEvidence::Bank, 3> result;
  for (uint64_t sequence = 1; sequence <= 4; ++sequence)
  {
    auto frame = map(1, 1); frame.doppler = {5, 5};
    const double time = .05 * double(sequence - 1);
    frame.data[0][0] = std::polar(1.0, 2 * pi * 5 * time);
    result = engine.process(frame, 8, sequence, (sequence - 1) * 1000, 20000);
  }
  REQUIRE(result[1].ready);
  REQUIRE(result[1].valid[0] == 1);
  REQUIRE(std::norm(result[1].map->data[0][0]) ==
    Catch::Approx(4).epsilon(1e-12));
  auto gap = map(1, 1); gap.doppler = {5, 5};
  REQUIRE_THROWS(engine.process(gap, 8, 5, 5000, 20000));
}

TEST_CASE("temporal shadow tracker scores only disjoint windows with the existing tracker")
{
  using ShadowTracker = blah2::integration::TemporalShadowTracker;
  blah2::integration::TemporalEvidence temporal({8, 0, 0});
  ShadowTracker::Config config;
  config.enabled = true;
  config.pfa = .01;
  config.guardCells = 1;
  config.trainingCells = 4;
  config.maximumCandidates = 32;
  config.confirmationM = 2;
  config.confirmationN = 3;
  config.cpiSeconds = .5;
  config.rangeResolutionM = 100;
  config.wavelengthM = .5;
  ShadowTracker tracker(config);
  Map<std::complex<double>> map(5, 33);
  map.delay.resize(33);
  std::iota(map.delay.begin(), map.delay.end(), 0);
  map.doppler = {-2, -1, 0, 1, 2};
  std::array<std::vector<std::vector<uint64_t>>, 3> credited;
  for (uint64_t sequence = 1; sequence <= 16; ++sequence)
  {
    for (auto& row : map.data)
      std::fill(row.begin(), row.end(), std::complex<double>{1, 0});
    map.data[3][16] = {100, 0};
    map.set_metrics();
    const auto evidence = temporal.process(map, 77, sequence);
    const auto status = tracker.process(evidence, 1000 + sequence * 500);
    for (std::size_t lane = 0; lane < status.lanes.size(); ++lane)
      if (status.lanes[lane].evaluated)
        for (const auto& candidate : status.lanes[lane].latestCandidates)
          credited[lane].push_back(candidate.lineage.supportSequences());
  }
  const auto status = tracker.status();
  REQUIRE(status.lanes[0].evaluatedWindows == 8);
  REQUIRE(status.lanes[1].evaluatedWindows == 4);
  REQUIRE(status.lanes[2].evaluatedWindows == 2);
  REQUIRE(status.lanes[0].skippedOverlappingWindows == 7);
  REQUIRE(status.lanes[1].skippedOverlappingWindows == 9);
  REQUIRE(status.lanes[2].skippedOverlappingWindows == 7);
  for (std::size_t lane = 0; lane < credited.size(); ++lane)
    for (std::size_t left = 0; left < credited[lane].size(); ++left)
      for (std::size_t right = left + 1; right < credited[lane].size(); ++right)
        for (const auto sequence : credited[lane][left])
          REQUIRE(std::find(credited[lane][right].begin(),
            credited[lane][right].end(), sequence) == credited[lane][right].end());
  tracker.reset();
  const auto reset = tracker.status();
  REQUIRE(reset.epoch == 0);
  REQUIRE(reset.sequence == 0);
  REQUIRE(reset.lanes[0].evaluatedWindows == 8);
  REQUIRE(reset.lanes[1].evaluatedWindows == 4);
  REQUIRE(reset.lanes[2].evaluatedWindows == 2);
  REQUIRE(reset.lanes[0].tracks == 0);
  REQUIRE(reset.lanes[1].tracks == 0);
  REQUIRE(reset.lanes[2].tracks == 0);
}

TEST_CASE("coherent temporal evidence does not lock timing from rejected input")
{
  blah2::integration::CoherentPathEvidence::Config config;
  config.maximumCpis = 4; config.delayMotionBins = 0;
  config.dopplerMotionBins = 0;
  blah2::integration::CoherentPathEvidence engine(config);
  auto frame = map(1, 1); frame.doppler = {0}; frame.data[0][0] = {1, 0};
  engine.process(frame, 8, 1, 0, 20000);
  frame.data[0][0] = {std::numeric_limits<double>::quiet_NaN(), 0};
  REQUIRE_THROWS_AS(engine.process(frame, 8, 2, 1000, 20000), std::invalid_argument);
  frame.data[0][0] = {1, 0};
  // The first accepted hop is 500 samples; the invalid frame cannot fix 1000.
  REQUIRE_NOTHROW(engine.process(frame, 8, 2, 500, 20000));
  REQUIRE_NOTHROW(engine.process(frame, 8, 3, 1000, 20000));
  REQUIRE_THROWS_AS(engine.process(frame, 8, 4, 2000, 20000), std::invalid_argument);
}

TEST_CASE("coherent temporal evidence integrates a linear Doppler trajectory")
{
  blah2::integration::CoherentPathEvidence::Config config;
  config.maximumCpis = 4; config.delayMotionBins = 0;
  config.dopplerMotionBins = 1; config.minimumCoherence = .99;
  blah2::integration::CoherentPathEvidence engine(config);
  constexpr double pi = 3.14159265358979323846;
  std::array<blah2::integration::CoherentPathEvidence::Bank, 3> result;
  for (uint64_t sequence = 1; sequence <= 4; ++sequence)
  {
    auto frame = map(4, 1); frame.doppler = {0, 1, 2, 3};
    const double time = .1 * double(sequence - 1);
    frame.data[sequence - 1][0] = std::polar(1.0, pi * 10 * time * time);
    result = engine.process(frame, 8, sequence, 2000 * (sequence - 1), 20000);
  }
  REQUIRE(result[1].valid[3] == 1);
  REQUIRE(result[1].coherence[3] == Catch::Approx(1).epsilon(1e-12));
  REQUIRE(std::norm(result[1].map->data[3][0]) == Catch::Approx(4).epsilon(1e-12));
  REQUIRE(result[1].lineageAt(3).dopplerMotion == 1);
}

TEST_CASE("coherent temporal evidence integrates every nonuniform Doppler segment")
{
  blah2::integration::CoherentPathEvidence::Config config;
  config.maximumCpis = 4; config.delayMotionBins = 0;
  config.dopplerMotionBins = 1; config.minimumCoherence = .99;
  blah2::integration::CoherentPathEvidence engine(config);
  constexpr double pi = 3.14159265358979323846;
  // Frequencies 0,1,4,9 Hz at 0,.1,.2,.3 s have piecewise-linear integrated
  // phase cycles 0,.05,.30,.95. A single endpoint trapezoid is incorrect.
  const std::array<double, 4> phaseCycles{0, .05, .30, .95};
  std::array<blah2::integration::CoherentPathEvidence::Bank, 3> result;
  for (uint64_t sequence = 1; sequence <= 4; ++sequence)
  {
    auto frame = map(4, 1); frame.doppler = {0, 1, 4, 9};
    frame.data[sequence - 1][0] = std::polar(1.0, 2 * pi * phaseCycles[sequence - 1]);
    result = engine.process(frame, 8, sequence, 2000 * (sequence - 1), 20000);
  }
  REQUIRE(result[1].valid[3] == 1);
  REQUIRE(result[1].coherence[3] == Catch::Approx(1).epsilon(1e-12));
  REQUIRE(std::norm(result[1].map->data[3][0]) == Catch::Approx(4).epsilon(1e-12));
}

TEST_CASE("empirical null calibration cannot resolve unsupported or unfrozen tails")
{
  using blah2::detection::EmpiricalNullCalibrator;
  REQUIRE_THROWS_AS(EmpiricalNullCalibrator(3, 4), std::invalid_argument);
  EmpiricalNullCalibrator calibration(4, 4);
  REQUIRE_THROWS_AS(calibration.freeze(), std::logic_error);
  REQUIRE_THROWS_AS(calibration.add("x", std::numeric_limits<double>::quiet_NaN()),
    std::invalid_argument);
  for (double score : {1., 2., 2.}) calibration.add("x", score);
  REQUIRE_THROWS_AS(calibration.freeze(), std::logic_error);
  REQUIRE_FALSE(calibration.frozen());
  REQUIRE_THROWS_AS(calibration.upperTailP("x", 100), std::logic_error);
  calibration.add("x", 2.);
  REQUIRE_THROWS_AS(calibration.add("x", 3.), std::length_error);
  calibration.freeze();
  REQUIRE(calibration.upperTailP("x", 2) == Catch::Approx(.8));
  REQUIRE(calibration.upperTailP("x", 1e100) == Catch::Approx(.2));
}

TEST_CASE("normalized innovation gate controls probability and gate volume")
{
  blah2::tracker::NormalizedInnovationGate gate({9.21, 100, 1e-9});
  const auto accepted = gate.evaluate({1, 1}, {1, 0, 0, 1}, {1, 0, 0, 1});
  REQUIRE(accepted.valid); REQUIRE(accepted.accepted); REQUIRE(accepted.nis == Catch::Approx(1));
  const auto distant = gate.evaluate({10, 10}, {1, 0, 0, 1}, {1, 0, 0, 1});
  REQUIRE(distant.valid); REQUIRE_FALSE(distant.accepted);
  const auto diffuse = gate.evaluate({0, 0}, {100, 0, 0, 100}, {1, 0, 0, 1});
  REQUIRE(diffuse.valid); REQUIRE_FALSE(diffuse.accepted);
  REQUIRE(diffuse.area > 100);
  const auto singular = gate.evaluate({0, 0}, {1, 2, 2, 1}, {0, 0, 0, 0});
  REQUIRE_FALSE(singular.valid); REQUIRE_FALSE(singular.accepted);
}

TEST_CASE("temporal evidence artifact is bounded binary with warmup records")
{
  const char* retained = std::getenv("VECTORWARP_KEEP_TEMPORAL_ARTIFACT");
  const auto path = retained ? std::filesystem::path(retained) :
    std::filesystem::temp_directory_path() /
      ("vectorwarp-temporal-artifact-" +
        std::to_string(std::random_device{}()) + ".vwtbd");
  std::filesystem::remove(path);
  blah2::integration::TemporalEvidence engine;
  {
    blah2::integration::TemporalEvidenceArtifact artifact(path.string(),
      64ULL << 20, 8, 2, 4, 100, 1, "fused", "diagonal");
    for (uint64_t sequence = 1; sequence <= 8; ++sequence)
    {
      auto frame = map(2, 2);
      frame.data[0][0] = {double(sequence), 0};
      const auto result = engine.process(frame, 3, sequence);
      artifact.write(frame, result, {}, (sequence - 1) * 100, sequence * 100 - 1);
      REQUIRE(artifact.records() == sequence);
    }
    REQUIRE(artifact.bytes() > 32);
  }
  std::ifstream input(path, std::ios::binary);
  const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
  REQUIRE(bytes.size() > 32);
  REQUIRE(std::string(bytes.data(), 7) == "VWTBD01");
  const std::string marker = "FRM1";
  REQUIRE(std::search(bytes.begin(), bytes.end(),
    marker.begin(), marker.end()) != bytes.end());
  REQUIRE_THROWS(blah2::integration::TemporalEvidenceArtifact(path.string(),
    64ULL << 20, 8, 2, 4, 100, 1, "fused", "diagonal"));
  if (!retained) std::filesystem::remove(path);
}

} // namespace
