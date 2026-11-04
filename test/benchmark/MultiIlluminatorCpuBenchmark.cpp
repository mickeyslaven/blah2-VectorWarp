#include "process/analysis/MultiIlluminatorRadar.h"
#include "process/utility/FixedExecutor.h"
#include "process/utility/FftwThreads.h"
#include "process/utility/ProcessingThreads.h"

#include <fftw3.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr double pi = 3.141592653589793238462643383279502884;

struct Sample
{
  double radarMs = 0;
  double jsonMs = 0;
  std::size_t jsonBytes = 0;
};

double milliseconds(Clock::time_point first, Clock::time_point last)
{
  return std::chrono::duration<double, std::milli>(last - first).count();
}

double percentile(std::vector<double> values, double fraction)
{
  if (values.empty()) throw std::logic_error("Empty benchmark sample set");
  std::sort(values.begin(), values.end());
  const std::size_t index = std::min(values.size() - 1,
    std::size_t(std::ceil(fraction * values.size())) - 1);
  return values[index];
}

double peak_rss_mib()
{
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage))
    throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
  return double(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
  return double(usage.ru_maxrss) / 1024.0;
#endif
}

std::vector<std::vector<std::complex<float>>> make_base_channels(
  std::size_t physicalChannels, uint32_t outputRate,
  std::size_t cpiSamples)
{
  std::mt19937 generator(0x5eed1234U);
  std::normal_distribution<float> normal(0, 1);
  std::vector<std::complex<float>> direct(cpiSamples);
  for (auto& value : direct) value = {normal(generator), normal(generator)};
  std::vector<std::vector<std::complex<float>>> result(physicalChannels,
    std::vector<std::complex<float>>(cpiSamples));
  constexpr std::size_t echoDelay = 47;
  constexpr double echoDoppler = 42;
  for (std::size_t channel = 0; channel < physicalChannels; ++channel)
    for (std::size_t sample = 0; sample < cpiSamples; ++sample)
    {
      auto value = std::complex<double>(direct[sample]) *
        std::polar(1.0, .17 * channel);
      if (sample >= echoDelay)
        value += .18 * std::complex<double>(direct[sample - echoDelay]) *
          std::polar(1.0, .41 * channel +
            2 * pi * echoDoppler * sample / outputRate);
      result[channel][sample] = std::complex<float>(value);
    }
  return result;
}

std::vector<blah2::channelizer::IlluminatorDefinition> definitions(
  std::size_t lanes, uint32_t outputRate)
{
  std::vector<blah2::channelizer::IlluminatorDefinition> result;
  result.reserve(lanes);
  for (std::size_t lane = 0; lane < lanes; ++lane)
    result.push_back({"lane-" + std::to_string(lane),
      100000000ULL + lane * 100000ULL, 200000, outputRate, 24});
  return result;
}

std::vector<blah2::channelizer::IlluminatorOutput> make_input(
  const std::vector<blah2::channelizer::IlluminatorDefinition>& stations,
  const std::vector<std::vector<std::complex<float>>>& base,
  uint64_t firstInputSample, uint32_t decimation)
{
  std::vector<blah2::channelizer::IlluminatorOutput> result;
  result.reserve(stations.size());
  for (const auto& station : stations)
  {
    blah2::channelizer::IlluminatorOutput output;
    output.id = station.id;
    output.frequencyHz = station.frequencyHz;
    output.samples.channels = base;
    output.samples.firstInputSample = firstInputSample;
    output.samples.hasFirstInputSample = true;
    output.samples.inputSamplesPerOutput = decimation;
    output.samples.groupDelayInputSamples = 96;
    result.push_back(std::move(output));
  }
  return result;
}

