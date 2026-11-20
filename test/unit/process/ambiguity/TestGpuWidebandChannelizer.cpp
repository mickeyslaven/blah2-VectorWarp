#include "process/ambiguity/GpuWidebandChannelizer.h"
#include "process/ambiguity/GpuWidebandFallback.h"
#include "process/conditioning/SubbandChannelizer.h"
#include "process/conditioning/SamplePhase.h"

#include <catch2/catch_test_macros.hpp>
#include <fftw3.h>
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>
#include <string>
#include <vector>
#include <memory>
#include <cstring>

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

namespace
{
using RawChannels = std::vector<std::vector<std::complex<float>>>;

RawChannels qualificationInput(std::size_t channels, std::size_t samples,
  uint32_t seed = 98137)
{
  RawChannels result(channels, std::vector<std::complex<float>>(samples));
  for (auto& channel : result)
    for (auto& value : channel)
    {
      seed = seed * 1664525u + 1013904223u;
      const float real = float(int32_t(seed >> 8) - 8388608) / 8388608;
      seed = seed * 1664525u + 1013904223u;
      const float imaginary = float(int32_t(seed >> 8) - 8388608) / 8388608;
      value = {real, imaginary};
    }
  return result;
}

std::vector<blah2::wideband_gpu::LaneOutput> cpuQualificationOutputs(
  const blah2::wideband_gpu::Config& config,
  std::vector<std::unique_ptr<blah2::channelizer::SubbandChannelizer>>& cpu,
  const RawChannels& input, uint64_t first)
{
  std::vector<blah2::wideband_gpu::LaneOutput> result;
  for (std::size_t lane = 0; lane < cpu.size(); ++lane)
  {
    auto output = cpu[lane]->process(input, first);
    result.push_back({config.lanes[lane].frequencyHz, output.firstInputSample,
      output.hasFirstInputSample, output.inputSamplesPerOutput,
      std::move(output.channels)});
  }
  return result;
}

struct QualificationFixture
{
  blah2::wideband_gpu::Config config{24576000, 2, 8192, {}};
  std::vector<std::unique_ptr<blah2::channelizer::SubbandChannelizer>> cpu;

  QualificationFixture()
  {
    for (const auto offset : {-9000000.0, -3000000.0, 9000000.0})
    {
      const uint32_t taps = offset == -3000000 ? 128 : 64;
      config.lanes.push_back({uint64_t(500000000 + offset), 4, offset,
        blah2::wideband_gpu::designFilter(config.inputRateHz, 6144000,
          5300000, taps)});
      cpu.push_back(std::make_unique<blah2::channelizer::SubbandChannelizer>(
        blah2::channelizer::SubbandChannelizer::Config{config.inputRateHz,
          6144000, offset, 5300000, taps}, config.channels));
    }
  }
};
}

TEST_CASE("bounded portable mixer anchors preserve long CPI phase at either sign")
{
  using blah2::channelizer::mixerTurn;
  constexpr uint32_t rate = 2400000, overlap = 128;
  for (double offset : {-437501.0, 437501.0, -437500.25, 437500.25})
    for (uint64_t first : {uint64_t(17), uint64_t(1000017), (uint64_t(1)<<54)+17}) {
      const auto origin = mixerTurn(first, offset, rate)*std::conj(mixerTurn(overlap, offset, rate));
      for (uint32_t at : {0u, 127u, 128u, 4095u, 4096u, 4097u, 999999u, 1000127u}) {
        if (first + at < overlap) continue;
        const std::complex<float> base(origin*mixerTurn(uint64_t(at>>12)*4096, offset, rate));
        const std::complex<float> turn(mixerTurn(at&4095, offset, rate));
        // Match the production shader's FP32 complex multiply.
        const std::complex<float> actual(base.real()*turn.real()-base.imag()*turn.imag(),
          base.real()*turn.imag()+base.imag()*turn.real());
        const auto expected = mixerTurn(first + at - overlap, offset, rate);
        REQUIRE(std::abs(std::complex<double>(actual)-expected) < 2e-7);
      }
    }
}

