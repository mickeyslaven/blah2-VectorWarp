#include "HeimdallFrame.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{
uint32_t read_be_u32(const uint8_t *bytes)
{
  return (static_cast<uint32_t>(bytes[0]) << 24) |
    (static_cast<uint32_t>(bytes[1]) << 16) |
    (static_cast<uint32_t>(bytes[2]) << 8) |
    static_cast<uint32_t>(bytes[3]);
}

uint32_t read_le_u32(const uint8_t *bytes)
{
  return static_cast<uint32_t>(bytes[0]) |
    (static_cast<uint32_t>(bytes[1]) << 8) |
    (static_cast<uint32_t>(bytes[2]) << 16) |
    (static_cast<uint32_t>(bytes[3]) << 24);
}

float read_le_float(const uint8_t *bytes)
{
  const uint32_t bits = read_le_u32(bytes);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
uint64_t read_be_u64(const uint8_t *bytes)
{
  return (static_cast<uint64_t>(read_be_u32(bytes)) << 32) | read_be_u32(bytes + 4);
}
}

HeimdallFrame::Header HeimdallFrame::decode_header(
  const std::array<uint8_t, FIXED_HEADER_SIZE>& bytes)
{
  const auto magic = read_be_u32(bytes.data());
  if (magic != MAGIC && magic != TIMED_MAGIC)
    throw std::runtime_error("Invalid Heimdall V2 frame magic");

  Header header;
  header.timing.available = magic == TIMED_MAGIC;
  header.numChannels = read_be_u32(bytes.data() + 4);
  header.samplesPerChannel = read_be_u32(bytes.data() + 8);
  header.phaseState = read_be_u32(bytes.data() + 12);
  header.noiseSource = read_be_u32(bytes.data() + 16);
  header.frequencyChangeCounter = read_be_u32(bytes.data() + 20);
  header.currentGroupIndex = read_be_u32(bytes.data() + 24);
  header.retuningInProgress = read_be_u32(bytes.data() + 28);

  if (header.numChannels == 0 || header.numChannels > 8)
    throw std::runtime_error("Invalid Heimdall V2 channel count");
  if (header.samplesPerChannel == 0 || header.samplesPerChannel > 65536)
    throw std::runtime_error("Invalid Heimdall V2 sample count");
  return header;
}

std::size_t HeimdallFrame::metadata_size(const Header& header)
{
  return static_cast<std::size_t>(header.numChannels) * 2 * sizeof(float) +
    (header.timing.available ? TIMING_SIZE : 0);
}

void HeimdallFrame::decode_metadata(
  Header& header, const std::vector<uint8_t>& bytes)
{
  if (bytes.size() != metadata_size(header))
    throw std::runtime_error("Incomplete Heimdall V2 channel metadata");
  header.frequencies.resize(header.numChannels);
  header.gains.resize(header.numChannels);
  for (uint32_t channel = 0; channel < header.numChannels; channel++)
  {
    const std::size_t offset = static_cast<std::size_t>(channel) * 8;
    const float frequency = read_le_float(bytes.data() + offset);
    const float gain = read_le_float(bytes.data() + offset + 4);
    if (!std::isfinite(frequency))
      throw std::runtime_error("Invalid Heimdall V2 RF frequency metadata");
    if (!std::isfinite(gain))
      throw std::runtime_error("Invalid Heimdall V2 gain metadata");
    header.frequencies[channel] = frequency;
    header.gains[channel] = gain;
  }
  if (header.timing.available) {
    const auto* data = bytes.data() + header.numChannels * 8;
    if (read_be_u32(data) != 1 || read_be_u32(data + 4) != TIMING_SIZE ||
        read_be_u32(data + 12) >= header.numChannels)
      throw std::runtime_error("Unsupported Heimdall ingestion timing extension");
    auto& t = header.timing;
    t.flags = read_be_u32(data + 8); t.referenceChannel = read_be_u32(data + 12);
    t.epoch = read_be_u64(data + 16); t.firstSample = read_be_u64(data + 24);
    t.transferSequence = read_be_u64(data + 32); t.monotonicNanoseconds = read_be_u64(data + 40);
    t.taiNanoseconds = read_be_u64(data + 48); t.realtimeNanoseconds = read_be_u64(data + 56);
    t.clockReadSpanNanoseconds = read_be_u64(data + 64);
    t.systemMaxErrorNanoseconds = read_be_u64(data + 72); t.bufferSequence = read_be_u64(data + 80);
    t.anchorSample = read_be_u64(data + 88);
    if ((t.flags & ~7U) || t.epoch == 0 || t.realtimeNanoseconds == 0 ||
        t.realtimeNanoseconds > uint64_t(std::numeric_limits<int64_t>::max()) ||
        t.firstSample > std::numeric_limits<uint64_t>::max() - header.samplesPerChannel ||
        t.anchorSample < t.firstSample + header.samplesPerChannel)
      throw std::runtime_error("Invalid Heimdall ingestion timing extension");
  }
}

