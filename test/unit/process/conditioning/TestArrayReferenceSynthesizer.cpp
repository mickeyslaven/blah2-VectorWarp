#include "process/conditioning/ArrayReferenceSynthesizer.h"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

TEST_CASE("direct-path diagnostic pilot preserves normalized weighted channel phase")
{
  IqData reference(4);
  const std::deque<std::complex<double>> samples{{1, 0}, {0, 2}, {-3, 0}, {0, -4}};
  reference.replace(std::deque<std::complex<double>>(samples));
  std::vector<std::unique_ptr<IqData>> surveillance;
  for (const auto gain : {std::complex<double>{0, 3}, std::complex<double>{-2, 0}}) {
    auto channel = std::make_unique<IqData>(4);
    auto values = samples;
    for (auto& value : values) value *= gain;
    channel->replace(std::move(values));
    surveillance.push_back(std::move(channel));
  }
  const auto pilot = [&](const std::vector<uint32_t>& channels,
                         const std::vector<uint32_t>& references = {},
                         const std::vector<std::complex<double>>& weights = {}) {
    return ArrayReferenceSynthesizer::direct_path_pilot(reference, surveillance,
      channels, references, weights);
  };
  // Dedicated inputs use unit weights; each channel's gain is normalized away.
  auto result = pilot({3, 1});
  REQUIRE(result);
  REQUIRE(std::abs(*result - std::complex<double>{-1, 1}) < 1e-14);
  auto anchor = ArrayReferenceSynthesizer::direct_path_anchor_pilot(
    reference, surveillance, {3, 1}, 3);
  REQUIRE(anchor);
  REQUIRE(std::abs(*anchor - std::complex<double>{0, 1}) < 1e-14);
  anchor = ArrayReferenceSynthesizer::direct_path_anchor_pilot(
    reference, surveillance, {3, 1}, 1);
  REQUIRE(anchor);
  REQUIRE(std::abs(*anchor - std::complex<double>{-1, 0}) < 1e-14);
  REQUIRE_FALSE(ArrayReferenceSynthesizer::direct_path_anchor_pilot(
    reference, surveillance, {3, 1}, 2));
  // Physical channel IDs select conjugated weights, independently of vector order.
  result = pilot({3, 1}, {1, 3}, {{.5, 0}, {0, .25}});
  REQUIRE(result);
  REQUIRE(std::abs(*result - std::complex<double>{-.25, 0}) < 1e-14);
  // Canonical contiguous workspaces and legacy FIFO input share the same math.
  for (auto* channel : {&reference, surveillance[0].get(), surveillance[1].get()}) {
    const auto view = channel->sample_view();
    const std::vector<std::complex<double>> values(view.begin(), view.end());
    auto& contiguous = channel->resize_contiguous_for_write(static_cast<uint32_t>(values.size()));
    std::copy(values.begin(), values.end(), contiguous.begin());
  }
  const auto contiguousPilot = pilot({3, 1}, {1, 3}, {{.5, 0}, {0, .25}});
  REQUIRE(contiguousPilot);
  REQUIRE(std::abs(*contiguousPilot - *result) < 1e-14);
  REQUIRE_FALSE(pilot({3, 1}, {1, 3}, {{1, 0}, {0, 1}}));
  REQUIRE_FALSE(pilot({3}));
  surveillance[1]->replace(std::deque<std::complex<double>>(4));
  result = pilot({3, 1});
  REQUIRE(result);
  REQUIRE(std::abs(*result - std::complex<double>{0, 1}) < 1e-14);
  surveillance[0]->replace(std::deque<std::complex<double>>(4,
    {std::numeric_limits<double>::infinity(), 0}));
  REQUIRE_FALSE(pilot({3, 1}));
  surveillance[0]->replace(std::deque<std::complex<double>>(3, {1, 0}));
  REQUIRE_FALSE(pilot({3, 1}));
  reference.replace({});
  REQUIRE_FALSE(pilot({3, 1}));
}