blah2::multi_illuminator::RadarConfig radar_config(bool b210)
{
  blah2::multi_illuminator::RadarConfig config;
  config.cpiSeconds = .5;
  config.delayMin = -10; config.delayMax = 400;
  config.dopplerMin = -200; config.dopplerMax = 200;
  config.clutterDelayMin = -10; config.clutterDelayMax = 400;
  config.pfa = .00001;
  config.guardCells = 2; config.trainingCells = 6;
  config.minimumDelay = 5; config.minimumDopplerHz = 15;
  config.centroidRadius = 6;
  config.trackerM = 4; config.trackerN = 6; config.trackerDelete = 3;
  config.trackerMaximumAcceleration = 10;
  config.trackerMinimumSnrDb = 5;
  config.workers = b210 ? 1 : 5;
  config.arrayReference = !b210;
  config.referenceChannel = 0;
  config.referenceChannels = b210 ? std::vector<uint32_t>{} :
    std::vector<uint32_t>{0, 1, 2, 3, 4};
  config.surveillanceChannels = b210 ? std::vector<uint32_t>{1} :
    std::vector<uint32_t>{0, 1, 2, 3, 4};
  config.reference.analysisSamples = 32768;
  config.reference.analysisInterval = 10;
  config.reference.powerIterations = 12;
  config.reference.covarianceSmoothing = .8;
  config.reference.diagonalLoading = .001;
  return config;
}

std::size_t parse_count(const char* value, const char* name)
{
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (!value[0] || !end || *end || !parsed)
    throw std::invalid_argument(std::string("Invalid ") + name);
  return parsed;
}
}

