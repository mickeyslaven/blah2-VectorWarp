#include "process/ambiguity/Ambiguity.h"
#include "process/ambiguity/BearingSteering.h"
#include "process/clutter/WienerHopf.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/utility/FftwThreads.h"

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <vector>

namespace {
using Complex = std::complex<double>;
constexpr unsigned Channels = 5, Samples = 4096, Delay = 7, Beams = 60;
constexpr double Pi = 3.1415926535897932384626433832795;
constexpr double WaveNumber = 2 * Pi * 527000000.0 / 299792458.0;
constexpr double Radius = .193548;
constexpr double Direct = 51 * Pi / 180;
using Vector = std::array<Complex, Channels>;

double power(const Vector& value) {
  double result = 0;
  for (auto item : value) result += std::norm(item);
  return result;
}

Complex inner(const Vector& left, const Vector& right) {
  Complex result{};
  for (unsigned i = 0; i < Channels; ++i) result += std::conj(left[i]) * right[i];
  return result;
}

double coherence(const Vector& left, const Vector& right) {
  return std::norm(inner(left, right)) / (power(left) * power(right));
}

Vector rawSteering(double angle) {
  Vector result{};
  for (unsigned channel = 0; channel < Channels; ++channel) {
    const double element = -2 * Pi * channel / Channels;
    // Physical raw-IQ convention, independently of the bearing helper.
    result[channel] = std::polar(1.0, -WaveNumber * Radius * std::cos(element - angle));
  }
  return result;
}

Vector projectedOracle(Vector response, const Vector& weights) {
  const auto factor = inner(weights, response) / power(weights);
  for (unsigned channel = 0; channel < Channels; ++channel)
    response[channel] -= weights[channel] * factor;
  return response;
}

bool productionNormalize(Vector& response, const Vector& weights, bool project) {
  return blah2::bearing_steering::normalizeModel(response.data(), Channels, weights.data(), project);
}

Vector bearingModel(double angle) {
  Vector result{};
  const auto physicalDirect = rawSteering(Direct);
  for (unsigned channel = 0; channel < Channels; ++channel) {
    const double element = -2 * Pi * channel / Channels;
    const double relative = WaveNumber * Radius *
      (std::cos(element - angle) - std::cos(element - Direct));
    // The helper returns the coefficient multiplied by the CAF response.
    result[channel] = std::conj(blah2::bearing_steering::mapCoefficient(physicalDirect[channel], relative));
  }
  return result;
}

struct Observation { Vector response{}, weights{}; };

Observation actualCaf(unsigned beam, bool arrayReference, bool cancel) {
  REQUIRE(fftw_init_threads() != 0);
  blah2::set_fftw_planner_threads(1);
  const double angle = 2 * Pi * beam / Beams;
  const auto target = rawSteering(angle), direct = rawSteering(Direct);
  constexpr int doppler = 17;
  constexpr double amplitude = .1;
  IqData dedicated(Samples), reference(Samples);
  std::vector<std::unique_ptr<IqData>> storage;
  std::vector<IqData*> channels;
  std::array<Complex, Samples> waveform{};
  // Disjoint direct/echo supports make the target CAF direction exact. The
  // signed Doppler phase is accumulated globally, not reset at each pulse.
  for (unsigned sample = 64; sample + Delay < Samples; sample += 64)
    waveform[sample] = std::polar(1.0, .173 * sample);
  for (auto value : waveform) dedicated.push_back(value);
  for (unsigned channel = 0; channel < Channels; ++channel) {
    storage.push_back(std::make_unique<IqData>(Samples));
    channels.push_back(storage.back().get());
    for (unsigned sample = 0; sample < Samples; ++sample) {
      const Complex echo = sample >= Delay ? amplitude * waveform[sample - Delay] *
        std::polar(1.0, 2 * Pi * doppler * sample / Samples) : Complex{};
      channels.back()->push_back(direct[channel] * waveform[sample] + target[channel] * echo);
    }
  }
  Observation result;
  if (arrayReference) {
    ArrayReferenceSynthesizer::Config config;
    config.analysisSamples = Samples;
    config.analysisInterval = 1;
    config.covarianceSmoothing = 0;
    ArrayReferenceSynthesizer synthesizer(config);
    synthesizer.process_into(channels, reference);
    const auto& weights = synthesizer.get_metrics().weights;
    std::copy(weights.begin(), weights.end(), result.weights.begin());
  } else {
    reference.replace(dedicated.get_data());
    // Deliberately nonzero: bypass must depend on the explicit mode, not on
    // the accidental absence of a reference vector.
    result.weights = rawSteering(Direct);
  }
  WienerHopf::PreparedReference prepared(0, 1, Samples);
  if (cancel) REQUIRE(prepared.prepare(reference));
  Ambiguity caf(Delay, Delay, doppler, doppler, Samples, Samples);
  for (unsigned channel = 0; channel < Channels; ++channel) {
    if (cancel) {
      WienerHopf filter(0, 1, Samples, WienerHopf::Workspace::SharedReference);
      REQUIRE(filter.process(prepared, channels[channel]));
    }
    const auto* map = caf.process(reference.view_data(), channels[channel], true);
    REQUIRE(map->delay.size() == 1);
    REQUIRE(map->doppler.size() == 1);
    REQUIRE(map->delay[0] == Delay);
    REQUIRE(map->doppler[0] == doppler);
    result.response[channel] = map->data[0][0];
  }
  REQUIRE(power(result.response) > 1e-6);
  return result;
}

unsigned winningBeam(const Vector& response, const Vector& weights, bool project) {
  double best = -1;
  unsigned winner = Beams;
  for (unsigned beam = 0; beam < Beams; ++beam) {
    auto model = bearingModel(2 * Pi * beam / Beams);
    if (!productionNormalize(model, weights, project)) continue;
    const double score = std::norm(inner(model, response));
    REQUIRE(std::isfinite(score));
    if (score > best) { best = score; winner = beam; }
  }
  return winner;
}
} // namespace