TEST_CASE("native float pilot matches explicitly widened double precision math")
{
  const std::vector<std::complex<float>> reference{
    {1.0f, .125f}, {-.75f, 2.0f}, {.00003125f, -.5f}, {3.25f, -1.5f}};
  std::vector<std::complex<float>> anchor(reference.size());
  const std::complex<float> gain{.625f, -.375f};
  std::transform(reference.begin(), reference.end(), anchor.begin(),
    [&](const auto value) { return value * gain; });
  const auto native = ArrayReferenceSynthesizer::direct_path_anchor_pilot(
    blah2::iq::SampleView{reference.data(), reference.size()},
    blah2::iq::SampleView{anchor.data(), anchor.size()});
  const std::vector<std::complex<double>> widenedReference(
    reference.begin(), reference.end());
  const std::vector<std::complex<double>> widenedAnchor(
    anchor.begin(), anchor.end());
  const auto widened = ArrayReferenceSynthesizer::direct_path_anchor_pilot(
    IqSampleView{widenedReference.data(), widenedReference.size()},
    IqSampleView{widenedAnchor.data(), widenedAnchor.size()});
  REQUIRE(native);
  REQUIRE(widened);
  REQUIRE(std::abs(*native - *widened) < 1e-15);
}

TEST_CASE("array reference coherently combines two to eight channels")
{
  constexpr std::size_t samples = 1024;
  for (const std::size_t channelCount : {2U, 3U, 5U, 8U})
  {
    std::vector<std::unique_ptr<IqData>> storage;
    std::vector<IqData *> channels;
    for (std::size_t channel = 0; channel < channelCount; channel++)
    {
      auto data = std::make_unique<IqData>(samples);
      std::deque<std::complex<double>> iq;
      for (std::size_t sample = 0; sample < samples; sample++)
        iq.push_back(std::polar(1.0, 0.02 * sample + 0.3 * channel));
      data->replace(std::move(iq));
      channels.push_back(data.get());
      storage.push_back(std::move(data));
    }

    ArrayReferenceSynthesizer::Config config;
    config.analysisSamples = samples;
    config.analysisInterval = 1;
    config.covarianceSmoothing = 0;
    ArrayReferenceSynthesizer synthesizer(config);
    const auto result = synthesizer.process(channels);
    const auto& metrics = synthesizer.get_metrics();

    REQUIRE(result->view_data().size() == samples);
    REQUIRE(metrics.weights.size() == channelCount);
    REQUIRE(metrics.coherentFraction > 0.99);
    REQUIRE(metrics.coherentGainDb >
      10.0 * std::log10(static_cast<double>(channelCount)) - 0.15);
  }
}

TEST_CASE("array reference reuses storage across changing CPIs without stale samples")
{
  constexpr uint32_t samples = 1024;
  IqData first(samples), second(samples), output(samples);
  std::vector<IqData*> channels{&first, &second};
  ArrayReferenceSynthesizer::Config config;
  config.analysisInterval = 3;
  ArrayReferenceSynthesizer synthesizer(config);
  const std::complex<double>* storage = nullptr;
  for (unsigned frame = 0; frame < 9; ++frame)
  {
    for (unsigned channel = 0; channel < channels.size(); ++channel)
    {
      auto& values = channels[channel]->resize_for_write(samples);
      for (unsigned i = 0; i < samples; ++i)
        values[i] = std::polar(0.7 + 0.1 * frame + 0.2 * channel,
          0.02 * i + 0.31 * channel + 0.13 * frame);
    }
    synthesizer.process_into(channels, output);
    if (storage) REQUIRE(output.sample_view().data() == storage);
    storage = output.sample_view().data();

    // Independent unit-norm combination oracle, including frames where
    // weights are retained. Reused storage must never accumulate the old CPI.
    std::vector<std::complex<double>> expected(samples);
    const auto& weights = synthesizer.get_metrics().weights;
    for (unsigned i = 0; i < samples; ++i)
    {
      for (unsigned channel = 0; channel < channels.size(); ++channel)
      {
        expected[i] += std::conj(weights[channel]) * channels[channel]->view_data()[i];
      }
    }
    for (unsigned i = 0; i < samples; ++i)
      REQUIRE(std::abs(output.sample_view()[i] - expected[i]) < 1e-12);
  }

  const auto before = first.get_data();
  const auto updates = synthesizer.get_metrics().updates;
  REQUIRE_THROWS(synthesizer.process_into(channels, first));
  REQUIRE(first.get_data() == before);
  REQUIRE(synthesizer.get_metrics().updates == updates);
  IqData tooSmall(samples - 1);
  REQUIRE_THROWS(synthesizer.process_into(channels, tooSmall));
  REQUIRE(tooSmall.get_length() == 0);
  REQUIRE(synthesizer.get_metrics().updates == updates);

  const auto saved = synthesizer.process(channels);
  const auto savedSamples = saved->get_data();
  first.resize_for_write(samples).front() += std::complex<double>(1, 2);
  synthesizer.process_into(channels, output);
  REQUIRE(saved->get_data() == savedSamples);
}

