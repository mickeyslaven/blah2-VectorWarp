#include "process/analysis/ProductionRawIq.h"
#include "process/utility/FftwThreads.h"
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <memory>
#include <random>

namespace {
using Complex = std::complex<double>;
constexpr uint32_t Samples = 4096, Channels = 3;

struct Buffers {
  IqData reference{Samples};
  std::vector<std::unique_ptr<IqData>> owned;
  std::vector<IqData*> channels;
  std::vector<std::unique_ptr<Ambiguity>> ambiguity;
  std::vector<Ambiguity*> processors;
  Buffers() {
    for (uint32_t channel = 0; channel < Channels; ++channel) {
      owned.push_back(std::make_unique<IqData>(Samples));
      channels.push_back(owned.back().get());
      ambiguity.push_back(std::make_unique<Ambiguity>(-2, 9, -64, 64,
        Samples, Samples, true));
      processors.push_back(ambiguity.back().get());
    }
  }
};

void populate(Buffers& a, Buffers& b, uint32_t frame) {
  std::mt19937 generator(42 + frame);
  std::normal_distribution<double> random;
  std::vector<Complex> direct(Samples), echo(Samples);
  for (uint32_t sample = 0; sample < Samples; ++sample) {
    direct[sample] = {random(generator), random(generator)};
    echo[sample] = {random(generator), random(generator)};
  }
  for (uint32_t channel = 0; channel < Channels; ++channel) {
    auto& output = a.channels[channel]->resize_for_write(Samples);
    for (uint32_t sample = 0; sample < Samples; ++sample)
      output[sample] = std::polar(1.0 + .1 * channel, .3 * channel + .02 * frame) *
        direct[sample] + std::polar(.2, .7 * channel) * echo[sample] +
        Complex(random(generator), random(generator)) * .03;
    b.channels[channel]->replace(a.channels[channel]->get_data());
  }
}

void compare(Buffers& expected, Buffers& actual) {
  REQUIRE(actual.reference.view_data() == expected.reference.view_data());
  for (uint32_t channel = 0; channel < Channels; ++channel) {
    REQUIRE(actual.channels[channel]->view_data() == expected.channels[channel]->view_data());
    const auto* left = expected.processors[channel]->result();
    const auto* right = actual.processors[channel]->result();
    REQUIRE(right->data == left->data);
    REQUIRE(right->delay == left->delay);
    REQUIRE(right->doppler == left->doppler);
  }
}
}

TEST_CASE("Raw-IQ library is exact against the former live orchestration across CPIs") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  for (bool borrowed : {false, true}) {
    for (bool preserve : {false, true}) {
      CAPTURE(borrowed, preserve);
      ArrayReferenceSynthesizer::Config referenceConfig;
      referenceConfig.analysisSamples = 256;
      referenceConfig.analysisInterval = 2;
      ArrayReferenceSynthesizer original(referenceConfig);
      blah2::production_replay::RawIqStages extracted({-2, 4, Samples,
        Channels, 2, borrowed, referenceConfig});
      Buffers expected, actual;
      std::vector<std::unique_ptr<WienerHopf>> filters;
      WienerHopf::PreparedReference prepared(-2, 4, Samples);
      for (uint32_t channel = 0; channel < Channels; ++channel)
        filters.push_back(std::make_unique<WienerHopf>(-2, 4, Samples,
          borrowed ? WienerHopf::Workspace::Pi : WienerHopf::Workspace::SharedReference));
      for (uint32_t frame = 0; frame < 12; ++frame) {
        CAPTURE(frame);
        if (frame == 7) {
          original = ArrayReferenceSynthesizer(referenceConfig);
          extracted.resetReference();
        }
        populate(expected, actual, frame);
        original.process_into(expected.channels, expected.reference);
        extracted.reference().process_into(actual.channels, actual.reference);
        REQUIRE(extracted.reference().get_metrics().weights == original.get_metrics().weights);
        REQUIRE(extracted.reference().get_metrics().updates == original.get_metrics().updates);
        REQUIRE(extracted.reference().get_metrics().coherentFraction == original.get_metrics().coherentFraction);
        REQUIRE(extracted.reference().get_metrics().coherentGainDb == original.get_metrics().coherentGainDb);
        if (!borrowed) REQUIRE(prepared.prepare(expected.reference));
        for (uint32_t channel = 0; channel < Channels; ++channel) {
          if (borrowed)
            REQUIRE(filters[channel]->process(&expected.reference, expected.channels[channel], true));
          else
            REQUIRE(filters[channel]->process(prepared, expected.channels[channel]));
        }
        REQUIRE(extracted.clutter(actual.reference, actual.channels));
        for (uint32_t channel = 0; channel < Channels; ++channel) {
          if (borrowed) {
            const auto view = filters[channel]->filtered_view();
            expected.processors[channel]->process_borrowed(expected.reference.view_data(),
              expected.channels[channel], view.rotatedReference, view.filteredSurveillance,
              view.samples, view.delayMin, preserve);
          } else {
            expected.processors[channel]->process(expected.reference.view_data(),
              expected.channels[channel], preserve);
          }
        }
        extracted.ambiguity(actual.reference, actual.channels, actual.processors, preserve);
        compare(expected, actual);
      }
    }
  }
}

