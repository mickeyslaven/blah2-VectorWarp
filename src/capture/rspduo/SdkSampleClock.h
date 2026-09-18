#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>

// The SDK normally reports the decimated output counter.  Some dual-tuner
// firmware reports a counter derived from the 6 MHz ADC instead.  Keep the
// latter opt-in: its phase at the 32-bit ADC rollover is ambiguous until an
// observation rules phases out.
class SdkSampleClock {
public:
  struct ScaledDuoConfig {
    double adcHz;
    uint32_t outputHz;
    unsigned decimation;
    bool if1620;
    bool dualTuner;
  };

  static bool supportsRatio3(const ScaledDuoConfig& config) {
    return config.adcHz == 6000000.0 && config.outputHz == 2000000 &&
      config.decimation == 1 && config.if1620 && config.dualTuner;
  }

  struct Result { bool sequenceBreak; bool wrapped; };

  explicit SdkSampleClock(uint32_t ratio = 1) { configure(ratio); }

  void configure(uint32_t ratio) {
    if (ratio != 1 && ratio != 3)
      throw std::invalid_argument("Unvalidated SDK counter ratio");
    ratio_ = ratio;
    clear();
  }

  void clear() { have_ = false; phases_ = 0; previousRaw_ = 0; }

  Result observe(uint32_t first, uint32_t count, bool reset = false) {
    bool gap = false;
    if (have_ && !reset) {
      unsigned kept = 0;
      for (unsigned i = 0; i < phases_; ++i) {
        if (adcNext_[i] / ratio_ == first)
          adcNext_[kept++] = static_cast<uint32_t>(
            static_cast<uint64_t>(adcNext_[i]) + static_cast<uint64_t>(count) * ratio_);
      }
      phases_ = kept;
      gap = phases_ == 0;
    }
    const bool continuing = have_ && !reset && !gap;
    const bool wrapped = continuing && first < previousRaw_;
    if (!continuing) seed(first, count);
    have_ = true;
    previousRaw_ = first;
    return {reset || gap, wrapped};
  }

private:
  void seed(uint32_t first, uint32_t count) {
    phases_ = 0;
    for (uint32_t remainder = 0; remainder < ratio_; ++remainder) {
      const uint64_t adc = static_cast<uint64_t>(first) * ratio_ + remainder;
      if (adc < (uint64_t(1) << 32))
        adcNext_[phases_++] = static_cast<uint32_t>(
          adc + static_cast<uint64_t>(count) * ratio_);
    }
    if (!phases_)
      throw std::invalid_argument("SDK counter outside configured ADC ratio");
  }

  uint32_t ratio_ = 1;
  std::array<uint32_t, 3> adcNext_{};
  unsigned phases_ = 0;
  uint32_t previousRaw_ = 0;
  bool have_ = false;
};
