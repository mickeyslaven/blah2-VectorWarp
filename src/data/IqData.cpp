#include "IqData.h"
#include <iostream>
#include <cstdlib>

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

uint32_t IqData::get_length()
{
  return data->size();
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
  return *data;
}

const std::deque<std::complex<double>>& IqData::view_data() const
{
  return *data;
}

std::deque<std::complex<double>> IqData::drain_front(uint32_t count)
{
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

void IqData::discard_front(uint32_t count)
{
  if (count > data->size())
    throw std::runtime_error("Attempting to discard past the end of a deque");
  data->erase(data->begin(), data->begin() + count);
}

void IqData::subtract_clutter(const std::complex<float>* estimate, uint32_t count)
{
  if ((!estimate && count) || count > data->size())
    throw std::runtime_error("Clutter estimate does not match the IQ block");
  auto sample = data->begin();
  for (uint32_t i = 0; i < count; ++i, ++sample)
    *sample -= std::complex<double>(estimate[i]);
  // Match the filter's existing contract: publish only the conditioned CPI.
  data->erase(sample, data->end());
}

void IqData::replace(std::deque<std::complex<double>>&& samples)
{
  if (samples.size() > n)
  {
    samples.erase(samples.begin(), samples.end() - n);
  }
  *data = std::move(samples);
}

void IqData::push_back(std::complex<double> sample)
{
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

std::complex<double> IqData::pop_front()
{
  if (data->empty()) {
    throw std::runtime_error("Attempting to pop from an empty deque");
  }
  std::complex<double> sample = data->front();
  data->pop_front();
  return sample;
}
void IqData::print()
{
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
