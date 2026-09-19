/// Wiener-Hopf clutter cancellation with reusable full/block FFT workspaces.
#pragma once
#include "data/IqData.h"
#include <complex>
#include <cstdint>
#include <memory>

class WienerHopf {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  struct FilteredView {
    const std::complex<double>* rotatedReference;
    const std::complex<double>* filteredSurveillance;
    uint32_t samples;
    int32_t delayMin;
    uint64_t generation;
  };
  WienerHopf(int32_t delayMin, int32_t delayMax, uint32_t nSamples);
  ~WienerHopf();
  WienerHopf(const WienerHopf&) = delete;
  WienerHopf& operator=(const WienerHopf&) = delete;
  uint32_t filter_fft_length() const;
  bool process(IqData* reference, IqData* surveillance, bool borrowOutput = false);
  FilteredView filtered_view() const;
};
