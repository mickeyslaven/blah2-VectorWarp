#include "process/pipeline/RawIqStages.h"
#include "process/pipeline/RadarPipeline.h"
#include "process/utility/FftwThreads.h"
#include "process/conditioning/FmReferenceQualityFilter.h"
#include "process/ambiguity/GpuResidentInput.h"
#include "process/ambiguity/RangeRowWorker.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
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

TEST_CASE("Canonical fusion reuses exclusive matrices and preserves retained frames") {
  Map<Complex> first(3, 5), second(3, 5);
  Noncoherent noncoherent;
  CoherentBeamBank coherent({539000000, 8, {{0, 0, 0}, {.1, 0, 0}}});
  for (bool useCoherent : {false, true}) {
    CAPTURE(useCoherent);
    std::unique_ptr<Map<Complex>> direct;
    std::shared_ptr<Map<Complex>> shared;
    const auto fuse = [&] {
      const std::vector<Map<Complex>*> maps{&first, &second};
      auto* selected = useCoherent ? &coherent : nullptr;
      blah2::pipeline::fuseMaps(maps, noncoherent, selected, direct);
      blah2::pipeline::fuseMaps(maps, noncoherent, selected, shared);
      Map<Complex> expected(first.get_nRows(), first.get_nCols());
      expected.delay = first.delay; expected.doppler = first.doppler;
      for (uint32_t row = 0; row < first.get_nRows(); ++row)
        for (uint32_t col = 0; col < first.get_nCols(); ++col) {
          double power = (std::norm(first.data[row][col]) +
            std::norm(second.data[row][col])) / 2;
          if (useCoherent) {
            const auto beams = coherent.powerBankAt(maps, row, col);
            power = *std::max_element(beams.begin(), beams.end());
          }
          expected.data[row][col] = {std::sqrt(power), 0};
        }
      expected.set_metrics();
      for (auto* actual : {direct.get(), shared.get()}) {
        REQUIRE(actual->data == expected.data);
        REQUIRE(actual->delay == expected.delay);
        REQUIRE(actual->doppler == expected.doppler);
        REQUIRE(actual->noisePower == expected.noisePower);
        REQUIRE(actual->maxPower == expected.maxPower);
      }
    };
    for (uint32_t frame = 0; frame < 3; ++frame) {
      first.delay = second.delay = {-2, -1, 0, 1, 2};
      first.doppler = second.doppler = {-10., 0., 10.};
      for (uint32_t row = 0; row < 3; ++row)
        for (uint32_t col = 0; col < 5; ++col) {
          first.data[row][col] = {double(frame + row + 1), double(col + 1)};
          second.data[row][col] = {double(col + 2), -double(frame + row + 2)};
        }
      const auto* directMatrix = direct ? direct->data.front().data() : nullptr;
      const auto* sharedMatrix = shared ? shared->data.front().data() : nullptr;
      fuse();
      if (frame) {
        REQUIRE(direct->data.front().data() == directMatrix);
        REQUIRE(shared->data.front().data() == sharedMatrix);
      }
    }
    const auto retained = shared;
    const auto retainedValues = retained->data;
    const auto retainedNoise = retained->noisePower;
    first.data[0][0] *= 4;
    first.doppler.front() = -20;
    fuse();
    REQUIRE(shared.get() != retained.get());
    REQUIRE(retained->data == retainedValues);
    REQUIRE(retained->noisePower == retainedNoise);
    REQUIRE(retained->doppler.front() == -10);
    // A shape change must replace exclusive output as well as shared output.
    first = Map<Complex>(2, 4); second = Map<Complex>(2, 4);
    first.delay = second.delay = {-1, 0, 1, 2};
    first.doppler = second.doppler = {-5, 5};
    fuse();
    REQUIRE(shared->get_nRows() == 2);
    REQUIRE(direct->get_nCols() == 4);
    REQUIRE_THROWS_AS(blah2::pipeline::fuseMaps({}, noncoherent, nullptr, shared),
      std::invalid_argument);
    REQUIRE_THROWS_AS(blah2::pipeline::fuseMaps({nullptr}, noncoherent, nullptr, direct),
      std::invalid_argument);
    first = Map<Complex>(3, 5); second = Map<Complex>(3, 5);
  }
}

