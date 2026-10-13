#include "process/ambiguity/GpuClutterBatch.h"
#include <complex>
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
}

int main() try {
  for (uint32_t channels = 1; channels <= 8; ++channels)
    for (uint32_t batch = 1; batch <= channels; ++batch) verifyChannels(channels, batch);
  verifyFailureAndShapes();
  std::cout << "PASS clutter channel batches, partial padding, rejection and shapes\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << '\n';
  return 1;
}
