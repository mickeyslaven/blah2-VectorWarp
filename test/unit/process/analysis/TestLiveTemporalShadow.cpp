#include "process/integration/LiveTemporalShadow.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <numeric>
#include <fstream>
#include <cstdlib>

namespace {
using Shadow = blah2::integration::LiveTemporalShadow;
using Bootstrap = blah2::detection::PostClutterBootstrap;
using Temporal = blah2::integration::TemporalEvidence;
using Clock = std::chrono::steady_clock;

Shadow::ComplexMap shadowMap(uint32_t rows = 5, uint32_t columns = 33) {
  Shadow::ComplexMap map(rows, columns);
  for (uint32_t column = 0; column < columns; ++column) map.delay.push_back(int(column));
  for (uint32_t row = 0; row < rows; ++row) {
    map.doppler.push_back(double(row) - rows / 2);
    for (uint32_t column = 0; column < columns; ++column)
      map.data[row][column] = {1.1234567 + .1 * row + .001 * column,
        .4312345 + .03 * column};
  }
  return map;
}
Shadow::Config shadowConfig() {
  Shadow::Config config; config.enabled = true;
  config.temporal.delayMotionBins = 1; config.temporal.dopplerMotionBins = 1;
  config.scoring.maximumCpis = config.temporal.maximumCpis;
  config.scoring.delayMotionBins = config.temporal.delayMotionBins;
  config.scoring.dopplerMotionBins = config.temporal.dopplerMotionBins;
  config.maximumWorkMs = 25; config.minimumIntervalMs = 100;
  return config;
}
Shadow::Frame shadowFrame(uint64_t sequence) {
  return {19, sequence, (sequence - 1) * 4096, sequence * 4096,
    527000000, 1, 8192, true};
}
void settle(Shadow& shadow) {
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (shadow.status().busy && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE_FALSE(shadow.status().busy);
}
void pace() {
  // Tests that intentionally exercise identity resets must cross the default
  // observer admission interval; this is not production timing behavior.
  std::this_thread::sleep_for(std::chrono::milliseconds(105));
}
TEST_CASE("live temporal drop ledger records exclusive reasons") {
  using Reason = Shadow::DropReason;
  REQUIRE(Shadow::admissionDropReason(false, true, true, false) ==
    Reason::lockContention);
  REQUIRE(Shadow::admissionDropReason(true, true, true, false) ==
    Reason::workerBusy);
  REQUIRE(Shadow::admissionDropReason(true, false, true, false) ==
    Reason::minimumInterval);
  REQUIRE(Shadow::admissionDropReason(true, false, false, false) ==
    Reason::invalidMetadata);
  Shadow::DropLedger ledger;
  for (std::size_t index = 0; index < Shadow::dropReasonNames.size(); ++index)
    ledger.record(Reason(index));
  ledger.record(Reason::none);
  REQUIRE(ledger.snapshot().total() == Shadow::dropReasonNames.size());
}

TEST_CASE("live temporal shadow matches full precision portable banks without mutating CAFs") {
  auto map = shadowMap(); auto second = shadowMap();
  for (auto& row : second.data) for (auto& value : row) value *= Shadow::Complex{.7, .2};
  const auto original = map.data, secondOriginal = second.data;
  map.set_metrics();
  const auto presentation = map.to_json_presentation_km(1234, 8192, true, 65536);
  auto config = shadowConfig();
  config.bootstrap.covariance = Bootstrap::Covariance::robustShrinkage;
  Shadow shadow(config, map, {0, 1});
  Bootstrap bootstrap(config.bootstrap); Temporal temporal(config.temporal);
  std::vector<Shadow::ComplexMap*> maps{&map, &second};
  std::unique_ptr<Shadow::Evidence> observed;
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    REQUIRE(shadow.offerCpu(shadowFrame(sequence), maps));
    auto input = bootstrap.process(maps);
    const auto expected = temporal.process(*input.map, 19, sequence);
    settle(shadow); observed = shadow.takeLatest(); REQUIRE(observed);
    REQUIRE(observed->bootstrap.map->data == input.map->data);
    REQUIRE(observed->bootstrap.statisticFamily == input.statisticFamily);
    REQUIRE(observed->temporal.map->data == expected.map->data);
    REQUIRE(observed->temporal.selectedBank == expected.selectedBank);
    for (std::size_t bank = 0; bank < expected.banks.size(); ++bank) {
      REQUIRE(observed->temporal.banks[bank].ready == expected.banks[bank].ready);
      if (expected.banks[bank].ready) {
        REQUIRE(observed->temporal.banks[bank].map->data == expected.banks[bank].map->data);
        REQUIRE(observed->temporal.banks[bank].window.supportSequences() ==
          expected.banks[bank].window.supportSequences());
      }
    }
  }
  REQUIRE(observed->temporal.availableCpis == 8);
  REQUIRE(observed->temporal.banks[3].window.depth == 8);
  REQUIRE(map.data == original); REQUIRE(second.data == secondOriginal);
  REQUIRE(map.to_json_presentation_km(1234, 8192, true, 65536) == presentation);
  REQUIRE(shadow.status().residentBytesBound < (64ULL << 20));
  REQUIRE(shadow.status().copyMs >= 0);
  REQUIRE(shadow.status().copyOverruns == 0);
  REQUIRE(shadow.status().dropReasons[Shadow::DropReason::minimumInterval] == 0);
  REQUIRE(shadow.status().copyOverrunStreak == 0);
  REQUIRE(shadow.status().disableReason.empty());
  const double value = observed->bootstrap.map->data[2][16].real();
  REQUIRE(value != std::round(value * 100) / 100); // No presentation quantization.
}

TEST_CASE("live temporal shadow scores independent disjoint tracker lanes") {
  auto map = shadowMap();
  for (auto& row : map.data)
    std::fill(row.begin(), row.end(), Shadow::Complex{1, 0});
  map.data[3][16] = {100, 0};
  auto config = shadowConfig();
  config.maximumWorkMs = 100; config.minimumIntervalMs = 400;
  config.scoring.enabled = true;
  config.scoring.pfa = .01;
  config.scoring.guardCells = 1;
  config.scoring.trainingCells = 4;
  config.scoring.maximumCandidates = 32;
  config.scoring.confirmationM = 2;
  config.scoring.confirmationN = 3;
  config.scoring.rangeResolutionM = 100;
  config.scoring.wavelengthM = .5;
  Shadow shadow(config, map, {1});
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    auto frame = shadowFrame(sequence);
    frame.timestampMs = 1000 + sequence * 500;
    REQUIRE(shadow.offerCpu(frame, {&map})); settle(shadow);
    REQUIRE(shadow.status().scoringEnabled);
    if (sequence < 8) std::this_thread::sleep_for(std::chrono::milliseconds(405));
  }
  const auto status = shadow.status();
  REQUIRE(status.scoring[0].evaluatedWindows == 4);
  REQUIRE(status.scoring[1].evaluatedWindows == 2);
  REQUIRE(status.scoring[2].evaluatedWindows == 1);
  REQUIRE(status.scoring[0].skippedOverlappingWindows == 3);
  REQUIRE(status.scoring[1].skippedOverlappingWindows == 3);
  REQUIRE(status.scoring[2].skippedOverlappingWindows == 0);
  REQUIRE(status.scoring[0].candidates > 0);
  REQUIRE_FALSE(status.scoring[2].latestCandidates.empty());
  REQUIRE(status.scoring[2].latestCandidates.front().lineage.depth == 8);
  auto latest = shadow.takeLatest(); REQUIRE(latest);
  REQUIRE(latest->scoring);
  REQUIRE(latest->scoring->lanes[2].evaluated);
  REQUIRE_FALSE(latest->scoring->lanes[2].latestCandidates.empty());
  REQUIRE(latest->scoring->lanes[2].latestCandidates.front().lineage.depth == 8);
}