TEST_CASE("sampled wideband oracle matches full CPU FIR across phases and gaps")
{
  using namespace blah2::wideband_gpu;
  QualificationFixture fixture;
  SelectedOutputQualifier oracle(fixture.config);
  uint64_t first = (uint64_t(1) << 54) + 3;
  for (const std::size_t count : {37u, 53u, 8192u, 79u})
  {
    auto input = qualificationInput(2, count, uint32_t(first));
    const auto output = cpuQualificationOutputs(fixture.config, fixture.cpu,
      input, first);
    const auto result = oracle.check(input, first, output, 1, 250);
    INFO(result.reason);
    INFO(result.worstRms);
    REQUIRE(result.accepted);
    REQUIRE(result.comparedSamples > 0);
    REQUIRE(result.oracleMultiplyAdds <= SelectedOutputQualifier::maximumMultiplyAdds);
    REQUIRE(result.worstRms < 1e-5);
    first += count;
    if (count == 8192) first += 101; // Both independent histories must reset.
  }
}

TEST_CASE("sampled wideband oracle preserves fractional mixers and empty prefixes")
{
  using namespace blah2::wideband_gpu;
  for (const double offset : {-37000.25, 37000.25})
  {
    Config config{240000, 1, 2048, {{101100000, 8, offset,
      designFilter(240000, 30000, 20000, 24)}}};
    std::vector<std::unique_ptr<blah2::channelizer::SubbandChannelizer>> cpu;
    cpu.push_back(std::make_unique<blah2::channelizer::SubbandChannelizer>(
      blah2::channelizer::SubbandChannelizer::Config{
        240000, 30000, offset, 20000, 24}, 1));
    SelectedOutputQualifier oracle(config);
    uint64_t first = 17;
    const RawChannels silent(1, std::vector<std::complex<float>>(2));
    auto output = cpuQualificationOutputs(config, cpu, silent, first);
    REQUIRE(output[0].channels[0].empty());
    const auto empty = oracle.check(silent, first, output, 1, 250);
    REQUIRE_FALSE(empty.accepted);
    REQUIRE(empty.verifiedSilence);
    first += 2;
    for (const std::size_t count : {3u, 2048u, 29u})
    {
      const auto input = qualificationInput(1, count, uint32_t(first));
      output = cpuQualificationOutputs(config, cpu, input, first);
      const auto result = oracle.check(input, first, output, 1, 250);
      if (output[0].channels[0].empty())
      {
        REQUIRE_FALSE(result.accepted);
        REQUIRE_FALSE(result.verifiedSilence);
        // Production fails closed here; reset both independent histories for
        // the next comparison rather than retaining an unaccepted prefix.
        oracle.reset(); cpu[0]->reset();
      }
      else
      {
        INFO(result.reason);
        REQUIRE(result.accepted);
        REQUIRE(result.worstRms < 1e-5);
      }
      first += count;
    }
  }
}

TEST_CASE("sampled wideband oracle rejects phase coefficient and boundary faults")
{
  using namespace blah2::wideband_gpu;
  QualificationFixture fixture;
  const auto input = qualificationInput(2, 8192);
  const uint64_t first = 11;
  const auto good = cpuQualificationOutputs(fixture.config, fixture.cpu, input, first);
  {
    SelectedOutputQualifier oracle(fixture.config);
    REQUIRE(oracle.check(input, first, good, 1, 250).accepted);
  }
  for (const std::size_t where : {std::size_t(0), std::size_t(64),
      good[0].channels[0].size() - 1})
  {
    auto damaged = good;
    damaged[0].channels[0][where] += std::complex<float>(.5f, -.3f);
    SelectedOutputQualifier oracle(fixture.config);
    REQUIRE_FALSE(oracle.check(input, first, damaged, 1, 250).accepted);
  }
  {
    auto damaged = good;
    for (auto& value : damaged[1].channels[1]) value *= std::polar(1.0f, .05f);
    SelectedOutputQualifier oracle(fixture.config);
    REQUIRE_FALSE(oracle.check(input, first, damaged, 1, 250).accepted);
  }
  {
    auto damaged = good;
    auto changedFilter = fixture.config.lanes[2];
    changedFilter.taps[changedFilter.taps.size() / 2] += .05;
    damaged[2].channels = overlapSave(changedFilter, fixture.config.inputRateHz,
      input, first);
    SelectedOutputQualifier oracle(fixture.config);
    REQUIRE_FALSE(oracle.check(input, first, damaged, 1, 250).accepted);
  }
  {
    auto damaged = good;
    const auto selected = SelectedOutputQualifier::selectedIndices(
      damaged[0].channels[0].size(), fixture.config.lanes[0].taps.size(), 4);
    std::size_t unsampled = 0;
    while (std::binary_search(selected.begin(), selected.end(), unsampled)) ++unsampled;
    REQUIRE(unsampled < damaged[0].channels[0].size());
    damaged[0].channels[0][unsampled] =
      {std::numeric_limits<float>::quiet_NaN(), 0};
    SelectedOutputQualifier oracle(fixture.config);
    REQUIRE_FALSE(oracle.check(input, first, damaged, 1, 250).accepted);
  }
  {
    auto damaged = good;
    ++damaged[0].firstInputSample;
    SelectedOutputQualifier oracle(fixture.config);
    REQUIRE_FALSE(oracle.check(input, first, damaged, 1, 250).accepted);
    REQUIRE(oracle.check(input, first, good, 251, 250).accepted);
  }
}

