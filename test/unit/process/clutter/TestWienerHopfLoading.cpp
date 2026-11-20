#include "process/clutter/WienerHopf.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <fstream>
#include "../../../../tools/WienerHopfReplayContract.h"

using Complex = std::complex<double>;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void fillFullRank(IqData& reference, IqData& surveillance) {
  for (unsigned i = 0; i < 128; ++i) {
    const Complex signal(std::cos(.17 * i) + .2 * std::sin(.03 * i), std::sin(.11 * i));
    reference.push_back(signal);
    surveillance.push_back(signal * Complex(.8, -.2) + Complex(.01 * i, -.02 * i));
  }
}
void verifyExecutableIdentity(const char* argument) {
  namespace fs = std::filesystem;
  using blah2::integration::cancellation_replay::executablePath;
  using blah2::integration::cancellation_replay::runningExecutablePath;
  char temporary[] = ".wiener-hopf-path-XXXXXX";
  const auto created = ::mkdtemp(temporary);
  require(created != nullptr, "Cannot create executable identity fixture");
  const auto root = fs::absolute(created), early = root / "early", late = root / "late";
  struct Cleanup {
    fs::path root, early, late;
    ~Cleanup() {
      std::error_code error;
      fs::remove(early / "runner", error); fs::remove(late / "runner", error);
      fs::remove(early, error); fs::remove(late, error); fs::remove(root, error);
    }
  } cleanup{root, early, late};
  fs::create_directory(early); fs::create_directory(late);
  { std::ofstream shadow(early / "runner"); shadow << "not executable\n"; }
  { std::ofstream binary(late / "runner"); binary << "#!/bin/sh\nexit 0\n"; }
  fs::permissions(early / "runner", fs::perms::owner_read | fs::perms::owner_write);
  fs::permissions(late / "runner", fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec);
  const auto search = early.string() + ":" + late.string();
  require(executablePath("runner", search) == fs::canonical(late / "runner"),
    "Non-executable PATH shadow changed executable provenance");
  bool rejected = false;
  try { (void)executablePath((early / "runner").string(), search); }
  catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "Explicit non-executable file accepted as executable");
#if defined(__linux__) || defined(__APPLE__)
  require(fs::equivalent(runningExecutablePath("runner", search), fs::canonical(argument)),
    "Running executable identity trusted a substituted argv[0]/PATH");
#else
  (void)argument;
