#ifndef FM_REFERENCE_QUALITY_FILTER_H
#define FM_REFERENCE_QUALITY_FILTER_H

#include "data/IqData.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace blah2::conditioning
{

// FM programme content periodically becomes too narrowband for a useful
// ambiguity surface.  Score short blocks from the demodulated phase and omit
// only the lowest-quality quarter from the matched reference.  Surveillance
// IQ is never altered and no delay/Doppler cell is notched.
class FmReferenceQualityFilter
{
public:
  struct Metrics
  {
    std::size_t blocks = 0;
    std::size_t retainedBlocks = 0;
    double retainedFraction = 1;
    bool applied = false;
  };

  explicit FmReferenceQualityFilter(uint32_t sampleRateHz,
    double keepFraction = .75, double blockSeconds = .005)
    : blockSamples_(std::max<std::size_t>(3,
        std::llround(sampleRateHz * blockSeconds))),
      keepFraction_(keepFraction)
  {
    if (!sampleRateHz || !std::isfinite(keepFraction) || keepFraction <= 0 ||
        keepFraction > 1 || !std::isfinite(blockSeconds) || blockSeconds <= 0)
      throw std::invalid_argument("Invalid FM reference-quality filter configuration");
  }

  const Metrics& process(const IqData& input, IqData& output)
  {
    const auto& samples = input.view_data();
    if (samples.size() > output.get_n())
      throw std::invalid_argument("FM reference-quality output is too small");
    auto& result = output.resize_for_write(static_cast<uint32_t>(samples.size()));
    const std::size_t blocks = (samples.size() + blockSamples_ - 1) /
      blockSamples_;
    metrics_ = {blocks, blocks, 1, false};
    if (blocks < 2 || keepFraction_ >= 1)
    {
      std::copy(samples.begin(), samples.end(), result.begin());
      return metrics_;
    }

    std::vector<double> quality(blocks);
    for (std::size_t block = 0; block < blocks; ++block)
    {
      const std::size_t begin = block * blockSamples_;
      const std::size_t end = std::min(samples.size(), begin + blockSamples_);
      double mean = 0, squared = 0;
      std::size_t count = 0;
      for (std::size_t sample = begin + 1; sample < end; ++sample)
      {
        const auto step = samples[sample] * std::conj(samples[sample - 1]);
        const double phase = std::atan2(step.imag(), step.real());
        count++;
        const double delta = phase - mean;
        mean += delta / count;
        squared += delta * (phase - mean);
      }
      // A short tail cannot be scored reliably, so retain it.
      quality[block] = count > 1 ? std::sqrt(squared / count) :
        std::numeric_limits<double>::infinity();
    }

    const auto finite = std::minmax_element(quality.begin(), quality.end());
    const double scale = std::max(1.0, std::abs(*finite.second));
    if (*finite.second - *finite.first <= 1e-12 * scale)
    {
      std::copy(samples.begin(), samples.end(), result.begin());
      return metrics_;
    }
    auto ranked = quality;
    const std::size_t rejected = static_cast<std::size_t>(
      std::floor((1 - keepFraction_) * blocks));
    std::nth_element(ranked.begin(), ranked.begin() + rejected, ranked.end());
    const double threshold = ranked[rejected];

    metrics_.retainedBlocks = 0;
    for (std::size_t block = 0; block < blocks; ++block)
    {
      const bool retain = quality[block] >= threshold;
      if (retain) metrics_.retainedBlocks++;
      const std::size_t begin = block * blockSamples_;
      const std::size_t end = std::min(samples.size(), begin + blockSamples_);
      if (retain)
        std::copy(samples.begin() + begin, samples.begin() + end,
          result.begin() + begin);
      else
        std::fill(result.begin() + begin, result.begin() + end,
          std::complex<double>{});
    }
    metrics_.retainedFraction = static_cast<double>(metrics_.retainedBlocks) /
      blocks;
    metrics_.applied = metrics_.retainedBlocks < blocks;
    return metrics_;
  }

  const Metrics& metrics() const { return metrics_; }

private:
  std::size_t blockSamples_;
  double keepFraction_;
  Metrics metrics_;
};

} // namespace blah2::conditioning

#endif