TEST_CASE("sampled wideband oracle distinguishes exact silence from qualification")
{
  using namespace blah2::wideband_gpu;
  QualificationFixture fixture;
  SelectedOutputQualifier oracle(fixture.config);
  RawChannels silent(2, std::vector<std::complex<float>>(8192));
  auto output = cpuQualificationOutputs(fixture.config, fixture.cpu, silent, 0);
  const auto silence = oracle.check(silent, 0, output, 1, 250);
  REQUIRE_FALSE(silence.accepted);
  REQUIRE(silence.status == QualificationStatus::inconclusive);
  REQUIRE(silence.verifiedSilence);
  output[0].channels[0][100] = {1, 0};
  SelectedOutputQualifier corruptSilence(fixture.config);
  const auto corrupt = corruptSilence.check(silent, 0, output, 1, 250);
  REQUIRE_FALSE(corrupt.accepted);
  REQUIRE_FALSE(corrupt.verifiedSilence);
  const auto signal = qualificationInput(2, 8192);
  output = cpuQualificationOutputs(fixture.config, fixture.cpu, signal, 8192);
  REQUIRE(oracle.check(signal, 8192, output, 1, 250).accepted);
}

TEST_CASE("sampled wideband oracle has a CPI-independent operation bound")
{
  using namespace blah2::wideband_gpu;
  QualificationFixture fixture;
  uint64_t work = 0;
  for (const auto& lane : fixture.config.lanes)
  {
    const auto selected = SelectedOutputQualifier::selectedIndices(
      6144000 / 4, lane.taps.size(), lane.decimation);
    REQUIRE(selected.size() <= 83);
    REQUIRE(selected.front() == 0);
    REQUIRE(selected.back() == 6144000 / 4 - 1);
    work += selected.size() * lane.taps.size() * fixture.config.channels;
  }
  REQUIRE(work < 200000);
  Config expensive{2400000, 8, 1024, {{100000000, 2, 0,
    std::vector<double>(maxFilterTaps, 1.0 / maxFilterTaps)}}};
  SelectedOutputQualifier bounded(expensive);
  const RawChannels input(8, std::vector<std::complex<float>>(1024, {1, 0}));
  const std::vector<LaneOutput> output{{100000000, 0, true, 2,
    RawChannels(8, std::vector<std::complex<float>>(512))}};
  const auto result = bounded.check(input, 0, output, 1, 250);
  REQUIRE_FALSE(result.accepted);
  REQUIRE(result.reason.find("operation budget") != std::string::npos);
}

