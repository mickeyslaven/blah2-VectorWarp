#pragma once
#include "process/ambiguity/GpuWidebandChannelizer.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>
namespace blah2::wideband_gpu {
inline bool needsCpuFallback(const std::string& mode,bool gpuActive){return !gpuActive||mode=="auto";}
template<class Gpu,class Failure> bool attemptWithAutoFallback(bool automatic,Gpu&& gpu,Failure&& failure){try{std::forward<Gpu>(gpu)();return true;}catch(const std::exception& error){if(!automatic)throw;std::forward<Failure>(failure)(error);return false;}}

enum class QualificationStatus {passed,inconclusive,failed};
struct QualificationResult
{
  QualificationStatus status=QualificationStatus::failed;
  bool accepted=false;
  double worstRms=0,worstPeak=0;
  std::string reason;
  bool verifiedSilence=false;
  uint64_t comparedSamples=0,oracleMultiplyAdds=0;
};

// Compare the first real capture after both stateful channelizers process it.
// The caller publishes the CPU output for that capture regardless of outcome;
// on success both histories are already positioned for the next GPU capture,
// while on failure the CPU history is ready for fail-closed continuation.
inline QualificationResult qualifyFirstBlock(
  const std::vector<LaneOutput>& cpu,const std::vector<LaneOutput>& gpu,
  double gpuMilliseconds,double deadlineMilliseconds,
  double rmsLimit=2e-3,double peakLimit=5e-3)
{
  QualificationResult result;
  if(!std::isfinite(gpuMilliseconds)||gpuMilliseconds<0||
      !std::isfinite(deadlineMilliseconds)||deadlineMilliseconds<=0||
      gpuMilliseconds>deadlineMilliseconds||!std::isfinite(rmsLimit)||
      !std::isfinite(peakLimit)||rmsLimit<0||peakLimit<0)
  {result.reason="wideband GPU first-block deadline qualification failed";return result;}
  if(cpu.size()!=gpu.size())
  {result.reason="wideband GPU qualification lane count differs";return result;}
  double representativeEnergy=0,representativeSamples=0;
  for(size_t lane=0;lane<cpu.size();++lane)
  {
    const auto& expected=cpu[lane];const auto& actual=gpu[lane];
    if(expected.frequencyHz!=actual.frequencyHz||
       expected.firstInputSample!=actual.firstInputSample||
       expected.hasFirstInputSample!=actual.hasFirstInputSample||
       expected.inputSamplesPerOutput!=actual.inputSamplesPerOutput||
       expected.channels.size()!=actual.channels.size())
    {result.reason="wideband GPU qualification metadata differs";return result;}
    for(size_t channel=0;channel<expected.channels.size();++channel)
    {
      if(expected.channels[channel].size()!=actual.channels[channel].size())
      {result.reason="wideband GPU qualification sample count differs";return result;}
      double error=0,signal=0,maxError=0,peak=0;
      for(size_t sample=0;sample<expected.channels[channel].size();++sample)
      {
        const std::complex<double> reference(expected.channels[channel][sample]);
        const std::complex<double> difference=
          std::complex<double>(actual.channels[channel][sample])-reference;
        const double e=std::norm(difference),s=std::norm(reference);
        if(!std::isfinite(e)||!std::isfinite(s))
        {result.reason="wideband GPU qualification output is non-finite";return result;}
        error+=e;signal+=s;representativeEnergy+=s;
        representativeSamples+=1;maxError=std::max(maxError,e);
        peak=std::max(peak,s);
      }
      if(!std::isfinite(error)||!std::isfinite(signal)||
          !std::isfinite(representativeEnergy))
      {result.reason="wideband GPU qualification energy overflowed";
       return result;}
      const double rms=std::sqrt(error/std::max(signal,1e-30));
      const double relativePeak=std::sqrt(maxError/std::max(peak,1e-30));
      result.worstRms=std::max(result.worstRms,rms);
      result.worstPeak=std::max(result.worstPeak,relativePeak);
      if(rms>rmsLimit||relativePeak>peakLimit)
      {result.reason="wideband GPU first-block numerical qualification failed";return result;}
    }
  }
  if(!representativeSamples||
      representativeEnergy<=representativeSamples*1e-20)
  {result.status=QualificationStatus::inconclusive;
   result.reason="wideband GPU qualification input is not representative";
   return result;}
  result.status=QualificationStatus::passed;result.accepted=true;
  result.reason="wideband GPU first-block qualification passed";
  return result;
}

// Independent, bounded direct-FIR checks of the full-size worker result. This
// is a sampled runtime qualification, not an exhaustive proof of every output;
// exhaustive streaming/backend parity remains an offline qualification gate.
// No input/output is shortened and no partially advanced CPU channelizer is
// used. Only the preceding raw FIR support is retained between checks.
class SelectedOutputQualifier
{
public:
  static constexpr uint64_t maximumMultiplyAdds = 4'000'000;