TEST_CASE("live copy policy bounds soft and hard overruns deterministically") {
  const auto below = Shadow::copyPolicy(2.0, 2);
  REQUIRE(below.admit); REQUIRE_FALSE(below.latch);
  REQUIRE(below.nextStreak == 0);
  const auto first = Shadow::copyPolicy(2.0001, 0);
  REQUIRE_FALSE(first.admit); REQUIRE_FALSE(first.latch);
  REQUIRE(first.nextStreak == 1);
  const auto second = Shadow::copyPolicy(9.999, first.nextStreak);
  REQUIRE_FALSE(second.admit); REQUIRE_FALSE(second.latch);
  REQUIRE(second.nextStreak == 2);
  const auto third = Shadow::copyPolicy(10.0, second.nextStreak);
  REQUIRE_FALSE(third.admit); REQUIRE(third.latch);
  REQUIRE_FALSE(third.hardOverrun); REQUIRE(third.nextStreak == 3);
  const auto hard = Shadow::copyPolicy(10.0001, 0);
  REQUIRE_FALSE(hard.admit); REQUIRE(hard.latch);
  REQUIRE(hard.hardOverrun); REQUIRE(hard.nextStreak == 1);
  const auto invalid = Shadow::copyPolicy(
    std::numeric_limits<double>::quiet_NaN(), 0);
  REQUIRE_FALSE(invalid.admit); REQUIRE(invalid.latch);
  REQUIRE(invalid.hardOverrun);
}

