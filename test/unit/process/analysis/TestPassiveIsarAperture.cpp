#include "process/integration/PassiveIsarAperture.h"
#include "ResidentReplayFixture.h"
#include "process/ambiguity/Ambiguity.h"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <type_traits>
#include <unistd.h>

namespace cr = blah2::integration::coherent_replay;
namespace rr = blah2::resident_replay;
using Complex = std::complex<double>;
using Migration = blah2::integration::RangeMigrationReplay;
namespace {
constexpr double tau = 6.283185307179586476925286766559;
constexpr uint32_t samples = 1088, rateHz = 1088, rows = 17, columns = 49, correlation = 64, hop = 512;
constexpr uint64_t startSample = 9007199254741001ULL;
constexpr double carrier = 4352000.;
void check(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }
template<class F> void rejects(F operation, const char* why) {
  bool rejected = false; try { operation(); } catch (const std::exception&) { rejected = true; } check(rejected, why);
}
void close(Complex actual, Complex expected, double tolerance, const char* why) {
  check(std::abs(actual - expected) <= tolerance * std::max(1., std::abs(expected)), why);
}
// Independent continuous Fourier synthesis; no candidate interpolation kernel.
Complex waveform(double sample) {
  Complex value{};
  for (int k = 1; k <= 19; ++k)
    value += std::polar(1., tau * (.013 * k - .132) * sample + .173 * k * k);
  return value / std::sqrt(19.);
}
Complex target(double sample, bool pair) {
  Complex value{};
  for (int k = 0; k < (pair ? 2 : 1); ++k) {
    const double doppler = k ? -2. : 2.;
    const double delay = -doppler * sample / carrier; // fD = -fc * delay_rate / fs.
    value += waveform(sample - delay) * std::polar(k ? .7 : 1., -tau * carrier * delay / rateHz + .3 * k);
  }
  return value;
}
blah2::resident::Config configuration() {
  blah2::resident::Config c; auto& g = c.geometry;
  g.range = 90; g.doppler = rows; g.delays = columns; g.channels = 2;
  g.delayMin = -24; g.clutterSamples = samples; g.clutterBins = 4;
  c.inputChannels = 2; c.correlation = correlation; c.sampleRate = rateHz; c.epoch = 9;
  c.surveillanceChannels = {0, 1}; c.reference.channels = {0, 1}; c.reference.analysisSamples = samples;
  c.reference.bandwidthHz = 64; c.reference.centreHz = carrier; c.spectrumBins = 17;
  for (int d = -24; d <= 24; ++d) c.delayAxis.push_back(d);
  for (int f = -8; f <= 8; ++f) c.dopplerAxis.push_back(f);
  c.detector.enabled = true; c.detector.train = 2; c.detector.pfa = .001;
  c.detector.centroidDopplerResolution = 2; c.device = "synthetic-aperture-contract-only";
  rr::test::enable(c); c.replay.frequencyHz = carrier; return c;
}
blah2::integration::DirectPathCoherentExperiment::Config request() {
  blah2::integration::DirectPathCoherentExperiment::Config c;
  c.channelIds = {0, 1}; c.anchorChannelId = 0; c.spatialCoefficients = {0., 1.};
  c.migrationRates = {0., -.0005, .0005}; return c;
}
cr::ValidatedSnapshot fixture(const std::filesystem::path& path, uint64_t sequence, bool pair,
    bool goodPilot = true, uint64_t firstOverride = 0) {
  auto c = configuration(); auto s = rr::test::fixture(c, 1, {sequence, sequence, sequence + 1000});
  std::deque<Complex> reference; IqData surveillance(samples);
  auto writer = [&](rr::BlockKind kind) { auto& b = s.blocks.at(uint32_t(kind) - 1).bytes;
    return blah2::resident::wire::Writer{{b.data(), b.size()}}; };
  auto raw = writer(rr::BlockKind::rawIq), ref = writer(rr::BlockKind::referenceIq);
  const double gauge = .29 * sequence;
  for (uint32_t n = 0; n < samples; ++n) {
    const double global = double((sequence - 1) * hop + n);
    const auto x = waveform(global), y = target(global, pair), r = x * std::polar(1., gauge);
    reference.push_back(r); surveillance.push_back(y);
    raw.f64(n * 16, goodPilot ? x.real() : 0); raw.f64(n * 16 + 8, goodPilot ? x.imag() : 0);
    raw.f64((samples + n) * 16, y.real()); raw.f64((samples + n) * 16 + 8, y.imag());
    ref.f64(n * 16, r.real()); ref.f64(n * 16 + 8, r.imag());
  }
  Ambiguity ambiguity(-24, 24, -8, 8, rateHz, samples, true);
  check(ambiguity.get_n_corr() == correlation, "Fixture correlation geometry");
  const auto caf = ambiguity.process(reference, &surveillance);
  auto maps = writer(rr::BlockKind::channelMaps);
  for (uint32_t row = 0; row < rows; ++row) for (uint32_t col = 0; col < columns; ++col) {
    const auto at = (uint64_t(columns + col) * rows + (row + rows / 2 + 1) % rows) * 8;
    maps.f32(at, float(caf->data[row][col].real())); maps.f32(at + 4, float(caf->data[row][col].imag()));
  }
  const auto& rawBytes = s.blocks[uint32_t(rr::BlockKind::rawIq) - 1].bytes;
  blah2::Sha256 hash; hash.update(rawBytes.data(), rawBytes.size()); s.identity.rawIqHash = hash.finishBytes();
  const auto bytes = rr::encode(s, c);
  { std::ofstream file(path, std::ios::binary); file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); check(bool(file), "Fixture write"); }
  cr::Sidecar side; side.identity = s.identity; side.sourceHash = c.replay.sourceHash; side.rfHash = c.replay.rfHash;
  side.captureEpoch = 3; side.firstSample = firstOverride ? firstOverride : startSample + (sequence - 1) * hop;
  side.endSample = side.firstSample + samples; side.tuningHz = uint64_t(carrier); side.sampleRateHz = rateHz;
  side.gainCalibrationEpoch = 2; side.actualGainDb = c.replay.actualGainDb;
  cr::writeSidecarExclusive(path, side); return cr::loadValidatedSnapshot(path);
}
Complex bruteBlock(uint64_t first, int delay, bool pair, double center = 0, uint64_t frameStart = 0) {
  Complex sum{};
  for (int n = 0; n < int(correlation); ++n) if (n - delay >= 0 && n - delay < int(correlation)) {
    const double global = double(first + n), refSample = double(first + n - delay);
    sum += target(global, pair) * std::conj(waveform(refSample) *
      std::polar(1., tau * center * (refSample - double(frameStart)) / rateHz));
  }
  return sum;
}
void numericalCenter() {
  for (int center : {-3, 0, 3}) {
    std::deque<Complex> ref; IqData y(samples);
    for (uint32_t n = 0; n < samples; ++n) { ref.push_back(waveform(n)); y.push_back(target(n, true)); }
    Ambiguity ambiguity(-24, 24, center - 8, center + 8, rateHz, samples, true);
    const auto caf = ambiguity.process(ref, &y);
    const auto data = Migration::process(*caf, rateHz, correlation, {0., .5, -.5}, true);
    for (uint32_t b = 0; b < rows; ++b) for (int d : {-9, 0, 7})
      close(data.rangeSlowTime[b * columns + d + 24], bruteBlock(b * correlation, d, true, center),
        1e-10, "Independent continuous-IQ block correlation/center oracle");
    for (const auto& bank : data.banks) {
      double gain = 0;
      for (const auto& kernel : bank.interpolationKernels) for (double w : kernel.weights) gain += w * w / rows;
      check(std::abs(gain - bank.whiteNoiseGain) < 1e-12, "Interpolation noise gain mismatch");
      for (uint32_t f = 0; f < rows; ++f) for (uint32_t col = 0; col < data.columns; ++col) {
        Complex expected{};
        for (uint32_t b = 0; b < rows; ++b) expected += bank.rangeSlowTime[b * data.columns + col] *
          std::polar(1., -tau * (double(f) - rows / 2) * b / rows);
        close(bank.cells[f * data.columns + col], expected, 1e-10, "Aperture export changed migration DFT/gauge");
      }
    }
  }
}
void endToEnd(const std::filesystem::path& directory, bool pair, std::ostream* evidence = nullptr) {
  cr::ExperimentReader reader(request()); cr::PassiveIsarAperture aperture;
  std::vector<double> frequencies; for (int i = -16; i <= 16; ++i) frequencies.push_back(.25 * i);
  for (uint64_t seq = 1; seq <= 8; ++seq) {
    const auto sealed = fixture(directory / (std::to_string(pair) + "-" + std::to_string(seq) + ".vwrps"), seq, pair);
    const auto frame = reader.aperture(sealed);
    static_assert(std::is_const_v<std::remove_reference_t<decltype(*frame)>>, "Aperture is mutable");
    check(frame->source.firstSample == startSample + (seq - 1) * hop && frame->blockHashes.size() == 20,
      "Exact sample/config/stage lineage missing");
    close(frame->gaugeCorrection, std::polar(1., .29 * seq), 1e-12, "Pilot gauge recovery");
    for (uint32_t b = 0; b < rows; ++b)
      close(frame->migration[1].rangeSlowTime[b * columns + 24] * frame->gaugeCorrection,
        bruteBlock((seq - 1) * hop + b * correlation, 0, pair), 2e-6, "Sealed FP32 CAF independent complex oracle");
    aperture.append(frame);
    check(aperture.uniqueBlocks() == rows + (seq - 1) * hop / correlation, "Overlapping raw blocks received duplicate credit");
    rejects([&] { aperture.append(frame); }, "Repeated frame admitted");
    if (seq == 2 || seq == 4 || seq == 8) {
      const auto spectrum = aperture.spectrum(0, 0., frequencies);
      std::vector<Complex> oracle;
      for (size_t k = 0; k < frequencies.size(); ++k) {
        Complex expected{};
        for (uint64_t b = 0; b < spectrum.uniqueBlocks; ++b) expected += bruteBlock(b * correlation, 0, pair) *
          std::polar(1., -tau * frequencies[k] * (b * correlation + .5 * (correlation - 1)) / rateHz);
        expected /= std::sqrt(double(spectrum.uniqueBlocks));
        close(spectrum.amplitude[k], expected, 3e-6, "2/4/8-CPI independent PSF/complex spectrum oracle");
        oracle.push_back(expected);
      }
      const auto peak = std::max_element(spectrum.amplitude.begin(), spectrum.amplitude.end(),
        [](auto a, auto b) { return std::norm(a) < std::norm(b); });
      check(frequencies[size_t(peak - spectrum.amplitude.begin())] == 2., "Point PSF shifted");
      if (pair) check(std::abs(spectrum.amplitude[8]) > .5 * std::abs(spectrum.amplitude[24]), "Resolved second scatterer lost");
      rejects([&] { aperture.spectrum(0, 100., frequencies); }, "Missing interpolation support padded");
      rejects([&] { aperture.spectrum(0, 0., {0., 2., 1.}); }, "Reordered spectrum admitted");
      rejects([&] { aperture.spectrum(0, 0., {-30., 0., 30.}); }, "Aliased spectrum admitted");
      if (evidence) {
        *evidence << std::setprecision(17) << "{\"schema\":\"vectorwarp-synthetic-aperture-psf-v1\",\"synthetic_only\":true,\"metric_image\":false,\"scatterers\":"
          << (pair ? 2 : 1) << ",\"frames\":" << seq << ",\"unique_blocks\":" << spectrum.uniqueBlocks
          << ",\"sample_rate_hz\":" << rateHz << ",\"correlation_samples\":" << correlation
          << ",\"first_sample\":\"" << startSample << "\",\"bins\":[";
        for (size_t k = 0; k < frequencies.size(); ++k) {
          if (k) *evidence << ',';
          *evidence << '[' << frequencies[k] << ',' << spectrum.amplitude[k].real() << ','
            << spectrum.amplitude[k].imag() << ',' << oracle[k].real() << ',' << oracle[k].imag() << ']';
        }
        *evidence << "]}\n";
      }
    }
  }
  const auto lost = reader.aperture(fixture(directory / (std::to_string(pair) + "-lost.vwrps"), 9, pair, false));
  check(!lost->phaseQualified, "Pilot loss marked coherent");
  cr::PassiveIsarAperture fresh;
  rejects([&] { fresh.append(lost); }, "Power fallback manufactured aperture phase");
  const auto gap = reader.aperture(fixture(directory / (std::to_string(pair) + "-gap.vwrps"), 2, pair, true, startSample + 100 * hop));
  fresh.append(aperture.entries()[0].frame);
  rejects([&] { fresh.append(gap); }, "Raw gap crossed");
  const auto misaligned = reader.aperture(fixture(directory / (std::to_string(pair) + "-unaligned.vwrps"), 2, pair, true, startSample + hop + 1));
  rejects([&] { fresh.append(misaligned); }, "Partial block overlap was silently double-counted");
}
}
int main(int argc, char** argv) try {
  numericalCenter();
  check(argc == 1 || (argc == 3 && std::string(argv[1]) == "--evidence-dir"), "Usage: testPassiveIsarAperture [--evidence-dir NEW_DIRECTORY]");
  std::filesystem::path directory;
  if (argc == 3) {
    directory = argv[2]; check(std::filesystem::create_directory(directory), "Evidence directory must be new");
  } else {
    auto temporary = (std::filesystem::temp_directory_path() / "passive-isar-contract-XXXXXX").string();
    std::vector<char> name(temporary.begin(), temporary.end()); name.push_back(0);
    check(mkdtemp(name.data()) != nullptr, "Unique fixture directory"); directory = name.data();
  }
  std::ofstream evidence;
  if (argc == 3) { evidence.open(directory / "synthetic-psf.jsonl"); check(bool(evidence), "Evidence file open"); }
  endToEnd(directory, false, argc == 3 ? &evidence : nullptr);
  endToEnd(directory, true, argc == 3 ? &evidence : nullptr);
  if (argc == 3) { evidence.flush(); check(bool(evidence), "Evidence write"); }
  else std::filesystem::remove_all(directory); // Only this test's newly created fixture directory.
  std::cout << "Passive ISAR aperture: immutable phase, independent IQ/PSF, overlap, center and refusal contracts passed\n";
  return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
