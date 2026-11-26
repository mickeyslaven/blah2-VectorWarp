#include "process/detection/PostClutterBootstrap.h"
#include "process/detection/EmpiricalNullCalibrator.h"
#include "process/detection/DetectorIntegration.h"
#include "process/integration/CoherentPathEvidence.h"
#include "process/integration/TemporalEvidence.h"
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
  result.delay.resize(columns);
  result.doppler.resize(rows);
  for (uint32_t column = 0; column < columns; ++column)
    result.delay[column] = int(column) - 1;
  for (uint32_t row = 0; row < rows; ++row) {
    result.doppler[row] = double(row) - double(rows) / 2;
    for (uint32_t column = 0; column < columns; ++column)
      result.data[row][column] = {};
  }
  return result;
}

TEST_CASE("deep integration preserves per-CPI detection and independent confirmation",
  "[temporal][lineage][single-cpi]")
{
  constexpr uint32_t rows = 3, columns = 33;
  for (const uint32_t maximum : {4u, 8u}) for (const uint32_t positiveCpis : {1u, 4u}) {
    blah2::detection::DetectorIntegration integration(maximum, 0);
    CfarDetector1D detector(1e-6, 1, 4, 0, 0, maximum);
    Centroid centroid(2, 1, 10); Interpolate interpolate(true, true);
    Tracker tracker(4, 25, 20, 1, 0, 100, 2, -100);
    tracker.configureMaximumEvidenceDepth(maximum);
    uint64_t credits = 0;
    for (uint64_t sequence = 1; sequence <= positiveCpis + maximum; ++sequence) {
      CAPTURE(maximum, positiveCpis, sequence);
      auto frame = std::make_unique<Map<std::complex<double>>>(rows, columns);
      frame->doppler = {-10, 0, 10};
      for (uint32_t column = 0; column < columns; ++column) frame->delay.push_back(column);
      for (auto& row : frame->data) for (auto& value : row) value = {1, 0};
      if (sequence <= positiveCpis) frame->data[1][16] = {sequence == 1 ? 1000.0 : 100.0, 0};
      frame->set_metrics();
      const auto metrics = integration.process(frame, 29, (sequence - 1) * 100, sequence * 100, true);
      REQUIRE(metrics.ready);
      CfarDetector1D::Trials trials;
      auto detections = blah2::detection::detectProducts(*frame, detector, centroid,
        interpolate, metrics, nullptr, {}, &trials, 7);
      const uint64_t readyPasses = 1 + (sequence >= 2 ? 2 : 0) +
        (sequence >= 4 ? 1 : 0) + (maximum == 8 && sequence >= 8 ? 1 : 0);
      REQUIRE(trials.evaluated == rows * columns * readyPasses);
      if (sequence <= positiveCpis) {
        REQUIRE(detections->get_nDetections() == 1);
        const auto& source = detections->get_evidence().front();
        REQUIRE(source.depth == 1);
        REQUIRE(source.bank == 0);
        REQUIRE(source.newestSequence == sequence);
        REQUIRE(source.sequences[0] == sequence);
        REQUIRE(source.sequences[1] == 0);
        REQUIRE(source.delayMotion == 0);
        REQUIRE(source.dopplerMotion == 0);
      } else for (const auto& source : detections->get_evidence()) REQUIRE(source.depth > 1);
      auto tracks = tracker.process(detections.get(), sequence * 1000);
      credits += tracker.frame_diagnostics().independentCredits;
      REQUIRE(credits == std::min<uint64_t>(sequence, positiveCpis));
      REQUIRE(tracker.frame_diagnostics().creditedCpis == (sequence <= positiveCpis ? 1 : 0));
      REQUIRE(tracks->get_n() == 1);
      if (sequence <= positiveCpis) {
        REQUIRE(tracks->get_current_source(0) == Track::Source::detector);
        REQUIRE(tracks->get_current(0).get_evidence().front().depth == 1);
        REQUIRE(tracks->get_state(0) == (sequence >= 4 ? "ACTIVE" : sequence == 1 ? "TENTATIVE" : "ASSOCIATED"));
      }
      if (positiveCpis == 1) REQUIRE(tracks->get_state(0) != "ACTIVE");
      if (sequence == positiveCpis + maximum) {
        REQUIRE(detections->get_nDetections() == 0);
        REQUIRE(tracks->get_current_source(0) == Track::Source::prediction);
        rapidjson::Document published; published.Parse(tracks->to_json(sequence * 1000).c_str());
        REQUIRE_FALSE(published.HasParseError());
        REQUIRE(published.HasMember("data")); REQUIRE(published["data"].IsArray());
        if (positiveCpis == 1) REQUIRE(published["data"].Empty()); // Tentative tracks are not published.
        else {
          REQUIRE(published["data"].Size() == 1);
          REQUIRE_FALSE(published["data"][0]["measured"].GetBool());
          REQUIRE_FALSE(published["data"][0].HasMember("measurement_evidence"));
        }
      }
    }
  }
}

