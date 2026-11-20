#include "IqData.h"
#include <algorithm>
#include <iostream>
#include <cstdlib>
#include <chrono>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filewritestream.h"

// constructor
IqData::IqData(uint32_t _n)
{
  n = _n;
  min = 0;
  max = 0;
  mean = 0;
  data = new std::deque<std::complex<double>>;
}

IqData::~IqData()
{
  delete data;
}

uint32_t IqData::get_n()
{
  return n;
}

void IqData::set_capacity_unlocked(uint32_t capacity)
{
  if (capacity < get_length())
    throw std::runtime_error("IQ FIFO capacity is shorter than queued data");
  n = capacity;
}

uint32_t IqData::get_length()
{
  return floatCapture ? floatCaptureData->size() :
    contiguousActive ? contiguousCount : data->size();
}

IqSampleView IqData::sample_view() const
{
  if (floatCapture)
    throw std::logic_error("Native float capture storage has no FP64 view");
  return contiguousActive ? IqSampleView(contiguousData.data(), contiguousCount) : IqSampleView(*data);
}

void IqData::materialize_deque() const
{
  if (!contiguousActive) return;
  const auto started = std::chrono::steady_clock::now();
  data->assign(contiguousData.begin(), contiguousData.begin() + contiguousCount);
  contiguousActive = false;
  ++storageMetrics.materializations;
  storageMetrics.materializedSamples += contiguousCount;
  storageMetrics.materializationMs += std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - started).count();
}

std::vector<std::complex<double>>& IqData::resize_contiguous_for_write(uint32_t count)
{
  if (floatCapture || count > n)
    throw std::invalid_argument("Invalid contiguous FP64 IQ block");
  const auto capacity = contiguousData.capacity();
  if (!contiguousActive) {
    // Allocate before releasing the authoritative FIFO on exceptional paths.
    contiguousData.assign(data->begin(), data->end());
    contiguousData.resize(count);
    data->clear();
    contiguousActive = true;
  } else contiguousData.resize(count);
  contiguousCount = count;
  if (contiguousData.capacity() != capacity) ++storageMetrics.allocations;
  return contiguousData;
}

IqData::StorageMetrics IqData::take_storage_metrics()
{
  const auto result = storageMetrics;
  storageMetrics = {};
  return result;
}

uint64_t IqData::get_dropped_samples() const { return droppedSamples; }

void IqData::lock()
{
  mutex_lock.lock();
}

void IqData::unlock()
{
  mutex_lock.unlock();
}

std::deque<std::complex<double>> IqData::get_data()
{
  if (floatCapture)
    return floatCaptureData->copy(floatCaptureData->size());
  if (contiguousActive) {
    const auto started = std::chrono::steady_clock::now();
    std::deque<std::complex<double>> result(contiguousData.begin(), contiguousData.begin() + contiguousCount);
    ++storageMetrics.materializations;
    storageMetrics.materializedSamples += contiguousCount;
    storageMetrics.materializationMs += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count();
    return result;
  }
  return *data;
}

const std::deque<std::complex<double>>& IqData::view_data() const
{
  if (floatCapture)
    throw std::logic_error("Native float capture storage has no FP64 borrowed view");
  materialize_deque();
  return *data;
}

std::deque<std::complex<double>> IqData::drain_front(uint32_t count)
{
  if (floatCapture)
  {
    auto samples = floatCaptureData->copy(count);
    floatCaptureData->discard(count);
    return samples;
  }
  materialize_deque();
  if (count > data->size())
  {
    throw std::runtime_error("Attempting to drain past the end of a deque");
  }
  if (count == data->size())
  {
    std::deque<std::complex<double>> samples;
    samples.swap(*data);
    return samples;
  }
  auto end = data->begin() + count;
  if (count > data->size() - count)
  {
    // Capture commonly has a CPI plus a few new USB samples. Copy the short
    // retained tail, not the entire CPI, while the receive thread waits for us.
    // Allocate before mutation so allocation failure leaves the queue intact.
    std::deque<std::complex<double>> tail(end, data->end());
    data->erase(end, data->end());
    tail.swap(*data);
    return tail; // Owns the original prefix; its sample storage was not copied.
  }
  std::deque<std::complex<double>> samples(data->begin(), end);
  data->erase(data->begin(), end);
  return samples;
}

std::deque<std::complex<double>> IqData::take_all()
{
  if (floatCapture)
  {
    auto samples = floatCaptureData->copy(floatCaptureData->size());
    floatCaptureData->clear();
    return samples;
  }
  materialize_deque();
  std::deque<std::complex<double>> samples;
  samples.swap(*data);
  return samples;
}

void IqData::enable_float_capture(uint32_t blockSamples, uint32_t warmBlocks)
{
  materialize_deque();
  if (!data->empty() || (floatCaptureData && !floatCaptureData->empty()))
    throw std::logic_error("Float capture storage must be enabled on an empty FIFO");
  floatCaptureData = std::make_unique<FloatCaptureStorage>(
    blockSamples ? blockSamples : std::max(uint32_t(1), n), n, warmBlocks);
  floatCapture = true;
}