int main(int argc, char** argv)
try
{
  std::size_t lanes = 0, warmups = 2, iterations = 20, laneWorkers = 0;
  uint32_t captureRate = 2400000, outputRate = 300000;
  std::string topology = "kraken";
  for (int index = 1; index < argc; ++index)
  {
    const std::string argument = argv[index];
    if (argument == "--lanes" && index + 1 < argc)
      lanes = parse_count(argv[++index], "lane count");
    else if (argument == "--warmup" && index + 1 < argc)
      warmups = parse_count(argv[++index], "warmup count");
    else if (argument == "--iterations" && index + 1 < argc)
      iterations = parse_count(argv[++index], "iteration count");
    else if (argument == "--topology" && index + 1 < argc)
      topology = argv[++index];
    else if (argument == "--lane-workers" && index + 1 < argc)
      laneWorkers = parse_count(argv[++index], "lane worker count");
    else if (argument == "--capture-rate" && index + 1 < argc)
      captureRate = uint32_t(parse_count(argv[++index], "capture rate"));
    else if (argument == "--sample-rate" && index + 1 < argc)
      outputRate = uint32_t(parse_count(argv[++index], "sample rate"));
    else
      throw std::invalid_argument("Usage: benchmarkMultiIlluminatorCpu --lanes 2|4|8|16 [--topology kraken|b210] [--lane-workers N] [--warmup N] [--iterations N]");
  }
  if (lanes != 2 && lanes != 4 && lanes != 8 && lanes != 16)
    throw std::invalid_argument("Lane count must be 2, 4, 8, or 16");
  if (topology != "kraken" && topology != "b210")
    throw std::invalid_argument("Topology must be kraken or b210");
  if (!outputRate || captureRate % outputRate)
    throw std::invalid_argument("Capture rate must be an integer multiple of sample rate");
  const uint32_t decimation = captureRate / outputRate;
  const std::size_t cpiSamples = outputRate / 2;
  const bool b210 = topology == "b210";
  const std::size_t physicalChannels = b210 ? 2 : 5;
  if (fftw_init_threads() == 0)
    throw std::runtime_error("FFTW thread initialization failed");
  const std::size_t availableCpus = blah2::available_cpu_threads();
  const std::size_t configuredFftMaximum = b210 ? 4 : 1;
  const std::size_t activeLaneWorkers =
    blah2::choose_illuminator_lane_workers(lanes, laneWorkers, availableCpus);
  const std::size_t fftThreads = blah2::cap_fft_threads_for_lanes(
    configuredFftMaximum, activeLaneWorkers, availableCpus);
  blah2::set_fftw_planner_threads(int(fftThreads));

  const auto stations = definitions(lanes, outputRate);
  const auto base = make_base_channels(physicalChannels, outputRate,
    cpiSamples);
  auto config = radar_config(b210);
  config.laneWorkers = activeLaneWorkers;
  blah2::multi_illuminator::MultiIlluminatorRadar radar(
    captureRate, physicalChannels, stations, config);
  blah2::utility::FixedExecutor serializer(std::min(lanes,
    std::max<std::size_t>(1, availableCpus - 1)));
  std::vector<Sample> measured;
  measured.reserve(iterations);
  volatile std::size_t outputSink = 0;
  for (std::size_t frame = 0; frame < warmups + iterations; ++frame)
  {
    const uint64_t first = frame * uint64_t(cpiSamples) * decimation;
    auto input = make_input(stations, base, first, decimation);
    const auto start = Clock::now();
    auto frames = radar.process(std::move(input));
    const auto radarDone = Clock::now();
    std::vector<std::size_t> productBytes(frames.size());
    const uint64_t timestamp = 1700000000000ULL + frame * 500;
    serializer.parallel_for(frames.size(), [&](std::size_t index) {
      const auto& result = frames[index];
      productBytes[index] =
        result.map->to_json_km(timestamp, result.sampleRateHz).size() +
        result.detections->to_json_km(timestamp, result.sampleRateHz).size() +
        result.tracks->to_json(timestamp).size();
    });
    std::size_t bytes = 0;
    for (const auto count : productBytes) bytes += count;
    outputSink = outputSink ^ bytes;
    const auto jsonDone = Clock::now();
    if (frame >= warmups)
      measured.push_back({milliseconds(start, radarDone),
        milliseconds(radarDone, jsonDone), bytes});
  }

  std::vector<double> radarTimings, json, total;
  std::size_t bytes = 0;
  for (const auto& sample : measured)
  {
    radarTimings.push_back(sample.radarMs);
    json.push_back(sample.jsonMs);
    total.push_back(sample.radarMs + sample.jsonMs);
    bytes = std::max(bytes, sample.jsonBytes);
  }
  std::cout << std::fixed << std::setprecision(3)
    << "{\"schema\":\"vectorwarp-multi-illuminator-cpu-benchmark-v1\""
    << ",\"lanes\":" << lanes
    << ",\"topology\":\"" << topology << "\""
    << ",\"physical_channels\":" << physicalChannels
    << ",\"capture_rate_hz\":" << captureRate
    << ",\"sample_rate_hz\":" << outputRate
    << ",\"cpi_ms\":500"
    << ",\"taps_per_phase\":24"
    << ",\"configured_fft_maximum\":" << configuredFftMaximum
    << ",\"fftw_threads\":" << fftThreads
    << ",\"serialization_workers\":" << serializer.size()
    << ",\"radar_lane_workers\":" << activeLaneWorkers
    << ",\"available_cpus\":" << availableCpus
    << ",\"warmups\":" << warmups
    << ",\"iterations\":" << iterations
    << ",\"radar_ms\":{\"p50\":" << percentile(radarTimings, .50)
    << ",\"p95\":" << percentile(radarTimings, .95)
    << ",\"max\":" << *std::max_element(radarTimings.begin(), radarTimings.end()) << "}"
    << ",\"json_ms\":{\"p50\":" << percentile(json, .50)
    << ",\"p95\":" << percentile(json, .95)
    << ",\"max\":" << *std::max_element(json.begin(), json.end()) << "}"
    << ",\"total_ms\":{\"p50\":" << percentile(total, .50)
    << ",\"p95\":" << percentile(total, .95)
    << ",\"max\":" << *std::max_element(total.begin(), total.end()) << "}"
    << ",\"max_json_bytes\":" << bytes
    << ",\"peak_rss_mib\":" << peak_rss_mib()
    << ",\"deadline_ms\":500"
    << ",\"p95_under_deadline\":"
    << (percentile(total, .95) < 500 ? "true" : "false")
    << ",\"sink\":" << outputSink << "}\n";
  return 0;
}
catch (const std::exception& error)
{
  std::cerr << error.what() << '\n';
  return 1;
}