TEST_CASE("resident CAF layout and CPU input produce identical live evidence") {
  auto map = shadowMap();
  std::vector<Shadow::ComplexMap> channels(5, map);
  std::vector<Shadow::ComplexMap*> pointers;
  std::vector<std::complex<float>> resident(5 * 5 * 33);
  for (uint32_t channel = 0; channel < 5; ++channel) {
    pointers.push_back(&channels[channel]);
    for (uint32_t row = 0; row < 5; ++row)
      for (uint32_t column = 0; column < 33; ++column) {
        const auto value = std::complex<float>(map.data[row][column] * double(channel + 1));
        channels[channel].data[row][column] = value;
        resident[channel * 165 + column * 5 + (row + 3) % 5] = value;
      }
  }
  Shadow cpu(shadowConfig(), map, {0, 1, 2, 3, 4});
  Shadow gpu(shadowConfig(), map, {0, 1, 2, 3, 4});
  REQUIRE(cpu.offerCpu(shadowFrame(1), pointers));
  REQUIRE(gpu.offerResident(shadowFrame(1), resident.data(), resident.size()));
  settle(cpu); settle(gpu);
  const auto expected = cpu.takeLatest(), observed = gpu.takeLatest();
  REQUIRE(expected); REQUIRE(observed);
  REQUIRE(observed->bootstrap.map->data == expected->bootstrap.map->data);
  REQUIRE(observed->bootstrap.statisticFamily == expected->bootstrap.statisticFamily);
  REQUIRE(observed->temporal.map->data == expected->temporal.map->data);
}

TEST_CASE("live shadow invalidates pressure drops and never bridges changed capture lineage") {
  auto map = shadowMap(); const std::vector<Shadow::ComplexMap*> maps{&map};
  Shadow shadow(shadowConfig(), map, {1});
  REQUIRE(shadow.offerCpu(shadowFrame(1), maps));
  // Offer immediately, before worker timing can make this host-dependent.
  REQUIRE_FALSE(shadow.offerCpu(shadowFrame(2), maps)); // Pacing/backpressure drop.
  settle(shadow);
  REQUIRE_FALSE(shadow.status().current); REQUIRE_FALSE(shadow.takeLatest());
  pace(); REQUIRE(shadow.offerCpu(shadowFrame(3), maps)); settle(shadow);
  auto evidence = shadow.takeLatest(); REQUIRE(evidence);
  REQUIRE(evidence->temporal.availableCpis == 1);
  for (const auto change : {0, 1, 2, 3, 4, 5}) {
    pace();
    auto frame = shadowFrame(4 + change);
    if (change == 0) ++frame.captureEpoch;
    if (change == 1) frame.firstSample += 1;
    if (change == 2) ++frame.configurationEpoch;
    if (change == 3) ++frame.tuningHz;
    if (change == 4) ++frame.sampleRateHz;
    if (change == 5) frame.sequence += 1;
    REQUIRE(shadow.offerCpu(frame, maps)); settle(shadow);
    evidence = shadow.takeLatest(); REQUIRE(evidence);
    REQUIRE(evidence->temporal.availableCpis == 1);
  }
  shadow.invalidate(); REQUIRE_FALSE(shadow.status().current);
}

