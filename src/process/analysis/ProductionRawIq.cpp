#include "ProductionRawIq.h"

#include <algorithm>
#include <future>
#include <stdexcept>
#include <utility>

namespace blah2::production_replay {
namespace {
// Keep the live ordering, including visiting all paths after a rejection.
template <typename Work>
bool processPaths(size_t pathCount, size_t workerCount, Work work) {
  bool success = true;
  workerCount = std::max<size_t>(1, std::min(workerCount, pathCount));
  if (workerCount == 1) {
    for (size_t path = 0; path < pathCount; ++path)
      success = work(path) && success;
    return success;
  }
  for (size_t first = 0; first < pathCount; first += workerCount) {
    std::vector<std::future<bool>> tasks;
    const size_t last = std::min(first + workerCount, pathCount);
    for (size_t path = first; path < last; ++path)
      tasks.push_back(std::async(std::launch::async, work, path));
    for (auto& task : tasks) success = task.get() && success;
  }
  return success;
}

void validatePaths(const std::vector<IqData*>& paths, size_t expected) {
  if (paths.size() != expected ||
      std::any_of(paths.begin(), paths.end(), [](auto* path) { return !path; }))
    throw std::invalid_argument("Production raw-IQ surveillance paths differ");
}
}

RawIqStages::RawIqStages(Config config)
  : config_(std::move(config)), reference_(config_.reference),
    filters_(config_.paths) {
  if (!config_.samples || !config_.paths)
    throw std::invalid_argument("Production raw-IQ geometry is empty");
  // The qualified Pi path plans before capture starts. Other CPU execution,
  // including GPU qualification, lazily plans the shared reference workspace.
  if (config_.borrowed)
    for (auto& filter : filters_)
      filter = std::make_unique<WienerHopf>(config_.clutterDelayMin,
        config_.clutterDelayMax, config_.samples);
}

void RawIqStages::resetReference() {
  reference_ = ArrayReferenceSynthesizer(config_.reference);
}

bool RawIqStages::clutter(IqData& reference,
  const std::vector<IqData*>& surveillance) {
  validatePaths(surveillance, config_.paths);
  if (config_.borrowed)
    return processPaths(surveillance.size(), config_.workers, [&](size_t path) {
      return filters_[path]->process(&reference, surveillance[path], true);
    });
  if (!prepared_) {
    auto pending = std::make_unique<WienerHopf::PreparedReference>(
      config_.clutterDelayMin, config_.clutterDelayMax, config_.samples);
    std::vector<std::unique_ptr<WienerHopf>> filters;
    filters.reserve(filters_.size());
    for (size_t path = 0; path < filters_.size(); ++path)
      filters.push_back(std::make_unique<WienerHopf>(config_.clutterDelayMin,
        config_.clutterDelayMax, config_.samples, WienerHopf::Workspace::SharedReference));
    // Publish only a complete set: failed qualification must be retryable.
    filters_ = std::move(filters);
    prepared_ = std::move(pending);
  }
  if (!prepared_->prepare(reference)) return false;
  return processPaths(surveillance.size(), config_.workers, [&](size_t path) {
    return filters_[path]->process(*prepared_, surveillance[path]);
  });
}

bool RawIqStages::fallbackClutter(IqData& reference, IqData& surveillance) {
  if (filters_.empty() || !filters_.front())
    throw std::logic_error("Production raw-IQ mixed fallback is unavailable");
  return filters_.front()->process(&reference, &surveillance, config_.borrowed);
}

void RawIqStages::ambiguity(const IqData& reference,
  const std::vector<IqData*>& surveillance,
  const std::vector<Ambiguity*>& processors, bool preserveSurveillance) {
  validatePaths(surveillance, config_.paths);
  if (processors.size() != config_.paths ||
      std::any_of(processors.begin(), processors.end(), [](auto* path) { return !path; }))
    throw std::invalid_argument("Production raw-IQ ambiguity paths differ");
  processPaths(surveillance.size(), config_.workers, [&](size_t path) {
    if (config_.borrowed) {
      const auto view = filters_[path]->filtered_view();
      processors[path]->process_borrowed(reference.view_data(), surveillance[path],
        view.rotatedReference, view.filteredSurveillance, view.samples,
        view.delayMin, preserveSurveillance);
    } else {
      processors[path]->process(reference.view_data(), surveillance[path],
        preserveSurveillance);
    }
    return true;
  });
}
}
