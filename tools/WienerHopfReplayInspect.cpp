#include "WienerHopfReplayContract.h"
#include "process/clutter/WienerHopf.h"
#include "process/ambiguity/GpuResidentHash.h"
#include "process/utility/FftwThreads.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace blah2::integration::coherent_replay;
using Hash = blah2::resident_replay::Hash;
using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
using Clock = std::chrono::steady_clock;
constexpr std::array<double, 3> lambdas{{0, .001, .01}};
constexpr std::array<const char*, 3> arms{{"A", "D", "E"}};
constexpr std::array<const char*, 5> missing{{
  "predeclared paired injected/uninjected grid and fixed-versus-refitted filter controls",
  "target/local-background regions independent of evaluated outcomes",
  "independent reference reset/warmup end-to-end replay of a selected arm",
  "disjoint calibration/evaluation whole-pass labels and adjudicated negative exposure",
  "paired downstream detector/association/track-continuity evaluation at equal empirical false-alarm exposure"}};
std::string hex(const Hash& hash) {
  constexpr char digits[] = "0123456789abcdef"; std::string result;
  for (auto byte : hash) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
Hash digest(const std::vector<uint8_t>& bytes) {
  blah2::resident::Sha256 hash; hash.add(bytes.data(), bytes.size()); return hash.finish();
}
std::string executableHash(const char* argument) {
  const char* path = std::getenv("PATH");
  std::ifstream input(blah2::integration::cancellation_replay::runningExecutablePath(
    argument, path ? path : ""), std::ios::binary);
  if (!input) throw std::runtime_error("Cannot bind replay executable provenance");
  blah2::resident::Sha256 hash; std::array<char, 65536> buffer{};
  while (input.read(buffer.data(), buffer.size()) || input.gcount())
    hash.add(buffer.data(), size_t(input.gcount()));
  if (!input.eof()) throw std::runtime_error("Cannot read replay executable provenance");
  return hex(hash.finish());
}
void string(Writer& w, const char* key, const std::string& value) {
  w.Key(key); w.String(value.c_str(), rapidjson::SizeType(value.size()));
}
void number(Writer& w, const char* key, double value) {
  w.Key(key); if (std::isfinite(value)) w.Double(value); else w.Null();
}
void unavailable(Writer& w) {
  w.Key("missing_evidence"); w.StartArray(); for (const auto* item : missing) w.String(item); w.EndArray();
  for (const auto* key : {"differential_echo_retention", "target_local_background_contrast",
      "pfa", "false_track_delta", "whole_pass_performance", "track_continuity"}) {
    w.Key(key); w.Null();
  }
  for (const auto* key : {"promotion_authorized", "detector_mutation", "gpu_execution"}) {
    w.Key(key); w.Bool(false);
  }
}
double power(const IqData& data) {
  double total = 0; for (const auto value : data.view_data()) total += std::norm(value);
  return total / double(data.view_data().size());
}
double errorPower(const IqData& a, const IqData& b) {
  double total = 0;
  for (size_t i = 0; i < a.view_data().size(); ++i) total += std::norm(a.view_data()[i] - b.view_data()[i]);
  return total / double(a.view_data().size());
}
void loadIq(IqData& result, const std::vector<uint8_t>& bytes, size_t offset, uint32_t samples) {
  if (offset > bytes.size() || bytes.size() - offset < size_t(samples) * 16)
    throw std::invalid_argument("Missing evidence: snapshot IQ block layout differs");
  for (uint32_t i = 0; i < samples; ++i) {
    double real = 0, imag = 0;
    std::memcpy(&real, bytes.data() + offset + size_t(i) * 16, 8);
    std::memcpy(&imag, bytes.data() + offset + size_t(i) * 16 + 8, 8);
    if (!std::isfinite(real) || !std::isfinite(imag))
      throw std::invalid_argument("Missing evidence: snapshot IQ contains nonfinite value");
    result.push_back({real, imag});
  }
}
const std::vector<uint8_t>& block(const ValidatedSnapshot& frame, blah2::resident_replay::BlockKind kind) {
  return frame.snapshot.blocks.at(uint32_t(kind) - 1).bytes;
}
void geometry(const ValidatedSnapshot& frame) {
  const auto& g = frame.config.geometry;
  if (!g.clutterSamples || !g.clutterBins || g.clutterBins > g.clutterSamples ||
      g.clutterSamples > 2000000 || g.clutterBins > 1024 || g.channels > 8 ||
      int64_t(g.clutterDelayMin) + g.clutterBins > INT32_MAX)
    throw std::invalid_argument("Cancellation replay exceeds 2M samples/1024 taps/8 channels bound");
}
double milliseconds(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
}

int main(int argc, char** argv) {
  std::string activePath;
  try {
    const bool diagnostic = argc > 1 && std::string(argv[1]) == "--diagnostic-only";
    const int first = diagnostic ? 2 : 1;
    if (argc <= first || argc - first > 20) {
      std::cerr << "Usage: wienerHopfReplayInspect [--diagnostic-only] FRAME.vwrps... (1..20 ordered frames)\n"
        << "Without --diagnostic-only, unavailable evaluation evidence is a hard failure.\n";
      return 2;
    }
    // Validate the entire cohort before solving; retain hashes, not all payloads.
    blah2::integration::cancellation_replay::Cohort cohort;
    std::vector<Hash> frameHashes; blah2::resident::Sha256 cohortHash;
    for (int argument = first; argument < argc; ++argument) {
      activePath = argv[argument]; const auto frame = loadValidatedSnapshot(activePath);
      geometry(frame); cohort.append(frame.sidecar, frame.config.geometry.clutterSamples);
      frameHashes.push_back(blah2::integration::cancellation_replay::frameIdentity(frame.sidecar));
      cohortHash.add(frameHashes.back().data(), frameHashes.back().size());
    }
    const auto cohortDigest = hex(cohortHash.finish());
    rapidjson::StringBuffer manifest; Writer mw(manifest); mw.StartObject();
    string(mw, "record", "manifest"); string(mw, "status", diagnostic ? "diagnostic_only" : "blocked_missing_evidence");
    string(mw, "cohort_sha256", cohortDigest); string(mw, "replay_executable_sha256", executableHash(argv[0]));
    string(mw, "reference_policy", "frozen recorded reference IQ and applied checkpoint weights; no reference refit");
    string(mw, "solve", "CPU full-CPI circular correlation; (R + lambda*real(R00)*I)h=b; full linear FIR");
    mw.Key("frames"); mw.Uint(unsigned(frameHashes.size())); mw.Key("arms"); mw.StartArray();
    for (size_t a = 0; a < arms.size(); ++a) {
      mw.StartObject(); string(mw, "arm", arms[a]); number(mw, "lambda", lambdas[a]); mw.EndObject();
    } mw.EndArray(); unavailable(mw); mw.EndObject(); std::cout << manifest.GetString() << '\n';
    if (!diagnostic) return 3;
    if (!fftw_init_threads()) throw std::runtime_error("FFTW threads initialization failed");
    blah2::set_fftw_planner_threads(1);
    unsigned rejected = 0, measurements = 0;
    for (int argument = first; argument < argc; ++argument) {
      activePath = argv[argument]; const auto frame = loadValidatedSnapshot(activePath);
      blah2::integration::cancellation_replay::requireFrozen(frame.sidecar, frameHashes[size_t(argument - first)]);
      const auto& g = frame.config.geometry;
      const auto& referenceBytes = block(frame, blah2::resident_replay::BlockKind::referenceIq);
      const auto& raw = block(frame, blah2::resident_replay::BlockKind::rawIq);
      const auto& saved = block(frame, blah2::resident_replay::BlockKind::filteredIq);
      IqData reference(g.clutterSamples); loadIq(reference, referenceBytes, 0, g.clutterSamples);
      const auto frozenReference = reference.view_data();
      const auto referenceHash = hex(digest(referenceBytes));
      const auto weightsHash = hex(digest(block(frame, blah2::resident_replay::BlockKind::referenceCheckpoint)));
      std::vector<std::unique_ptr<IqData>> baselines; std::vector<bool> baselineAccepted;
      for (size_t arm = 0; arm < arms.size(); ++arm) {
        const auto prepareStart = Clock::now();
        WienerHopf::PreparedReference prepared(g.clutterDelayMin,
          g.clutterDelayMin + int32_t(g.clutterBins), g.clutterSamples, lambdas[arm]);
        const bool preparedOk = prepared.prepare(reference); const double prepareMs = milliseconds(prepareStart);
        WienerHopf filter(g.clutterDelayMin, g.clutterDelayMin + int32_t(g.clutterBins),
          g.clutterSamples, WienerHopf::Workspace::SharedReference, lambdas[arm]);
        for (uint32_t channel = 0; channel < g.channels; ++channel) {
          const size_t offset = size_t(channel) * g.clutterSamples * 16;
          auto filtered = std::make_unique<IqData>(g.clutterSamples); loadIq(*filtered, raw, offset, g.clutterSamples);
          const double pre = power(*filtered); const auto solveStart = Clock::now();
          const bool accepted = preparedOk && filter.process(prepared, filtered.get());
          const double solveMs = milliseconds(solveStart), post = power(*filtered);
          if (!std::isfinite(pre) || !std::isfinite(post)) throw std::runtime_error("Nonfinite cancellation power");
          ++measurements; rejected += !accepted;
          rapidjson::StringBuffer result; Writer w(result); w.StartObject(); string(w, "record", "measurement");
          string(w, "snapshot", activePath); string(w, "snapshot_sha256", hex(frame.sidecar.snapshotHash));
          string(w, "snapshot_and_lineage_sha256", hex(frameHashes[size_t(argument - first)]));
          string(w, "cohort_sha256", cohortDigest); string(w, "reference_sha256", referenceHash);
          string(w, "reference_checkpoint_sha256", weightsHash); string(w, "arm", arms[arm]);
          w.Key("frame"); w.Uint64(frame.sidecar.identity.logicalFrame);
          w.Key("first_sample"); w.Uint64(frame.sidecar.firstSample); w.Key("end_sample"); w.Uint64(frame.sidecar.endSample);
          w.Key("channel"); w.Uint(frame.config.surveillanceChannels.at(channel));
          w.Key("delay_min"); w.Int(g.clutterDelayMin); w.Key("delay_max_exclusive"); w.Int(g.clutterDelayMin + int32_t(g.clutterBins));
          number(w, "lambda", lambdas[arm]); number(w, "pre_power", pre); number(w, "residual_total_power", post);
          number(w, "residual_to_raw_ratio", pre > 0 ? post / pre : NAN);
          number(w, "prepare_ms_shared_across_channels", prepareMs); number(w, "solve_filter_ms", solveMs);
          number(w, "cholesky_diagonal_ratio_not_rcond", prepared.cholesky_diagonal_ratio());
          w.Key("solve_accepted"); w.Bool(accepted);
          if (arm == 0) {
            IqData repeated(g.clutterSamples), gpu(g.clutterSamples);
            loadIq(repeated, raw, offset, g.clutterSamples); loadIq(gpu, saved, offset, g.clutterSamples);
            const bool repeatAccepted = preparedOk && filter.process(prepared, &repeated);
            if (repeatAccepted != accepted || repeated.view_data() != filtered->view_data())
              throw std::runtime_error("Immutable CPU baseline repeat changed");
            number(w, "saved_native_baseline_error_over_raw", pre > 0 ? errorPower(*filtered, gpu) / pre : NAN);
            baselines.push_back(std::move(filtered)); baselineAccepted.push_back(accepted);
          } else {
            const bool paired = accepted && baselineAccepted[channel]; w.Key("paired_baseline_accepted"); w.Bool(paired);
            number(w, "residual_difference_power_from_A", paired ? errorPower(*filtered, *baselines[channel]) : NAN);
            const double basePower = power(*baselines[channel]);
            number(w, "residual_power_ratio_to_A", paired && basePower > 0 ? post / basePower : NAN);
          }
          w.EndObject(); std::cout << result.GetString() << '\n';
        }
      }
      if (reference.view_data() != frozenReference) throw std::runtime_error("Frozen reference was mutated");
    }
    rapidjson::StringBuffer summary; Writer w(summary); w.StartObject(); string(w, "record", "summary");
    string(w, "status", "diagnostic_complete_experiment_incomplete"); string(w, "cohort_sha256", cohortDigest);
    w.Key("measurements"); w.Uint(measurements); w.Key("solve_rejections"); w.Uint(rejected);
    unavailable(w); w.EndObject(); std::cout << summary.GetString() << '\n'; return 0;
  } catch (const std::exception& error) {
    rapidjson::StringBuffer result; Writer w(result); w.StartObject(); string(w, "record", "failure");
    string(w, "status", "blocked_invalid_or_missing_evidence"); string(w, "snapshot", activePath);
    string(w, "reason", error.what()); w.Key("promotion_authorized"); w.Bool(false);
    w.EndObject(); std::cout << result.GetString() << '\n'; return 1;
  }
}