TEST_CASE("array reference target protection nulls a predeclared relative direction")
{
  constexpr std::size_t samples = 2048;
  std::vector<std::unique_ptr<IqData>> storage;
  std::vector<IqData *> channels;
  // A direct-path rank-one field with a deliberately distinct protected
  // direction.  The test asserts the reference-domain constraint, rather
  // than claiming that a target echo itself is present in the calibration CPI.
  const std::vector<std::complex<double>> protectedDirection{
    {1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 0}};
  for (std::size_t channel = 0; channel < protectedDirection.size(); ++channel)
  {
    auto data = std::make_unique<IqData>(samples);
    auto& values = data->resize_for_write(samples);
    for (std::size_t sample = 0; sample < samples; ++sample)
      values[sample] = std::polar(1.0, .015 * double(sample) + .2 * double(channel));
    channels.push_back(data.get());
    storage.push_back(std::move(data));
  }
  ArrayReferenceSynthesizer::Config config;
  config.analysisSamples = samples;
  config.analysisInterval = 1;
  config.covarianceSmoothing = 0;
  ArrayReferenceSynthesizer baseline(config);
  baseline.process(channels);
  const auto directPathWeights = baseline.get_metrics().weights;
  config.protectionRelativeSteering = protectedDirection;
  ArrayReferenceSynthesizer synthesizer(config);
  synthesizer.process(channels);
  const auto& weights = synthesizer.get_metrics().weights;
  std::complex<double> response{};
  for (std::size_t channel = 0; channel < weights.size(); ++channel)
  {
    const auto target = directPathWeights[channel] * protectedDirection[channel];
    response += std::conj(target) * weights[channel];
  }
  REQUIRE(std::abs(response) < 1e-10);
}

TEST_CASE("target protection never becomes the learned direct-path anchor")
{
  constexpr std::size_t samples = 512;
  const std::vector<std::complex<double>> relative{
    {1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 0}};
  std::vector<std::unique_ptr<IqData>> storage;
  std::vector<IqData*> channels;
  for (std::size_t channel = 0; channel < relative.size(); ++channel)
  {
    storage.push_back(std::make_unique<IqData>(samples));
    channels.push_back(storage.back().get());
    auto& values = channels.back()->resize_for_write(samples);
    for (std::size_t sample = 0; sample < samples; ++sample)
      values[sample] = std::polar(1.0,
        .017 * double(sample) + .23 * double(channel));
  }
  ArrayReferenceSynthesizer::Config baselineConfig;
  baselineConfig.analysisSamples = samples;
  baselineConfig.analysisInterval = 1;
  ArrayReferenceSynthesizer baseline(baselineConfig);
  auto protectedConfig = baselineConfig;
  protectedConfig.protectionRelativeSteering = relative;
  ArrayReferenceSynthesizer protectedReference(protectedConfig);

  for (unsigned frame = 0; frame < 8; ++frame)
  {
    baseline.process(channels);
    protectedReference.process(channels);
    const auto& plain = baseline.get_metrics();
    const auto& guarded = protectedReference.get_metrics();
    REQUIRE(guarded.directPathWeights.size() == relative.size());
    for (std::size_t channel = 0; channel < relative.size(); ++channel)
      REQUIRE(std::abs(guarded.directPathWeights[channel] -
                       plain.directPathWeights[channel]) < 1e-11);
    std::complex<double> response{};
    for (std::size_t channel = 0; channel < relative.size(); ++channel)
      response += std::conj(guarded.directPathWeights[channel] * relative[channel]) *
        guarded.weights[channel];
    REQUIRE(std::abs(response) < 1e-10);
  }
}

