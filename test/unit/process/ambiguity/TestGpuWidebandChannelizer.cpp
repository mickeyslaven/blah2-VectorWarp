#include "process/ambiguity/GpuWidebandChannelizer.h"
#include "process/conditioning/SubbandChannelizer.h"

#include <catch2/catch_test_macros.hpp>
#include <fftw3.h>
#include <cmath>
#include <complex>
#include <limits>
#include <string>
#include <vector>

namespace
{
constexpr double pi = 3.141592653589793238462643383279502884;

std::vector<double> filter(uint32_t inputRate, uint32_t outputRate,
  uint32_t occupied, uint32_t tapsPerPhase)
{
  const uint32_t decimation = inputRate / outputRate;
  std::size_t count = std::size_t(tapsPerPhase) * decimation + 1;
  if (!(count & 1)) ++count;
  const double normalized = (occupied / 2.0 + outputRate / 2.0) /
    2.0 / inputRate;
  const double middle = (count - 1) / 2.0;
  std::vector<double> result(count);
  double sum = 0;
  for (std::size_t index = 0; index < count; ++index)
  {
    const double offset = double(index) - middle;
    const double sinc = offset == 0 ? 2 * normalized :
      std::sin(2 * pi * normalized * offset) / (pi * offset);
    const double window = .54 - .46 *
      std::cos(2 * pi * index / double(count - 1));
    sum += result[index] = sinc * window;
  }
  for (auto& value : result) value /= sum;
  return result;
}

blah2::wideband_gpu::Config configuration()
{
  constexpr uint32_t input = 2400000;
  return {input, 5, 4096, {
    {90500000, 8, -500000, filter(input, 300000, 200000, 128)},
    {91500000, 8, 500000, filter(input, 300000, 200000, 128)},
    {92000000, 4, 1000000, filter(input, 600000, 400000, 64)}}};
}

std::vector<std::vector<std::complex<float>>> overlapSave(
  const blah2::wideband_gpu::LaneConfig& lane, uint32_t inputRate,
  const std::vector<std::vector<std::complex<float>>>& input,
  uint64_t first)
{
  uint32_t fft = 1;
  while (fft < 2 * lane.taps.size()) fft *= 2;
  const uint32_t overlap = uint32_t(lane.taps.size() - 1);
  const uint32_t hop = fft - overlap;
  std::vector<std::complex<double>> filterInput(fft), spectrum(fft), work(fft);
  for (std::size_t i = 0; i < lane.taps.size(); ++i)
    filterInput[i] = lane.taps[i];
  auto* fi = reinterpret_cast<fftw_complex*>(filterInput.data());
  auto* sp = reinterpret_cast<fftw_complex*>(spectrum.data());
  auto* wk = reinterpret_cast<fftw_complex*>(work.data());
  fftw_plan filterPlan = fftw_plan_dft_1d(int(fft), fi, sp,
    FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_execute(filterPlan);
  fftw_destroy_plan(filterPlan);
  fftw_plan forward = fftw_plan_dft_1d(int(fft), wk, wk,
    FFTW_FORWARD, FFTW_ESTIMATE);
  fftw_plan inverse = fftw_plan_dft_1d(int(fft), wk, wk,
    FFTW_BACKWARD, FFTW_ESTIMATE);
  std::vector<std::vector<std::complex<float>>> result(input.size());
  for (std::size_t channel = 0; channel < input.size(); ++channel)
  {
    std::vector<std::complex<double>> filtered(input[channel].size());
    for (uint32_t block = 0; block * hop < input[channel].size(); ++block)
    {
      for (uint32_t j = 0; j < fft; ++j)
      {
        const int64_t sample = int64_t(block) * hop + j - overlap;
        if (sample < 0 || uint64_t(sample) >= input[channel].size())
          work[j] = {};
        else
          work[j] = std::complex<double>(input[channel][sample]) *
            std::polar(1.0, -2 * pi * lane.centerOffsetHz *
              double(first + sample) / inputRate);
      }
      fftw_execute(forward);
      for (uint32_t j = 0; j < fft; ++j)
        work[j] = work[j] * spectrum[j] / double(fft);
      fftw_execute(inverse);
      for (uint32_t j = 0; j < hop; ++j)
      {
        const uint32_t sample = block * hop + j;
        if (sample < filtered.size()) filtered[sample] = work[overlap + j];
      }
    }
    for (uint32_t sample = 0; sample < filtered.size(); ++sample)
      if ((first + sample) % lane.decimation == 0)
        result[channel].push_back(std::complex<float>(filtered[sample]));
  }
  fftw_destroy_plan(forward);
  fftw_destroy_plan(inverse);
  return result;
}
}

TEST_CASE("wideband GPU plan packs five channels and all illuminators once")
{
  using namespace blah2::wideband_gpu;
  Plan plan(configuration());
  REQUIRE(plan.config().channels == 5);
  REQUIRE(plan.config().lanes.size() == 3);
  REQUIRE(plan.historySamples() == 1024);
  REQUIRE(plan.layout().inputElements == 5 * (4096 + 1024));
  REQUIRE(plan.layout().tapElements == 1025 + 1025 + 257);
  REQUIRE(plan.layout().dispatchBytes == 3 * sizeof(LaneDispatch));
  REQUIRE(plan.runtimeSelectable());
  REQUIRE(std::string(plan.runtimeBlockReason()).empty());

  auto first = plan.prepare(3, 4096);
  REQUIRE(first.resetHistory);
  REQUIRE(first.historySamples == 0);
  REQUIRE(first.lanes[0].firstRemainder == 5);
  REQUIRE(first.lanes[0].outputSamples == 512);
  REQUIRE(first.lanes[1].outputOffset == 512 * 5);
  REQUIRE(first.lanes[2].outputOffset == 1024 * 5);
  REQUIRE(first.outputElements == (512 + 512 + 1024) * 5);

  auto contiguous = plan.prepare(4099, 100);
  REQUIRE_FALSE(contiguous.resetHistory);
  REQUIRE(contiguous.historySamples == 1024);
  for (const auto& lane : contiguous.lanes)
    REQUIRE(lane.inputPrefix == 1024);

  auto gap = plan.prepare(9000, 64);
  REQUIRE(gap.resetHistory);
  REQUIRE(gap.historySamples == 0);
  for (const auto& lane : gap.lanes) REQUIRE(lane.inputPrefix == 0);
}

TEST_CASE("wideband GPU admits a bounded small-decimation direct envelope")
{
  using namespace blah2::wideband_gpu;
  constexpr uint32_t input = 24576000;
  Config config{input, 2, input / 2, {
    {509000000, 4, -9000000, filter(input, 6144000, 5380000, 64)},
    {515000000, 4, -3000000, filter(input, 6144000, 5832844, 128)},
    {527000000, 4, 9000000, filter(input, 6144000, 5380000, 64)}}};
  Plan plan(std::move(config));
  REQUIRE(plan.runtimeSelectable());
  REQUIRE(std::string(plan.runtimeBlockReason()).empty());

  auto excessive = configuration();
  excessive.inputRateHz = input;
  excessive.maxInputSamples = input;
  for (auto& lane : excessive.lanes) lane.decimation = 2;
  REQUIRE_FALSE(Plan(std::move(excessive)).runtimeSelectable());
}

TEST_CASE("wideband GPU plan preserves independent lane metadata")
{
  using namespace blah2::wideband_gpu;
  Plan plan(configuration());
  const auto dispatch = plan.prepare((uint64_t(7) << 32) + 1, 37);
  REQUIRE(dispatch.lanes.size() == 3);
  REQUIRE(dispatch.lanes[0].decimation == 8);
  REQUIRE(dispatch.lanes[2].decimation == 4);
  REQUIRE(dispatch.lanes[0].tapOffset == 0);
  REQUIRE(dispatch.lanes[1].tapOffset == 1025);
  REQUIRE(dispatch.lanes[2].tapOffset == 2050);
  REQUIRE((dispatch.lanes[0].centerLow != dispatch.lanes[1].centerLow ||
    dispatch.lanes[0].centerHigh != dispatch.lanes[1].centerHigh));
  REQUIRE(dispatch.lanes[0].outputOffset < dispatch.lanes[1].outputOffset);
  REQUIRE(dispatch.lanes[1].outputOffset <= dispatch.lanes[2].outputOffset);

  const std::string shader = Plan::qualificationShaderSource();
  REQUIRE(shader.find("GL_ARB_gpu_shader_fp64") != std::string::npos);
  REQUIRE(shader.find("binding=0") != std::string::npos);
  REQUIRE(shader.find("lane<p.laneCount") != std::string::npos);
  REQUIRE(shader.find("double center=packDouble2x32(c.xy)") !=
    std::string::npos);
}

TEST_CASE("wideband GPU ABI qualifies full B210-width folded filter quality")
{
  using namespace blah2::wideband_gpu;
  constexpr uint32_t input = 60000000, output = 300000;
  Config config{input, 5, 65536, {{101100000, input / output, 0,
    filter(input, output, 200000, 24)}}};
  REQUIRE(config.lanes.front().taps.size() == 4801);
  Plan plan(std::move(config));
  REQUIRE(plan.packedTaps().size() == 4801);
  REQUIRE(plan.historySamples() == 4800);
  REQUIRE(plan.runtimeSelectable());
  const auto dispatch = plan.prepare(0, 65536);
  REQUIRE(dispatch.lanes.front().tapCount == 4801);
  Config odd{input,1,65536,{{101100000,input/output,0,
    std::vector<double>(4800,1.0/4800)}}};
  REQUIRE_THROWS(Plan(std::move(odd)));
  Config hostile{input,1,65536,{{101100000,input/output,1,
    filter(input,output,200000,24)}}};
  REQUIRE_FALSE(Plan(std::move(hostile)).runtimeSelectable());

  constexpr uint32_t mimoInput=30720000,mimoOutput=320000;
  Config mimo{mimoInput,2,65536,{}};
  for(int lane=0;lane<16;++lane)mimo.lanes.push_back({
    uint64_t(100000000+(lane-8)*200000),mimoInput/mimoOutput,
    double((lane-8)*200000),filter(mimoInput,mimoOutput,200000,24)});
  REQUIRE(mimo.lanes.front().decimation==96);
  REQUIRE(mimo.lanes.front().taps.size()==2305);
  Plan mimoPlan(std::move(mimo));
  REQUIRE(mimoPlan.runtimeSelectable());
  REQUIRE(mimoPlan.historySamples()==2304);
}

TEST_CASE("B210 MIMO D96 convention preserves full-quality CPU parity")
{
  constexpr uint32_t inputRate=30720000,outputRate=320000;
  blah2::channelizer::SubbandChannelizer::Config cpuConfig{
    inputRate,outputRate,400000,200000,24};
  blah2::channelizer::SubbandChannelizer cpu(cpuConfig,2);
  blah2::wideband_gpu::LaneConfig lane{100400000,inputRate/outputRate,
    cpuConfig.centerOffsetHz,filter(inputRate,outputRate,
      uint32_t(cpuConfig.occupiedBandwidthHz),cpuConfig.tapsPerPhase)};
  std::vector<std::vector<std::complex<float>>> input(2,
    std::vector<std::complex<float>>(12000));
  for(size_t channel=0;channel<input.size();++channel)
    for(size_t sample=0;sample<input[channel].size();++sample)
      input[channel][sample]=std::polar(float(.3+.1*channel),
        float(.083*sample+.17*channel));
  constexpr uint64_t first=317;
  const auto expected=cpu.process(input,first);
  const auto actual=overlapSave(lane,inputRate,input,first);
  for(size_t channel=0;channel<actual.size();++channel)
    for(size_t sample=0;sample<actual[channel].size();++sample)
      REQUIRE(std::abs(actual[channel][sample]-
        expected.channels[channel][sample])<2e-5f);
}

TEST_CASE("batched overlap-save convention matches the production CPU channelizer")
{
  constexpr uint32_t inputRate = 240000, outputRate = 60000;
  blah2::channelizer::SubbandChannelizer::Config cpuConfig{
    inputRate, outputRate, 37000, 40000, 24};
  blah2::channelizer::SubbandChannelizer cpu(cpuConfig, 5);
  blah2::wideband_gpu::LaneConfig lane{101100000,
    inputRate / outputRate, cpuConfig.centerOffsetHz,
    filter(inputRate, outputRate, uint32_t(cpuConfig.occupiedBandwidthHz),
      cpuConfig.tapsPerPhase)};
  std::vector<std::vector<std::complex<float>>> input(5,
    std::vector<std::complex<float>>(1500));
  for (std::size_t channel = 0; channel < input.size(); ++channel)
    for (std::size_t sample = 0; sample < input[channel].size(); ++sample)
      input[channel][sample] = std::polar(float(.2 + .03 * channel),
        float(.017 * sample + .2 * channel));
  constexpr uint64_t first = 7;
  const auto expected = cpu.process(input, first);
  const auto actual = overlapSave(lane, inputRate, input, first);
  REQUIRE(actual.size() == expected.channels.size());
  for (std::size_t channel = 0; channel < actual.size(); ++channel)
  {
    REQUIRE(actual[channel].size() == expected.channels[channel].size());
    for (std::size_t sample = 0; sample < actual[channel].size(); ++sample)
      REQUIRE(std::abs(actual[channel][sample] -
        expected.channels[channel][sample]) < 2e-5f);
  }
}

TEST_CASE("wideband GPU plan rejects truncation and unsafe geometry")
{
  using namespace blah2::wideband_gpu;
  auto config = configuration();
  config.channels = 9;
  REQUIRE_THROWS_AS(Plan(config), std::invalid_argument);
  config = configuration();
  config.lanes.front().taps.push_back(0);
  REQUIRE_THROWS_AS(Plan(config), std::invalid_argument);
  config = configuration();
  config.lanes.front().taps.front() =
    std::numeric_limits<double>::quiet_NaN();
  REQUIRE_THROWS_AS(Plan(config), std::invalid_argument);
  Plan valid(configuration());
  REQUIRE_THROWS_AS(valid.prepare(0, 4097), std::invalid_argument);
  REQUIRE_THROWS_AS(valid.prepare(std::numeric_limits<uint64_t>::max(), 2),
    std::invalid_argument);
}
