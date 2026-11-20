#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace blah2::analysis {
inline void validateTemporalExperimentCells(uint64_t rows, uint64_t columns) {
  // A full 0.5-second HDTV lane is 601 x 1261 cells.  The former 524288-cell
  // ceiling therefore rejected the exact production geometry and meant that
  // the replay experiment was never actually exercised on the misses it was meant
  // to evaluate.  This remains replay-only and the artifact has its own hard
  // byte bound; one mebicell admits full geometry without weakening live caps.
  constexpr uint64_t maximumCells = 1ULL << 20;
  if (!rows || !columns || rows > maximumCells || columns > maximumCells / rows)
    throw std::invalid_argument("Experimental replay temporal comparison is limited to 1048576 map cells");
}
inline void validateTargetProtection(bool enabled, bool /*replay*/, bool multi,
    const std::string& acceleration, bool arrayReference, bool coherentGeometry,
    uint32_t beams, uint32_t index) {
  if (!enabled) return;
  if (multi || (acceleration != "cpu" && acceleration != "gpu") ||
      !arrayReference || !coherentGeometry)
    throw std::invalid_argument("Experimental target protection requires single-lane coherent-beam-bank processing with confirmed array geometry and explicit CPU or portable GPU acceleration");
  if (beams < 4 || beams > 360 || index >= beams)
    throw std::invalid_argument("Experimental target-protection beam index must be within its configured bank");
}

inline void validateComplexCutoutRoute(bool enabled, bool replay,
    const std::string& acceleration) {
  if (enabled && (!replay || (acceleration != "cpu" && acceleration != "gpu")))
    throw std::invalid_argument("Experimental complex cutout requires replay with explicit CPU or portable GPU acceleration");
}
} // namespace blah2::analysis