TEST_CASE("mixed temporal suppression preserves strong fresh evidence and weak deep births",
    "[temporal][lineage][single-cpi]")
{
  struct Case { double current, deep, minimum; bool fresh; };
  for (const auto test : {Case{6,6,7,false}, Case{6,8,7,false}, Case{8,10,7,true},
      Case{8,10,9,false}, Case{9.1000002,10,9.1,true}, Case{8,10,0,false}})
    for (double separation : {0.0, 1.0}) for (uint32_t maximum : {4u, 8u}) {
      Detection candidates({16,16+separation}, {0,0}, {test.deep,test.current});
      candidates.set_evidence({{29,8,4,3,0,0,{8,7,6,5}}, {29,8,1,0,0,0,{8}}});
      Centroid centroid(2,1,1);
      const auto selected = centroid.process(&candidates, test.minimum);
      REQUIRE(selected->get_nDetections() == 1);
      REQUIRE(selected->get_evidence().front().depth == (test.fresh ? 1 : 4));
      // Same-key consolidation and the final neighborhood use one ordering.
      REQUIRE(blah2::detection::integrated_cfar::temporalCandidateBetter(true,test.current,
        false,test.deep,test.minimum,false) == test.fresh);
      if (!test.minimum) continue; // Detector-only mode has no birth guarantee.
      Tracker tracker(2,9,20,1,0,100,2,test.minimum == 7 ? 5 : test.minimum);
      tracker.configureMaximumEvidenceDepth(maximum);
      const auto tracks = tracker.process(selected.get(), 8000);
      REQUIRE(tracks->get_n() == 1);
      REQUIRE(tracks->get_current(0).get_evidence().front().depth == (test.fresh ? 1 : 4));
    }
  Detection weak({16}, {0}, {6}); weak.set_evidence({{29,8,1,0,0,0,{8}}});
  Tracker tracker(2,9,20,1,0,100,2,5); tracker.configureMaximumEvidenceDepth(4);
  REQUIRE(tracker.process(&weak,8000)->get_n() == 0); // Gate is not relaxed.
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

TEST_CASE("two-CPI nextafter motion ties retain the first complete mean-power path")
{
  for(uint32_t maximum:{2u,4u,8u}) {
    blah2::integration::TemporalEvidence engine({maximum,1,1});
    auto previous=map(3,3);
    previous.data[2][2]={1,0}; // First path: dr=-1, dc=-1.
    previous.data[2][1]={std::nextafter(1.0,2.0),0}; // Larger old power, same rounded mean.
    engine.process(previous,7,1);
    auto current=map(3,3);current.data[1][1]={2,0};
    const auto result=engine.process(current,7,2);
    const auto& bank=result.banks[1];
    REQUIRE(bank.map->data[1][1].real()==std::sqrt(2.5));
    const auto lineage=bank.lineageAt(4);
    REQUIRE(lineage.depth==2);
    REQUIRE(lineage.delayMotion==-1);
    REQUIRE(lineage.dopplerMotion==-1);
    REQUIRE(lineage.supportSequences()==std::vector<uint64_t>{2,1});
  }
}

TEST_CASE("borrowed temporal rows preserve exact banks metrics masks and lineage",
  "[temporal-parallel]")
{
  blah2::utility::FixedExecutor executor(3);
  std::atomic<uint64_t> generation{7};
  const blah2::integration::EvidenceWorkBudget budget{
    std::chrono::steady_clock::time_point::max(), nullptr, &executor, &generation, 7};
  for (const uint32_t depth : {2u, 4u, 8u})
    for (const bool gated : {false, true}) {
      if (depth == 2 && gated) continue;
      const double drift = depth == 2 ? 0 : -.566;
      const blah2::integration::TemporalEvidence::Config config{
        depth, 1, 2, drift, gated ? 8u : 0u, gated ? 0u : UINT32_MAX};
      auto oracleConfig = config;
      // All-unknown beam labels leave every path compatible but force the
      // scalar kernel, independently checking the contiguous non-beam path.
      if (!gated && depth >= 4) { oracleConfig.beamCount = 8; oracleConfig.beamTolerance = 0; }
      blah2::integration::TemporalEvidence serial(oracleConfig), parallel(config);
      // Cross multiple bounded tiles and create invalid fractional/beam paths.
      auto frame = map(73, 257);
      std::vector<uint32_t> beams(73 * 257);
      const std::vector<uint32_t> unknownBeams(73 * 257, 0);
      for (uint64_t sequence = 1; sequence <= depth + 2; ++sequence) {
        for (uint32_t row = 0; row < 73; ++row)
          for (uint32_t column = 0; column < 257; ++column) {
            const auto cell = row * 257 + column;
            frame.data[row][column] = {sequence == 1 ? 1. :
              std::ldexp(1 + double((cell * 7 + sequence * 13) % 23) / 23,
                int(cell % 11) - 5), column % 2 ? -.25 : .25};
            beams[cell] = (cell + sequence) % 9;
          }
        const auto expected = serial.process(frame, 19, sequence, nullptr,
          gated ? &beams : depth >= 4 ? &unknownBeams : nullptr);
        const auto actual = parallel.process(frame, 19, sequence, &budget, gated ? &beams : nullptr);
        REQUIRE(actual.availableCpis == expected.availableCpis);
        REQUIRE(actual.map->data == expected.map->data);
        REQUIRE(actual.map->noisePower == expected.map->noisePower);
        REQUIRE(actual.map->maxPower == expected.map->maxPower);
        REQUIRE(actual.selectedBank == expected.selectedBank);
        for (std::size_t index = 0; index < actual.banks.size(); ++index) {
          const auto& a = actual.banks[index]; const auto& e = expected.banks[index];
          REQUIRE(a.ready == e.ready);
          if (!a.ready) continue;
          REQUIRE(a.detectorPower == e.detectorPower);
          REQUIRE(a.map->data == e.map->data);
          REQUIRE(a.map->noisePower == e.map->noisePower);
          REQUIRE(a.map->maxPower == e.map->maxPower);
          REQUIRE(a.validCells == e.validCells);
          REQUIRE(a.window.supportSequences() == e.window.supportSequences());
          for (std::size_t cell = 0; cell < a.motion.size(); ++cell) {
            REQUIRE(a.motion[cell].delay == e.motion[cell].delay);
            REQUIRE(a.motion[cell].doppler == e.motion[cell].doppler);
          }
        }
      }
      parallel.reset(); serial.reset();
      REQUIRE(parallel.process(frame, 20, 1, &budget, gated ? &beams : nullptr).availableCpis == 1);
      REQUIRE_THROWS(parallel.process(frame, 20, 3, &budget, gated ? &beams : nullptr));
    }
  auto frame = map(73, 257);
  blah2::integration::TemporalEvidence cancelled;
  generation = 8;
  REQUIRE_THROWS(cancelled.process(frame, 1, 1, &budget));
  generation = 7;
  REQUIRE(cancelled.process(frame, 1, 1, &budget).availableCpis == 1);
  // A wide accepted map cannot acquire a new scratch-size refusal merely
  // because a worker pool is available. One row falls back to ordered work.
  auto wide = map(1, 30000);
  blah2::integration::TemporalEvidence wideSerial({2, 1, 0}), wideParallel({2, 1, 0});
  for (uint64_t sequence = 1; sequence <= 2; ++sequence) {
    const auto a = wideParallel.process(wide, 1, sequence, &budget);
    const auto e = wideSerial.process(wide, 1, sequence);
    REQUIRE(a.map->data == e.map->data);
    REQUIRE(a.map->noisePower == e.map->noisePower);
    REQUIRE(a.banks[0].validCells == e.banks[0].validCells);
  }
}

TEST_CASE("production detector integration borrows its bounded executor",
  "[temporal-parallel]")
{
  blah2::utility::FixedExecutor executor(2);
  const auto motion = track_before_detect::encodeMotionBins(1, 2, true);
  blah2::detection::DetectorIntegration serial(8, motion, -.566), parallel(8, motion, -.566);
  for (uint64_t frame = 0; frame < 10; ++frame) {
    auto expected = std::make_unique<Map<std::complex<double>>>(map(17, 65));
    for (uint32_t row = 0; row < 17; ++row)
      for (uint32_t column = 0; column < 65; ++column)
        expected->data[row][column] = {double(1 + (frame + row * 7 + column * 13) % 23), 0};
    auto actual = std::make_unique<Map<std::complex<double>>>(*expected);
    const auto a = parallel.process(actual, 1, frame * 100, (frame + 1) * 100, true, nullptr, &executor);
    const auto e = serial.process(expected, 1, frame * 100, (frame + 1) * 100, true);
    REQUIRE(a.availableCpis == e.availableCpis);
    REQUIRE(a.ready == e.ready);
    REQUIRE(actual->data == expected->data);
    REQUIRE(actual->noisePower == expected->noisePower);
    REQUIRE(a.evidence->selectedBank == e.evidence->selectedBank);
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
  const auto checkMetrics = [](auto& output, const std::vector<uint8_t>& valid) {
    double noise = 0, peak = -std::numeric_limits<double>::infinity();
    uint64_t count = 0, cell = 0;
    for (const auto& row : output.data)
      for (const auto value : row) {
        const bool measured = valid.empty() || valid[cell]; ++cell;
        if (!measured) continue;
        const double level = 10 * std::log10(std::max(1e-30, std::abs(value)));
        noise += level; peak = std::max(peak, level); ++count;
      }
    REQUIRE(output.noisePower == (count ? noise / count : 0));
    REQUIRE(output.maxPower == (count ? peak - noise / count : 0));
  };
  for (const auto config : {blah2::integration::TemporalEvidence::Config{8, 0, 0},
       blah2::integration::TemporalEvidence::Config{8, 2, 4},
       blah2::integration::TemporalEvidence::Config{8, 1, 2, -.4}}) {
  blah2::integration::TemporalEvidence engine(config);
  std::vector<std::vector<double>> history;
  const auto reference = [&](uint32_t row, uint32_t column, uint32_t depth) {
    double best = -1; int bestDelay = 0, bestDoppler = 0;
    for (int dr = -int(config.dopplerMotionBins); dr <= int(config.dopplerMotionBins); ++dr)
      for (int dc = -int(config.delayMotionBins); dc <= int(config.delayMotionBins); ++dc)
      {
        double sum = 0; bool valid = true;
        for (uint32_t age = 0; age < depth; ++age)
        {
          const int rr = int(row) - dr * int(age);
          if (rr < 0 || rr >= int(rows)) { valid = false; break; }
          const int cc = track_before_detect::delayPathColumn(int(column), dc, age,
            config.delayDriftPerHz, double(row) - double(rows) / 2,
            double(rr) - double(rows) / 2);
          if (rr < 0 || cc < 0 || rr >= int(rows) || cc >= int(columns))
          { valid = false; break; }
          sum += history[history.size() - 1 - age][uint64_t(rr) * columns + cc];
        }
        const double mean = valid ? sum / depth : -1;
        if (mean > best) { best = mean; bestDelay = dc; bestDoppler = dr; }
      }
    return std::tuple{best, bestDelay, bestDoppler};
  };
  for (uint64_t sequence = 1; sequence <= 16; ++sequence)
  {
    auto frame = map(rows, columns);
    history.emplace_back(uint64_t(rows) * columns);
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column)
      {
        const double amplitude = std::ldexp(1 + double(
          (sequence * 17 + row * 7 + column * 3) % 101) / 101,
          int((sequence * 13 + row * 5 + column) % 25) - 12);
        frame.data[row][column] = {amplitude, amplitude / 3};
        history.back()[uint64_t(row) * columns + column] =
          std::norm(frame.data[row][column]);
      }
    const auto result = engine.process(frame, 11, sequence);
    REQUIRE(result.banks[0].ready == (sequence >= 2));
    REQUIRE(result.banks[1].ready == (sequence >= 2));
    REQUIRE(result.banks[2].ready == (sequence >= 4));
    REQUIRE(result.banks[3].ready == (sequence >= 8));
    if (result.banks[0].ready) checkMetrics(*result.banks[0].map, {});
    for (std::size_t bank = 1; bank < result.banks.size(); ++bank)
    {
      const auto& output = result.banks[bank];
      if (!output.ready) continue;
      for (uint32_t row = 0; row < rows; ++row)
        for (uint32_t column = 0; column < columns; ++column)
        {
          const auto [power, delay, doppler] = reference(row, column, output.depth);
          const uint64_t cell = uint64_t(row) * columns + column;
          REQUIRE(output.map->data[row][column] ==
            std::complex<double>{std::sqrt(std::max(0.0, power)), 0});
          const auto lineage = output.lineageAt(cell);
          if (power < 0) {
            REQUIRE(lineage.depth == 0);
            REQUIRE(output.validCells[cell] == 0);
            continue;
          }
          REQUIRE(lineage.delayMotion == delay);
          REQUIRE(lineage.dopplerMotion == doppler);
          REQUIRE(lineage.newestSequence == sequence);
        }
      checkMetrics(*output.map, output.validCells);
    }
    for (uint32_t row = 0; row < rows; ++row)
      for (uint32_t column = 0; column < columns; ++column) {
        const uint64_t cell = uint64_t(row) * columns + column;
        double selected = history.back()[cell]; uint8_t selectedBank = 0;
        if (result.banks[0].ready) {
          selected = .5 * (history.back()[cell] + history[history.size() - 2][cell]);
          selectedBank = 1;
        }
        for (std::size_t bank = 1; bank < result.banks.size(); ++bank)
          if (result.banks[bank].ready) {
            const auto [power, delay, doppler] = reference(row, column,
              result.banks[bank].depth);
            (void)delay; (void)doppler;
            if (power > selected) { selected = power; selectedBank = bank + 1; }
          }
        REQUIRE(result.selectedBank[cell] == selectedBank);
        REQUIRE(result.map->data[row][column] ==
          std::complex<double>{std::sqrt(std::max(0.0, selected)), 0});
      }
    checkMetrics(*result.map, {});
  }
  }
}

TEST_CASE("row-local temporal offsets preserve exact scalar powers and lineage")
{
  constexpr uint32_t rows = 9, columns = 43;
  const std::vector<double> axes{-600, -450, -300, -150, 0, 150, 300, 450, 600};
  // Three live-carrier geometries plus a half-bin rounding boundary. The
  // scalar oracle still calculates coordinates independently for every cell.
  for (const auto maximum : {4u, 8u})
    for (const double drift : {-3072000. / 539000000., -3072000. / 551000000.,
         -3072000. / 557000000., -.01}) {
      blah2::integration::TemporalEvidence engine({maximum, 1, 2, drift});
      for (uint64_t epoch : {11u, 12u}) {
        engine.reset();
        std::vector<std::vector<double>> history;
        for (uint64_t sequence = 1; sequence <= 10; ++sequence) {
          auto frame = map(rows, columns);
          frame.doppler.assign(axes.begin(), axes.end());
          history.insert(history.begin(), std::vector<double>(rows * columns));
          for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t column = 0; column < columns; ++column) {
              const double amplitude = std::ldexp(1 + double(
                (sequence * 17 + row * 7 + column * 3) % 101) / 101,
                int((sequence * 13 + row * 5 + column) % 25) - 12);
              frame.data[row][column] = {amplitude, amplitude / 3};
              history[0][row * columns + column] = std::norm(frame.data[row][column]);
            }
          const auto result = engine.process(frame, epoch, sequence);
          for (const auto& bank : result.banks) {
            if (!bank.ready || bank.name == "baseline2") continue;
            for (uint32_t row = 0; row < rows; ++row)
              for (uint32_t column = 0; column < columns; ++column) {
                double best = -1; int bestDelay = 0, bestDoppler = 0;
                for (int dr = -2; dr <= 2; ++dr)
                  for (int dc = -1; dc <= 1; ++dc) {
                    const double power = track_before_detect::pathMean(history,
                      columns, axes, row, column, bank.depth, dr, dc, drift);
                    if (power > best) { best = power; bestDelay = dc; bestDoppler = dr; }
                  }
                const uint64_t cell = row * columns + column;
                REQUIRE(bank.detectorPower[cell] == std::max(0., best));
                const auto lineage = bank.lineageAt(cell);
                REQUIRE(lineage.depth == (best < 0 ? 0 : bank.depth));
                if (best < 0) continue;
                REQUIRE(lineage.epoch == epoch);
                REQUIRE(lineage.newestSequence == sequence);
                REQUIRE(lineage.delayMotion == bestDelay);
                REQUIRE(lineage.dopplerMotion == bestDoppler);
              }
          }
        }
      }
    }
}

