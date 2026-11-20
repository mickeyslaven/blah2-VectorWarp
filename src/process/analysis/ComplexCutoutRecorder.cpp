#include "ComplexCutoutRecorder.h"
#include "data/meta/Constants.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace blah2::analysis {
namespace {

template<class Axis>
size_t nearest(const Axis& axis, double value)
{
  if (axis.empty()) throw std::invalid_argument("Complex cutout axis is empty");
  size_t best = 0;
  double distance = std::abs(double(axis[0]) - value);
  for (size_t index = 1; index < axis.size(); ++index) {
    const double candidate = std::abs(double(axis[index]) - value);
    if (candidate < distance) { best = index; distance = candidate; }
  }
  return best;
}

size_t lower(size_t center, uint32_t radius)
{
  return center > radius ? center - radius : 0;
}

size_t upper(size_t center, uint32_t radius, size_t count)
{
  return std::min(count, center + size_t(radius) + 1);
}

}

ComplexCutoutRecorder::ComplexCutoutRecorder(const Config& config,
  uint32_t sampleRate, bool replayInput)
  : config_(config), sampleRate_(sampleRate)
{
  if (!config_.enabled) return;
  if (!replayInput)
    throw std::invalid_argument("Complex cutout recording is restricted to replay input");
  if (!sampleRate_ || config_.path.empty() || !std::isfinite(config_.delayKm) ||
      !std::isfinite(config_.dopplerHz) || config_.delayRadiusBins > 64 ||
      config_.dopplerRadiusBins > 64 || !config_.maxFrames ||
      config_.maxFrames > 10000 || config_.maximumBytes < (1ULL << 20) ||
      config_.maximumBytes > (256ULL << 20))
    throw std::invalid_argument("Invalid complex cutout configuration");
  const int descriptor = ::open(config_.path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (descriptor < 0) throw std::runtime_error("Cannot create exclusive complex cutout output; choose a new path");
  output_ = ::fdopen(descriptor, "w");
  if (!output_) { ::close(descriptor); throw std::runtime_error("Cannot open complex cutout output stream"); }
}

ComplexCutoutRecorder::~ComplexCutoutRecorder() { if (output_) std::fclose(output_); }

void ComplexCutoutRecorder::append(uint64_t timestamp,
  const std::vector<Map<std::complex<double>>*>& maps,
  const std::vector<std::complex<double>>& referenceWeights,
  std::optional<std::complex<double>> pilotReference,
  std::optional<uint32_t> pilotAnchorChannelId)
{
  if (!config_.enabled || complete()) return;
  const std::vector<const Map<std::complex<double>>*> native(maps.begin(), maps.end());
  appendNative(timestamp, native, referenceWeights, pilotReference, nullptr,
    pilotAnchorChannelId);
}

void ComplexCutoutRecorder::appendNative(uint64_t timestamp,
  const std::vector<const Map<std::complex<double>>*>& maps,
  const std::vector<std::complex<double>>& referenceWeights,
  std::optional<std::complex<double>> pilotReference,
  const NativeProductIdentity* identity,
  std::optional<uint32_t> pilotAnchorChannelId)
{
  if (!config_.enabled || complete()) return;
  if (maps.empty() || maps.size() > 8 || maps.front() == nullptr)
    throw std::invalid_argument("Complex cutout requires channel maps");
  auto* first = maps.front();
  const size_t rows = first->data.size();
  const size_t columns = rows ? first->data.front().size() : 0;
  if (!rows || !columns || first->doppler.size() != rows || first->delay.size() != columns)
    throw std::invalid_argument("Complex cutout map axes do not match its data");
  for (auto* map : maps) {
    if (!map || map->data.size() != rows ||
        map->doppler != first->doppler || map->delay != first->delay)
      throw std::invalid_argument("Complex cutout channel geometry changed");
    for (const auto& row : map->data)
      if (row.size() != columns)
        throw std::invalid_argument("Complex cutout channel geometry changed");
  }
  const size_t referenceCount = identity && !identity->referenceChannelIds.empty() ?
    identity->referenceChannelIds.size() : maps.size();
  if (!referenceWeights.empty() && referenceWeights.size() != referenceCount)
    throw std::invalid_argument("Complex cutout reference-weight channel count changed");
  if (identity && (!identity->sampleRateHz ||
      identity->channelIds.size() != maps.size()))
    throw std::invalid_argument("Complex cutout native channel identity is invalid");
  if (pilotReference && (!std::isfinite(pilotReference->real()) ||
                         !std::isfinite(pilotReference->imag()) ||
                         std::abs(*pilotReference) <= 1e-30 ||
                         std::norm(*pilotReference) > 1 + 1e-9))
    throw std::invalid_argument("Complex cutout pilot reference is invalid");
  if (pilotAnchorChannelId && (!pilotReference || !identity ||
      std::find(identity->channelIds.begin(), identity->channelIds.end(),
        *pilotAnchorChannelId) == identity->channelIds.end()))
    throw std::invalid_argument("Complex cutout pilot anchor identity is invalid");

  const uint32_t sampleRate = identity ? identity->sampleRateHz : sampleRate_;
  const double delayBin = config_.delayKm * 1000.0 * sampleRate / Constants::c;
  const size_t centerColumn = nearest(first->delay, delayBin);
  const size_t centerRow = nearest(first->doppler, config_.dopplerHz);
  const size_t firstColumn = lower(centerColumn, config_.delayRadiusBins);
  const size_t lastColumn = upper(centerColumn, config_.delayRadiusBins, columns);
  const size_t firstRow = lower(centerRow, config_.dopplerRadiusBins);
  const size_t lastRow = upper(centerRow, config_.dopplerRadiusBins, rows);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("schema"); writer.String("vectorwarp-complex-caf-cutout-v1");
  writer.Key("frame"); writer.Uint(frames_);
  writer.Key("timestamp"); writer.Uint64(timestamp);
  writer.Key("requested_delay_km"); writer.Double(config_.delayKm);
  writer.Key("requested_doppler_hz"); writer.Double(config_.dopplerHz);
  writer.Key("center_delay_km");
  writer.Double(double(first->delay[centerColumn]) * Constants::c / sampleRate / 1000.0);
  writer.Key("center_doppler_hz"); writer.Double(first->doppler[centerRow]);
  writer.Key("delay_km"); writer.StartArray();
  for (size_t column = firstColumn; column < lastColumn; ++column)
    writer.Double(double(first->delay[column]) * Constants::c / sampleRate / 1000.0);
  writer.EndArray();
  writer.Key("doppler_hz"); writer.StartArray();
  for (size_t row = firstRow; row < lastRow; ++row)
    writer.Double(first->doppler[row]);
  writer.EndArray();
  writer.Key("reference_weights"); writer.StartArray();
  for (const auto value : referenceWeights) {
    if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
      throw std::runtime_error("Complex cutout reference weight is non-finite");
    writer.StartArray(); writer.Double(value.real()); writer.Double(value.imag()); writer.EndArray();
  }
  writer.EndArray();
  // This is the normalized, pre-cancellation direct-path/reference
  // correlation for this CPI.  It is optional to preserve v1 reader
  // compatibility and is replay-only because this recorder is replay-only.
  if (pilotReference) {
    writer.Key("pilot_reference"); writer.StartArray();
    writer.Double(pilotReference->real()); writer.Double(pilotReference->imag());
    writer.EndArray();
  }
  if (pilotAnchorChannelId) {
    writer.Key("pilot_anchor_channel_id");
    writer.Uint(*pilotAnchorChannelId);
  }
  writer.Key("channels"); writer.StartArray();
  for (auto* map : maps) {
    writer.StartArray();
    for (size_t row = firstRow; row < lastRow; ++row) {
      writer.StartArray();
      for (size_t column = firstColumn; column < lastColumn; ++column) {
        const auto value = map->data[row][column];
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
          throw std::runtime_error("Complex cutout contains a non-finite sample");
        writer.StartArray(); writer.Double(value.real()); writer.Double(value.imag()); writer.EndArray();
      }
      writer.EndArray();
    }
    writer.EndArray();
  }
  writer.EndArray();
  writer.EndObject();
  const std::string record = identity ?
    annotate_native_product(buffer.GetString(), *identity) : buffer.GetString();
  if (record.size() + 1 > config_.maximumBytes - bytes_) {
    capacityReached_ = true;
    return;
  }
  if (std::fwrite(record.data(), 1, record.size(), output_) != record.size() ||
      std::fputc('\n', output_) == EOF || std::fflush(output_) != 0)
    throw std::runtime_error("Cannot write complex cutout output");
  ++frames_;
  bytes_ += record.size() + 1;
}

std::string ComplexCutoutRecorder::statusJson() const
{
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();
  writer.Key("enabled"); writer.Bool(config_.enabled);
  writer.Key("frames"); writer.Uint(frames_);
  writer.Key("bytes"); writer.Uint64(bytes_);
  writer.Key("complete"); writer.Bool(complete());
  writer.Key("maximum_frames"); writer.Uint(config_.maxFrames);
  writer.Key("maximum_bytes"); writer.Uint64(config_.maximumBytes);
  writer.Key("path"); writer.String(config_.path.c_str());
  writer.EndObject();
  return buffer.GetString();
}

}