bool HeimdallFrame::timing_contiguous(const Header& previous, const Header& current)
{
  const auto& a = previous.timing; const auto& b = current.timing;
  if (a.available != b.available) return false;
  if (!a.available) return true;
  return a.epoch == b.epoch && a.referenceChannel == b.referenceChannel &&
    a.firstSample + previous.samplesPerChannel == b.firstSample &&
    a.transferSequence + 1 == b.transferSequence && a.bufferSequence + 1 == b.bufferSequence &&
    (!(a.flags & b.flags & 1) || b.monotonicNanoseconds > a.monotonicNanoseconds);
}

std::size_t HeimdallFrame::payload_size(const Header& header)
{
  constexpr std::size_t bytesPerComplex = 2;
  if (header.samplesPerChannel > std::numeric_limits<std::size_t>::max() /
      header.numChannels / bytesPerComplex)
    throw std::overflow_error("Heimdall V2 payload size overflow");
  return static_cast<std::size_t>(header.samplesPerChannel) *
    header.numChannels * bytesPerComplex;
}

std::vector<std::vector<std::complex<float>>> HeimdallFrame::decode_payload(
  const Header& header, const std::vector<uint8_t>& bytes)
{
  if (bytes.size() != payload_size(header))
    throw std::runtime_error("Incomplete Heimdall V2 payload");
  if (header.gains.size() != header.numChannels)
    throw std::runtime_error("Heimdall V2 gain metadata is unavailable");

  // Suite reports the actual tuner gain for every channel.  Refer all IQ
  // amplitudes to the least-amplified channel so covariance, reference
  // synthesis and beamforming see antenna voltage rather than a deterministic
  // tuner-gain bias.  Attenuating to the minimum avoids inventing ADC bits or
  // digitally amplifying the noisier channels.
  const float referenceGain = *std::min_element(
    header.gains.begin(), header.gains.end());
  std::vector<float> gainScales(header.numChannels);
  for (uint32_t channel = 0; channel < header.numChannels; ++channel)
  {
    gainScales[channel] = std::pow(10.0F,
      (referenceGain - header.gains[channel]) / 20.0F);
    if (!std::isfinite(gainScales[channel]) || gainScales[channel] <= 0 ||
        gainScales[channel] > 1.00001F)
      throw std::runtime_error("Invalid Heimdall V2 gain equalization");
  }

  std::vector<std::vector<std::complex<float>>> channels(
    header.numChannels,
    std::vector<std::complex<float>>(header.samplesPerChannel));
  for (uint32_t channel = 0; channel < header.numChannels; channel++)
  {
    const std::size_t start = static_cast<std::size_t>(channel) *
      header.samplesPerChannel * 2;
    double meanI = 0;
    double meanQ = 0;
    for (uint32_t sample = 0; sample < header.samplesPerChannel; sample++)
    {
      meanI += bytes[start + sample * 2];
      meanQ += bytes[start + sample * 2 + 1];
    }
    meanI /= header.samplesPerChannel;
    meanQ /= header.samplesPerChannel;
    for (uint32_t sample = 0; sample < header.samplesPerChannel; sample++)
    {
      channels[channel][sample] = {
        static_cast<float>((bytes[start + sample * 2] - meanI) / 127.5) *
          gainScales[channel],
        static_cast<float>((bytes[start + sample * 2 + 1] - meanQ) / 127.5) *
          gainScales[channel]};
    }
  }
  return channels;
}

bool HeimdallFrame::is_synchronized_data(const Header& header)
{
  return (header.phaseState & 0xffU) == CALIBRATED &&
    (header.phaseState & (STALE_CALIBRATION | WIDEBAND_MODE | INDEPENDENT_MODE)) == 0 &&
    header.noiseSource == 0 && header.retuningInProgress == 0;
}
