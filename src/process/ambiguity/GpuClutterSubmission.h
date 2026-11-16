#pragma once

#include <exception>
#include <stdexcept>

namespace blah2 {

// One shared queue fence, never more than one outstanding clutter submission.
// Vulkan-independent so exception/fence sequencing is tested without a driver.
class ClutterSubmission {
  bool pending_ = false, poisoned_ = false;
public:
  ClutterSubmission() = default;
  ClutterSubmission(const ClutterSubmission&) = delete;
  ClutterSubmission& operator=(const ClutterSubmission&) = delete;
  void ensureIdle() const {
    if (pending_ || poisoned_)
      throw std::logic_error("GPU clutter submission is pending or poisoned");
  }
  // A failed enqueue can leave device state uncertain. Teardown must drain
  // even if the API never reported a successful submission in that case.
  bool needsDrain() const noexcept { return pending_ || poisoned_; }
  template<class Enqueue> void enqueue(Enqueue&& enqueueOperation) {
    ensureIdle();
    try { enqueueOperation(); pending_ = true; }
    catch (...) { poisoned_ = true; throw; }
  }
  template<class Wait, class Reset> void complete(Wait&& wait, Reset&& reset) {
    if (!pending_ || poisoned_)
      throw std::logic_error("GPU clutter completion has no healthy submission");
    try {
      wait();             // Never reset a fence while its command is pending.
      reset();
      pending_ = false;   // Reset failure must not advertise a reusable fence.
    } catch (...) { poisoned_ = true; throw; }
  }
  template<class Host, class Complete> void overlap(Host&& host, Complete&& completeOperation) {
    if (!pending_ || poisoned_)
      throw std::logic_error("GPU clutter overlap has no healthy submission");
    std::exception_ptr hostFailure;
    try { host(); } catch (...) { hostFailure = std::current_exception(); }
    // Drain/reset even after allocation, numerical, or non-standard host
    // exceptions. A device completion fault takes precedence: it must not be
    // downgraded to a recoverable numerical rejection with an unsafe fence.
    completeOperation();
    ensureIdle();
    if (hostFailure) std::rethrow_exception(hostFailure);
  }
};
} // namespace blah2
