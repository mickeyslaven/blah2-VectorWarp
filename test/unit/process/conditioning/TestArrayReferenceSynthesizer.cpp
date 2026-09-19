#include "process/conditioning/ArrayReferenceSynthesizer.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <deque>

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
    if (storage) REQUIRE(&output.view_data().front() == storage);
    storage = &output.view_data().front();

    // Independent combination/normalization oracle, including frames where
    // weights are retained. Reused storage must never accumulate the old CPI.
    std::vector<std::complex<double>> expected(samples);
    double inputPower = 0, outputPower = 0;
    const auto& weights = synthesizer.get_metrics().weights;
    for (unsigned i = 0; i < samples; ++i)
    {
      for (unsigned channel = 0; channel < channels.size(); ++channel)
      {
        expected[i] += std::conj(weights[channel]) * channels[channel]->view_data()[i];
        inputPower += std::norm(channels[channel]->view_data()[i]) / channels.size();
      }
      outputPower += std::norm(expected[i]);
    }
    const double scale = std::sqrt(inputPower / outputPower);
    for (unsigned i = 0; i < samples; ++i)
      REQUIRE(std::abs(output.view_data()[i] - expected[i] * scale) < 1e-12);
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
