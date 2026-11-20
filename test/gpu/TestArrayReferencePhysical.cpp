// Opt-in tiny physical Vulkan test of the actual resident reference shaders.
// No capture, radar, FFT plans, service access or software-device fallback.
#include "../../core/VulkanBackend.private.cpp"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include <random>

namespace {
void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
void run(blah2::Context& context, uint32_t samples, uint32_t channels) {
  using namespace blah2;
  using Z = std::complex<double>;
  constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  constexpr auto preferred = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
    VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  const uint32_t stride = std::max(1u, samples / 64);
  const uint32_t observations = 1 + (samples - 1) / stride;
  const uint32_t tile = 32, tiles = 1 + (observations - 1) / tile;
  Buffer metadata(context, gpu_reference::metaWords * 4, host, preferred);
  Buffer raw(context, uint64_t(samples) * channels * 16, host, preferred);
  Buffer reference(context, uint64_t(samples) * 16, host, preferred);
  Buffer scratch(context, std::max(uint64_t(samples),
    uint64_t(channels) * channels * tiles) * 16, host, preferred);
  Buffer phases(context, 16, host, preferred), output(context, 512, host, preferred);
  const auto sources = gpu_reference::shaderSources();
  std::vector<std::unique_ptr<Kernel>> kernels;
  for (const unsigned i : {0U, 1U, 2U, 3U, 4U, 5U, 8U})
    kernels.push_back(std::make_unique<Kernel>(context, sources[i].second.c_str(),
      std::vector<Buffer*>{&metadata, &raw, &reference, &scratch, &phases, &output}));
  std::array<uint32_t, gpu_reference::metaWords> words{};
  words[16] = samples; words[17] = channels; words[18] = channels;
  words[19] = 1; words[20] = 64; words[21] = 10; words[22] = 12;
  words[27] = stride; words[28] = observations; words[29] = 1 + (samples - 1) / 32;
  for (uint32_t c = 0; c < channels; ++c) words[32 + c] = c;
  const double smoothing = .8, loading = .001;
  std::memcpy(words.data() + 448, &smoothing, 8);
  std::memcpy(words.data() + 450, &loading, 8);
  std::memcpy(metadata.mapped, words.data(), sizeof(words));
  metadata.flush(0, metadata.bytes);
  gpu_reference::CommandOwner command(context);
  command.allocate();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command, &begin), "reference test record");
  vkCmdFillBuffer(command.command, metadata.handle, 0, 64, 0);
  VkBufferCopy copy{128 * 4, 64 * 4, 52 * 4};
  vkCmdCopyBuffer(command.command, metadata.handle, metadata.handle, 1, &copy);
  barrier(command.command);
  kernels[0]->append(command.command, {1, 0, 0, 0, 0}); barrier(command.command);
  kernels[1]->append(command.command, {channels * channels * tiles, 0, tile, tiles, 0}); barrier(command.command);
  kernels[1]->append(command.command, {channels * channels, 1, tile, tiles, 0}); barrier(command.command);
  kernels[2]->append(command.command, {1, 0, 0, 0, 0}); barrier(command.command);
  kernels[3]->append(command.command, {samples, 0, 0, 0, 0}); barrier(command.command);
  kernels[4]->append(command.command, {1, 0, 0, 0, 0}); barrier(command.command);
  kernels[5]->append(command.command, {samples, 0, 0, 0, 0}); barrier(command.command);
  kernels[6]->append(command.command, {52, 0, 0, 0, 0}); barrier(command.command);
  VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  read.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &read, 0, nullptr, 0, nullptr);
  check(vkEndCommandBuffer(command.command), "reference test record");
  ArrayReferenceSynthesizer::Config config;
  config.analysisSamples = 64;
  ArrayReferenceSynthesizer cpu(config);
  std::vector<std::unique_ptr<IqData>> storage;
  std::vector<IqData*> inputs;
  for (uint32_t c = 0; c < channels; ++c) {
    storage.push_back(std::make_unique<IqData>(samples));
    inputs.push_back(storage.back().get());
  }
  std::mt19937 generator(42);
  std::uniform_real_distribution<double> noise(-.01, .01);
  double maxError = 0;
  for (uint32_t frame = 0; frame < 19; ++frame) {
    const bool scaled = frame >= 16;
    const double inputScale = frame == 17 ? 1e-7 : frame == 18 ? 1e-8 : 1;
    if (scaled) {
      cpu.reset();
      // Each amplitude starts without retained weights: parity must exercise
      // low-energy covariance iteration, not reuse a unit-scale eigenvector.
      std::fill(words.begin() + 64, words.begin() + 180, 0);
      std::memcpy(metadata.mapped, words.data(), sizeof(words));
      metadata.flush(0, metadata.bytes);
    }
    auto* rawValues = static_cast<Z*>(raw.mapped);
    for (uint32_t c = 0; c < channels; ++c) {
      auto& values = inputs[c]->resize_for_write(samples);
      for (uint32_t n = 0; n < samples; ++n) {
        const double phase = frame == 2 ? 0 : 2 * std::acos(-1.) * c / channels;
        const double amplitude = frame == 3 ? 0 : frame == 1 ? 2 : 1;
        values[n] = amplitude * std::polar(1.0, phase + .021 * n);
        if (frame >= 5) values[n] += Z(noise(generator), noise(generator));
        if (frame >= 12 && channels >= 3)
          values[n] = c == 0 ? std::sqrt(.9) * (n % 2 ? -1.0 : 1.0) :
            1.0 / std::sqrt(double(channels - 1));
        if (scaled) values[n] = std::polar(inputScale, .3*c + .02*n);
        rawValues[size_t(c) * samples + n] = values[n];
      }
    }
    raw.flush(0, raw.bytes);
    const auto expected = cpu.process(inputs);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command.command;
    check(vkQueueSubmit(context.queue, 1, &submit, context.fence), "reference test submit");
    check(vkWaitForFences(context.device, 1, &context.fence, VK_TRUE,
      UINT64_C(10000000000)), "reference test fence");
    check(vkResetFences(context.device, 1, &context.fence), "reference test fence reset");
    metadata.invalidate(0, metadata.bytes); reference.invalidate(0, reference.bytes);
    std::memcpy(words.data(), metadata.mapped, sizeof(words));
    require(words[0] == 0, "Device reference numeric rejection");
    require(words[4] == (words[1] ? config.powerIterations * channels : 0),
      "Device reference seed/iteration coverage differs");
    const auto* actual = static_cast<const Z*>(reference.mapped);
    for (uint32_t n = 0; n < samples; ++n) {
      const double error = std::abs(actual[n] - expected->view_data()[n]);
      maxError = std::max(maxError, error);
      require(std::isfinite(error) && error < 1e-12, "Resident reference oracle rejected frame");
    }
    const auto& metrics = cpu.get_metrics();
    if (scaled) {
      require(!metrics.fallback, "Amplitude scaling changed the reference beam");
      require(std::abs(metrics.coherentGainDb - 10*std::log10(double(channels))) < 1e-10,
        "Amplitude scaling lost physical coherent gain");
      for (uint32_t n = 0; n < samples; ++n)
        require(std::abs(std::abs(actual[n])/inputScale - std::sqrt(double(channels))) < 1e-10,
          "Resident low-energy reference lost coherent amplitude");
    }
    require(words[130] == metrics.updates && words[131] == 0 && words[132] == channels,
      "Resident reference counters differ");
    for (uint32_t c = 0; c < channels; ++c) {
      Z weight; std::memcpy(static_cast<void*>(&weight), words.data() + 136 + 4 * c, 16);
      require(std::abs(weight - metrics.weights[c]) <= 1e-12,
        "Resident reference weight oracle rejected frame");
    }
    double fraction, gain, scale;
    std::memcpy(&fraction, words.data() + 168, 8);
    std::memcpy(&gain, words.data() + 170, 8);
    std::memcpy(&scale, words.data() + 172, 8);
    require(std::abs(fraction - metrics.coherentFraction) <= 1e-12 &&
      std::abs(gain - metrics.coherentGainDb) <= 1e-10,
      "Resident reference metric oracle rejected frame");
    require(scale == 1, "Per-CPI normalization returned");
  }
  std::cout << "PASS physical reference channels=" << channels << " samples="
    << samples << " frames=19 max_error=" << maxError << '\n';
}
}
int main() {
  try {
    auto instance = std::make_shared<blah2::Instance>();
    const auto candidates = blah2::enumerate(*instance);
    require(!candidates.empty(), "No physical Vulkan device available");
    auto selected = candidates.begin();
    if (const char* id = std::getenv("VECTORWARP_REFERENCE_TEST_DEVICE")) {
      selected = std::find_if(candidates.begin(), candidates.end(),
        [&](const auto& candidate) { return candidate.info.id == id; });
      require(selected != candidates.end(), "Requested physical test device is absent");
    }
    std::cout << "device=" << selected->info.name << " id=" << selected->info.id << std::endl;
    blah2::Context context(instance, *selected);
    for (const uint32_t samples : {64U, 257U})
      for (const uint32_t channels : {2U, 3U, 5U, 8U}) run(context, samples, channels);
    std::cout << "COMPLETE physical_reference_parity fixtures=8 frames=152 tolerance=1e-12\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL physical reference: " << error.what() << '\n'; return 1;
  }
}
