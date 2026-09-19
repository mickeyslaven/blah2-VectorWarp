#pragma once
#include <complex>
#include <cstdint>

namespace blah2::mixed {
constexpr uint32_t version = 0x4d585032; // MXP2: explicit packed signed16 frame operations
constexpr uint32_t samples = 1000000;
constexpr uint32_t rows = 301;
constexpr uint32_t delays = 411;
constexpr uint32_t usedSamples = rows * (samples / rows);
constexpr uint32_t tailSamples = samples - usedSamples;
constexpr uint64_t mapSamples = uint64_t(rows) * delays;
constexpr uint64_t inputSamples = uint64_t(samples) * 2;
constexpr uint64_t totalSamples = inputSamples + mapSamples + tailSamples;
constexpr uint64_t sharedBytes = totalSamples * sizeof(std::complex<double>);
constexpr uint64_t pairedInputBytes = uint64_t(samples) * 4 * sizeof(int16_t);
enum class Operation : uint32_t { initialize=1, ready=2, frame=3, failure=4, quit=5,
  pairedFrameReference0=6, pairedFrameReference1=7 };
struct Message {
  uint32_t protocol=version;
  Operation operation=Operation::initialize;
  uint64_t sequence=0;
  uint64_t bytes=sharedBytes;
  uint32_t sampleCount=samples,rowCount=rows,delayCount=delays;
  char reason[256]{};
};
static_assert(tailSamples == 78);
static_assert(sizeof(std::complex<double>) == 16);
static_assert(pairedInputBytes <= inputSamples * sizeof(std::complex<double>));
}
