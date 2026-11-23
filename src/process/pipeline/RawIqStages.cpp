#include "RawIqStages.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace blah2::pipeline {
namespace {
// Keep the live ordering, including visiting all paths after a rejection.
template <typename Work>
bool processPaths(size_t pathCount, blah2::utility::FixedExecutor& executor,
  Work work) {
  if (executor.size() == 1 || pathCount == 1) {
    bool success = true;
    for (size_t path = 0; path < pathCount; ++path)
      success = work(path) && success;
    return success;
  }
  std::vector<uint8_t> accepted(pathCount);
  executor.parallel_for(pathCount, [&](size_t path) {
    accepted[path] = work(path) ? 1 : 0;
  });
  const bool success = std::all_of(accepted.begin(), accepted.end(),
    [](uint8_t value) { return value != 0; });
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
    filters_(config_.paths), executor_(std::make_unique<
      blah2::utility::FixedExecutor>(std::max<size_t>(1,
        std::max(std::min(config_.workers, config_.paths), config_.temporalWorkers)))) {
  if (!config_.samples || !config_.paths)
    throw std::invalid_argument("Production raw-IQ geometry is empty");
  if (config_.clutterWorkerSlots > 8)
    throw std::invalid_argument("Production clutter worker slots exceed the supported maximum");
  // Borrowed workspaces plan before capture starts. Other CPU execution,
  // including GPU qualification, lazily plans the shared reference workspace.
  if (config_.borrowed)
    for (auto& filter : filters_)
      filter = std::make_unique<WienerHopf>(config_.clutterDelayMin,
        config_.clutterDelayMax, config_.samples, WienerHopf::Workspace::Pi,
        0, uint32_t(config_.clutterWorkerSlots));
  else if (config_.eagerPlanning)
    initializeShared();
}

void RawIqStages::initializeShared() {
  auto pending = std::make_unique<WienerHopf::PreparedReference>(
    config_.clutterDelayMin, config_.clutterDelayMax, config_.samples);
  std::vector<std::unique_ptr<WienerHopf>> filters;
  filters.reserve(filters_.size());
  for (size_t path = 0; path < filters_.size(); ++path)
    filters.push_back(std::make_unique<WienerHopf>(config_.clutterDelayMin,
      config_.clutterDelayMax, config_.samples,
      WienerHopf::Workspace::SharedReference));
  filters_ = std::move(filters);
  prepared_ = std::move(pending);
}

void RawIqStages::resetReference() {
  reference_ = ArrayReferenceSynthesizer(config_.reference);
  prepared_.reset();
  for (auto& filter : filters_) filter.reset();
  if (config_.borrowed)
    for (auto& filter : filters_)
      filter = std::make_unique<WienerHopf>(config_.clutterDelayMin,
        config_.clutterDelayMax, config_.samples, WienerHopf::Workspace::Pi,
        0, uint32_t(config_.clutterWorkerSlots));
  else if (config_.eagerPlanning)
    initializeShared();
}

void RawIqStages::setBorrowed(bool borrowed) {
  if (borrowed == config_.borrowed) return;
  std::unique_ptr<WienerHopf::PreparedReference> prepared;
  std::vector<std::unique_ptr<WienerHopf>> filters(config_.paths);
  if (borrowed) {
    for (auto& filter : filters)
      filter = std::make_unique<WienerHopf>(config_.clutterDelayMin,
        config_.clutterDelayMax, config_.samples, WienerHopf::Workspace::Pi,
        0, uint32_t(config_.clutterWorkerSlots));
  } else if (config_.eagerPlanning) {
    prepared = std::make_unique<WienerHopf::PreparedReference>(
      config_.clutterDelayMin, config_.clutterDelayMax, config_.samples);
    for (auto& filter : filters)
      filter = std::make_unique<WienerHopf>(config_.clutterDelayMin,
        config_.clutterDelayMax, config_.samples,
        WienerHopf::Workspace::SharedReference);
  }
  // Publish only complete workspaces; allocation/planning failure leaves the
  // prior mode and reference learner intact.
  filters_ = std::move(filters);
  prepared_ = std::move(prepared);
  config_.borrowed = borrowed;
}

bool RawIqStages::clutter(IqData& reference,
  const std::vector<IqData*>& surveillance) {
  validatePaths(surveillance, config_.paths);
  if (config_.borrowed)
    return processPaths(surveillance.size(), *executor_, [&](size_t path) {
      return filters_[path]->process(&reference, surveillance[path], true);
    });
  if (!prepared_) {
    // Publish only a complete set: failed qualification must be retryable.
    initializeShared();
  }
  if (!prepared_->prepare(reference)) return false;
  return processPaths(surveillance.size(), *executor_, [&](size_t path) {
    return filters_[path]->process(*prepared_, surveillance[path]);
  });
}

bool RawIqStages::clutter_float(const std::complex<float>* reference,
  const std::vector<const std::complex<float>*>& surveillance,
  uint32_t samples) {
  if (!config_.borrowed || !reference || samples < config_.samples ||
      surveillance.size() != config_.paths ||
      std::any_of(surveillance.begin(), surveillance.end(),
        [](const auto* path) { return path == nullptr; }))
    throw std::invalid_argument("Direct float clutter input differs");
  return processPaths(surveillance.size(), *executor_, [&](size_t path) {
    return filters_[path]->process_float(reference, surveillance[path],
      samples);
  });
}

void RawIqStages::ambiguity(const IqData& reference,
  const std::vector<IqData*>& surveillance,
  const std::vector<Ambiguity*>& processors, bool preserveSurveillance) {
  validatePaths(surveillance, config_.paths);
  if (processors.size() != config_.paths ||
      std::any_of(processors.begin(), processors.end(), [](auto* path) { return !path; }))
    throw std::invalid_argument("Production raw-IQ ambiguity paths differ");
  processPaths(surveillance.size(), *executor_, [&](size_t path) {
    if (config_.borrowed) {
      const auto view = filters_[path]->filtered_view();
      processors[path]->process_borrowed(reference.sample_view(), surveillance[path],
        view.rotatedReference, view.filteredSurveillance, view.samples,
        view.delayMin, preserveSurveillance);
    } else {
      processors[path]->process(reference.sample_view(), surveillance[path],
        preserveSurveillance);
    }
    return true;
  });
}

void RawIqStages::ambiguity_borrowed(
  const std::vector<Ambiguity*>& processors) {
  if (!config_.borrowed || processors.size() != config_.paths ||
      std::any_of(processors.begin(), processors.end(),
        [](auto* path) { return path == nullptr; }))
    throw std::invalid_argument("Direct borrowed ambiguity paths differ");
  static const std::deque<std::complex<double>> emptyReference;
  processPaths(processors.size(), *executor_, [&](size_t path) {
    const auto view = filters_[path]->filtered_view();
    processors[path]->process_borrowed(emptyReference, nullptr,
      view.rotatedReference, view.filteredSurveillance, view.samples,
      view.delayMin, true);
    return true;
  });
}

void RawIqStages::ambiguity_borrowed_reference(const IqData& reference,
  const std::vector<Ambiguity*>& processors) {
  if (!config_.borrowed || processors.size() != config_.paths ||
      std::any_of(processors.begin(), processors.end(),
        [](auto* path) { return path == nullptr; }) ||
      reference.sample_view().size() < config_.samples)
    throw std::invalid_argument("Qualified borrowed ambiguity reference differs");
  processPaths(processors.size(), *executor_, [&](size_t path) {
    const auto view = filters_[path]->filtered_view();
    processors[path]->process_borrowed(reference.sample_view(), nullptr, nullptr,
      view.filteredSurveillance, view.samples, view.delayMin, true);
    return true;
  });
}

void RawIqStages::ambiguity_borrowed_reference(
  const std::complex<double>* reference, uint32_t samples,
  const std::vector<Ambiguity*>& processors) {
  if (!config_.borrowed || !reference || samples < config_.samples ||
      processors.size() != config_.paths ||
      std::any_of(processors.begin(), processors.end(),
        [](auto* path) { return path == nullptr; }))
    throw std::invalid_argument("Contiguous borrowed ambiguity reference differs");
  static const std::deque<std::complex<double>> emptyReference;
  processPaths(processors.size(), *executor_, [&](size_t path) {
    const auto view = filters_[path]->filtered_view();
    processors[path]->process_borrowed(emptyReference, nullptr, reference,
      view.filteredSurveillance, samples, 0, true);
    return true;
  });
}

std::vector<WienerHopf::FilteredView> RawIqStages::borrowed_views() const {
  if (!config_.borrowed)
    throw std::logic_error("Borrowed clutter views are unavailable");
  std::vector<WienerHopf::FilteredView> result;
  result.reserve(filters_.size());
  for (const auto& filter : filters_) result.push_back(filter->filtered_view());
  return result;
}
}
