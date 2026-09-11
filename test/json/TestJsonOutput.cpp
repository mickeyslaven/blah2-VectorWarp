#include "LegacyJson.h"
#include "data/Detection.h"
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <time.h>

namespace {
using Complex = std::complex<double>;
void require(bool value, const std::string& reason) {
  if (!value) throw std::runtime_error(reason);
}
template<class Run> std::string outcome(Run run) {
  try { return "json:" + run(); }
  catch (const std::invalid_argument& error) { return "invalid_argument:" + std::string(error.what()); }
  catch (const std::runtime_error& error) { return "runtime_error:" + std::string(error.what()); }
}
void same(const std::string& expected, const std::string& actual, const std::string& context) {
  if (expected != actual) {
    size_t at = 0;
    while (at < std::min(expected.size(), actual.size()) && expected[at] == actual[at]) ++at;
    throw std::runtime_error(context + " differs at byte " + std::to_string(at) +
      ": expected=" + expected.substr(at, 90) + " actual=" + actual.substr(at, 90));
  }
}
template<class T> void checkMap(Map<T>& source, uint64_t timestamp, uint32_t fs,
    const std::string& label) {
  const auto original = source.data;
  const auto expected = outcome([&] { return legacy_json::mapKm(source, timestamp, fs); });
  same(outcome([&] { return legacy_json::map(source, timestamp); }),
    outcome([&] { return source.to_json(timestamp); }), label + " raw oracle");
  same(expected, outcome([&] { return source.to_json_km(timestamp, fs); }), label);
  if (expected.find("json:") == 0)
    require(source.data == original, "Serialization mutated map samples");
}
Map<Complex> fixture(uint32_t rows, uint32_t columns = 256) {
  Map<Complex> source(rows, columns);
  for (uint32_t column = 0; column < columns; ++column) source.delay.push_back(int(column) - 10);
  std::mt19937 random(193015);
  std::normal_distribution<double> noise;
  for (uint32_t row = 0; row < rows; ++row) {
    source.doppler.push_back((int64_t(row) - rows / 2) * 5.0);
    for (uint32_t column = 0; column < columns; ++column) {
      source.data[row][column] = {noise(random), noise(random)};
      if ((row * columns + column) % 97 == 0) source.data[row][column] = {};
    }
  }
  source.set_metrics();
  return source;
}
void correctness() {
  auto source = fixture(3, 7);
  source.delay = {-10, -5, -1, 0, 1, 245, std::numeric_limits<int>::max()};
  source.doppler = {-4800.1234567, -0.0, 1e-300};
  for (const uint64_t timestamp : {uint64_t(0), uint64_t(1700000000123ULL), UINT64_MAX})
    for (const uint32_t fs : {1u, 2400000u, 6000000u, UINT32_MAX})
      checkMap(source, timestamp, fs, "map timestamp/fs");
  const std::vector<double> extremes{0, -0.0, 1e-300, 1e-30, 0.00999999999,
    .01, .019999999999, .1, 123.456789, -1e9, -1e9 + .015, 1e9 - .015,
    1e9, std::nextafter(1e9, std::numeric_limits<double>::infinity()),
    1e12, 1e20, 1.234567890123456e100,
    1e300, std::numeric_limits<double>::max()};
  for (const auto value : extremes) {
    source.data[0][0] = {value, 0};
    source.noisePower = value;
    source.maxPower = value;
    source.doppler[0] = value;
    checkMap(source, 1700000000123ULL, 2400000, "finite extreme " + std::to_string(value));
  }
  std::mt19937_64 bitPatterns(248396);
  auto varied = fixture(1, 1);
  for (unsigned trial = 0; trial < 4096; ++trial) {
    const uint64_t bits = bitPatterns();
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    if (!std::isfinite(value)) continue;
    varied.noisePower = value; varied.maxPower = -value; varied.doppler[0] = value;
    checkMap(varied, trial, 2400000, "finite IEEE pattern " + std::to_string(trial));
    const std::vector<double> delay{value}, doppler{-value}, snr{value};
    Detection detection(delay, doppler, snr);
    same(outcome([&] { return legacy_json::kilometres(
      legacy_json::detection(delay, doppler, snr, trial), delay, 2400000,
      "Cannot convert invalid detections or zero sample rate"); }),
      outcome([&] { return detection.to_json_km(trial, 2400000); }), "detection IEEE pattern");
  }
  for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    for (const uint32_t fs : {0u, 2400000u}) {
      auto bad = fixture(3, 7); bad.data[1][2] = {invalid, 0};
      checkMap(bad, 1, fs, "invalid sample");
      bad = fixture(3, 7); bad.noisePower = invalid;
      checkMap(bad, 1, fs, "invalid noise");
      bad = fixture(3, 7); bad.maxPower = invalid;
      checkMap(bad, 1, fs, "invalid maximum");
      bad = fixture(3, 7); bad.doppler[1] = invalid;
      checkMap(bad, 1, fs, "invalid Doppler");
    }
  }
  source = fixture(3, 7);
  checkMap(source, 1, 0, "zero sample rate");
  for (auto& row : source.data) std::fill(row.begin(), row.end(), Complex{});
  source.set_metrics();
  checkMap(source, 1, 2400000, "complete silence");
  source.doppler.pop_back();
  require(outcome([&] { return source.to_json_km(1, 2400000); }) ==
    "invalid_argument:Cannot convert an invalid radar map or zero sample rate",
    "Short Doppler axis entered unchecked memory access");
  Map<double> real(1, 5); real.delay = {-2, -1, 0, 1, 2}; real.doppler = {0};
  real.data[0] = {0, -1, 1e-300, 1.2345, 1e300}; real.set_metrics();
  checkMap(real, 1, 6000000, "real-valued map");
  for (const uint32_t rows : {321u, 961u, 1921u}) {
    auto large = fixture(rows);
    checkMap(large, 1700000000123ULL, 2400000, "standard-range production map");
  }
  for (const auto value : extremes) {
    const std::vector<double> delay{-10, 0, value, 245.98765};
    const std::vector<double> doppler{-4800.12345, -0.0, value, 4800};
    const std::vector<double> snr{0, .0199999, value, 123.45678};
    Detection detection(delay, doppler, snr);
    for (const uint32_t fs : {0u, 1u, 2400000u, 6000000u}) {
      const auto expected = outcome([&] { return legacy_json::kilometres(
        legacy_json::detection(delay, doppler, snr, UINT64_MAX), delay, fs,
        "Cannot convert invalid detections or zero sample rate"); });
      same(expected, outcome([&] { return detection.to_json_km(UINT64_MAX, fs); }), "detection extreme");
    }
  }
  for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    for (unsigned field = 0; field < 3; ++field) {
      std::vector<double> delay{1}, doppler{2}, snr{3};
      (field == 0 ? delay : field == 1 ? doppler : snr)[0] = invalid;
      Detection detection(delay, doppler, snr);
      same(outcome([&] { return legacy_json::kilometres(
        legacy_json::detection(delay, doppler, snr, 1), delay, 2400000,
        "Cannot convert invalid detections or zero sample rate"); }),
        outcome([&] { return detection.to_json_km(1, 2400000); }), "invalid detection");
    }
  }
  Detection empty(std::vector<double>{}, std::vector<double>{}, std::vector<double>{});
  same(legacy_json::kilometres(legacy_json::detection({}, {}, {}, 0), std::vector<double>{},
    2400000, "Cannot convert invalid detections or zero sample rate"), empty.to_json_km(0, 2400000),
    "empty detections");
  std::cout << "PASS frozen legacy JSON byte/exception equality, full-range maps, timestamps, zero/extreme/invalid values\n";
}
double threadMs() {
  timespec time{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time)) throw std::runtime_error("thread clock failed");
  return time.tv_sec * 1000.0 + time.tv_nsec / 1e6;
}
struct Measurement { double cpuMs, wallMs; std::string json; };
template<class Run> Measurement measure(Run run) {
  const auto start = std::chrono::steady_clock::now();
  const auto cpuStart = threadMs();
  auto output = run();
  const auto cpuEnd = threadMs();
  const auto end = std::chrono::steady_clock::now();
  return {cpuEnd - cpuStart, std::chrono::duration<double, std::milli>(end - start).count(), std::move(output)};
}
void benchmark() {
  std::cout << "rows,columns,repeat,variant,thread_cpu_ms,wall_ms,json_bytes\n" << std::setprecision(9);
  for (const uint32_t rows : {321u, 961u, 1921u}) {
    auto source = fixture(rows);
    const auto expected = legacy_json::mapKm(source, 1700000000123ULL, 2400000);
    same(expected, source.to_json_km(1700000000123ULL, 2400000), "benchmark preflight");
    for (unsigned repeat = 0; repeat < 9; ++repeat) {
      for (unsigned position = 0; position < 2; ++position) {
        const bool legacy = (repeat + position) % 2 == 0;
        const auto result = measure([&] { return legacy ?
          legacy_json::mapKm(source, 1700000000123ULL, 2400000) :
          source.to_json_km(1700000000123ULL, 2400000); });
        same(expected, result.json, "benchmark output outside timing");
        std::cout << rows << ",256," << repeat << ',' << (legacy ? "legacy" : "direct")
          << ',' << result.cpuMs << ',' << result.wallMs << ',' << result.json.size() << '\n';
      }
    }
  }
}
}
int main(int argc, char** argv) try {
  if (argc == 2 && std::string(argv[1]) == "--benchmark") benchmark();
  else correctness();
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