TEST_CASE("sampled wideband oracle times full CPI geometry", "[.wideband-full-cpi]")
{
  using namespace blah2::wideband_gpu;
  QualificationFixture fixture;
  // HomePortal's live geometry is a 0.5-second block at 24.576 MS/s.
  constexpr std::size_t period = 4096, samples = 12288000;
  auto shortInput = qualificationInput(2, 2 * period);
  for (auto& channel : shortInput)
    std::copy(channel.begin(), channel.begin() + period, channel.begin() + period);
  // The input and all three integer NCOs repeat after 4096 raw samples. The
  // second period is past every FIR transient, so its exact CPU result can
  // populate a full-size synthetic worker result without a full CPU CPI.
  auto output = cpuQualificationOutputs(fixture.config, fixture.cpu, shortInput, 0);
  RawChannels fullInput(2, std::vector<std::complex<float>>(samples));
  for (std::size_t channel = 0; channel < fullInput.size(); ++channel)
    for (std::size_t sample = 0; sample < samples; ++sample)
      fullInput[channel][sample] = shortInput[channel][sample % period];
  for (auto& lane : output)
    for (auto& channel : lane.channels)
    {
      const auto prefix = channel;
      channel.resize(samples / 4);
      for (std::size_t sample = 0; sample < channel.size(); ++sample)
        channel[sample] = prefix[sample < prefix.size() ? sample :
          period / 4 + sample % (period / 4)];
    }
  fixture.config.maxInputSamples = samples;
  SelectedOutputQualifier oracle(fixture.config);
  const auto before = std::chrono::steady_clock::now();
  const auto result = oracle.check(fullInput, 0, output, 1, 500);
  const double milliseconds = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - before).count();
  INFO("full-CPI oracle milliseconds=" << milliseconds);
  INFO("compared=" << result.comparedSamples << ", MACs=" << result.oracleMultiplyAdds);
  INFO(result.reason);
  REQUIRE(result.accepted);
  REQUIRE(result.oracleMultiplyAdds < 200000);
  // Wall time is diagnostic, not a machine-specific correctness threshold.
}

TEST_CASE("wideband GPU plan shares exact absolute fractional CPU phase")
{
  using namespace blah2::wideband_gpu;
  constexpr uint32_t rate = 131070;
  constexpr uint64_t base = (uint64_t(1) << 63) + 71;
  for (double offset : {-1., 1., -.5, .5, -.1, .1, 1e-10}) {
    Config config{rate, 2, 256, {{100000000, 2, offset,
      designFilter(rate, rate / 2, 40000, 4)}}};
    Plan plan(config);
    SelectedOutputQualifier oracle(config);
    std::vector<std::unique_ptr<blah2::channelizer::SubbandChannelizer>> cpu;
    cpu.push_back(std::make_unique<blah2::channelizer::SubbandChannelizer>(
      blah2::channelizer::SubbandChannelizer::Config{rate, rate / 2, offset,
        40000, 4}, 2));
    uint64_t previous = 0;
    for (uint64_t first : {base, base + 31, base + 1001, base - 500}) {
      const auto dispatch = plan.prepare(first, 31);
      REQUIRE(dispatch.resetHistory == (first != previous + 31));
      previous = first;
      float real = 0, imaginary = 0;
      std::memcpy(&real, &dispatch.lanes[0].baseReal, sizeof(real));
      std::memcpy(&imaginary, &dispatch.lanes[0].baseImaginary, sizeof(imaginary));
      const auto expected = std::polar(1., 2 * pi *
        blah2::channelizer::mixerPhaseCycles(first, offset, rate));
      REQUIRE(std::abs(std::complex<double>(real, imaginary) - expected) < 1e-7);
      if (std::trunc(offset) != offset)
        REQUIRE(dispatch.lanes[0].padding3 == 0); // No rounded integer period.
      const auto input = qualificationInput(2, 31, 73);
      const auto output = cpuQualificationOutputs(config, cpu, input, first);
      const auto result = oracle.check(input, first, output, 1, 250);
      INFO(result.reason);
      REQUIRE(result.accepted);
      REQUIRE(result.worstRms < 1e-5);
    }
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

TEST_CASE("wideband GPU admits high-rate generic FFT geometry without a folding veto")
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

  auto generic = configuration();
  generic.inputRateHz = input;
  generic.lanes.front().taps.resize(1033);
  REQUIRE(Plan(std::move(generic)).runtimeSelectable());
  Config mixed{3200000, 2, 4096, {
    {100000000, 8, -.25, filter(3200000, 400000, 200000, 16)},
    {101000000, 10, 100000.25, filter(3200000, 320000, 200000, 16)}}};
  Plan mixedPlan(mixed);
  REQUIRE(mixedPlan.runtimeSelectable());
  REQUIRE(mixedPlan.prepare(11, 4096).lanes.size() == 2);
  mixed.maxInputSamples = UINT32_MAX;
  REQUIRE_THROWS(Plan(mixed)); // No weakening of checked layout/memory bounds.
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
  REQUIRE(Plan(std::move(hostile)).runtimeSelectable());

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
