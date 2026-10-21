#include "ComplexCutoutRecorder.h"
#include "data/meta/Constants.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

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
      config_.maxFrames > 10000)
    throw std::invalid_argument("Invalid complex cutout configuration");
  output_.open(config_.path, std::ios::out | std::ios::trunc);
  if (!output_) throw std::runtime_error("Cannot open complex cutout output");
}

void ComplexCutoutRecorder::append(uint64_t timestamp,
  const std::vector<Map<std::complex<double>>*>& maps,
  const std::vector<std::complex<double>>& referenceWeights)
{
  if (!config_.enabled || frames_ >= config_.maxFrames) return;
  if (maps.empty() || maps.front() == nullptr)
    throw std::invalid_argument("Complex cutout requires channel maps");
  auto* first = maps.front();
  const size_t rows = first->get_nRows(), columns = first->get_nCols();
  if (first->doppler.size() != rows || first->delay.size() != columns)
    throw std::invalid_argument("Complex cutout map axes do not match its data");
  for (auto* map : maps)
    if (!map || map->get_nRows() != rows || map->get_nCols() != columns ||
        map->doppler != first->doppler || map->delay != first->delay)
      throw std::invalid_argument("Complex cutout channel geometry changed");
  if (!referenceWeights.empty() && referenceWeights.size() != maps.size())
    throw std::invalid_argument("Complex cutout reference-weight channel count changed");

  const double delayBin = config_.delayKm * 1000.0 * sampleRate_ / Constants::c;
  const size_t centerColumn = nearest(first->delay, delayBin);
  const size_t centerRow = nearest(first->doppler, config_.dopplerHz);
  const size_t firstColumn = lower(centerColumn, config_.delayRadiusBins);
  const size_t lastColumn = upper(centerColumn, config_.delayRadiusBins, columns);
  const size_t firstRow = lower(centerRow, config_.dopplerRadiusBins);
  const size_t lastRow = upper(centerRow, config_.dopplerRadiusBins, rows);

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.SetMaxDecimalPlaces(10);
  writer.StartObject();
  writer.Key("schema"); writer.String("vectorwarp-complex-caf-cutout-v1");
  writer.Key("frame"); writer.Uint(frames_);
  writer.Key("timestamp"); writer.Uint64(timestamp);
  writer.Key("requested_delay_km"); writer.Double(config_.delayKm);
  writer.Key("requested_doppler_hz"); writer.Double(config_.dopplerHz);
  writer.Key("center_delay_km");
  writer.Double(double(first->delay[centerColumn]) * Constants::c / sampleRate_ / 1000.0);
  writer.Key("center_doppler_hz"); writer.Double(first->doppler[centerRow]);
  writer.Key("delay_km"); writer.StartArray();
  for (size_t column = firstColumn; column < lastColumn; ++column)
    writer.Double(double(first->delay[column]) * Constants::c / sampleRate_ / 1000.0);
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
  output_.write(buffer.GetString(), std::streamsize(buffer.GetSize()));
  output_.put('\n'); output_.flush();
  if (!output_) throw std::runtime_error("Cannot write complex cutout output");
  ++frames_;
}

}
