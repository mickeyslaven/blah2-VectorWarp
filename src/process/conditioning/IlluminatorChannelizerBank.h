#ifndef ILLUMINATOR_CHANNELIZER_BANK_H
#define ILLUMINATOR_CHANNELIZER_BANK_H

#include "process/conditioning/SubbandChannelizer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace blah2::channelizer
{

struct IlluminatorDefinition
{
  std::string id;
  uint64_t frequencyHz = 0;
  uint32_t occupiedBandwidthHz = 0;
  uint32_t outputRateHz = 0;
  uint32_t tapsPerPhase = 16;
};

struct IlluminatorOutput
{
  std::string id;
  uint64_t frequencyHz = 0;
  SubbandChannelizer::Output samples;
};

// Fans one coherent physical capture into independent, phase-preserving
// illuminator streams. It performs no receiver I/O and can therefore be used
// identically by live SDRs and replay.
class IlluminatorChannelizerBank
{
public:
  IlluminatorChannelizerBank(uint64_t captureCenterHz,
    uint32_t captureRateHz, std::size_t channelCount,
    std::vector<IlluminatorDefinition> illuminators);

  std::vector<IlluminatorOutput> process(
    const std::vector<std::vector<std::complex<float>>>& input,
    uint64_t firstInputSample);
  void reset();

  const std::vector<IlluminatorDefinition>& definitions() const
  { return definitions_; }

private:
  uint64_t captureCenterHz_ = 0;
  uint32_t captureRateHz_ = 0;
  std::size_t channelCount_ = 0;
  std::vector<IlluminatorDefinition> definitions_;
  std::vector<std::unique_ptr<SubbandChannelizer>> channelizers_;
};

} // namespace blah2::channelizer

#endif
