#ifndef SUBBAND_CHANNELIZER_H
#define SUBBAND_CHANNELIZER_H

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace blah2::channelizer
{

// Streaming coherent digital down-converter for one logical illuminator.
// Every physical input channel uses the same oscillator, filter and output
// schedule so reference/surveillance phase relationships are preserved.
class SubbandChannelizer
{
public:
  struct Config
  {
    uint32_t inputRateHz = 0;
    uint32_t outputRateHz = 0;
    double centerOffsetHz = 0;
    double occupiedBandwidthHz = 0;
    uint32_t tapsPerPhase = 16;
  };

  struct Output
  {
    std::vector<std::vector<std::complex<float>>> channels;
    // Input index at which the first output was produced. The corresponding
    // RF time is earlier by groupDelayInputSamples.
    uint64_t firstInputSample = 0;
    bool hasFirstInputSample = false;
    uint32_t inputSamplesPerOutput = 0;
    double groupDelayInputSamples = 0;
  };

  SubbandChannelizer(Config config, std::size_t channelCount);

  Output process(const std::vector<std::vector<std::complex<float>>>& input,
    uint64_t firstInputSample);
  void reset();

  const Config& config() const { return config_; }
  uint32_t decimation() const { return decimation_; }
  double group_delay_input_samples() const { return groupDelayInputSamples_; }

private:
  Config config_;
  std::size_t channelCount_ = 0;
  uint32_t decimation_ = 0;
  double groupDelayInputSamples_ = 0;
  std::vector<double> taps_;
  std::vector<std::vector<std::complex<double>>> history_;
  std::size_t writeIndex_ = 0;
  std::size_t historyFill_ = 0;
  bool started_ = false;
  uint64_t nextInputSample_ = 0;
  std::complex<double> oscillator_{1, 0};
  std::complex<double> oscillatorStep_{1, 0};

  static std::vector<double> design_filter(const Config& config,
    uint32_t decimation);
};

} // namespace blah2::channelizer

#endif