std::deque<std::complex<float>> IqData::take_all_float()
{
  if (!floatCapture)
    throw std::logic_error("Native float capture storage is not enabled");
  auto samples = floatCaptureData->copy<std::complex<float>>(
    floatCaptureData->size());
  floatCaptureData->clear();
  return samples;
}

FloatCaptureBlockBatch IqData::take_float_blocks(std::size_t minimumSamples)
{
  if (!floatCapture)
    throw std::logic_error("Native float capture storage is not enabled");
  return floatCaptureData->take_complete(minimumSamples);
}

FloatCaptureBlockPool::Stats IqData::float_capture_pool_stats() const
{
  if (!floatCapture)
    throw std::logic_error("Native float capture storage is not enabled");
  return floatCaptureData->stats();
}

void IqData::prepare_float_capture_reserve()
{
  if (!floatCapture) throw std::logic_error("Native float capture storage is not enabled");
  floatCaptureData->prepare_reserve();
}

FloatCaptureBlockPool::ReserveStats IqData::float_capture_reserve_stats() const
{
  if (!floatCapture) throw std::logic_error("Native float capture storage is not enabled");
  return floatCaptureData->reserve_stats();
}

void IqData::discard_front(uint32_t count)
{
  if (floatCapture)
  {
    floatCaptureData->discard(count);
    return;
  }
  if (contiguousActive) {
    if (count > contiguousCount) throw std::runtime_error("Attempting to discard past the end of IQ");
    if (count && count < contiguousCount)
      std::move(contiguousData.begin() + count, contiguousData.begin() + contiguousCount, contiguousData.begin());
    contiguousCount -= count; // Retain initialized scratch; the next CPI overwrites it.
    return;
  }
  if (count > data->size())
    throw std::runtime_error("Attempting to discard past the end of a deque");
  data->erase(data->begin(), data->begin() + count);
}

void IqData::subtract_clutter(const std::complex<float>* estimate, uint32_t count)
{
  if ((!estimate && count) || count > sample_view().size())
    throw std::runtime_error("Clutter estimate does not match the IQ block");
  if (contiguousActive) {
    for (uint32_t i = 0; i < count; ++i) contiguousData[i] -= std::complex<double>(estimate[i]);
    contiguousCount = count;
    return;
  }
  auto sample = data->begin();
  for (uint32_t i = 0; i < count; ++i, ++sample)
    *sample -= std::complex<double>(estimate[i]);
  // Match the filter's existing contract: publish only the conditioned CPI.
  data->erase(sample, data->end());
}

void IqData::subtract_clutter(const std::complex<double>* estimate, uint32_t count,
                              uint32_t divisor)
{
  if ((!estimate && count) || !divisor || count > sample_view().size())
    throw std::runtime_error("Clutter estimate does not match the IQ block");
  if (contiguousActive) {
    for (uint32_t i = 0; i < count; ++i)
      contiguousData[i] = contiguousData[i] - (estimate[i] / double(divisor));
    contiguousCount = count;
    return;
  }
  auto sample = data->begin();
  for (uint32_t i = 0; i < count; ++i, ++sample)
    *sample = *sample - (estimate[i] / double(divisor));
  // Match the filter's existing contract: publish only the conditioned CPI.
  data->erase(sample, data->end());
}

void IqData::replace(std::deque<std::complex<double>>&& samples)
{
  if (floatCapture)
  {
    if (samples.size() > n)
      samples.erase(samples.begin(), samples.end() - n);
    const std::vector<std::complex<float>> converted(samples.begin(), samples.end());
    floatCaptureData->clear();
    floatCaptureData->append(converted.data(), converted.size());
    return;
  }
  if (samples.size() > n)
  {
    samples.erase(samples.begin(), samples.end() - n);
  }
  if (contiguousActive) {
    const auto capacity = contiguousData.capacity();
    contiguousData.assign(samples.begin(), samples.end());
    contiguousCount = samples.size();
    if (contiguousData.capacity() != capacity) ++storageMetrics.allocations;
    return;
  }
  *data = std::move(samples);
}

std::deque<std::complex<double>>& IqData::resize_for_write(uint32_t count)
{
  if (floatCapture)
    throw std::logic_error("Native float capture storage has no FP64 writable view");
  if (count > n)
    throw std::runtime_error("Writable IQ block exceeds FIFO capacity");
  materialize_deque();
  if (count != data->size())
    data->resize(count);
  return *data;
}

void IqData::push_back(std::complex<double> sample)
{
  if (floatCapture)
  {
    const std::complex<float> value(sample);
    append_unlocked(&value, 1);
    return;
  }
  materialize_deque();
  if (!n) { ++droppedSamples; return; }
  if (data->size() < n)
  {
    data->push_back(sample);
  }
  else
  {
    data->pop_front();
    ++droppedSamples;
    data->push_back(sample);
  }
}