TEST_CASE("Frontend reference preparation preserves exact maps and disabled clutter") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  for (bool clutter : {false, true}) for (bool acceleration : {false, true}) {
    CAPTURE(clutter, acceleration);
    Buffers expected, actual;
    populate(expected, actual, 17);
    expected.reference.replace(expected.channels.front()->get_data());
    actual.reference.replace(expected.reference.get_data());
    const auto rawReference = actual.reference.get_data();
    IqData projectedExpected(Samples), projectedActual(Samples);
    const auto project = [&](IqData& output) {
      auto& destination = output.resize_for_write(Samples);
      std::transform(rawReference.begin(), rawReference.end(), destination.begin(),
        [](Complex sample) { return sample * Complex(.75, .25); });
    };
    ArrayReferenceSynthesizer::Config referenceConfig;
    blah2::pipeline::RawIqStages::Config config{-2, 4, Samples,
      Channels, 2, false, referenceConfig, clutter};
    blah2::pipeline::RawIqStages expectedStages(config), actualStages(config);
    project(projectedExpected);
    if (clutter) REQUIRE(expectedStages.clutter(expected.reference, expected.channels));
    expectedStages.ambiguity(projectedExpected, expected.channels, expected.processors, true);
    const auto& shape = *actual.processors.front();
    blah2::Acceleration cpu("cpu", {shape.get_nfft(), shape.get_n_doppler_bins(),
      shape.get_n_delay_bins(), Channels, -2, Samples, 6, -2},
      shape.get_n_corr(), Samples, 0);
    unsigned preparations = 0;
    REQUIRE(blah2::pipeline::processFrontend(actualStages, acceleration ? &cpu : nullptr,
      actual.reference, actual.channels, actual.processors, clutter, true, {}, {}, nullptr,
      [&]() -> IqData* {
        ++preparations;
        REQUIRE(actual.reference.view_data() == rawReference);
        project(projectedActual);
        return &projectedActual;
      }));
    REQUIRE(preparations == 1);
    REQUIRE(projectedActual.view_data() == projectedExpected.view_data());
    compare(expected, actual);
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
      blah2::pipeline::RawIqStages extracted({-2, 4, Samples,
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
        for (auto* channel : actual.channels) {
          channel->resize_contiguous_for_write(Samples);
          channel->take_storage_metrics();
        }
        actual.reference.take_storage_metrics();
        original.process_into(expected.channels, expected.reference);
        expected.reference.view_data(); // Keep the historical deque oracle.
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
        REQUIRE(actual.reference.take_storage_metrics().materializations == 0);
        for (auto* channel : actual.channels)
          REQUIRE(channel->take_storage_metrics().materializations == 0);
        compare(expected, actual);
      }
    }
  }
}

TEST_CASE("Raw-IQ library preserves the uncluttered CPU path and validates shape") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  Buffers expected, actual;
  blah2::pipeline::RawIqStages extracted({-2, 4, Samples, Channels, 1, false, {}});
  populate(expected, actual, 0);
  expected.reference.replace(expected.channels.front()->get_data());
  actual.reference.replace(actual.channels.front()->get_data());
  actual.reference.resize_contiguous_for_write(Samples);
  for (auto* channel : actual.channels) channel->resize_contiguous_for_write(Samples);
  for (uint32_t channel = 0; channel < Channels; ++channel)
    expected.processors[channel]->process(expected.reference.view_data(), expected.channels[channel], false);
  extracted.ambiguity(actual.reference, actual.channels, actual.processors, false);
  REQUIRE(actual.reference.take_storage_metrics().materializations == 0);
  for (auto* channel : actual.channels)
    REQUIRE(channel->take_storage_metrics().materializations == 0);
  compare(expected, actual);
  REQUIRE_THROWS_AS(extracted.clutter(actual.reference, {}), std::invalid_argument);
  REQUIRE_THROWS_AS(extracted.ambiguity(actual.reference, actual.channels, {}, false), std::invalid_argument);
}