TEST_CASE("temporal motion validity excludes partial and empty rows from CFAR",
  "[temporal-validity]")
{
  constexpr uint32_t columns = 411;
  Map<std::complex<double>> frame(5, columns);
  frame.delay.resize(columns);
  std::iota(frame.delay.begin(), frame.delay.end(), 0);
  frame.doppler = {-620, -600, 0, 600, 620};
  blah2::integration::TemporalEvidence engine({8, 2, 0, -.1});
  blah2::integration::TemporalEvidence::Result result;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence)
    result = engine.process(frame, 31, sequence);
  // A 60-bin/CPI drift leaves five complete paths at one edge. At 62
  // bins/CPI the entire depth-eight row is unavailable, in either direction.
  const std::array<uint32_t, 5> validCounts{0, 5, columns, 5, 0};
  const auto& mature = result.banks[3];
  REQUIRE(mature.validCells.size() == 5 * columns);
  for (uint32_t row = 0; row < 5; ++row) {
    uint32_t count = 0;
    for (uint32_t column = 0; column < columns; ++column) {
      const uint64_t cell = uint64_t(row) * columns + column;
      const bool valid = mature.validCells[cell] != 0;
      count += valid;
      REQUIRE(std::norm(mature.map->data[row][column]) == (valid ? 1 : 0));
      const auto lineage = mature.lineageAt(cell);
      REQUIRE(lineage.depth == (valid ? 8 : 0));
      REQUIRE(lineage.supportSequences().size() == (valid ? 8 : 0));
    }
    REQUIRE(count == validCounts[row]);
  }
  for (auto& bank : result.banks) {
    REQUIRE(bank.ready);
    REQUIRE(bank.map->noisePower == 0);
    REQUIRE(bank.map->maxPower == 0);
    CfarDetector1D detector(1e-6 / 30, 2, 32, 0, 0, bank.depth);
    REQUIRE(detector.process(bank.map.get(), bank.validCells)->get_nDetections() == 0);
  }

  std::optional<blah2::detection::IntegrationMetrics> integration(std::in_place);
  integration->ready = true;
  integration->cpis = 8;
  integration->delayMotionBins = 2;
  integration->evidence = std::make_shared<blah2::integration::TemporalEvidence::Result>(
    std::move(result));
  integration->evidence->currentMap = std::make_unique<Map<std::complex<double>>>(frame);
  CfarDetector1D detector(1e-6, 2, 32, 0, 0);
  Centroid centroid(1, 1, 1);
  Interpolate interpolate(true, true);
  REQUIRE(blah2::detection::detectProducts(*integration->evidence->map,
    detector, centroid, interpolate, integration)->get_nDetections() == 0);
}