  explicit SelectedOutputQualifier(Config config) : config_(std::move(config))
  {
    const Plan validated(config_);
    historySamples_ = validated.historySamples();
    history_.assign(config_.channels,
      std::vector<std::complex<float>>(historySamples_));
  }

  void reset()
  {
    for (auto& channel : history_)
      std::fill(channel.begin(), channel.end(), std::complex<float>{});
    started_ = false;
  }

  // Every lane includes the startup/settling boundary, both ends, and
  // stratified interior samples. Adjacent samples expose phase/scheduling
  // faults which a single tone or a prefix-only comparison can conceal.
  static std::vector<std::size_t> selectedIndices(std::size_t count,
    std::size_t taps, uint32_t decimation)
  {
    std::vector<std::size_t> result;
    if (!count || !taps || !decimation) return result;
    const auto add = [&](std::size_t index) {
      if (index < count) result.push_back(index);
    };
    for (std::size_t i = 0; i < std::min<std::size_t>(8, count); ++i)
    { add(i); add(count - 1 - i); }
    for (std::size_t i = 0; i < 32; ++i)
    {
      const std::size_t index = uint64_t(count - 1) * i / 31;
      add(index); if (index + 1 < count) add(index + 1);
    }
    const std::size_t settling = (taps - 1) / decimation;
    if (settling) add(settling - 1);
    add(settling); add(settling + 1);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  }