TEST_CASE("live shadow preserves source epoch zero while requiring explicit sample position") {
  auto map = shadowMap(); const std::vector<Shadow::ComplexMap*> maps{&map};
  auto frame = shadowFrame(1); frame.captureEpoch = 0;
  Shadow shadow(shadowConfig(), map, {1});
  REQUIRE(shadow.offerCpu(frame, maps));
  settle(shadow);
  const auto evidence = shadow.takeLatest();
  REQUIRE(evidence);
  REQUIRE(evidence->temporal.epoch == 0);
  REQUIRE(evidence->temporal.availableCpis == 1);
  Shadow unpositioned(shadowConfig(), map, {1});
  frame.samplePositionAvailable = false;
  REQUIRE_FALSE(unpositioned.offerCpu(frame, maps));
}

TEST_CASE("live shadow rejects unavailable lineage malformed maps and nonfinite CAFs") {
  auto map = shadowMap(); const std::vector<Shadow::ComplexMap*> maps{&map};
  Shadow shadow(shadowConfig(), map, {1});
  auto frame = shadowFrame(1); frame.samplePositionAvailable = false;
  REQUIRE_FALSE(shadow.offerCpu(frame, maps));
  map.data[0].pop_back(); REQUIRE_FALSE(shadow.offerCpu(shadowFrame(1), maps));
  map = shadowMap(); pace();
  map.data[0][0] = {std::numeric_limits<double>::quiet_NaN(), 0};
  REQUIRE(shadow.offerCpu(shadowFrame(1), maps)); settle(shadow);
  REQUIRE(shadow.status().disabled); REQUIRE(shadow.status().rejected == 1);
  REQUIRE_FALSE(shadow.takeLatest());
  REQUIRE_FALSE(shadow.offerCpu(shadowFrame(2), maps));
}

TEST_CASE("capture invalidation suppresses in-flight shadow completion and mask changes reset warmup") {
  auto map = shadowMap(); const std::vector<Shadow::ComplexMap*> maps{&map};
  Shadow shadow(shadowConfig(), map, {1});
  REQUIRE(shadow.offerCpu(shadowFrame(1), maps));
  shadow.invalidate(); settle(shadow);
  REQUIRE_FALSE(shadow.takeLatest()); REQUIRE_FALSE(shadow.status().current);
  pace(); REQUIRE(shadow.offerCpu(shadowFrame(2), maps)); settle(shadow);
  auto output = shadow.takeLatest(); REQUIRE(output);
  REQUIRE(output->temporal.availableCpis == 1);
  pace(); REQUIRE(shadow.offerCpu(shadowFrame(3), maps)); settle(shadow);
  output = shadow.takeLatest(); REQUIRE(output);
  REQUIRE(output->temporal.availableCpis == 2);
  for (auto& value : map.data[0]) value = {};
  pace(); REQUIRE(shadow.offerCpu(shadowFrame(4), maps)); settle(shadow);
  output = shadow.takeLatest(); REQUIRE(output);
  REQUIRE(output->bootstrap.invalidCells > 0);
  REQUIRE(output->temporal.availableCpis == 1);
  pace(); REQUIRE(shadow.offerCpu(shadowFrame(4), maps)); settle(shadow);
  output = shadow.takeLatest(); REQUIRE(output);
  REQUIRE(output->temporal.availableCpis == 1); // Duplicate cannot create evidence.
}

