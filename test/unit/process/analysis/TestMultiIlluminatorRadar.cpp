#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/utility/FftwThreads.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <random>
#include <utility>
#include <vector>

namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

blah2::channelizer::IlluminatorOutput signal(const std::string& id,
  uint64_t frequency, uint32_t rate, uint32_t decimation,
  std::size_t channels, std::size_t samples, uint32_t delay, double doppler,
  uint32_t seed)
{
  std::mt19937 generator(seed);
  std::normal_distribution<double> random;
  std::vector<std::complex<double>> direct(samples);
  for (auto& value : direct) value = {random(generator), random(generator)};
  blah2::channelizer::IlluminatorOutput result;
  result.id = id;
  result.frequencyHz = frequency;
  result.samples.channels.assign(channels,
    std::vector<std::complex<float>>(samples));
  result.samples.firstInputSample = 0;
  result.samples.hasFirstInputSample = true;
  result.samples.inputSamplesPerOutput = decimation;
  for (std::size_t channel = 0; channel < channels; ++channel)
    for (std::size_t sample = 0; sample < samples; ++sample)
    {
      const auto main = direct[sample] * std::polar(1.0, .19 * channel);
      std::complex<double> echo{};
      if (sample >= delay)
        echo = .55 * direct[sample - delay] *
          std::polar(1.0, .83 * channel + 2 * pi * doppler * sample / rate);
      result.samples.channels[channel][sample] =
        std::complex<float>(main + echo +
          std::complex<double>(random(generator), random(generator)) * .005);
    }
  return result;
}

std::pair<double, double> strongest(const Map<std::complex<double>>& map)
{
  double power = -1, delay = 0, doppler = 0;
  for (std::size_t row = 0; row < map.data.size(); ++row)
    for (std::size_t column = 0; column < map.data[row].size(); ++column)
      if (std::norm(map.data[row][column]) > power)
      {
        power = std::norm(map.data[row][column]);
        delay = map.delay[column];
        doppler = map.doppler[row];
      }
  return {delay, doppler};
}
}

TEST_CASE("independent illuminators produce independent radar products")
{
  REQUIRE(fftw_init_threads());
  blah2::set_fftw_planner_threads(1);
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  constexpr std::size_t channels = 5, samples = 2000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"lower", 100000000, 10000, outputRate, 16},
    {"upper", 101000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.pfa = .001; config.guardCells = 1; config.trainingCells = 4;
  config.minimumDelay = 3; config.minimumDopplerHz = 5;
  config.centroidRadius = 2;
  config.trackerM = 1; config.trackerN = 1; config.trackerDelete = 2;
  config.trackerMinimumSnrDb = -100;
  config.reference.analysisSamples = 512;
  config.reference.analysisInterval = 1;
  config.reference.covarianceSmoothing = 0;
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    captureRate, channels, definitions, config);

  std::vector<blah2::channelizer::IlluminatorOutput> input;
  input.push_back(signal("lower", 100000000, outputRate, 2,
    channels, samples, 8, 30, 41));
  input.push_back(signal("upper", 101000000, outputRate, 2,
    channels, samples, 13, -40, 99));
  auto frames = radar.process(std::move(input));
  REQUIRE(frames.size() == 2);
  REQUIRE(frames[0].illuminatorId == "lower");
  REQUIRE(frames[1].illuminatorId == "upper");
  REQUIRE(frames[0].sampleRateHz == outputRate);
  REQUIRE(frames[0].endInputSample == (samples - 1) * 2);

  const auto lower = strongest(*frames[0].map);
  const auto upper = strongest(*frames[1].map);
  REQUIRE(std::abs(lower.first - 8) <= 1);
  REQUIRE(std::abs(lower.second - 30) <= 10);
  REQUIRE(std::abs(upper.first - 13) <= 1);
  REQUIRE(std::abs(upper.second + 40) <= 10);
  REQUIRE(frames[0].detections->get_nDetections() > 0);
  REQUIRE(frames[1].detections->get_nDetections() > 0);
  REQUIRE(frames[0].tracks->get_n() > 0);
  REQUIRE(frames[1].tracks->get_n() > 0);
}

TEST_CASE("multi-illuminator radar buffers partial CPIs and rejects identity changes")
{
  constexpr uint32_t captureRate = 40000, outputRate = 20000;
  const std::vector<blah2::channelizer::IlluminatorDefinition> definitions{
    {"only", 100000000, 10000, outputRate, 16}};
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .1;
  config.delayMin = -2; config.delayMax = 20;
  config.dopplerMin = -100; config.dopplerMax = 100;
  config.clutterDelayMin = -2; config.clutterDelayMax = 4;
  config.reference.analysisSamples = 256;
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    captureRate, 2, definitions, config);
  auto half = signal("only", 100000000, outputRate, 2, 2, 1000, 8, 30, 4);
  REQUIRE(radar.process({half}).empty());
  half.samples.firstInputSample = 2000;
  auto frames = radar.process({half});
  REQUIRE(frames.size() == 1);
  auto changed = half;
  changed.id = "different";
  changed.samples.firstInputSample = 4000;
  REQUIRE_THROWS(radar.process({changed}));
  radar.reset();
  auto complete = signal("only", 100000000, outputRate, 2, 2, 2000, 8, 30, 8);
  complete.samples.firstInputSample = 0;
  REQUIRE(radar.process({complete}).size() == 1);
}