TEST_CASE("target protection rejects a degenerate direct-path null")
{
  IqData first(128), second(128);
  first.replace(std::deque<std::complex<double>>(128, {1, 0}));
  second.replace(std::deque<std::complex<double>>(128, {1, 0}));
  ArrayReferenceSynthesizer::Config config;
  config.protectionRelativeSteering = {{1, 0}, {1, 0}};
  ArrayReferenceSynthesizer synthesizer(config);
  REQUIRE_THROWS_AS(synthesizer.process({&first, &second}), std::runtime_error);
}

TEST_CASE("target protection shares direct-path collapse fallback and fails closed")
{
  IqData first(64), second(64), output(64);
  first.replace(std::deque<std::complex<double>>(64, {1, 0}));
  second.replace(std::deque<std::complex<double>>(64, {1, 0}));
  ArrayReferenceSynthesizer::Config baselineConfig;
  baselineConfig.analysisInterval = 10;
  ArrayReferenceSynthesizer baseline(baselineConfig);
  auto protectedConfig = baselineConfig;
  protectedConfig.protectionRelativeSteering = {{1, 0}, {0, 1}};
  ArrayReferenceSynthesizer protectedReference(protectedConfig);
  baseline.process_into({&first, &second}, output);
  protectedReference.process_into({&first, &second}, output);
  baseline.process_into({&first, &second}, output);
  protectedReference.process_into({&first, &second}, output);

  second.replace(std::deque<std::complex<double>>(64, {-1, 0}));
  baseline.process_into({&first, &second}, output);
  REQUIRE(baseline.get_metrics().fallback);
  REQUIRE(baseline.get_metrics().reason == "strongest_channel");
  REQUIRE_THROWS_AS(protectedReference.process_into({&first, &second}, output),
                    std::runtime_error);
  REQUIRE(protectedReference.get_metrics().fallback);
  REQUIRE(protectedReference.get_metrics().reason == "strongest_channel");
  REQUIRE(protectedReference.get_metrics().directPathWeights ==
          baseline.get_metrics().directPathWeights);
}

TEST_CASE("array reference common-mode decorrelation is opt-in and capped")
{
  constexpr uint32_t samples = 128;
  IqData reference(samples), surveillance(samples), output(samples);
  auto& r = reference.resize_for_write(samples);
  auto& s = surveillance.resize_for_write(samples);
  for (uint32_t index = 0; index < samples; ++index)
  {
    r[index] = std::polar(1.0, .11 * index);
    // A distinct quadrature component is target-like and must not be removed
    // by the rank-one common-mode operation.
    s[index] = r[index] + .4 * (index % 2 ? -r[index] : r[index]);
  }
  ArrayReferenceSynthesizer untouched;
  untouched.decorrelate_surveillance_into(reference, surveillance, output);
  REQUIRE(output.get_data() == surveillance.get_data());
  REQUIRE(untouched.get_metrics().commonModeFraction == 0);

  ArrayReferenceSynthesizer::Config config;
  config.commonModeDecorrelation = .5;
  config.commonModeMinimumCoherence = 0;
  ArrayReferenceSynthesizer partial(config);
  partial.decorrelate_surveillance_into(reference, surveillance, output);
  for (uint32_t index = 0; index < samples; ++index)
  {
    const auto expected = surveillance.view_data()[index] - .5 * reference.view_data()[index];
    REQUIRE(std::abs(output.view_data()[index] - expected) < 1e-12);
  }
  REQUIRE(partial.get_metrics().commonModeFraction == .5);
  config.commonModeDecorrelation = .500001;
  REQUIRE_THROWS_AS(ArrayReferenceSynthesizer(config), std::invalid_argument);
}