#endif
}
void verifyPaths(unsigned count, int first, int last, double loading) {
  IqData reference(count), direct(count), shared(count), widenedReference(count), widenedOutput(count);
  std::vector<std::complex<float>> floats(count), surveillance(count);
  uint32_t state = 19;
  for (unsigned i = 0; i < count; ++i) {
    state = state * 1664525u + 1013904223u;
    const double real = double(state) / UINT32_MAX - .5;
    state = state * 1664525u + 1013904223u;
    const Complex value(real, double(state) / UINT32_MAX - .5);
    const Complex target = .7 * value + Complex(.01 * std::sin(.13 * i), .02 * std::cos(.07 * i));
    reference.push_back(value); direct.push_back(target); shared.push_back(target);
    floats[i] = value; surveillance[i] = target;
    widenedReference.push_back(Complex(floats[i])); widenedOutput.push_back(Complex(surveillance[i]));
  }
  const auto frozen = reference.view_data();
  WienerHopf a(first, last, count, WienerHopf::Workspace::Pi, loading);
  WienerHopf b(first, last, count, WienerHopf::Workspace::SharedReference, loading);
  WienerHopf::PreparedReference prepared(first, last, count, loading);
  require(prepared.prepare(reference), "Full-rank prepared reference failed");
  require(a.process(&reference, &direct) && b.process(prepared, &shared), "Full-rank loaded solve failed");
  require(prepared.cholesky_diagonal_ratio() > 0 && prepared.cholesky_diagonal_ratio() <= 1,
    "Invalid factor diagonal spread diagnostic");
  for (unsigned i = 0; i < count; ++i)
    require(std::abs(direct.view_data()[i] - shared.view_data()[i]) < 1e-9,
      "Full/blocked prepared/unprepared loading differs");
  require(a.process_float(floats.data(), surveillance.data(), count), "Float loading failed");
  const auto floatView = a.filtered_view();
  require(b.process(prepared, &shared), "Repeated prepared solve failed");
  WienerHopf widened(first, last, count, WienerHopf::Workspace::Pi, loading);
  require(widened.process(&widenedReference, &widenedOutput), "Widened float loading failed");
  for (unsigned i = 0; i < count; ++i)
    require(std::abs(floatView.filteredSurveillance[i] - widenedOutput.view_data()[i]) < 1e-10,
      "Float and widened-double loading differ");
  require(reference.view_data() == frozen, "Cancellation mutated frozen reference");
}
void verifyCohort() {
  using blah2::integration::cancellation_replay::Cohort;
  using blah2::integration::coherent_replay::Sidecar;
  Sidecar a; a.firstSample = 100; a.endSample = 228;
  a.identity.logicalFrame = a.identity.generation = a.identity.consumedSequence = 1;
  const auto frozen = blah2::integration::cancellation_replay::frameIdentity(a);
  blah2::integration::cancellation_replay::requireFrozen(a, frozen);
  for (unsigned mutation = 0; mutation < 4; ++mutation) {
    auto replacement = a;
    if (mutation == 0) ++replacement.captureEpoch;
    if (mutation == 1) { ++replacement.firstSample; ++replacement.endSample; }
    if (mutation == 2) ++replacement.gainCalibrationEpoch;
    if (mutation == 3) replacement.actualGainDb.push_back(1.0);
    require(replacement.snapshotHash == a.snapshotHash, "Fixture unexpectedly changed snapshot");
    bool rejected = false;
    try { blah2::integration::cancellation_replay::requireFrozen(replacement, frozen); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Resealed sidecar replacement passed frozen cohort identity");
  }
  auto b = a; b.firstSample += 64; b.endSample += 64;
  ++b.identity.logicalFrame; ++b.identity.generation; ++b.identity.consumedSequence;
  Cohort accepted; accepted.append(a, 128); accepted.append(b, 128);
  for (unsigned mutation = 0; mutation < 6; ++mutation) {
    auto bad = b;
    if (mutation == 0) bad = a;
    if (mutation == 1) ++bad.identity.consumedSequence;
    if (mutation == 2) ++bad.identity.configHash[0];
    if (mutation == 3) ++bad.endSample;
    if (mutation == 4) { bad.firstSample += 128; bad.endSample += 128; }
    if (mutation == 5) ++bad.gainCalibrationEpoch;
    Cohort rejected; rejected.append(a, 128); bool threw = false;
    try { rejected.append(bad, 128); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "Invalid cancellation cohort accepted");
  }
  auto c = b; c.firstSample += 63; c.endSample += 63;
  ++c.identity.logicalFrame; ++c.identity.generation; ++c.identity.consumedSequence;
  bool threw = false; try { accepted.append(c, 128); } catch (const std::invalid_argument&) { threw = true; }
  require(threw, "Changed sample hop accepted");
}
}

