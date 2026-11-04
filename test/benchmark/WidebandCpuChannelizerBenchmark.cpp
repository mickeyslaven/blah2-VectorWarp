#include "process/conditioning/IlluminatorChannelizerBank.h"
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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
constexpr uint32_t outputRate = 300000;

std::size_t parse_count(const char* value, const char* name)
{
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (!value[0] || !end || *end || !parsed)
    throw std::invalid_argument(std::string("Invalid ") + name);
  return parsed;
}

double percentile(std::vector<double> values, double fraction)
{
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

std::vector<std::vector<std::complex<float>>> input_fixture(
  std::size_t physicalChannels, std::size_t inputSamples)
{
  std::vector<std::vector<std::complex<float>>> input(physicalChannels,
    std::vector<std::complex<float>>(inputSamples));
  uint32_t state = 0x63d83595U;
  for (std::size_t sample = 0; sample < inputSamples; ++sample)
  {
    state = state * 1664525U + 1013904223U;
    const float i = float(int32_t(state)) / 2147483648.0f;
    state = state * 1664525U + 1013904223U;
    const float q = float(int32_t(state)) / 2147483648.0f;
    input[0][sample] = {i, q};
    input[1][sample] = std::complex<float>(
      std::complex<double>(i, q) * std::polar(1.0, .37));
  }
  return input;
}

std::vector<blah2::channelizer::IlluminatorDefinition> definitions(
  std::size_t lanes, uint32_t inputRate)
{
  const int64_t spacing = inputRate >= 60000000 ? 300000 : 100000;
  std::vector<blah2::channelizer::IlluminatorDefinition> result;
  for (std::size_t lane = 0; lane < lanes; ++lane)
  {
    const int64_t offset = (int64_t(lane) - int64_t(lanes / 2)) * spacing;
    result.push_back({"fm-" + std::to_string(lane),
      uint64_t(100000000 + offset), 200000, outputRate, 24});
  }
  return result;
}
}

int main(int argc, char** argv)
try
{
  std::size_t lanes = 0, warmups = 1, iterations = 20;
  uint32_t inputRate = 60000000;
  std::size_t physicalChannels = 0, inputSamples = 0;
  for (int index = 1; index < argc; ++index)
  {
    const std::string argument = argv[index];
    if (argument == "--lanes" && index + 1 < argc)
      lanes = parse_count(argv[++index], "lane count");
    else if (argument == "--warmup" && index + 1 < argc)
      warmups = parse_count(argv[++index], "warmup count");
    else if (argument == "--iterations" && index + 1 < argc)
      iterations = parse_count(argv[++index], "iteration count");
    else if (argument == "--input-rate" && index + 1 < argc)
      inputRate = uint32_t(parse_count(argv[++index], "input rate"));
    else if (argument == "--physical-channels" && index + 1 < argc)
      physicalChannels = parse_count(argv[++index], "physical channel count");
    else if (argument == "--samples" && index + 1 < argc)
      inputSamples = parse_count(argv[++index], "input sample count");
    else
      throw std::invalid_argument("Usage: benchmarkWidebandCpuChannelizer --lanes 2|4|8|16 [--input-rate 60000000|2400000] [--physical-channels N] [--samples N] [--warmup N] [--iterations N]");
  }
  if (lanes != 2 && lanes != 4 && lanes != 8 && lanes != 16)
    throw std::invalid_argument("Lane count must be 2, 4, 8 or 16");
  if (inputRate != 60000000 && inputRate != 2400000)
    throw std::invalid_argument("Input rate must be 60000000 or 2400000");
  if (!physicalChannels) physicalChannels = inputRate == 2400000 ? 5 : 2;
  if (!inputSamples) inputSamples = inputRate / 2;
  if (fftw_init_threads() == 0)
    throw std::runtime_error("FFTW thread initialization failed");
  constexpr std::size_t configuredFftMaximum = 4;
  const std::size_t availableCpus = blah2::available_cpu_threads();
  const int fftThreads = int(blah2::cap_fft_threads_for_lanes(
    configuredFftMaximum, lanes, availableCpus));
  blah2::set_fftw_planner_threads(fftThreads);
  setenv("VECTORWARP_CHANNELIZER_STAGE_TIMING", "1", 1);
  const auto stations = definitions(lanes, inputRate);
  blah2::channelizer::IlluminatorChannelizerBank bank(100000000, inputRate,
    physicalChannels, stations);
  const auto input = input_fixture(physicalChannels, inputSamples);
  std::vector<double> timings;
  std::vector<double> forwardTimings, foldTimings, inverseTimings;
  timings.reserve(iterations);
  volatile std::size_t outputSink = 0;
  for (std::size_t frame = 0; frame < warmups + iterations; ++frame)
  {
    const uint64_t first = frame * uint64_t(inputSamples);
    const auto start = Clock::now();
    const auto output = bank.process(input, first);
    const auto finish = Clock::now();
    if (output.size() != lanes)
      throw std::runtime_error("Channelizer lane count changed");
    for (const auto& lane : output)
      for (const auto& channel : lane.samples.channels)
      {
        if (channel.size() != inputSamples / (inputRate / outputRate))
          throw std::runtime_error("Channelizer output count changed");
        outputSink = outputSink ^ channel.size();
      }
    if (frame >= warmups)
    {
      timings.push_back(std::chrono::duration<double, std::milli>(
        finish - start).count());
      const auto stages = bank.last_stage_timing();
      forwardTimings.push_back(stages.forwardMs);
      foldTimings.push_back(stages.foldMs);
      inverseTimings.push_back(stages.inverseMs);
    }
  }
  std::cout << std::fixed << std::setprecision(3)
    << "{\"schema\":\"vectorwarp-wideband-cpu-channelizer-benchmark-v1\""
    << ",\"lanes\":" << lanes
    << ",\"physical_channels\":" << physicalChannels
    << ",\"input_sample_rate_hz\":" << inputRate
    << ",\"output_sample_rate_hz\":" << outputRate
    << ",\"cpi_ms\":" << (1000.0 * inputSamples / inputRate)
    << ",\"taps_per_phase\":24"
    << ",\"configured_fft_maximum\":" << configuredFftMaximum
    << ",\"fftw_threads\":" << fftThreads
    << ",\"lane_workers\":" << bank.worker_count()
    << ",\"shared_forward\":" << (bank.uses_shared_forward() ? "true" : "false")
    << ",\"available_cpus\":" << availableCpus
    << ",\"warmups\":" << warmups
    << ",\"iterations\":" << iterations
    << ",\"latency_ms\":{\"p50\":" << percentile(timings, .50)
    << ",\"p95\":" << percentile(timings, .95)
    << ",\"max\":" << *std::max_element(timings.begin(), timings.end())
    << "},\"stage_p50_ms\":{\"forward\":" << percentile(forwardTimings, .50)
    << ",\"fold\":" << percentile(foldTimings, .50)
    << ",\"inverse\":" << percentile(inverseTimings, .50)
    << "},\"peak_rss_mib\":" << peak_rss_mib()
    << ",\"deadline_ms\":500"
    << ",\"p95_under_deadline\":"
    << (percentile(timings, .95) < 500 ? "true" : "false")
    << ",\"sink\":" << outputSink << "}\n";
  return 0;
}
catch (const std::exception& error)
{
  std::cerr << error.what() << '\n';
  return 1;
}