TEST_CASE("array reference phase-null seed retains physical coherent gain")
{
  constexpr uint32_t samples = 64;
  for (const unsigned count : {2U, 3U, 5U, 8U})
  {
    std::vector<std::unique_ptr<IqData>> storage;
    std::vector<IqData*> channels;
    for (unsigned c = 0; c < count; ++c)
    {
      storage.push_back(std::make_unique<IqData>(samples));
      channels.push_back(storage.back().get());
      auto& values = channels.back()->resize_for_write(samples);
      const auto phase = std::polar(1.0, 2 * std::acos(-1.0) * c / count);
      for (unsigned n = 0; n < samples; ++n) values[n] = phase;
    }
    ArrayReferenceSynthesizer synthesizer;
    auto result = synthesizer.process(channels);
    REQUIRE(std::abs(std::norm(result->view_data().front()) - count) < 1e-12);
    REQUIRE(synthesizer.get_metrics().hasSignal);
    REQUIRE_FALSE(synthesizer.get_metrics().fallback);
    const auto weights = synthesizer.get_metrics().weights;
    const auto covariance = synthesizer.get_metrics().covariance;
    REQUIRE(covariance.size() == count * count);
    for (auto* channel : channels)
      for (auto& value : channel->resize_for_write(samples)) value *= 3;
    result = synthesizer.process(channels);
    REQUIRE(std::abs(std::norm(result->view_data().front()) - 9 * count) < 1e-11);
    // Initial channel binding leaves the periodic counter at zero, so the
    // second frame is analyzed too. Covariance scales with amplitude squared.
    for (size_t i = 0; i < covariance.size(); ++i)
      REQUIRE(std::abs(synthesizer.get_metrics().covariance[i] - 9.0 * covariance[i]) < 1e-12);
    const auto analyzedCovariance = synthesizer.get_metrics().covariance;
    result = synthesizer.process(channels);
    REQUIRE(std::abs(std::norm(result->view_data().front()) - 9 * count) < 1e-11);
    REQUIRE(synthesizer.get_metrics().covariance == analyzedCovariance);
    for (unsigned c = 0; c < count; ++c)
      REQUIRE(std::abs(synthesizer.get_metrics().weights[c] - weights[c]) < 1e-12);
  }
}

TEST_CASE("array reference direction and coherent gain are amplitude invariant")
{
  constexpr uint32_t samples = 64;
  for (const unsigned count : {2U, 5U, 8U}) {
    std::vector<std::unique_ptr<IqData>> storage;
    std::vector<IqData*> channels;
    for (unsigned c = 0; c < count; ++c) {
      storage.push_back(std::make_unique<IqData>(samples));
      channels.push_back(storage.back().get());
    }
    std::vector<std::complex<double>> baseline;
    for (const double amplitude : {1.0, 1e-7, 1e-8}) {
      ArrayReferenceSynthesizer fresh;
      for (unsigned c = 0; c < count; ++c) {
        auto& values = channels[c]->resize_for_write(samples);
        for (unsigned n = 0; n < samples; ++n)
          values[n] = std::polar(amplitude, .3*c + .02*n);
      }
      const auto result = fresh.process(channels);
      const auto& metrics = fresh.get_metrics();
      REQUIRE(metrics.hasSignal);
      REQUIRE_FALSE(metrics.fallback);
      double norm = 0;
      for (auto weight : metrics.weights) norm += std::norm(weight);
      REQUIRE(std::abs(norm - 1) < 1e-12);
      REQUIRE(std::abs(metrics.coherentGainDb - 10*std::log10(double(count))) < 1e-10);
      if (baseline.empty()) baseline = metrics.weights;
      for (unsigned c = 0; c < count; ++c)
        REQUIRE(std::abs(metrics.weights[c] - baseline[c]) < 1e-12);
      for (auto value : result->sample_view())
        REQUIRE(std::abs(std::abs(value)/amplitude - std::sqrt(double(count))) < 1e-12);
    }
  }
}

