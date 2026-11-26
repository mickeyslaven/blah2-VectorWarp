// Offline synthetic acceptance driver and CTest consumer. No RF input, live
// worker, target-selected readout, or alternative selector mathematics.
#include "process/integration/CoherentFamilySelector.h"
#include "process/ambiguity/Ambiguity.h"
#include "ResidentReplayFixture.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

namespace {
using Experiment = blah2::integration::DirectPathCoherentExperiment;
using Selector = blah2::integration::CoherentFamilySelector;
using Complex = Experiment::Complex;
namespace cr = blah2::integration::coherent_replay;
namespace rr = blah2::resident_replay;
using Json = rapidjson::Writer<rapidjson::StringBuffer>;
constexpr uint32_t rows = 33, columns = 64, samples = rows * 64, windowSamples = 8 * samples;
constexpr uint32_t fitWindows = 512, jointWindows = 128, evaluationWindows = 128;
constexpr uint64_t epoch = 248, origin = 9007199254741000ULL;
constexpr double carrier = 20000, motionRate = .5, tau = 6.283185307179586476925286766559;
constexpr std::array<double, 4> amplitudes{0, .025, .05, .1};
// Versioned generator identity freezes every draw, readout, availability and
// acceptance choice. Source/executable hashes remain necessary reproduction pins.
const std::string generator =
  "canonical-iq-selector-v1 mt19937_64 seeds=0x248a,0x248b,0x248c "
  "window-seed=split-seed+0x9e3779b97f4a7c15*index uniform=((rng>>11)+.5)*2^-53 "
  "reference=64-unit-random-phase-tones f=-.45+.9*(k+.5)/64 noise=complex-BoxMuller-unit-power "
  "delay=32+.5*(n-(7*samples+(samples-1)/2))/fs doppler=-carrier*.5/fs "
  "gauge=.37*(cpi+1) last-pilot-missing=index%4==0 reset-every-window epoch=248 origin=9007199254741000 "
  "fs=2112 samples=2112 hop=2112 corr=64 rows=33 delays=0..63 rates=0,-.5,.5 "
  "depths=2,4,8 phase-trials=4,8,16 motion=1,1 source-domain=delay>=18,abs(hz)>=2 "
  "coherence-pre-gate=0 fractional-delay=true row=-5Hz readouts=32,18 "
  "A=512 B=128 E=128 alpha=.05 amplitudes=0,.025,.05,.1 paired-noise "
  "null-exceedance-limit=20/128 Wilson95 per-readout-per-window no-cell-search "
  "unavailable=omit-known-mask boundary-missing-stratum=refuse no-seed-retries";

void require(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
std::string hash(const std::string& text) {
  blah2::Sha256 h; h.update(text.data(), text.size()); return h.finish();
}
template<class F> void record(F&& write) {
  rapidjson::StringBuffer buffer; Json json(buffer); json.StartObject(); write(json); json.EndObject();
  std::cout << buffer.GetString() << '\n';
}
void text(Json& json, const char* key, const std::string& value) {
  json.Key(key); json.String(value.c_str(), rapidjson::SizeType(value.size()));
}
void integer(Json& json, const char* key, uint64_t value) {
  // Raw counters and identities are strings; never round them through JSON FP64.
  text(json, key, std::to_string(value));
}
std::string executableHash(const char* executable) {
  std::ifstream input(std::filesystem::absolute(executable), std::ios::binary);
  require(bool(input), "Invoke the acceptance executable by its explicit path for hashing");
  blah2::Sha256 h; std::array<char, 65536> bytes{};
  while (input) { input.read(bytes.data(), bytes.size()); h.update(bytes.data(), size_t(input.gcount())); }
  require(input.eof(), "Executable hash read failed"); return h.finish();
}
blah2::resident::Config nativeConfig() {
  blah2::resident::Config c; c.inputChannels = 3; c.epoch = epoch;
  c.geometry.range = 128; c.geometry.doppler = rows; c.geometry.delays = columns;
  c.geometry.channels = 2; c.geometry.clutterSamples = samples; c.geometry.clutterBins = 4;
  c.correlation = 64; c.sampleRate = samples; c.surveillanceChannels = {1, 2};
  for (uint32_t col = 0; col < columns; ++col) c.delayAxis.push_back(int(col));
  for (int row = -16; row <= 16; ++row) c.dopplerAxis.push_back(row);
  c.reference.mode = blah2::resident::ReferenceMode::capturedDedicated;
  c.reference.channels = {0}; c.reference.analysisSamples = samples; c.reference.centreHz = carrier;
  c.reference.bandwidthHz = 1000; c.spectrumBins = 64; c.device = "synthetic-selector-contract";
  c.detector.enabled = true; c.detector.guard = 1; c.detector.train = 4; c.detector.pfa = .05;
  c.detector.centroidDopplerResolution = 1; c.detector.integrationCpis = 8;
  c.detector.integrationMotionBins = track_before_detect::encodeMotionBins(1, 1, true);
  c.detector.integrationBeamTolerance = UINT32_MAX;
  rr::test::enable(c); c.replay.frequencyHz = carrier; return c;
}
Selector::Contract contract(const blah2::resident::Config& native, int readoutDelay = 32) {
  Selector::Contract c; auto& e = c.experiment; auto& t = e.temporal;
  e.channelIds = {1, 2}; e.anchorChannelId = 1; e.spatialCoefficients = {0., 1.};
  t.maximumCpis = 8; t.delayMotionBins = t.dopplerMotionBins = 1;
  t.cpiHopSeconds = 1; t.minimumCoherence = 0; t.refineDopplerPhase = true;
  t.interpolateDelay = true; t.delayDriftPerHz = track_before_detect::delayDriftPerHz(samples, carrier);
  t.sourceDomain = {true, 18, 2};
  c.planSha256 = Experiment::planSha256(blah2::resident::wire::configHash(native));
  e.upstreamPlanSha256 = c.planSha256;
  // max midpoint shift <1 bin: canonical common crop is 8+ceil(shift)=9.
  for (int delay = 9; delay <= 54; ++delay) c.delay.push_back(delay);
  c.doppler.assign(native.dopplerAxis.begin(), native.dopplerAxis.end());
  c.row = 11; c.column = uint32_t(readoutDelay - c.delay.front()); c.rates = {0, -.5, .5};
  c.sampleRateHz = samples; c.cpiSamples = c.hopSamples = samples;
  c.fitWindows = fitWindows; c.jointWindows = jointWindows; c.evaluationWindows = evaluationWindows;
  c.alpha = .05; c.generatorSha256 = hash(generator);
  c.fitSplitSha256 = hash(generator + " A seed=0x248a indices=0..511");
  c.jointSplitSha256 = hash(generator + " B seed=0x248b indices=0..127"); return c;
}
struct IqWindow {
  std::vector<Complex> reference, noise, echo;
  explicit IqWindow(uint64_t seed, uint32_t index)
    : reference(windowSamples), noise(windowSamples), echo(windowSamples) {
    std::mt19937_64 rng(seed + 0x9e3779b97f4a7c15ULL * index);
    const auto uniform = [&] { return (double(rng() >> 11) + .5) * 0x1p-53; };
    const double firstDelay = 32 - motionRate * (7 * samples + .5 * (samples - 1)) / samples;
    for (uint32_t tone = 0; tone < 64; ++tone) {
      const double frequency = -.45 + .9 * (tone + .5) / 64, phase = tau * uniform();
      Complex direct = std::polar(.125, phase), delayed = std::polar(.125, phase - tau * frequency * firstDelay);
      const auto step = std::polar(1., tau * frequency);
      const auto echoStep = std::polar(1., tau * frequency * (1 - motionRate / samples));
      for (uint32_t n = 0; n < windowSamples; ++n) {
        reference[n] += direct; echo[n] += delayed; direct *= step; delayed *= echoStep;
      }
    }
    const auto phaseStep = std::polar(1., -tau * carrier * motionRate / (double(samples) * samples));
    Complex phase{1, 0};
    for (uint32_t n = 0; n < windowSamples; ++n) {
      const double radius = std::sqrt(-std::log(uniform()));
      noise[n] = std::polar(radius, tau * uniform()); echo[n] *= phase; phase *= phaseStep;
    }
  }
};
struct MeasuredWindow {
  Selector::Observation interior, boundary;
  std::string iqSha256;
  double currentCpiZeroRatePower = 0;
};

// Synthetic ingress only: the canonical Ambiguity owner produces the channel
// CAF and the existing ExperimentReader owns ALL migration/projection/history
// math. An authenticated RF adapter can supply the same three Result pointers
// to Selector::readout; it must not reuse this unsealed fixture ingress.
class CanonicalFixture {
  cr::ValidatedSnapshot fixture_;
  Ambiguity ambiguity_{0, 63, -16, 16, samples, samples, true};
public:
  explicit CanonicalFixture(const blah2::resident::Config& config) {
    fixture_.config = config; fixture_.snapshot = rr::test::fixture(config);
    auto& side = fixture_.sidecar; side.captureEpoch = epoch; side.sampleRateHz = samples;
    side.tuningHz = uint64_t(carrier); side.gainCalibrationEpoch = 1;
    side.sourceHash = config.replay.sourceHash; side.rfHash = config.replay.rfHash;
    side.actualGainDb = config.replay.actualGainDb;
    require(ambiguity_.get_n_doppler_bins() == rows && ambiguity_.get_n_corr() == 64,
      "Synthetic canonical CAF geometry changed");
  }
  MeasuredWindow run(const IqWindow& iq, uint32_t id, bool missingPilot, double amplitude,
      const Selector::Contract& plan, const Selector& interior, const Selector& boundary) {
    auto request = plan.experiment; request.migrationRates.assign(plan.rates.begin(), plan.rates.end());
    cr::ExperimentReader reader(request); blah2::Sha256 iqHash;
    std::vector<cr::ExperimentReader::RateResult> results;
    const auto writer = [&](rr::BlockKind kind) {
      auto& bytes = fixture_.snapshot.blocks.at(uint32_t(kind) - 1).bytes;
      return blah2::resident::wire::Writer{{bytes.data(), bytes.size()}};
    };
    for (uint32_t cpi = 0; cpi < 8; ++cpi) {
      std::deque<Complex> reference; IqData surveillance(samples);
      auto raw = writer(rr::BlockKind::rawIq), ref = writer(rr::BlockKind::referenceIq);
      const auto gauge = std::polar(1., .37 * (cpi + 1));
      for (uint32_t n = 0; n < samples; ++n) {
        const auto i = cpi * samples + n;
        const Complex x = iq.reference[i] * gauge, y = iq.noise[i] + amplitude * iq.echo[i];
        const Complex anchor = missingPilot && cpi == 7 ? Complex{} : iq.reference[i];
        reference.push_back(x); surveillance.push_back(y);
        raw.f64(n * 16, anchor.real()); raw.f64(n * 16 + 8, anchor.imag());
        raw.f64((samples + n) * 16, y.real()); raw.f64((samples + n) * 16 + 8, y.imag());
        ref.f64(n * 16, x.real()); ref.f64(n * 16 + 8, x.imag());
      }
      const auto caf = ambiguity_.process(reference, &surveillance);
      auto maps = writer(rr::BlockKind::channelMaps);
      // Channel zero is pilot-only and has zero projection coefficient.
      for (uint32_t row = 0; row < rows; ++row) for (uint32_t col = 0; col < columns; ++col) {
        const size_t native = (size_t(columns) + col) * rows + (row + rows / 2 + 1) % rows;
        maps.f32(native * 8, float(caf->data[row][col].real()));
        maps.f32(native * 8 + 4, float(caf->data[row][col].imag()));
      }
      auto& identity = fixture_.snapshot.identity;
      identity.logicalFrame = identity.generation = identity.consumedSequence = cpi + 1;
      const auto& rawBytes = fixture_.snapshot.blocks.at(uint32_t(rr::BlockKind::rawIq) - 1).bytes;
      const auto& refBytes = fixture_.snapshot.blocks.at(uint32_t(rr::BlockKind::referenceIq) - 1).bytes;
      blah2::Sha256 rawHash; rawHash.update(rawBytes.data(), rawBytes.size()); identity.rawIqHash = rawHash.finishBytes();
      iqHash.update(rawBytes.data(), rawBytes.size()); iqHash.update(refBytes.data(), refBytes.size());
      fixture_.sidecar.identity = identity;
      fixture_.sidecar.firstSample = origin + uint64_t(id) * windowSamples + cpi * samples;
      fixture_.sidecar.endSample = fixture_.sidecar.firstSample + samples;
      results = reader.appendAll(fixture_);
    }
    require(results.size() == 3, "Canonical reader omitted a migration family");
    const std::array<const Experiment::Result*, 3> inputs{&results[0].result, &results[1].result, &results[2].result};
    const Selector::Window window{epoch, origin + uint64_t(id) * windowSamples,
      origin + uint64_t(id + 1) * windowSamples};
    MeasuredWindow measured{interior.readout(inputs, window, interior.contractSha256()),
      boundary.readout(inputs, window, boundary.contractSha256()), iqHash.finish(),
      results[0].fallback->data[plan.row][plan.column].real()};
    for (uint32_t i = 0; i < 18; ++i) {
      const auto& f = measured.interior.families[i];
      require(f.available == (i < 9 || !missingPilot), "Unexpected interior family availability");
      require(measured.boundary.families[i].available == (i < 9), "Fractional tap crossed source-domain boundary");
      if (!f.available) continue;
      require(f.ratio == f.raw.power / f.rowMedian, "Family borrowed a normalization denominator");
      require(f.raw.lineage.firstSamples[0] == window.firstSample + 7 * samples &&
        f.raw.lineage.endSamples[0] == window.endSample && f.raw.lineage.newestSequence == 8,
        "Selector lost exact terminal source lineage");
    }
    return measured;
  }
};
struct Counts {
  uint32_t windows = 0, candidate = 0, nc = 0, candidateOnly = 0, ncOnly = 0;
  std::array<uint32_t, 18> available{}, unavailable{};
  void observe(const Selector::Observation& observation) {
    ++windows;
    for (uint32_t i = 0; i < 18; ++i)
      ++(observation.families[i].available ? available[i] : unavailable[i]);
  }
  void decision(const Selector::Decision& value) {
    candidate += value.candidateDetected; nc += value.noncoherentDetected;
    candidateOnly += value.candidateDetected && !value.noncoherentDetected;
    ncOnly += value.noncoherentDetected && !value.candidateDetected;
  }
};
void confidence(Json& json, const char* key, uint32_t successes, uint32_t n) {
  constexpr double z = 1.959963984540054; const double p = double(successes) / n;
  const double denominator = 1 + z * z / n, center = (p + z * z / (2 * n)) / denominator;
  const double half = z * std::sqrt(p * (1 - p) / n + z * z / (4 * double(n) * n)) / denominator;
  json.Key(key); json.StartArray(); json.Double(std::max(0., center - half));
  json.Double(std::min(1., center + half)); json.EndArray();
}
void counts(Json& json, const Counts& c, bool evaluated = false) {
  json.Key("windows"); json.Uint(c.windows);
  for (const auto& entry : {std::make_pair("available", &c.available), std::make_pair("unavailable", &c.unavailable)}) {
    json.Key(entry.first); json.StartArray(); for (auto value : *entry.second) json.Uint(value); json.EndArray();
  }
  if (!evaluated) return;
  json.Key("selector18_exceedances"); json.Uint(c.candidate); json.Key("nc9_exceedances"); json.Uint(c.nc);
  json.Key("selector18_only"); json.Uint(c.candidateOnly); json.Key("nc9_only"); json.Uint(c.ncOnly);
  confidence(json, "selector18_wilson95", c.candidate, c.windows);
  confidence(json, "nc9_wilson95", c.nc, c.windows);
}
void winner(Json& json, const char* key, const Selector::Winner& winner) {
  json.Key(key); json.StartObject(); const auto& f = winner.family;
  json.Key("available"); json.Bool(winner.available); json.Key("coherent"); json.Bool(f.raw.coherent);
  json.Key("rate_index"); json.Uint(f.rateIndex); json.Key("rate"); json.Double(f.rate);
  json.Key("depth"); json.Uint(f.raw.lineage.depth); json.Key("power"); json.Double(f.raw.power);
  json.Key("own_row_median"); json.Double(f.rowMedian); json.Key("ratio"); json.Double(f.ratio);
  json.Key("valid_count"); json.Uint(f.validCount); json.Key("marginal_p"); json.Double(winner.marginalP);
  json.Key("delay"); json.Int(f.delay); json.Key("doppler_hz"); json.Double(f.dopplerHz);
  json.Key("delay_motion"); json.Int(f.raw.lineage.delayMotion);
  json.Key("doppler_motion"); json.Int(f.raw.lineage.dopplerMotion);
  json.Key("phase_offset_hz"); json.Double(f.phaseOffsetHz);
  integer(json, "epoch", f.raw.lineage.epoch); integer(json, "newest_sequence", f.raw.lineage.newestSequence);
  json.Key("raw_intervals"); json.StartArray();
  for (uint32_t age = 0; age < f.raw.lineage.depth; ++age) {
    json.StartObject(); integer(json, "sequence", f.raw.lineage.sequences[age]);
    integer(json, "first", f.raw.lineage.firstSamples[age]); integer(json, "end", f.raw.lineage.endSamples[age]); json.EndObject();
  }
  json.EndArray(); json.EndObject();
}
} // namespace

int main(int argc, char** argv) {
  std::string stage = "admission"; uint32_t unexpectedRefusals = 0;
  try {
    require(argc == 2 && (std::string(argv[1]) == "--run" || std::string(argv[1]) == "--fixture"),
      "Usage: testCoherentFamilyAcceptance --fixture|--run (JSONL receipt on stdout)");
    const bool fixtureOnly = std::string(argv[1]) == "--fixture";
    const auto began = std::chrono::steady_clock::now();
    const auto native = nativeConfig(); const auto plan = contract(native); Selector selector(plan), boundary(contract(native, 18));
    const auto binaryHash = executableHash(argv[0]); CanonicalFixture fixture(native);
    record([&](Json& j) {
      text(j, "kind", "contract"); text(j, "scope", "synthetic-development-only");
      text(j, "generator", generator); text(j, "generator_sha256", plan.generatorSha256);
      text(j, "binary_sha256", binaryHash); text(j, "plan_sha256", plan.planSha256);
      text(j, "selector_contract_sha256", selector.contractSha256());
      text(j, "boundary_contract_sha256", boundary.contractSha256());
      text(j, "fit_split_sha256", plan.fitSplitSha256); text(j, "joint_split_sha256", plan.jointSplitSha256);
      j.Key("fixture_only"); j.Bool(fixtureOnly); j.Key("declared_selector_trials"); j.Uint(837);
      j.Key("declared_nc_trials"); j.Uint(81); j.Key("alpha"); j.Double(plan.alpha);
      text(j, "native_control", "current-CPI zero-rate fixed-projection power only; native detector not executed");
    });
    if (fixtureOnly) {
      stage = "fixture"; const IqWindow iq(0x248a, 0);
      const auto full = fixture.run(iq, 0, false, 0, plan, selector, boundary);
      const auto loss = fixture.run(iq, 0, true, 0, plan, selector, boundary);
      const auto injected = fixture.run(iq, 0, false, amplitudes.back(), plan, selector, boundary);
      for (uint32_t i = 0; i < 9; ++i) {
        require(full.interior.families[i].ratio == loss.interior.families[i].ratio &&
          full.interior.families[i].rowMedian == loss.interior.families[i].rowMedian,
          "Pilot loss altered independent NC score or scale");
      }
      require(full.iqSha256 != injected.iqSha256 &&
        full.interior.families[0].raw.power != injected.interior.families[0].raw.power,
        "Paired weak injection did not reach canonical radar processing");
      record([&](Json& j) { text(j, "kind", "fixture_pass"); text(j, "null_iq_sha256", full.iqSha256);
        text(j, "injected_iq_sha256", injected.iqSha256); j.Key("unexpected_refusals"); j.Uint(0); });
      return 0;
    }
    Counts fitCounts, jointCounts, boundaryCounts; blah2::Sha256 fitIq, jointIq;
    stage = "fit";
    for (uint32_t i = 0; i < fitWindows; ++i) {
      const auto measured = fixture.run(IqWindow(0x248a, i), i, i % 4 == 0, 0, plan, selector, boundary);
      selector.addFit(measured.interior); boundary.addFit(measured.boundary);
      fitCounts.observe(measured.interior); boundaryCounts.observe(measured.boundary);
      fitIq.update(measured.iqSha256.data(), measured.iqSha256.size());
    }
    selector.freezeFit(); bool boundaryRefused = false;
    try { boundary.freezeFit(); } catch (const std::logic_error& error) {
      require(std::string(error.what()) == "Empirical null calibration stratum missing",
        "Boundary calibration failed for an unexpected reason");
      boundaryRefused = true;
    }
    require(boundaryRefused, "Permanently absent boundary coherent strata were silently calibrated");
    record([&](Json& j) { text(j, "kind", "fit"); text(j, "fit_sha256", selector.fitSha256());
      text(j, "ordered_iq_hashes_sha256", fitIq.finish()); counts(j, fitCounts); });
    record([&](Json& j) { text(j, "kind", "boundary_refusal");
      text(j, "reason", "coherent families unavailable: fractional support crosses source domain; missing fit strata");
      j.Key("expected_refusals"); j.Uint(1); counts(j, boundaryCounts); });
    stage = "joint";
    for (uint32_t i = 0; i < jointWindows; ++i) {
      const auto measured = fixture.run(IqWindow(0x248b, i), fitWindows + i, i % 4 == 0, 0, plan, selector, boundary);
      selector.addJoint(measured.interior); jointCounts.observe(measured.interior);
      jointIq.update(measured.iqSha256.data(), measured.iqSha256.size());
    }
    selector.freezeJoint();
    record([&](Json& j) { text(j, "kind", "joint"); text(j, "calibration_sha256", selector.calibrationSha256());
      text(j, "ordered_iq_hashes_sha256", jointIq.finish()); counts(j, jointCounts); });
    // Each amplitude gets the SAME frozen tables and exact same evaluation raw
    // intervals/noise. Copies do not pool paired arms into independent draws.
    std::array<Selector, amplitudes.size()> arms{selector, selector, selector, selector};
    std::array<Counts, amplitudes.size()> outcomes; stage = "evaluation";
    for (uint32_t i = 0; i < evaluationWindows; ++i) {
      const IqWindow iq(0x248c, i);
      for (uint32_t arm = 0; arm < amplitudes.size(); ++arm) {
        const auto measured = fixture.run(iq, fitWindows + jointWindows + i, i % 4 == 0,
          amplitudes[arm], plan, arms[arm], boundary);
        const auto decision = arms[arm].evaluate(measured.interior);
        outcomes[arm].observe(measured.interior); outcomes[arm].decision(decision);
        require(decision.availableFamilies == (i % 4 == 0 ? 9u : 18u) &&
          decision.availableBankTrialUpperBound == (i % 4 == 0 ? 81u : 837u),
          "Selector availability exposure changed");
        require(decision.noncoherent.available && !decision.noncoherent.family.raw.coherent,
          "Matched NC arm selected coherent evidence");
        record([&](Json& j) {
          text(j, "kind", "evaluation"); j.Key("paired_window"); j.Uint(i); j.Key("amplitude"); j.Double(amplitudes[arm]);
          text(j, "iq_sha256", measured.iqSha256); text(j, "calibration_sha256", decision.calibrationSha256);
          integer(j, "first_sample", decision.window.firstSample); integer(j, "end_sample", decision.window.endSample);
          j.Key("available_families"); j.Uint(decision.availableFamilies);
          j.Key("available_trial_upper_bound"); j.Uint(decision.availableBankTrialUpperBound);
          j.Key("selector18_p"); j.Double(decision.candidateP); j.Key("nc9_p"); j.Double(decision.noncoherentP);
          j.Key("selector18_attainable_p"); j.Double(decision.candidateAttainableP);
          j.Key("nc9_attainable_p"); j.Double(decision.noncoherentAttainableP);
          j.Key("selector18_detected"); j.Bool(decision.candidateDetected); j.Key("nc9_detected"); j.Bool(decision.noncoherentDetected);
          j.Key("current_cpi_zero_rate_power"); j.Double(measured.currentCpiZeroRatePower);
          winner(j, "selector18_winner", decision.candidate); winner(j, "nc9_winner", decision.noncoherent);
        });
      }
    }
    stage = "acceptance";
    for (uint32_t arm = 0; arm < amplitudes.size(); ++arm)
      record([&](Json& j) { text(j, "kind", "arm_summary"); j.Key("amplitude"); j.Double(amplitudes[arm]); counts(j, outcomes[arm], true); });
    // Broad deterministic development regression, not a precise 5% PFA claim.
    // No required coherent Pd improvement, monotonicity or theoretical gain.
    require(outcomes[0].candidate <= 20 && outcomes[0].nc <= 20,
      "Synthetic untouched-null exceedances crossed frozen development bound");
    record([&](Json& j) { text(j, "kind", "acceptance_pass"); j.Key("expected_refusals"); j.Uint(1);
      j.Key("unexpected_refusals"); j.Uint(unexpectedRefusals);
      j.Key("elapsed_seconds"); j.Double(std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
      text(j, "limits", "window-level synthetic development evidence; no RF, false-track, cadence, prospective or promotion acceptance"); });
    return 0;
  } catch (const std::exception& error) {
    ++unexpectedRefusals;
    record([&](Json& j) { text(j, "kind", "acceptance_refusal"); text(j, "stage", stage);
      text(j, "reason", error.what()); j.Key("unexpected_refusals"); j.Uint(unexpectedRefusals); });
    return 1;
  }
}
