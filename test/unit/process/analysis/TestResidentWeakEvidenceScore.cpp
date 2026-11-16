#include "ResidentWeakEvidenceScore.h"
#include "ResidentReplayFixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace rr = blah2::resident_replay;
namespace replay = blah2::integration::coherent_replay;
namespace score = blah2::resident_weak_score;
namespace fs = std::filesystem;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void near(double actual, double expected, const char* message) {
  check(std::abs(actual - expected) < 1e-5 * std::max(1., std::abs(expected)), message);
}
template<class F> void rejects(F&& operation, const char* message) {
  bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; }
  check(rejected, message);
}
blah2::resident::Config config() {
  blah2::resident::Config c; c.inputChannels = 2; auto& g = c.geometry;
  g.range = 15; g.doppler = 7; g.delays = 9; g.channels = 2; g.delayMin = -4;
  g.clutterSamples = 64; g.clutterBins = 4; c.correlation = 9; c.sampleRate = 128000;
  c.epoch = 9; c.surveillanceChannels = {0, 1}; c.spectrumBins = 64;
  for (int i = -4; i <= 4; ++i) c.delayAxis.push_back(i);
  const double resolution = 1. / (63. / c.sampleRate);
  for (int i = -3; i <= 3; ++i) c.dopplerAxis.push_back(i * resolution);
  c.reference.channels = {0, 1}; c.reference.analysisSamples = 64;
  c.detector.enabled = true; c.detector.guard = 2; c.detector.train = 12;
  c.detector.pfa = .001; c.detector.centroidDopplerResolution = 2; c.detector.integrationCpis = 1;
  c.device = "cpu-contract-only"; rr::test::enable(c); return c;
}
replay::ValidatedSnapshot frame(uint64_t sequence) {
  auto c = config(); auto snapshot = rr::test::fixture(c, 1, {sequence, sequence + 100, sequence + 1000});
  const auto writer = [&](rr::BlockKind kind) {
    auto& bytes = snapshot.blocks.at(unsigned(kind) - 1).bytes;
    return blah2::resident::wire::Writer{{bytes.data(), bytes.size()}};
  };
  const auto phase = std::polar(1., sequence * .71);
  auto raw = writer(rr::BlockKind::rawIq), reference = writer(rr::BlockKind::referenceIq);
  for (unsigned i = 0; i < 128; ++i) raw.f64(i * 16, 1);
  for (unsigned i = 0; i < 64; ++i) {
    reference.f64(i * 16, phase.real()); reference.f64(i * 16 + 8, -phase.imag());
  }
  auto maps = writer(rr::BlockKind::channelMaps), power = writer(rr::BlockKind::currentPower);
  for (unsigned channel = 0; channel < 2; ++channel)
    for (unsigned row = 0; row < 7; ++row) for (unsigned col = 0; col < 9; ++col) {
      const double amplitude = row == 3 && col == 4 ? 4 : 1;
      const auto offset = (channel * 63 + col * 7 + (row + 4) % 7) * 8;
      maps.f32(offset, float(amplitude * phase.real())); maps.f32(offset + 4, float(amplitude * phase.imag()));
      power.f64((row * 9 + col) * 8, amplitude * amplitude);
    }
  power.f64(0, 9.25);
  const auto& rawBytes = snapshot.blocks.at(unsigned(rr::BlockKind::rawIq) - 1).bytes;
  blah2::resident::Sha256 hash; hash.add(rawBytes.data(), rawBytes.size()); snapshot.identity.rawIqHash = hash.finish();
  replay::Sidecar sidecar; sidecar.identity = snapshot.identity;
  sidecar.sourceHash = c.replay.sourceHash; sidecar.rfHash = c.replay.rfHash;
  sidecar.captureEpoch = 3; sidecar.firstSample = 100 + sequence * 32; sidecar.endSample = sidecar.firstSample + 64;
  sidecar.tuningHz = 539000000; sidecar.gainCalibrationEpoch = 2; sidecar.sampleRateHz = 128000;
  sidecar.actualGainDb = c.replay.actualGainDb; return {sidecar, c, std::move(snapshot)};
}
void writeFrame(const fs::path& path, const replay::ValidatedSnapshot& frame) {
  const auto bytes = rr::encode(frame.snapshot, frame.config);
  { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    check(bool(out), "Fixture write failed"); }
  replay::writeSidecarExclusive(path, frame.sidecar);
}
}
int main(int argc, char** argv) try {
  if (argc == 3 && std::string(argv[1]) == "--fixtures") {
    check(fs::is_directory(argv[2]) && fs::is_empty(argv[2]), "Fixture destination must be empty");
    for (uint64_t i = 1; i <= 8; ++i) writeFrame(fs::path(argv[2]) / (std::to_string(i) + ".vwrps"), frame(i));
    return 0;
  }
  score::Scorer scorer(0); constexpr uint64_t target = 3 * 9 + 4;
  for (uint64_t i = 1; i <= 8; ++i) {
    auto sample = frame(i);
    // Exercise ABI seal/decoder independently of the map adapter.
    const auto bytes = rr::encode(sample.snapshot, sample.config);
    sample.snapshot = rr::decode(bytes.data(), bytes.size(), sample.config, sample.sidecar.identity);
    const auto surfaces = scorer.append(sample);
    check(surfaces.size() == 6, "Comparison omitted an arm");
    near(surfaces[0].power[0], 9.25, "Recorded power was squared again");
    near(surfaces[1].power[target], 32. / 1.001, "Diagonal statistic or channel-map layout differs");
    check(surfaces[2].power[target] < surfaces[1].power[target], "Shrinkage ignored correlated channels");
    for (size_t bank = 0; bank < 3; ++bank) {
      const unsigned depth = 2u << bank;
      check(surfaces[bank + 3].ready == (i >= depth), "Coherent depth warmup differs");
      if (i >= depth) {
        check(surfaces[bank + 3].valid[target], "Direct pilot did not preserve coherent target");
        near(surfaces[bank + 3].power[target], 32. * depth, "Pilot correction/depth normalization differs");
        const auto point = score::measure(surfaces[0], surfaces[bank + 3], target);
        check(point.available && point.trainingCells == 4, "Target comparison support differs");
        near(point.contrastDb, 10 * std::log10(16.), "Coherent target/local contrast differs");
      }
    }
  }
  rejects([&] { scorer.append(frame(8)); }, "Duplicate frame accepted");
  score::Scorer gaps(0); gaps.append(frame(1));
  rejects([&] { gaps.append(frame(3)); }, "Discontinuous sequence accepted");
  score::Scorer hops(0); hops.append(frame(1)); hops.append(frame(2));
  auto badHop = frame(3); ++badHop.sidecar.firstSample; ++badHop.sidecar.endSample;
  rejects([&] { hops.append(badHop); }, "Changed sample hop accepted");
  auto baseline = score::Surface{"baseline", 1, 9, true, std::vector<double>(9, 1), std::vector<uint8_t>(9, 1)};
  auto arm = baseline; baseline.power[4] = arm.power[4] = 16;
  arm.valid[0] = 0; baseline.power[0] = 1000;
  const auto matched = score::measure(baseline, arm, 4);
  check(matched.trainingCells == 3, "Invalid arm cell entered either background");
  near(matched.baselineLocalMean, 1., "Paired baseline did not use shared support");
  near(matched.contrastDb, matched.baselineContrastDb, "Identical matched powers changed contrast");
  arm.valid[1] = arm.valid[7] = 0;
  check(!score::measure(baseline, arm, 4).available, "Insufficient training background was scored");
  arm.ready = false; check(!score::measure(baseline, arm, 4).available, "Warmup fabricated a score");
  std::cout << "COMPLETE resident weak evidence CPU contract gpu_execution=0 detector_mutation=0\n"; return 0;
} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