void IqData::append_unlocked(
  const std::vector<std::complex<float>>& samples)
{
  append_unlocked(samples.data(), samples.size());
}

void IqData::append_unlocked(const std::complex<float>* samples, std::size_t count)
{
  if (!count) return;
  if (!samples) throw std::invalid_argument("Null IQ sample block");
  if (!n) { droppedSamples += count; return; }
  materialize_deque();
  if (floatCapture)
  {
    if (count >= n)
    {
      droppedSamples += floatCaptureData->size() + count - n;
      floatCaptureData->clear();
      floatCaptureData->append(samples + (count - n), n);
      return;
    }
    const std::size_t required = floatCaptureData->size() + count;
    if (required > n)
    {
      const std::size_t retired = required - n;
      floatCaptureData->discard(retired);
      droppedSamples += retired;
    }
    floatCaptureData->append(samples, count);
    return;
  }
  if (count >= n)
  {
    droppedSamples += data->size() + count - n;
    data->clear();
    data->insert(data->end(), samples + (count - n), samples + count);
    return;
  }
  const std::size_t required = data->size() + count;
  if (required > n)
  {
    const std::size_t retired = required - n;
    data->erase(data->begin(), data->begin() + retired);
    droppedSamples += retired;
  }
  data->insert(data->end(), samples, samples + count);
}

void IqData::assign_paired_i16(const int16_t* samples, uint32_t count, IqData& other)
{
  if (this == &other || count > n || count > other.n || (count && !samples))
    throw std::invalid_argument("Invalid paired IQ sample replacement");
  materialize_deque();
  other.materialize_deque();
  data->resize(count);
  other.data->resize(count);
  auto first = data->begin();
  auto second = other.data->begin();
  for (uint32_t i = 0; i < count; ++i, ++first, ++second, samples += 4) {
    *first = {double(samples[0]), double(samples[1])};
    *second = {double(samples[2]), double(samples[3])};
  }
}

void IqData::assign_complex(const std::complex<double>* samples, uint32_t count)
{
  if (count > n || (count && !samples))
    throw std::invalid_argument("Invalid complex IQ sample replacement");
  if (contiguousActive) {
    // A borrowed clutter tail may alias this block. Copy before resizing so
    // no iterator/pointer is invalidated before the overlap is consumed.
    const auto capacity = contiguousData.capacity();
    if (count > contiguousData.size()) contiguousData.resize(count);
    if (count && samples != contiguousData.data()) std::copy_n(samples, count, contiguousData.begin());
    contiguousCount = count;
    if (contiguousData.capacity() != capacity) ++storageMetrics.allocations;
    return;
  }
  data->resize(count);
  std::copy_n(samples, count, data->begin());
}

std::complex<double> IqData::pop_front()
{
  if (floatCapture)
  {
    const auto sample = floatCaptureData->copy(1).front();
    floatCaptureData->discard(1);
    return sample;
  }
  materialize_deque();
  if (data->empty()) {
    throw std::runtime_error("Attempting to pop from an empty deque");
  }
  std::complex<double> sample = data->front();
  data->pop_front();
  return sample;
}
void IqData::print()
{
  materialize_deque();
  int n = data->size();
  std::cout << data->size() << std::endl;
  for (int i = 0; i < n; i++)
  {
    std::cout << data->front() << std::endl;
    data->pop_front();
  }
}

void IqData::clear()
{
  data->clear();
  contiguousCount = 0;
  if (floatCaptureData) floatCaptureData->clear();
}

void IqData::update_spectrum(std::vector<std::complex<double>> _spectrum)
{
  spectrum = _spectrum;
}

void IqData::update_frequency(std::vector<double> _frequency)
{
  frequency = _frequency;
}

std::string IqData::to_json(uint64_t timestamp)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  // store frequency array
  rapidjson::Value arrayFrequency(rapidjson::kArrayType);
  for (size_t i = 0; i < frequency.size(); i++)
  {
    arrayFrequency.PushBack(frequency[i], allocator);
  }

  // store spectrum array
  rapidjson::Value arraySpectrum(rapidjson::kArrayType);
  for (size_t i = 0; i < spectrum.size(); i++)
  {
    // FFT values are amplitudes: 20 log10(amplitude), not 10 log10.
    // Bound the display floor so zero samples still produce valid JSON.
    arraySpectrum.PushBack(20 * std::log10(std::max(1e-15, std::abs(spectrum[i]))), allocator);
  }

  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("min", min, allocator);
  document.AddMember("max", max, allocator);
  document.AddMember("mean", mean, allocator);
  document.AddMember("frequency", arrayFrequency, allocator);
  document.AddMember("spectrum", arraySpectrum, allocator);

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}