TEST_CASE("live shadow enforces memory duty and cooperative computation budgets") {
  auto map = shadowMap(); auto config = shadowConfig();
  config.maximumCells = 164;
  Shadow cropped(config, map, {1});
  REQUIRE(cropped.status().sourceRows == 5);
  REQUIRE(cropped.status().sourceColumns == 33);
  REQUIRE(cropped.status().observedRows == 5);
  REQUIRE(cropped.status().observedColumns == 32);
  REQUIRE(cropped.status().observedColumnOffset == 0);
  REQUIRE(cropped.offerCpu(shadowFrame(1), {&map})); settle(cropped);
  auto croppedEvidence = cropped.takeLatest(); REQUIRE(croppedEvidence);
  REQUIRE(croppedEvidence->bootstrap.map->delay.size() == 32);
  REQUIRE(croppedEvidence->bootstrap.map->delay.front() == 0);
  REQUIRE(croppedEvidence->bootstrap.map->delay.back() == 31);
  config = shadowConfig(); config.minimumIntervalMs = 49;
  REQUIRE_THROWS_AS(Shadow::validate(config), std::invalid_argument);
  config = shadowConfig(); config.maximumWorkMs = 151;
  REQUIRE_THROWS_AS(Shadow::validate(config), std::invalid_argument);
  config = shadowConfig();
  REQUIRE_THROWS_AS(Shadow(config, map, {1, 1}), std::invalid_argument);
  config.enabled = false; Shadow dormant(config, map, {1});
  REQUIRE_FALSE(dormant.offerCpu(shadowFrame(1), {&map}));
  REQUIRE_FALSE(dormant.status().enabled);
  const blah2::integration::EvidenceWorkBudget expired{Clock::now()};
  Bootstrap bootstrap; Temporal temporal;
  REQUIRE_THROWS(bootstrap.process({&map}, &expired));
  REQUIRE_THROWS(temporal.process(map, 19, 1, &expired));
}

TEST_CASE("live observer memory accounting admits HP and exact bounded HomePortal ROI") {
  auto config = shadowConfig(); config.enabled = false;
  auto home = shadowMap(601, 1261);
  Shadow homeObserver(config, home, {1});
  REQUIRE(homeObserver.status().sourceRows == 601);
  REQUIRE(homeObserver.status().sourceColumns == 1261);
  REQUIRE(homeObserver.status().observedRows == 601);
  REQUIRE(homeObserver.status().observedColumns == 872);
  REQUIRE(homeObserver.status().observedColumnOffset == 0);
  REQUIRE(homeObserver.status().residentBytesBound ==
    524072ULL * (16 + 384) + 1024ULL * (601 + 872) + (2ULL << 20));
  auto hp = shadowMap(601, 511);
  Shadow hpObserver(config, hp, {0, 1, 2, 3, 4});
  REQUIRE(hpObserver.status().residentBytesBound ==
    307111ULL * (80 + 384) + 1024ULL * (601 + 511) + (2ULL << 20));
  REQUIRE(hpObserver.status().residentBytesBound < (256ULL << 20));
  auto excessive = shadowMap(512, 1024);
  REQUIRE_THROWS_AS(Shadow(config, excessive, {0, 1, 2, 3, 4, 5, 6, 7}),
    std::invalid_argument); // Cell cap alone is insufficient: byte cap also applies.
  auto malformed = shadowMap(); malformed.delay.pop_back();
  REQUIRE_THROWS_AS(Shadow(config, malformed, {1}), std::invalid_argument);
  malformed = shadowMap(); malformed.data[0].pop_back();
  REQUIRE_THROWS_AS(Shadow(config, malformed, {1}), std::invalid_argument);
  config.maximumCells = 4;
  REQUIRE_THROWS_AS(Shadow(config, shadowMap(), {1}), std::invalid_argument);
}