TEST_CASE("masked CFAR recalibrates the valid training count and retains targets",
  "[temporal-validity]")
{
  Map<std::complex<double>> frame(1, 7);
  frame.delay = {0, 1, 2, 3, 4, 5, 6}; frame.doppler = {10};
  const std::vector<uint8_t> valid{0, 0, 1, 1, 1, 1, 1};
  const std::vector<std::vector<double>> background(1, std::vector<double>(7, 0));
  for (const uint32_t depth : {1u, 2u, 4u, 8u}) {
    constexpr double pfa = 1e-5;
    const double alpha = depth == 1 ? 3 * (std::pow(pfa, -1.0 / 3) - 1) :
      blah2::detection::integrated_cfar::alpha(pfa, 3, depth);
    CfarDetector1D detector(pfa, 0, 2, 0, 0, depth);
    for (const bool above : {false, true}) {
      // Invalid cells include very large placeholders: neither CUT nor
      // training may use them. The target has exactly three valid neighbours.
      frame.data[0] = {{1e10, 0}, {1e10, 0}, {1, 0},
        {std::sqrt(alpha * (above ? 1 + 1e-8 : 1 - 1e-8)), 0},
        {1, 0}, {1, 0}, {1, 0}};
      frame.set_metrics(valid);
      for (const bool subtractBackground : {false, true}) {
        const auto detections = detector.process(&frame, valid,
          subtractBackground ? &background : nullptr);
        REQUIRE(detections->get_nDetections() == (above ? 1 : 0));
        if (above) REQUIRE(detections->get_delay().front() == 3);
      }
    }
    std::vector<uint8_t> unavailable(7, 0);
    frame.set_metrics(unavailable);
    REQUIRE(frame.noisePower == 0);
    REQUIRE(frame.maxPower == 0);
    REQUIRE(detector.process(&frame, unavailable)->get_nDetections() == 0);
    unavailable[3] = 1;
    REQUIRE(detector.process(&frame, unavailable)->get_nDetections() == 0);
    REQUIRE_THROWS_AS(detector.process(&frame, std::vector<uint8_t>(6, 1)),
      std::invalid_argument);
    REQUIRE_THROWS_AS(frame.set_metrics(std::vector<uint8_t>(6, 1)),
      std::invalid_argument);
  }
}