TEST_CASE("array reference phase jumps cannot freeze a zero beam")
{
  IqData first(64), second(64), output(64);
  first.replace(std::deque<std::complex<double>>(64, {1, 0}));
  second.replace(std::deque<std::complex<double>>(64, {1, 0}));
  ArrayReferenceSynthesizer::Config config;
  config.analysisInterval = 10;
  ArrayReferenceSynthesizer synthesizer(config);
  synthesizer.process_into({&first, &second}, output);
  synthesizer.process_into({&first, &second}, output);
  second.replace(std::deque<std::complex<double>>(64, {-1, 0}));
  const auto updates = synthesizer.get_metrics().updates;
  synthesizer.process_into({&first, &second}, output);
  REQUIRE(synthesizer.get_metrics().updates == updates);
  REQUIRE(synthesizer.get_metrics().fallback);
  REQUIRE(synthesizer.get_metrics().reason == "strongest_channel");
  REQUIRE(std::norm(output.view_data().front()) == 1);
  synthesizer.process_into({&first, &second}, output);
  REQUIRE(synthesizer.get_metrics().updates == updates + 1);
  REQUIRE(std::norm(output.view_data().front()) >= 1);
  first.replace(std::deque<std::complex<double>>(64, {0, 0}));
  second.replace(std::deque<std::complex<double>>(64, {0, 0}));
  synthesizer.process_into({&first, &second}, output);
  REQUIRE_FALSE(synthesizer.get_metrics().hasSignal);
  REQUIRE(synthesizer.get_metrics().reason == "zero_input");
  synthesizer.reset();
  REQUIRE(synthesizer.get_metrics().updates == 0);
  REQUIRE(synthesizer.get_metrics().weights.empty());
  REQUIRE(synthesizer.get_metrics().covariance.empty());
  first.replace(std::deque<std::complex<double>>(64, {2, 0}));
  second.replace(std::deque<std::complex<double>>(64, {-2, 0}));
  synthesizer.process_into({&first, &second}, output);
  REQUIRE(std::norm(output.view_data().front()) > 7.99);
}

TEST_CASE("array reference rejects empty and nonfinite state")
{
  IqData first(64), second(64);
  ArrayReferenceSynthesizer synthesizer;
  REQUIRE_THROWS(synthesizer.process({&first, &second}));
  ArrayReferenceSynthesizer::Config config;
  config.covarianceSmoothing = std::numeric_limits<double>::quiet_NaN();
  REQUIRE_THROWS_AS(ArrayReferenceSynthesizer{config}, std::invalid_argument);
}

TEST_CASE("array reference searches modes outside the largest diagonal")
{
  std::vector<std::unique_ptr<IqData>> storage;
  std::vector<IqData*> channels;
  for (unsigned c = 0; c < 5; ++c)
  {
    storage.push_back(std::make_unique<IqData>(64));
    channels.push_back(storage.back().get());
    auto& data = channels.back()->resize_for_write(64);
    for (unsigned n = 0; n < 64; ++n)
      data[n] = c == 0 ? std::sqrt(.9) * (n % 2 ? -1.0 : 1.0) : .5;
  }
  ArrayReferenceSynthesizer synthesizer;
  const auto output = synthesizer.process(channels);
  const auto& metrics = synthesizer.get_metrics();
  REQUIRE(std::abs(metrics.weights[0]) < 1e-12);
  REQUIRE(std::abs(metrics.coherentFraction - 1 / 1.9) < 1e-12);
  for (const auto value : output->view_data()) REQUIRE(std::abs(value - 1.0) < 1e-12);
}
