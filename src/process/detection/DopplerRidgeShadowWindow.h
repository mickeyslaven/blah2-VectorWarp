#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

// Observation-only comparisons run on the first N consecutive accepted CPIs
// of an input epoch, never a sampled/decimated tracker history. Completion is
// sticky until explicit reset, discontinuity or rejected-input reset; it does
// not change the configured mode or the production detector/tracker.
class DopplerRidgeShadowWindow {
public:
  explicit DopplerRidgeShadowWindow(uint32_t maximumCpis) : maximum_(maximumCpis) {
    if (!maximum_ || maximum_ > 65535)
      throw std::invalid_argument("Ridge shadow window must contain 1 to 65535 CPIs");
  }
  bool available() const { return observed_ < maximum_; }
  void observe(double milliseconds) {
    if (!available()) throw std::logic_error("Ridge shadow window is complete");
    ++observed_; totalMs_ += milliseconds;
  }
  void reset(std::string reason) {
    if (++epoch_ == 0) epoch_ = 1;
    observed_ = 0; totalMs_ = 0; resetReason_ = std::move(reason);
  }
  uint64_t epoch() const { return epoch_; }
  uint32_t observed() const { return observed_; }
  uint32_t maximum() const { return maximum_; }
  double totalMs() const { return totalMs_; }
  const std::string& resetReason() const { return resetReason_; }
private:
  uint32_t maximum_, observed_ = 0;
  uint64_t epoch_ = 1;
  double totalMs_ = 0;
  std::string resetReason_ = "initial_epoch";
};