TEST_CASE("fully valid temporal banks preserve metrics and detector results exactly",
  "[temporal-validity]")
{
  Map<std::complex<double>> frame(3, 41);
  frame.delay.resize(41); std::iota(frame.delay.begin(), frame.delay.end(), 0);
  frame.doppler = {-2, 0, 2};
  for (uint32_t row = 0; row < 3; ++row) {
    for (uint32_t column = 0; column < 41; ++column)
      frame.data[row][column] = {double(1 + (row + column) % 3), 0};
    frame.data[row][20] = {100, 0};
  }
  frame.set_metrics();
  const std::vector<uint8_t> allValid(3 * 41, 1);
  auto maskedMetrics = frame;
  maskedMetrics.set_metrics(allValid);
  REQUIRE(maskedMetrics.noisePower == frame.noisePower);
  REQUIRE(maskedMetrics.maxPower == frame.maxPower);
  for (const double drift : {0., -.001}) {
    // The fractional displacement rounds to zero at every history age.
    blah2::integration::TemporalEvidence engine({8, 0, 0, drift});
    blah2::integration::TemporalEvidence::Result result;
    for (uint64_t sequence = 1; sequence <= 8; ++sequence)
      result = engine.process(frame, 8, sequence);
    for (auto& bank : result.banks) {
      REQUIRE(bank.validCells.empty());
      REQUIRE(bank.map->data == frame.data);
      REQUIRE(bank.map->noisePower == frame.noisePower);
      REQUIRE(bank.map->maxPower == frame.maxPower);
      CfarDetector1D detector(1e-5, 1, 4, 0, 0, bank.depth);
      const auto original = detector.process(&frame);
      REQUIRE(original->get_nDetections() > 0);
      for (const auto* mask : std::array<const std::vector<uint8_t>*, 2>{
          &bank.validCells, &allValid}) {
        const auto actual = detector.process(bank.map.get(), *mask);
        REQUIRE(actual->get_delay() == original->get_delay());
        REQUIRE(actual->get_doppler() == original->get_doppler());
        REQUIRE(actual->get_snr() == original->get_snr());
      }
    }
  }
}