TEST_CASE("Contiguous FM conditioning and resident ingress preserve exact FP64 boundaries") {
  Buffers legacy, contiguous;
  blah2::conditioning::FmReferenceQualityFilter expectedFilter(Samples), actualFilter(Samples);
  IqData expectedOutput(Samples), actualOutput(Samples);
  blah2::resident::Config config;
  config.inputChannels = Channels; config.geometry.clutterSamples = Samples;
  std::vector<Complex> expectedResident(Channels * Samples), actualResident(Channels * Samples);
  for (unsigned frame = 0; frame < 3; ++frame) {
    populate(legacy, contiguous, frame);
    for (auto* channel : contiguous.channels) {
      channel->resize_contiguous_for_write(Samples);
      channel->take_storage_metrics();
    }
    const auto expected = expectedFilter.process(*legacy.channels.front(), expectedOutput);
    const auto actual = actualFilter.process(*contiguous.channels.front(), actualOutput);
    REQUIRE(actual.applied == expected.applied);
    REQUIRE(actual.retainedBlocks == expected.retainedBlocks);
    const auto a = actualOutput.sample_view(), b = expectedOutput.sample_view();
    REQUIRE(std::equal(a.begin(), a.end(), b.begin()));
    IqData inPlace(Samples);
    inPlace.replace(legacy.channels.front()->get_data());
    blah2::conditioning::FmReferenceQualityFilter inPlaceFilter(Samples);
    inPlaceFilter.process(inPlace, inPlace);
    const auto inPlaceResult = inPlace.sample_view();
    REQUIRE(std::equal(inPlaceResult.begin(), inPlaceResult.end(), b.begin()));
    blah2::resident::copyCanonicalInput(config, {expectedResident.data(), expectedResident.size()},
      legacy.owned, [](const auto& channel) -> const auto& { return channel->view_data(); });
    blah2::resident::copyCanonicalInput(config, {actualResident.data(), actualResident.size()},
      contiguous.owned, [](const auto& channel) { return channel->sample_view(); });
    REQUIRE(actualResident == expectedResident);
    for (auto* channel : contiguous.channels)
      REQUIRE(channel->take_storage_metrics().materializations == 0);
    REQUIRE(actualOutput.take_storage_metrics().materializations == 0);
  }
}

TEST_CASE("Contiguous ambiguity range workers preserve exact row math and synchronous lifetime") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  Buffers legacy, contiguous;
  populate(legacy, contiguous, 19);
  for (auto* channel : contiguous.channels) channel->resize_contiguous_for_write(Samples);
  constexpr uint32_t rows = 8, corr = 32, fft = 64, delays = 12;
  std::vector<Complex> expected(rows * delays), actual(rows * delays);
  std::mutex planner;
  RangeRowWorker caller(fft, FFTW_ESTIMATE, planner, false);
  RangeRowWorker threaded(fft, FFTW_ESTIMATE, planner, true);
  RangeRowWorker::Job oracle{legacy.channels[0]->sample_view(), legacy.channels[1]->sample_view(),
    &expected, 0, rows, corr, fft, delays, Samples, -2, 17};
  auto candidate = oracle;
  candidate.x = contiguous.channels[0]->sample_view();
  candidate.y = contiguous.channels[1]->sample_view(); candidate.rows = &actual;
  caller.executeCaller(oracle);
  threaded.start(candidate); threaded.finish();
  REQUIRE(actual == expected);
  for (auto* channel : contiguous.channels) {
    REQUIRE(channel->take_storage_metrics().materializations == 0);
    channel->clear(); // Worker has joined before its view can be overwritten.
  }
  REQUIRE(actual == expected);
}

TEST_CASE("Raw-IQ library preserves clutter rejection and recovery") {
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  Buffers expected, actual;
  blah2::pipeline::RawIqStages extracted({-2, 4, Samples, Channels, 1, false, {}});
  expected.reference.resize_for_write(Samples);
  actual.reference.resize_for_write(Samples);
  WienerHopf::PreparedReference original(-2, 4, Samples);
  REQUIRE_FALSE(original.prepare(expected.reference));
  REQUIRE_FALSE(extracted.clutter(actual.reference, actual.channels));
  populate(expected, actual, 1);
  actual.reference.replace(actual.channels.front()->get_data());
  REQUIRE(extracted.clutter(actual.reference, actual.channels));
}
