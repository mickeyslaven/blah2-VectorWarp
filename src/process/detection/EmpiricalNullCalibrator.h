#ifndef EMPIRICAL_NULL_CALIBRATOR_H
#define EMPIRICAL_NULL_CALIBRATOR_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace blah2::detection
{

// Distribution-free, upper-tail calibration for a single immutable detector
// stratum.  Callers must keep statistic family, beam policy and temporal depth
// in the stratum key: unlike a Gamma-CFAR approximation, those tails are not
// assumed interchangeable.  The plus-one p-value is conservative for an
// exchangeable held-out null sample and cannot claim resolution unsupported by
// the number of negative observations.
class EmpiricalNullCalibrator
{
public:
  explicit EmpiricalNullCalibrator(std::size_t maximumPerStratum = 1000000,
    std::size_t minimumPerStratum = 32)
    : maximumPerStratum_(maximumPerStratum),
      minimumPerStratum_(minimumPerStratum)
  {
    if (!maximumPerStratum_ || !minimumPerStratum_ ||
        minimumPerStratum_ > maximumPerStratum_)
      throw std::invalid_argument("Empirical null calibration bounds invalid");
  }

  void add(const std::string& stratum, double score)
  {
    if (frozen_) throw std::logic_error("Empirical null calibration is frozen");
    if (stratum.empty() || !std::isfinite(score))
      throw std::invalid_argument("Empirical null calibration sample invalid");
    auto& samples = samples_[stratum];
    if (samples.size() >= maximumPerStratum_)
      throw std::length_error("Empirical null calibration stratum is full");
    samples.push_back(score);
  }

  void freeze()
  {
    if (samples_.empty())
      throw std::logic_error("Empirical null calibration has no strata");
    for (auto& [_, samples] : samples_)
    {
      if (samples.size() < minimumPerStratum_)
        throw std::logic_error("Empirical null calibration stratum is too small");
      std::sort(samples.begin(), samples.end());
    }
    frozen_ = true;
  }

  double upperTailP(const std::string& stratum, double score) const
  {
    if (!frozen_ || !std::isfinite(score))
      throw std::logic_error("Empirical null calibration is unavailable");
    const auto found = samples_.find(stratum);
    if (found == samples_.end())
      throw std::out_of_range("Empirical null calibration stratum missing");
    const auto& samples = found->second;
    const auto first = std::lower_bound(samples.begin(), samples.end(), score);
    const auto exceedances = std::size_t(samples.end() - first);
    return double(exceedances + 1) / double(samples.size() + 1);
  }

  double minimumResolvableP(const std::string& stratum) const
  {
    const auto found = samples_.find(stratum);
    if (!frozen_ || found == samples_.end())
      throw std::logic_error("Empirical null calibration is unavailable");
    return 1.0 / double(found->second.size() + 1);
  }

  bool frozen() const { return frozen_; }

private:
  std::size_t maximumPerStratum_;
  std::size_t minimumPerStratum_;
  bool frozen_ = false;
  std::unordered_map<std::string, std::vector<double>> samples_;
};

} // namespace blah2::detection

#endif