TEST_CASE("real-valued map magnitude fast path preserves exact metrics")
{
  Map<std::complex<double>> frame(5, 257);
  for (uint32_t row = 0; row < 5; ++row)
    for (uint32_t column = 0; column < 257; ++column) {
      const double value = std::ldexp(1 + double(column) / 257,
        int((column * 23 + row * 7) % 2051) - 1050);
      frame.data[row][column] = {column % 2 ? -value : value,
        row % 2 ? -0.0 : 0.0};
    }
  frame.data[0][0] = {std::numeric_limits<double>::max(), 0};
  frame.data[0][1] = {0, -0.0};
  frame.data[0][2] = {3, 4}; // General complex magnitude remains unchanged.
  for (const bool masked : {false, true}) {
    std::vector<uint8_t> valid(masked ? 5 * 257 : 0, 1);
    if (masked) valid[17] = valid[258] = 0;
    double noise = 0, peak = -std::numeric_limits<double>::infinity();
    uint64_t count = 0;
    for (uint32_t row = 0; row < 5; ++row)
      for (uint32_t column = 0; column < 257; ++column) {
        if (masked && !valid[row * 257 + column]) continue;
        const double level = 10 * std::log10(std::max(1e-30,
          std::abs(frame.data[row][column])));
        noise += level; peak = std::max(peak, level); ++count;
      }
    frame.set_metrics(valid);
    REQUIRE(frame.noisePower == noise / count);
    REQUIRE(frame.maxPower == peak - noise / count);
  }
  frame.data[0][0] = {std::numeric_limits<double>::infinity(), 0};
  REQUIRE_THROWS(frame.set_metrics());
  frame.data[0][0] = {std::numeric_limits<double>::quiet_NaN(), -0.0};
  REQUIRE_THROWS(frame.set_metrics());
}

TEST_CASE("stationary temporal path retains finite coordinate validation")
{
  auto frame = map(1, 1);
  frame.doppler[0] = std::numeric_limits<double>::infinity();
  blah2::integration::TemporalEvidence engine({8, 0, 0});
  REQUIRE_NOTHROW(engine.process(frame, 19, 1));
  REQUIRE_THROWS(engine.process(frame, 19, 2));
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

} // namespace