TEST_CASE("bounded live ROI preserves exact CPU and resident values") {
  auto map = shadowMap(5, 33), second = shadowMap(5, 33);
  auto config = shadowConfig();
  config.maximumCells = 164;
  std::vector<std::complex<float>> resident(2 * 5 * 33);
  for (uint32_t channel = 0; channel < 2; ++channel)
    for (uint32_t row = 0; row < 5; ++row)
      for (uint32_t column = 0; column < 33; ++column) {
        const auto value = std::complex<float>(map.data[row][column] *
          double(channel + 1));
        (channel ? second : map).data[row][column] = value;
        resident[channel * 165 + column * 5 + (row + 3) % 5] = value;
      }
  Shadow cpu(config, map, {1, 2}); Shadow gpu(config, map, {1, 2});
  REQUIRE(cpu.offerCpu(shadowFrame(1), {&map, &second}));
  REQUIRE(gpu.offerResident(shadowFrame(1), resident.data(), resident.size()));
  settle(cpu); settle(gpu);
  const auto expected = cpu.takeLatest(), observed = gpu.takeLatest();
  REQUIRE(expected); REQUIRE(observed);
  REQUIRE(expected->bootstrap.map->delay.size() == 32);
  REQUIRE(observed->bootstrap.map->data == expected->bootstrap.map->data);
  REQUIRE(observed->bootstrap.statisticFamily == expected->bootstrap.statisticFamily);
  REQUIRE(observed->temporal.map->data == expected->temporal.map->data);
  config.artifactPath = "/tmp/vectorwarp-cropped-artifact-must-not-open.vww";
  config.artifactOnly = true;
  REQUIRE_THROWS_AS(Shadow(config, map, {1}), std::invalid_argument);
}

struct ArtifactPath {
  // Use the build filesystem: /tmp can be a small tmpfs that correctly fails
  // the production artifact's 128-GiB disk reserve even on a qualified host.
  std::filesystem::path value = std::filesystem::current_path() /
    ("vectorwarp-live-weak-evidence-" + std::to_string(Clock::now().time_since_epoch().count()) + ".vww");
  ~ArtifactPath() { std::error_code ignored; std::filesystem::remove(value, ignored); }
};

Shadow::Config artifactConfig(const ArtifactPath& path) {
  auto config = shadowConfig(); config.artifactPath = path.value.string();
  config.artifactOnly = true; config.artifactMaximumFrames = 1;
  config.maximumWorkMs = 100; config.minimumIntervalMs = 400;
  return config;
}

TEST_CASE("live weak evidence exports unthresholded resident CAFs exactly without numeric processing") {
  ArtifactPath path;
  auto map = shadowMap(); auto config = artifactConfig(path);
  std::vector<std::complex<float>> resident(5 * 33);
  for (std::size_t i = 0; i < resident.size(); ++i)
    resident[i] = {float(i) * .0001234567f, -float(i) * .001234567f};
  const auto unchanged = resident;
  Shadow shadow(config, map, {3});
  auto frame = shadowFrame(1); frame.timestampMs = 1794528000123ULL;
  frame.mapSampleRateHz = frame.sampleRateHz;
  frame.firstSample = 9007199254740993ULL; frame.endSample = frame.firstSample + 4096;
  REQUIRE(shadow.offerResident(frame, resident.data(), resident.size())); settle(shadow);
  const auto status = shadow.status();
  REQUIRE_FALSE(status.disabled); REQUIRE_FALSE(status.artifactDisabled);
  REQUIRE(status.artifactComplete); REQUIRE(status.artifactFrames == 1);
  REQUIRE(status.artifactLastFrame.firstSample == frame.firstSample);
  REQUIRE(status.availableCpis == 0); REQUIRE_FALSE(shadow.takeLatest());
  REQUIRE_FALSE(shadow.offerResident(frame, resident.data(), resident.size()));
  REQUIRE(resident == unchanged);
  const uint64_t headerBytes = 100 + 4 + 33 * 4 + 5 * 8;
  REQUIRE(status.artifactBytes == headerBytes + 120 + resident.size() * 8);
  REQUIRE(std::filesystem::file_size(path.value) == status.artifactBytes);
  std::ifstream stream(path.value, std::ios::binary);
  stream.seekg(headerBytes + 104);
  std::vector<std::complex<float>> roundtrip(resident.size());
  stream.read(reinterpret_cast<char*>(roundtrip.data()), roundtrip.size() * 8);
  REQUIRE(stream.good()); REQUIRE(roundtrip == resident);
  // Optional immutable fixture for the independent Python wire reader test.
  if (const auto* destination = std::getenv("VECTORWARP_WEAK_EVIDENCE_FIXTURE")) {
    REQUIRE(std::filesystem::path(destination).is_absolute());
    REQUIRE(std::filesystem::copy_file(path.value, destination,
      std::filesystem::copy_options::none));
  }
}