  QualificationResult check(
    const std::vector<std::vector<std::complex<float>>>& input,
    uint64_t first, const std::vector<LaneOutput>& gpu,
    double gpuMilliseconds, double deadlineMilliseconds,
    double rmsLimit=2e-3, double peakLimit=5e-3)
  {
    QualificationResult result;
    const auto fail = [&](const char* reason) {
      result.reason = reason; return result;
    };
    if (!std::isfinite(gpuMilliseconds) || gpuMilliseconds < 0 ||
        !std::isfinite(deadlineMilliseconds) || deadlineMilliseconds <= 0 ||
        gpuMilliseconds > deadlineMilliseconds || !std::isfinite(rmsLimit) ||
        !std::isfinite(peakLimit) || rmsLimit < 0 || peakLimit < 0)
      return fail("wideband GPU full-block deadline qualification failed");
    if (input.size() != config_.channels || gpu.size() != config_.lanes.size())
      return fail("wideband GPU qualification lane/channel count differs");
    const std::size_t samples = input.front().size();
    if (samples > config_.maxInputSamples || samples > UINT64_MAX - first)
      return fail("wideband GPU qualification input geometry is invalid");
    for (const auto& channel : input)
      if (channel.size() != samples)
        return fail("wideband GPU qualification input channels are unpaired");
    if (!started_ || first != nextInputSample_) reset();

    std::vector<std::vector<std::size_t>> selected(config_.lanes.size());
    for (std::size_t lane = 0; lane < config_.lanes.size(); ++lane)
    {
      const auto& cfg = config_.lanes[lane];
      const auto& actual = gpu[lane];
      const uint32_t remainder = uint32_t(first % cfg.decimation);
      const uint32_t start = remainder ? cfg.decimation - remainder : 0;
      const std::size_t count = start < samples ?
        1 + (samples - 1 - start) / cfg.decimation : 0;
      if (actual.frequencyHz != cfg.frequencyHz ||
          actual.inputSamplesPerOutput != cfg.decimation ||
          actual.hasFirstInputSample != (count != 0) ||
          (count && actual.firstInputSample != first + start) ||
          actual.channels.size() != config_.channels)
        return fail("wideband GPU qualification metadata differs");
      for (const auto& channel : actual.channels)
        if (channel.size() != count)
          return fail("wideband GPU qualification sample count differs");
      selected[lane] = selectedIndices(count, cfg.taps.size(), cfg.decimation);
      const uint64_t work = uint64_t(selected[lane].size()) *
        cfg.taps.size() * config_.channels;
      if (work > maximumMultiplyAdds - result.oracleMultiplyAdds)
        return fail("wideband GPU sampled oracle exceeds its operation budget");
      result.oracleMultiplyAdds += work;
    }

    bool silentInput = true, silentOutput = true;
    for (const auto& channel : input)
      for (const auto value : channel)
      {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
          return fail("wideband GPU qualification input is non-finite");
        silentInput = silentInput && value == std::complex<float>{};
      }
    for (const auto& channel : history_)
      for (const auto value : channel)
        silentInput = silentInput && value == std::complex<float>{};
    // A finite-value fault must not hide outside the sampled comparison set.
    for (const auto& lane : gpu)
      for (const auto& channel : lane.channels)
        for (const auto value : channel)
        {
          if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
            return fail("wideband GPU qualification output is non-finite");
          silentOutput = silentOutput && value == std::complex<float>{};
        }

    double representativeEnergy = 0;
    for (std::size_t lane = 0; lane < config_.lanes.size(); ++lane)
    {
      const auto& cfg = config_.lanes[lane];
      const uint32_t remainder = uint32_t(first % cfg.decimation);
      const uint32_t start = remainder ? cfg.decimation - remainder : 0;
      for (std::size_t channel = 0; channel < config_.channels; ++channel)
      {
        double error = 0, signal = 0, maxError = 0, peak = 0;
        for (const auto index : selected[lane])
        {
          const uint64_t at = uint64_t(start) + index * cfg.decimation;
          std::complex<double> expected{};
          for (std::size_t tap = 0; tap < cfg.taps.size(); ++tap)
          {
            std::complex<float> raw{};
            uint64_t absolute = 0;
            if (tap <= at)
            { raw = input[channel][at - tap]; absolute = first + at - tap; }
            else
            {
              const uint64_t behind = tap - at;
              if (behind > historySamples_ || behind > first) continue;
              raw = history_[channel][historySamples_ - behind];
              absolute = first - behind;
            }
            if (raw == std::complex<float>{}) continue;
            expected += std::complex<double>(raw) * cfg.taps[tap] *
              oscillator(cfg.centerOffsetHz, absolute);
          }
          const auto difference = std::complex<double>(
            gpu[lane].channels[channel][index]) - expected;
          const double e = std::norm(difference), s = std::norm(expected);
          if (!std::isfinite(e) || !std::isfinite(s))
            return fail("wideband GPU sampled oracle output is non-finite");
          error += e; signal += s;
          maxError = std::max(maxError, e); peak = std::max(peak, s);
          ++result.comparedSamples;
        }
        if (!std::isfinite(error) || !std::isfinite(signal))
          return fail("wideband GPU sampled oracle energy overflowed");
        representativeEnergy += signal;
        const double rms = std::sqrt(error / std::max(signal, 1e-30));
        const double relativePeak = std::sqrt(maxError / std::max(peak, 1e-30));
        result.worstRms = std::max(result.worstRms, rms);
        result.worstPeak = std::max(result.worstPeak, relativePeak);
        if (rms > rmsLimit || relativePeak > peakLimit)
          return fail("wideband GPU selected-output numerical qualification failed");
      }
    }
    if (!std::isfinite(representativeEnergy))
      return fail("wideband GPU sampled oracle energy overflowed");
    if (!result.comparedSamples ||
        representativeEnergy <= result.comparedSamples * 1e-20)
    {
      result.status = QualificationStatus::inconclusive;
      // Exact zero input/history implies exact zero FIR output everywhere.
      // Such a frame is safe to publish, but never qualifies the device.
      result.verifiedSilence = samples && silentInput && silentOutput;
      result.reason = "wideband GPU qualification input is not representative";
    }
    else
    {
      result.status = QualificationStatus::passed;
      result.accepted = true;
      result.reason = "wideband GPU full-block selected-output qualification passed";
    }
    if (result.accepted || result.verifiedSilence)
    {
      for (std::size_t channel = 0; channel < config_.channels; ++channel)
      {
        auto& history = history_[channel];
        if (samples >= historySamples_)
          std::copy(input[channel].end() - historySamples_, input[channel].end(),
            history.begin());
        else
        {
          std::move(history.begin() + samples, history.end(), history.begin());
          std::copy(input[channel].begin(), input[channel].end(),
            history.end() - samples);
        }
      }
      nextInputSample_ = first + samples; started_ = true;
    }
    return result;
  }

private:
  std::complex<double> oscillator(double offset, uint64_t sample) const
  {
    const double tau = 2 * std::acos(-1.0);
    // Integer-Hz capture configurations have an exact modular phase even
    // beyond 2^53 samples. Products of two uint32 residues fit uint64.
    const double reduced = std::fmod(offset, double(config_.inputRateHz));
    if (std::trunc(reduced) == reduced)
    {
      const uint64_t magnitude = uint64_t(std::abs(reduced));
      const uint64_t residue = magnitude * (sample % config_.inputRateHz) %
        config_.inputRateHz;
      const double phase = (reduced < 0 ? tau : -tau) *
        double(residue) / config_.inputRateHz;
      return {std::cos(phase), std::sin(phase)};
    }
    const long double phase = std::remainder(
      -2 * std::acos(-1.0L) * static_cast<long double>(offset) * sample /
        config_.inputRateHz, 2 * std::acos(-1.0L));
    return {double(std::cos(phase)), double(std::sin(phase))};
  }

  Config config_;
  std::vector<std::vector<std::complex<float>>> history_;
  std::size_t historySamples_ = 0;
  uint64_t nextInputSample_ = 0;
  bool started_ = false;
};
}
