#include "Map.h"
#include "JsonArrayFile.h"
#include "data/meta/Constants.h"
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {
// Preserve the existing map display scale while keeping silence finite.
template<class Value> double mapLevel(Value value) {
  return Map<Value>::MetricAccumulator::level(value);
}
}

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

// constructor
template <class T>
Map<T>::Map(uint32_t _nRows, uint32_t _nCols)
{
  nRows = _nRows;
  nCols = _nCols;
  if (!nRows || !nCols) throw std::invalid_argument("Radar map dimensions must be positive");
  noisePower = 0;
  maxPower = 0;
  // Fused maps are created for every lane on every CPI. Move the complete row
  // allocation into place instead of copy-allocating every row and cell twice.
  data = std::vector<std::vector<T>>(nRows, std::vector<T>(nCols, {1}));
}

template <class T>
void Map<T>::set_row(uint32_t i, const std::vector<T>& row)
{
  for (uint32_t j = 0; j < nCols; j++)
  {
    data[i][j] = row[j];
  }
}

template <class T>
void Map<T>::set_col(uint32_t i, const std::vector<T>& col)
{
  for (uint32_t j = 0; j < nRows; j++)
  {
    data[j][i] = col[j];
  }
}

template <class T>
uint32_t Map<T>::get_nRows()
{
  return nRows;
}

template <class T>
uint32_t Map<T>::get_nCols()
{
  return nCols;
}

template <class T>
std::vector<T> Map<T>::get_row(uint32_t row)
{
  return data[row];
}

template <class T>
std::vector<T> Map<T>::get_col(uint32_t col)
{
  std::vector<T> colData;

  for (uint32_t i = 0; i < nRows; i++)
  {
    colData.push_back(data[i][col]);
  }

  return colData;
}

template <class T>
Map<double> *Map<T>::get_map_db()
{
  Map<double> *map = new Map<double>(nRows, nCols);

  for (uint32_t i = 0; i < nRows; i++)
  {
    for (uint32_t j = 0; j < nCols; j++)
    {
      map->data[i][j] = mapLevel(data[i][j]);
    }
  }

  return map;
}

template <class T>
void Map<T>::print()
{
  for (uint32_t i = 0; i < nRows; i++)
  {
    for (uint32_t j = 0; j < nCols; j++)
    {
      std::cout << data[i][j];
      std::cout << " ";
    }
    std::cout << std::endl;
  }
}

template <class T>
uint32_t Map<T>::doppler_hz_to_bin(double dopplerHz)
{
  for (size_t i = 0; i < doppler.size(); i++)
  {
    if (dopplerHz == doppler[i])
    {
      return (int) i;
    }
  }
  return 0;
}

template <class T>
std::string Map<T>::to_json(uint64_t timestamp)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  // store data array
  rapidjson::Value array(rapidjson::kArrayType);
  for (size_t i = 0; i < data.size(); i++)
  {
    rapidjson::Value subarray(rapidjson::kArrayType);
    for (size_t j = 0; j < data[i].size(); j++)
    {
      subarray.PushBack(mapLevel(data[i][j]) - noisePower, document.GetAllocator());
    }
    array.PushBack(subarray, document.GetAllocator());
  }

  // store delay array
  rapidjson::Value arrayDelay(rapidjson::kArrayType);
  for (size_t i = 0; i < delay.size(); i++)
  {
    arrayDelay.PushBack(delay[i], allocator);
  }

  // store Doppler array
  rapidjson::Value arrayDoppler(rapidjson::kArrayType);
  for (uint32_t i = 0; i < get_nRows(); i++)
  {
    arrayDoppler.PushBack(doppler[i], allocator);
  }

  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("nRows", nRows, allocator);
  document.AddMember("nCols", nCols, allocator);
  document.AddMember("noisePower", noisePower, allocator);
  document.AddMember("maxPower", maxPower, allocator);
  document.AddMember("delay", arrayDelay, allocator);
  document.AddMember("doppler", arrayDoppler, allocator);
  document.AddMember(rapidjson::Value("data", document.GetAllocator()).Move(), array, document.GetAllocator());

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}