int main(int, char** argv) {
  try {
    verifyExecutableIdentity(argv[0]);
    using blah2::integration::cancellation_replay::executablePath;
    require(executablePath("/bin/sh", "").is_absolute(), "Absolute executable path did not resolve");
    require(executablePath("sh", "/unavailable:/bin").is_absolute(), "PATH executable did not resolve");
    bool unavailableExecutable = false;
    try { (void)executablePath("missing-replay-executable", "/unavailable"); }
    catch (const std::runtime_error&) { unavailableExecutable = true; }
    require(unavailableExecutable, "Missing replay executable accepted");
    verifyCohort();
    for (const double loading : {0., .001, .01}) {
      // 128 exercises full FFT correlation; 17003 crosses blocked-correlation
      // and FIR boundaries and includes a partial final block.
      verifyPaths(128, -4, 4, loading); verifyPaths(17003, -4, 4, loading);
      IqData ref(128), out(128); fillFullRank(ref, out);
      WienerHopf single(0, 1, 128, WienerHopf::Workspace::Pi, loading);
      Complex cross{}; double energy = 0;
      const auto original = out.view_data();
      for (unsigned i = 0; i < 128; ++i) {
        cross += original[i] * std::conj(ref.view_data()[i]); energy += std::norm(ref.view_data()[i]);
      }
      const auto weight = cross / (energy * (1 + loading));
      require(single.process(&ref, &out), "Analytic one-tap solve failed");
      for (unsigned i = 0; i < 128; ++i)
        require(std::abs(out.view_data()[i] - (original[i] - weight * ref.view_data()[i])) < 1e-10,
          "Loaded solution differs from analytic regularized objective");
    }
    constexpr unsigned samples = 128;
    IqData reference(samples), legacyOutput(samples), zeroOutput(samples);
    fillFullRank(reference, legacyOutput); for (const auto value : legacyOutput.view_data()) zeroOutput.push_back(value);
    WienerHopf legacy(-4, 4, samples);
    WienerHopf explicitZero(-4, 4, samples, WienerHopf::Workspace::Pi, 0);
    require(legacy.process(&reference, &legacyOutput) && explicitZero.process(&reference, &zeroOutput),
      "Zero-loading full-rank fixture rejected");
    for (unsigned i = 0; i < samples; ++i)
      require(std::abs(legacyOutput.view_data()[i] - zeroOutput.view_data()[i]) < 1e-12,
        "Explicit zero loading changed legacy output");

    IqData rankOne(samples), retained(samples), loaded(samples), preparedOutput(samples), unpreparedOutput(samples);
    for (unsigned i = 0; i < samples; ++i) {
      rankOne.push_back({1, 0}); retained.push_back({.7, -.2}); loaded.push_back({.7, -.2});
      preparedOutput.push_back({.7, -.2}); unpreparedOutput.push_back({.7, -.2});
    }
    const auto unchanged = retained.view_data();
    WienerHopf singular(-4, 4, samples);
    require(!singular.process(&rankOne, &retained) && retained.view_data() == unchanged,
      "Unloaded rank-one covariance was accepted");
    WienerHopf regularized(-4, 4, samples, WienerHopf::Workspace::Pi, .001);
    require(regularized.process(&rankOne, &loaded), "Positive loading did not regularize covariance");
    for (const auto value : loaded.view_data())
      require(std::isfinite(value.real()) && std::isfinite(value.imag()), "Regularized output is nonfinite");
    // Eight identical reference columns have one nonzero eigenvalue 8*R00.
    // After the seven-sample FIR startup, this is the exact regularized residual.
    for (unsigned i = 7; i < samples; ++i)
      require(std::abs(loaded.view_data()[i] - Complex(.7, -.2) * (.001 / (8 + .001))) < 1e-10,
        "Multi-tap loaded residual was undone or used a different objective");

    WienerHopf::PreparedReference prepared(-4, 4, samples, .001);
    require(prepared.prepare(rankOne), "Prepared positive loading rejected covariance");
    WienerHopf preparedFilter(-4, 4, samples, WienerHopf::Workspace::SharedReference, .001);
    WienerHopf unpreparedFilter(-4, 4, samples, WienerHopf::Workspace::Pi, .001);
    require(preparedFilter.process(prepared, &preparedOutput) && unpreparedFilter.process(&rankOne, &unpreparedOutput),
      "Prepared/unprepared positive loading rejected surveillance");
    for (unsigned i = 0; i < samples; ++i) {
      require(std::isfinite(preparedOutput.view_data()[i].real()) && std::isfinite(preparedOutput.view_data()[i].imag()),
        "Prepared regularized output is nonfinite");
      require(std::abs(preparedOutput.view_data()[i] - unpreparedOutput.view_data()[i]) < 1e-9,
        "Prepared and unprepared loading differ");
    }
    WienerHopf mismatch(-4, 4, samples, WienerHopf::Workspace::SharedReference);
    bool rejected = false; try { mismatch.process(prepared, &unpreparedOutput); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "Prepared loading mismatch accepted");
    for (const double invalid : {-1e-12, 1.000001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
      rejected = false; try { WienerHopf invalidFilter(-4, 4, samples, WienerHopf::Workspace::Pi, invalid); } catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Invalid filter loading accepted");
      rejected = false; try { WienerHopf::PreparedReference invalidReference(-4, 4, samples, invalid); } catch (const std::invalid_argument&) { rejected = true; }
      require(rejected, "Invalid prepared loading accepted");
    }
    std::cout << "PASS Wiener-Hopf relative diagonal loading CPU contract\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