TEST_CASE("actual CAF preserves physical surveillance phase without array projection", "[bearing][caf][regression]") {
  for (unsigned beam : {0u, 20u, 40u}) {
    for (auto mode : {std::pair<bool, bool>{false, false}, {false, true}, {true, false}}) {
      CAPTURE(beam, mode.first, mode.second);
      const auto observed = actualCaf(beam, mode.first, mode.second);
      const auto raw = rawSteering(2 * Pi * beam / Beams);
      // Passing independent control distinguishes fixture/correlation issues
      // from the following production bearing-model assertion.
      REQUIRE(coherence(observed.response, raw) > 1 - 1e-10);
      auto model = bearingModel(2 * Pi * beam / Beams);
      REQUIRE(productionNormalize(model, observed.weights, false));
      CHECK(coherence(observed.response, model) > 1 - 1e-10);
      CHECK(winningBeam(observed.response, observed.weights, false) == beam);
    }
  }
}

TEST_CASE("actual eigenbeam cancellation projects the target CAF spatial response", "[bearing][caf][projection][regression]") {
  for (unsigned beam : {0u, 20u, 40u}) {
    CAPTURE(beam);
    const auto observed = actualCaf(beam, true, true);
    const auto raw = rawSteering(2 * Pi * beam / Beams);
    const auto expected = projectedOracle(raw, observed.weights);
    REQUIRE(power(expected) > .01);
    REQUIRE(coherence(observed.response, expected) > 1 - 1e-10);
    REQUIRE(std::norm(inner(observed.weights, observed.response)) /
      (power(observed.weights) * power(observed.response)) < 1e-20);
    // Isolates projection independently of the sign helper: this still fails
    // if only the phase convention is corrected.
    auto projectionOnly = raw;
    REQUIRE(productionNormalize(projectionOnly, observed.weights, true));
    CHECK(coherence(observed.response, projectionOnly) > 1 - 1e-10);
    auto model = bearingModel(2 * Pi * beam / Beams);
    REQUIRE(productionNormalize(model, observed.weights, true));
    CHECK(coherence(observed.response, model) > 1 - 1e-10);
    CHECK(winningBeam(observed.response, observed.weights, true) == beam);
  }
}

TEST_CASE("reference null candidates cannot become normalized bearing peaks", "[bearing][projection][regression]") {
  const auto weights = rawSteering(Direct);
  auto null = weights;
  CHECK_FALSE(productionNormalize(null, weights, true));
  CHECK(power(null) == 0);
  auto nearNull = weights;
  nearNull[0] += Complex(1e-12, -1e-12);
  CHECK_FALSE(productionNormalize(nearNull, weights, true));
  CHECK(power(nearNull) == 0);
  auto retained = weights;
  REQUIRE(productionNormalize(retained, weights, false));
  CHECK(std::abs(power(retained) - 1) < 1e-12);
  auto nonNull = rawSteering(2 * Pi / 3);
  REQUIRE(productionNormalize(nonNull, weights, true));
  CHECK(std::abs(power(nonNull) - 1) < 1e-12);
  CHECK(std::norm(inner(weights, nonNull)) < 1e-20);
  auto scaledWeights = weights;
  for (auto& value : scaledWeights) value *= Complex(2, -3);
  auto same = rawSteering(2 * Pi / 3);
  REQUIRE(productionNormalize(same, scaledWeights, true));
  for (unsigned i = 0; i < Channels; ++i) CHECK(std::abs(same[i] - nonNull[i]) < 1e-12);
}

TEST_CASE("invalid candidate or projection inputs fail closed", "[bearing][projection][regression]") {
  const auto weights = rawSteering(Direct);
  Vector zero{};
  CHECK_FALSE(productionNormalize(zero, weights, false));
  CHECK(power(zero) == 0);
  for (double invalid : {std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
    auto badResponse = rawSteering(2 * Pi / 3);
    badResponse[2] = Complex(invalid, 1);
    CHECK_FALSE(productionNormalize(badResponse, weights, false));
    CHECK(power(badResponse) == 0);
    auto badWeights = weights;
    badWeights[2] = Complex(0, invalid);
    auto response = rawSteering(2 * Pi / 3);
    CHECK_FALSE(productionNormalize(response, badWeights, true));
    CHECK(power(response) == 0);
  }
  auto response = rawSteering(2 * Pi / 3);
  CHECK_FALSE(productionNormalize(response, zero, true));
  CHECK(power(response) == 0);
}

TEST_CASE("post-CAF measured response bypass does not conjugate or project again", "[bearing][measured][regression]") {
  const auto weights = rawSteering(Direct);
  const auto measured = projectedOracle(rawSteering(2 * Pi / 3), weights);
  auto retained = measured;
  REQUIRE(productionNormalize(retained, weights, false));
  CHECK(coherence(retained, measured) > 1 - 1e-12);
  // Distinguishable complex phases: a conjugation cannot pass accidentally.
  Vector conjugated{};
  for (unsigned i = 0; i < Channels; ++i) conjugated[i] = std::conj(measured[i]);
  REQUIRE(coherence(measured, conjugated) < .95);
  // A measured response need not inhabit the CURRENT CPI's reference-null
  // plane. Bypass must not project a saved calibration with different weights.
  const auto currentWeights = rawSteering(0);
  retained = measured;
  REQUIRE(productionNormalize(retained, currentWeights, false));
  CHECK(coherence(retained, measured) > 1 - 1e-12);
}