template <class T>
std::string Map<T>::to_json_km(uint64_t timestamp, uint32_t fs)
{
  if (doppler.size() < nRows)
    throw std::invalid_argument("Cannot convert an invalid radar map or zero sample rate");
  // Ordinary display values have a short, exactly representable decimal
  // mantissa after the existing two-place formatting. The legacy JSON parser
  // can perturb large scientific-notation values before its second write, so
  // retain that path for unusual metadata as well as invalid-value handling.
  const auto ordinary = [](double value) {
    return std::isfinite(value) && std::abs(value) <= 1e9;
  };
  if (!fs || !ordinary(noisePower) || !ordinary(maxPower) ||
      !std::all_of(doppler.begin(), doppler.begin() + nRows,
        ordinary))
    return delay_bin_to_km(to_json(timestamp), fs);

  // A capacity hint only: unusually long numbers still grow the buffer. Do not
  // build a DOM, serialize it, then parse and serialize every map cell again
  // merely to replace the small delay axis.
  const size_t capacity = nRows <= ((std::numeric_limits<size_t>::max() - 1024) / 12) / nCols
    ? size_t(nRows) * nCols * 12 + 1024 : 1024;
  rapidjson::StringBuffer buffer(nullptr, capacity);
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.SetMaxDecimalPlaces(2);
  writer.StartObject();
  writer.Key("timestamp"); writer.Uint64(timestamp);
  writer.Key("nRows"); writer.Uint(nRows);
  writer.Key("nCols"); writer.Uint(nCols);
  writer.Key("noisePower"); writer.Double(noisePower);
  writer.Key("maxPower"); writer.Double(maxPower);
  writer.Key("delay"); writer.StartArray();
  for (const auto value : delay)
    writer.Double(1.0 * value * (Constants::c / (double)fs) / 1000);
  writer.EndArray();
  writer.Key("doppler"); writer.StartArray();
  for (uint32_t i = 0; i < nRows; ++i) writer.Double(doppler[i]);
  writer.EndArray();
  writer.Key("data"); writer.StartArray();
  for (const auto& row : data) {
    writer.StartArray();
    for (const auto value : row) writer.Double(mapLevel(value) - noisePower);
    writer.EndArray();
  }
  writer.EndArray();
  writer.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

template <class T>
std::string Map<T>::to_json_metadata_km(uint64_t timestamp, uint32_t fs)
{
  if (!fs || doppler.size() < nRows)
    throw std::invalid_argument("Cannot convert an invalid radar map or zero sample rate");
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.SetMaxDecimalPlaces(2);
  writer.StartObject();
  writer.Key("timestamp"); writer.Uint64(timestamp);
  writer.Key("nRows"); writer.Uint(nRows);
  writer.Key("nCols"); writer.Uint(nCols);
  writer.Key("noisePower"); writer.Double(noisePower);
  writer.Key("maxPower"); writer.Double(maxPower);
  writer.Key("data_omitted"); writer.Bool(true);
  writer.Key("delay"); writer.StartArray();
  for (const auto value : delay)
    writer.Double(value * (Constants::c / static_cast<double>(fs)) / 1000.0);
  writer.EndArray();
  writer.Key("doppler"); writer.StartArray();
  for (uint32_t i = 0; i < nRows; ++i) writer.Double(doppler[i]);
  writer.EndArray();
  writer.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

template <class T>
std::string Map<T>::to_json_presentation_km(uint64_t timestamp, uint32_t fs,
  bool includeData, uint32_t maximumCells)
{
  if (!fs || !maximumCells || delay.size() < nCols || doppler.size() < nRows)
    throw std::invalid_argument("Cannot convert an invalid presentation map");
  if (!std::isfinite(noisePower) || !std::isfinite(maxPower) ||
      std::any_of(delay.begin(), delay.begin() + nCols,
        [](const auto value) { return !std::isfinite(double(value)); }) ||
      std::any_of(doppler.begin(), doppler.begin() + nRows,
        [](const auto value) { return !std::isfinite(double(value)); }))
    throw std::runtime_error("Presentation map contains non-finite metadata");
  uint32_t stride = 1;
  const auto extent = [](uint32_t count, uint32_t step) {
    return 1u + (count - 1u) / step;
  };
  while (uint64_t(extent(nRows, stride)) * extent(nCols, stride) >
      maximumCells)
    ++stride;
  const uint32_t rows = extent(nRows, stride);
  const uint32_t cols = extent(nCols, stride);
  const size_t capacity = rows <=
      ((std::numeric_limits<size_t>::max() - 1024) / 8) / cols ?
    size_t(rows) * cols * 8 + 1024 : 1024;
  rapidjson::StringBuffer buffer(nullptr, capacity);
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.SetMaxDecimalPlaces(2);
  writer.StartObject();
  writer.Key("timestamp"); writer.Uint64(timestamp);
  writer.Key("nRows"); writer.Uint(rows);
  writer.Key("nCols"); writer.Uint(cols);
  writer.Key("sourceRows"); writer.Uint(nRows);
  writer.Key("sourceCols"); writer.Uint(nCols);
  writer.Key("presentationStride"); writer.Uint(stride);
  writer.Key("presentation_only"); writer.Bool(true);
  writer.Key("presentation_pooling"); writer.String("max");
  writer.Key("presentation_precision_db"); writer.Double(0.25);
  writer.Key("noisePower"); writer.Double(noisePower);
  writer.Key("maxPower"); writer.Double(maxPower);
  if (!includeData) {
    writer.Key("data_omitted"); writer.Bool(true);
  }
  writer.Key("delay"); writer.StartArray();
  for (uint32_t column = 0; column < nCols; column += stride) {
    const uint32_t end = std::min<uint32_t>(column + stride, nCols);
    const double centre = (double(delay[column]) + delay[end - 1]) / 2.0;
    writer.Double(centre * (Constants::c / static_cast<double>(fs)) / 1000.0);
  }
  writer.EndArray();
  writer.Key("doppler"); writer.StartArray();
  for (uint32_t row = 0; row < nRows; row += stride) {
    const uint32_t end = std::min<uint32_t>(row + stride, nRows);
    writer.Double((doppler[row] + doppler[end - 1]) / 2.0);
  }
  writer.EndArray();
  if (includeData) {
    writer.Key("data"); writer.StartArray();
    for (uint32_t row = 0; row < nRows; row += stride) {
      writer.StartArray();
      for (uint32_t column = 0; column < nCols; column += stride) {
        const uint32_t rowEnd = std::min<uint32_t>(row + stride, nRows);
        const uint32_t columnEnd = std::min<uint32_t>(column + stride, nCols);
        double peak = -std::numeric_limits<double>::infinity();
        for (uint32_t sourceRow = row; sourceRow < rowEnd; ++sourceRow)
          for (uint32_t sourceColumn = column; sourceColumn < columnEnd; ++sourceColumn)
            peak = std::max(peak,
              mapLevel(data[sourceRow][sourceColumn]) - noisePower);
        writer.Double(std::round(peak * 4.0) / 4.0);
      }
      writer.EndArray();
    }
    writer.EndArray();
  }
  writer.EndObject();
  return std::string(buffer.GetString(), buffer.GetSize());
}

template <class T>
std::string Map<T>::delay_bin_to_km(std::string json, uint32_t fs)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();
  document.Parse(json.c_str());

  if (fs == 0 || document.HasParseError() || !document.IsObject() ||
      !document.HasMember("delay") || !document["delay"].IsArray())
    throw std::invalid_argument("Cannot convert an invalid radar map or zero sample rate");

  document["delay"].Clear();
  for (size_t i = 0; i < delay.size(); i++)
  {
    document["delay"].PushBack(1.0*delay[i]*(Constants::c/(double)fs)/1000, allocator);
  }

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}

template <class T>
void Map<T>::set_metrics()
{
  set_metrics({});
}

template <class T>
void Map<T>::set_metrics(const std::vector<uint8_t>& validCells)
{
  if (!validCells.empty() && validCells.size() != uint64_t(nRows) * nCols)
    throw std::invalid_argument("Radar map validity dimensions differ");
  MetricAccumulator metrics;
  for (uint32_t i = 0; i < nRows; i++)
  {
    for (uint32_t j = 0; j < nCols; j++)
    {
      if (!validCells.empty() && !validCells[uint64_t(i) * nCols + j]) continue;
      metrics.add(data[i][j]);
    }
  }
  // An unavailable bank has no noise estimate, not a measured zero-power
  // background. Keep metadata finite; its validity mask prevents detection.
  metrics.apply(*this);
}

template <class T>
bool Map<T>::save(std::string _json, std::string filename)
{
  return blah2::data::appendJsonArray(_json, filename);
}

// allowed types
template class Map<std::complex<double>>;
template class Map<double>;