TEST_CASE("live artifact exclusive creation failure affects only export") {
  ArtifactPath path;
  { std::ofstream existing(path.value); existing << "preserve me"; }
  auto map = shadowMap(); auto config = artifactConfig(path); config.artifactOnly = false;
  Shadow shadow(config, map, {1});
  auto frame = shadowFrame(1); frame.timestampMs = 1794528000123ULL;
  frame.mapSampleRateHz = frame.sampleRateHz;
  REQUIRE(shadow.offerCpu(frame, {&map})); settle(shadow);
  const auto status = shadow.status();
  REQUIRE(status.artifactDisabled); REQUIRE_FALSE(status.disabled);
  REQUIRE(status.current); REQUIRE(shadow.takeLatest());
  std::ifstream existing(path.value); std::string contents;
  std::getline(existing, contents); REQUIRE(contents == "preserve me");
}

TEST_CASE("live artifact rejects missing timestamps invalid CAFs and unbounded configuration") {
  ArtifactPath path;
  auto map = shadowMap(); auto config = artifactConfig(path);
  Shadow shadow(config, map, {1});
  REQUIRE_FALSE(shadow.offerCpu(shadowFrame(1), {&map}));
  REQUIRE_FALSE(std::filesystem::exists(path.value));
  auto frame = shadowFrame(1); frame.timestampMs = 1794528000123ULL;
  REQUIRE_FALSE(shadow.offerCpu(frame, {&map})); // CAF delay rate must be explicit.
  frame.mapSampleRateHz = frame.sampleRateHz;
  map.data[0][0] = {std::numeric_limits<double>::infinity(), 0};
  REQUIRE(shadow.offerCpu(frame, {&map})); settle(shadow);
  REQUIRE(shadow.status().artifactDisabled); REQUIRE(shadow.status().disabled);
  REQUIRE(shadow.status().artifactFrames == 0);
  REQUIRE_FALSE(shadow.takeLatest());
  config.artifactPath.clear(); REQUIRE_THROWS_AS(Shadow::validate(config), std::invalid_argument);
  config = artifactConfig(path); config.artifactPath = "relative.vww";
  REQUIRE_THROWS_AS(Shadow::validate(config), std::invalid_argument);
  config = artifactConfig(path); config.artifactMaximumFrames = 129;
  REQUIRE_THROWS_AS(Shadow::validate(config), std::invalid_argument);
  config = artifactConfig(path); config.artifactMaximumBytes = (1ULL << 30) + 1;
  REQUIRE_THROWS_AS(Shadow::validate(config), std::invalid_argument);
}

TEST_CASE("live artifact byte exhaustion completes before starting a partial frame") {
  ArtifactPath path;
  auto map = shadowMap(128, 513); // Double CAF exceeds the minimum one-MiB cap.
  auto config = artifactConfig(path); config.artifactMaximumBytes = 1ULL << 20;
  Shadow shadow(config, map, {1});
  auto frame = shadowFrame(1); frame.timestampMs = 1794528000123ULL;
  frame.mapSampleRateHz = frame.sampleRateHz;
  REQUIRE(shadow.offerCpu(frame, {&map})); settle(shadow);
  REQUIRE_FALSE(shadow.status().artifactDisabled);
  REQUIRE(shadow.status().artifactComplete); REQUIRE(shadow.status().artifactFrames == 0);
  REQUIRE(std::filesystem::file_size(path.value) == 100 + 4 + 513 * 4 + 128 * 8);
}
} // namespace
