#include "process/ambiguity/GpuClutterBatch.h"
#include <complex>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Sample = std::complex<float>;
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void verifyChannels(uint32_t channels, uint32_t batchChannels) {
  constexpr uint32_t samples = 7;
  std::vector<Sample> input(samples * channels), output(input.size());
  std::vector<Sample> workspace(samples * batchChannels, Sample{99, 99});
  for (size_t i = 0; i < input.size(); ++i) input[i] = Sample(float(i + 1), -float(i + 1));
  uint32_t calls = 0, completed = 0;
  const bool accepted = blah2::processClutterBatches(samples, channels, batchChannels,
    input.data(), input.size(), output.data(), output.size(),
    [&](uint32_t first, uint32_t active, const Sample* source, size_t count, Sample* target) {
      require(first == completed, "Clutter batch skipped or repeated a channel");
      require(active && active <= batchChannels && count == size_t(active) * samples,
        "Clutter batch count is invalid");
      require(source == input.data() + first * samples &&
        target == output.data() + first * samples, "Clutter batch offsets differ");
      blah2::copyClutterBatch(workspace.data(), workspace.size(), source, count);
      for (size_t i = count; i < workspace.size(); ++i)
        require(workspace[i] == Sample{}, "Final clutter batch retained previous channel samples");
      std::copy_n(workspace.data(), count, target);
      completed += active; ++calls;
      return true;
    });
  require(accepted && output == input && completed == channels,
    "Clutter batching changed channel data or omitted a channel");
  require(calls == (channels + batchChannels - 1) / batchChannels,
    "Clutter batching submitted an unexpected number of batches");
}

void verifyFailureAndShapes() {
  constexpr uint32_t samples = 7, channels = 5, batchChannels = 2;
  std::vector<Sample> input(samples * channels, Sample{1, 2});
  std::vector<Sample> output(input.size(), Sample{9, 9});
  uint32_t calls = 0;
  const bool accepted = blah2::processClutterBatches(samples, channels, batchChannels,
    input.data(), input.size(), output.data(), output.size(),
    [&](uint32_t first, uint32_t, const Sample* source, size_t count, Sample* target) {
      ++calls;
      std::copy_n(source, count, target);
      return first == 0;
    });
  require(!accepted && calls == 2, "A rejected batch did not reject the complete candidate frame");
  require(output.back() == Sample{9, 9}, "Clutter batching continued after a rejection");
  for (const auto shape : {0u, channels + 1}) {
    bool rejected = false;
    try {
      blah2::processClutterBatches(samples, channels, shape,
        input.data(), input.size(), output.data(), output.size(),
        [&](uint32_t, uint32_t, const Sample*, size_t, Sample*) { ++calls; return true; });
    } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && calls == 2, "Invalid clutter batch shape started processing");
  }
  bool rejected = false;
  try {
    blah2::processClutterBatches(samples, channels, batchChannels,
      input.data(), input.size() - 1, output.data(), output.size(),
      [&](uint32_t, uint32_t, const Sample*, size_t, Sample*) { ++calls; return true; });
  } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected && calls == 2, "An incomplete clutter frame started processing");
}
void verifyCircularReference() {
  for(uint32_t samples:{1u,7u,32u,1057u}) {
    for(uint32_t extra:{0u,3u}) {
    std::deque<std::complex<double>> source(samples+extra);
    for(uint32_t i=0;i<samples+extra;++i)
      source[i]={double(i)+.123456789,-double(i)-.987654321};
    source[0]={-0.,std::numeric_limits<double>::denorm_min()};
    for(int32_t delay:{INT32_MIN,-2115,-1,0,1,2115,INT32_MAX}) {
      std::vector<Sample> expected(samples),actual(samples);
      for(size_t i=0;i<samples;++i) {
        const int64_t shifted=(int64_t(i)-delay)%int64_t(samples);
        expected[i]=source[shifted<0?shifted+samples:shifted];
      }
      blah2::copyClutterReference(source,actual.data(),samples,delay);
      require(std::memcmp(actual.data(),expected.data(),samples*sizeof(Sample))==0,
        "Circular reference differs bitwise from signed-modulo oracle");
    }
    }
  }
  std::deque<std::complex<double>> source(7);
  std::vector<Sample> output(7);
  for(uint32_t samples:{0u,8u}) {
    bool rejected=false;
    try { blah2::copyClutterReference(source,output.data(),samples,0); }
    catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,"Invalid circular reference dimensions accepted");
  }
}
void verifyDirectBatchPadding() {
  std::vector<Sample> storage(23, Sample{99, -99});
  auto* destination = storage.data() + 1;
  for (size_t count : {21u, 7u, 14u, 21u}) {
    for (size_t i = 0; i < count; ++i) destination[i] = Sample(float(i), -float(i));
    blah2::padClutterBatch(destination, 21, count);
    for (size_t i = 0; i < count; ++i)
      require(destination[i] == Sample(float(i), -float(i)), "Padding changed active samples");
    for (size_t i = count; i < 21; ++i)
      require(destination[i] == Sample{}, "Direct batch retained inactive samples");
    require(storage.front() == Sample{99, -99} && storage.back() == Sample{99, -99},
      "Direct padding crossed the mapped batch bounds");
  }
  for (size_t count : {0u, 22u}) {
    const auto before = storage;
    bool rejected = false;
    try { blah2::padClutterBatch(destination, 21, count); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && storage == before, "Invalid direct padding mutated storage");
  }
  bool rejected = false;
  try { blah2::padClutterBatch<Sample>(nullptr, 21, 7); }
  catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "Null direct padding destination accepted");
}
}

int main() try {
  for (uint32_t channels = 1; channels <= 8; ++channels)
    for (uint32_t batch = 1; batch <= channels; ++batch) verifyChannels(channels, batch);
  verifyFailureAndShapes();
  verifyCircularReference();
  verifyDirectBatchPadding();
  std::cout << "PASS clutter channel batches, circular references, copied/direct partial padding, rejection and shapes\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << '\n';
  return 1;
}