TEST_CASE("Raw-IQ library preserves the uncluttered CPU path and validates shape") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  Buffers expected, actual;
  blah2::production_replay::RawIqStages extracted({-2, 4, Samples, Channels, 1, false, {}});
  populate(expected, actual, 0);
  expected.reference.replace(expected.channels.front()->get_data());
  actual.reference.replace(actual.channels.front()->get_data());
  for (uint32_t channel = 0; channel < Channels; ++channel)
    expected.processors[channel]->process(expected.reference.view_data(), expected.channels[channel], false);
  extracted.ambiguity(actual.reference, actual.channels, actual.processors, false);
  compare(expected, actual);
  REQUIRE_THROWS_AS(extracted.clutter(actual.reference, {}), std::invalid_argument);
  REQUIRE_THROWS_AS(extracted.ambiguity(actual.reference, actual.channels, {}, false), std::invalid_argument);
}

TEST_CASE("Raw-IQ library preserves clutter rejection and recovery") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  Buffers expected, actual;
  blah2::production_replay::RawIqStages extracted({-2, 4, Samples, Channels, 1, false, {}});
  expected.reference.resize_for_write(Samples);
  actual.reference.resize_for_write(Samples);
  WienerHopf::PreparedReference original(-2, 4, Samples);
  REQUIRE_FALSE(original.prepare(expected.reference));
  REQUIRE_FALSE(extracted.clutter(actual.reference, actual.channels));
  populate(expected, actual, 1);
  actual.reference.replace(actual.channels.front()->get_data());
  REQUIRE(extracted.clutter(actual.reference, actual.channels));
}

TEST_CASE("Mixed fallback uses the first filter even with multiple surveillance paths") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  Buffers expected, actual;
  blah2::production_replay::RawIqStages extracted({-2, 4, Samples, Channels, 1, true, {}});
  populate(expected, actual, 9);
  expected.reference.replace(expected.channels.front()->get_data());
  actual.reference.replace(actual.channels.front()->get_data());
  REQUIRE(extracted.clutter(actual.reference, actual.channels));
  // The original raw input remains authoritative until mixed-map commit.
  actual.channels.front()->replace(expected.channels.front()->get_data());
  WienerHopf original(-2, 4, Samples);
  REQUIRE(original.process(&expected.reference, expected.channels.front(), true));
  REQUIRE(extracted.fallbackClutter(actual.reference, *actual.channels.front()));
  const auto view = original.filtered_view();
  expected.processors.front()->process_borrowed(expected.reference.view_data(),
    expected.channels.front(), view.rotatedReference, view.filteredSurveillance,
    view.samples, view.delayMin, false);
  extracted.ambiguity(actual.reference, actual.channels, actual.processors, false);
  REQUIRE(actual.processors.front()->result()->data == expected.processors.front()->result()->data);
  REQUIRE(actual.channels.front()->view_data() == expected.channels.front()->view_data());
}
