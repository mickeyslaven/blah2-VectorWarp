#pragma once
#include "process/integration/CoherentFamilySelector.h"
#include "process/integration/PassiveIsarAperture.h"

namespace blah2::integration::coherent_replay {
// One bounded offline replay transaction: bootstrap followed by eight exact
// shared CPIs. Selection never controls retention, rate, delay grid or aperture.
// Validated native ingress is reusable by authenticated RF; synthetic ingress
// must identify itself as such in its outer receipt, never claim RF validation.
class CoherentPassiveIsarReplay {
public:
  using Selector = CoherentFamilySelector;
  explicit CoherentPassiveIsarReplay(const Selector::Contract& contract)
    : contract_(contract), reader_(request(contract)) {}
  void append(const ValidatedSnapshot& input) {
    using resident::wire::require;
    require(count_ < 9 && input.sidecar.endSample > input.sidecar.firstSample && input.sidecar.endSample - input.sidecar.firstSample == contract_.cpiSamples,
      "Paired replay requires one bootstrap and exactly eight fixed CPIs");
    if (!count_) bootstrap_ = input.sidecar;
    else require(input.sidecar.firstSample == previous_.firstSample + contract_.hopSamples &&
      input.sidecar.identity.logicalFrame == previous_.identity.logicalFrame + 1,
      "Paired replay raw window/hop discontinuity");
    if (contract_.experiment.retainApertureProducts)
      reader_.setApertureExecutionAllowance(contract_.experiment.apertureRetentionMaximumBytes - bytes_,
        reader_.remainingApertureWork());
    results_ = reader_.appendAll(input);
    if (contract_.experiment.retainApertureProducts) {
      const auto product = reader_.apertureProduct();
      require(bool(product) == (count_ != 0), "Paired replay bootstrap/product alignment differs");
      if (product) {
        auto frame = reader_.aperture(*product, reader_.remainingApertureWork(),
          contract_.experiment.apertureRetentionMaximumBytes - bytes_);
        require(frame->hopSamples == contract_.hopSamples && frame->producerSeal == product->seal,
          "Paired replay product lineage differs");
        bytes_ += frame->retainedBytes; frames_.push_back(std::move(frame));
      }
    }
    previous_ = input.sidecar; ++count_;
  }
  Selector::Observation readout(const Selector& selector) const {
    resident::wire::require(count_ == 9 && results_.size() == 3, "Paired replay terminal window incomplete");
    const std::array<const DirectPathCoherentExperiment::Result*, 3> inputs{
      &results_[0].result, &results_[1].result, &results_[2].result};
    return selector.readout(inputs, window(), selector.contractSha256());
  }
  Selector::Window window() const {
    resident::wire::require(count_ == 9, "Paired replay terminal window incomplete");
    return {bootstrap_.captureEpoch, bootstrap_.firstSample + contract_.hopSamples, previous_.endSample};
  }
  const Sidecar& bootstrap() const { return bootstrap_; }
  uint64_t remainingWork() const { return reader_.remainingApertureWork(); }
  const std::vector<ExperimentReader::RateResult>& results() const { return results_; }
  const std::vector<std::shared_ptr<const ExperimentReader::ApertureFrame>>& frames() const { return frames_; }
  PassiveIsarAperture aperture(uint32_t depth) const {
    resident::wire::require(count_ == 9 && frames_.size() == 8 && (depth == 2 || depth == 4 || depth == 8),
      "Paired replay aperture unavailable or depth not frozen");
    PassiveIsarAperture result;
    for (size_t i = frames_.size() - depth; i < frames_.size(); ++i) result.append(frames_[i]);
    return result;
  }
private:
  static DirectPathCoherentExperiment::Config request(const Selector::Contract& contract) {
    auto config = contract.experiment; config.migrationRates.assign(contract.rates.begin(), contract.rates.end());
    return config;
  }
  Selector::Contract contract_;
  ExperimentReader reader_;
  uint32_t count_ = 0;
  uint64_t bytes_ = 0;
  Sidecar bootstrap_, previous_;
  std::vector<ExperimentReader::RateResult> results_;
  std::vector<std::shared_ptr<const ExperimentReader::ApertureFrame>> frames_;
};
} // namespace blah2::integration::coherent_replay
