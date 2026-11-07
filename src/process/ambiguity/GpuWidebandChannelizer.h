#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>

namespace blah2::wideband_gpu
{

constexpr uint32_t maxLanes = 16;
constexpr uint32_t maxChannels = 8;
// Configuration/wire ceiling, not a direct-FIR execution ceiling. B210-wide
// captures can legitimately require tens of thousands of taps while retaining
// the configured taps-per-phase quality.
constexpr uint32_t maxFilterTaps = 131071;

// Worker-facing configuration. Taps are supplied by the qualified CPU filter
// designer so CPU and GPU never acquire subtly different channel definitions.
struct LaneConfig
{
  uint64_t frequencyHz = 0;
  uint32_t decimation = 0;
  double centerOffsetHz = 0;
  std::vector<double> taps;
};

struct Config
{
  uint32_t inputRateHz = 0;
  uint32_t channels = 0;
  uint32_t maxInputSamples = 0;
  std::vector<LaneConfig> lanes;
};

// One entry is consumed directly as four uvec4 records by the Vulkan shader.
// outputOffset is in complex<float> elements and already includes every prior
// lane and channel. inputPrefix is the number of valid samples preceding the
// current capture in the shared raw-history upload.
struct LaneDispatch
{
  uint32_t outputOffset = 0;
  uint32_t outputSamples = 0;
  uint32_t firstRemainder = 0;
  uint32_t decimation = 0;
  uint32_t tapOffset = 0;
  uint32_t tapCount = 0;
  uint32_t inputPrefix = 0;
  uint32_t reserved = 0;
  uint32_t centerLow = 0;
  uint32_t centerHigh = 0;
  uint32_t padding0 = 0;
  uint32_t padding1 = 0;
  uint32_t baseReal = 0;
  uint32_t baseImaginary = 0;
  uint32_t padding2 = 0;
  uint32_t padding3 = 0;
};
static_assert(sizeof(LaneDispatch) == 64, "Wideband GPU lane ABI changed");

struct Dispatch
{
  uint64_t firstInputSample = 0;
  uint32_t inputSamples = 0;
  uint32_t historySamples = 0;
  uint32_t outputElements = 0;
  bool resetHistory = false;
  std::vector<LaneDispatch> lanes;
};

struct Layout
{
  uint64_t inputElements = 0;
  uint64_t outputElements = 0;
  uint64_t tapElements = 0;
  uint64_t dispatchBytes = 0;
  uint64_t totalBytes = 0;
};

// Pure geometry/state owner shared by parent and worker validation. It does no
// DSP and owns no receiver. A single instance describes one capture upload and
// every illuminator lane in the worker.
class Plan
{
public:
  explicit Plan(Config config);

  const Config& config() const { return config_; }
  const Layout& layout() const { return layout_; }
  const std::vector<double>& packedTaps() const { return packedTaps_; }
  uint32_t historySamples() const { return historySamples_; }

  // Automatic selection is restricted to physically-qualified geometries.
  // The Vulkan module performs the final device/bin-compatibility check.
  bool runtimeSelectable() const noexcept;
  const char* runtimeBlockReason() const noexcept;

  Dispatch prepare(uint64_t firstInputSample, uint32_t inputSamples);
  void reset();

  // Validation-only GLSL 450 direct oracle. Bindings are: packed raw
  // input/history vec2, packed FP64 taps, LaneDispatch words, and packed vec2
  // outputs. This is intentionally not exposed through a worker factory.
  static const char* qualificationShaderSource();

private:
  Config config_;
  Layout layout_;
  std::vector<double> packedTaps_;
  std::array<uint32_t, maxLanes> tapOffsets_{};
  uint32_t historySamples_ = 0;
  bool started_ = false;
  uint64_t nextInputSample_ = 0;
};

struct LaneOutput
{
  uint64_t frequencyHz = 0;
  uint64_t firstInputSample = 0;
  bool hasFirstInputSample = false;
  uint32_t inputSamplesPerOutput = 0;
  std::vector<std::vector<std::complex<float>>> channels;
};

class Backend
{
public:
  struct Timing{double stagingMs=0,workerMs=0;};
  struct InputView{std::vector<const std::complex<float>*> channels;std::size_t samples=0;};
  virtual ~Backend() = default;
  virtual std::vector<LaneOutput> process(
    const std::vector<std::vector<std::complex<float>>>& input,
    uint64_t firstInputSample) = 0;
  virtual std::vector<LaneOutput> processView(const InputView& input,
    uint64_t firstInputSample);
  virtual std::vector<LaneOutput> processDeques(
    const std::vector<const std::deque<std::complex<double>>*>& input,
    uint64_t firstInputSample);
  // Isolated worker transports may split submission from completion so the
  // caller can process the prior CPI while exactly one GPU frame is running.
  virtual bool supportsSplitFrame() const noexcept { return false; }
  virtual void begin(
    const std::vector<std::vector<std::complex<float>>>&,
    uint64_t) { throw std::logic_error("Split wideband frame unsupported"); }
  virtual void beginDeques(
    const std::vector<const std::deque<std::complex<double>>*>&,
    uint64_t) { throw std::logic_error("Split wideband frame unsupported"); }
  virtual std::vector<LaneOutput> finishFrame()
    { throw std::logic_error("Split wideband frame unsupported"); }
  virtual bool frameInFlight() const noexcept { return false; }
  virtual void reset() = 0;
  virtual std::string deviceName() const = 0;
  virtual Timing lastTiming() const noexcept { return {}; }
};

constexpr unsigned moduleAbi = 1;
using Create = Backend* (*)(unsigned, const Config*, const char*);
std::vector<double> designFilter(uint32_t inputRateHz,uint32_t outputRateHz,
  uint32_t occupiedBandwidthHz,uint32_t tapsPerPhase);

} // namespace blah2::wideband_gpu
