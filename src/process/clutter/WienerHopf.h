/// Wiener-Hopf clutter cancellation with reusable full/block FFT workspaces.
#pragma once

#include "data/IqData.h"
#include <armadillo>
#include <complex>
#include <cstdint>
#include <fftw3.h>
#include <memory>
#include <vector>

class WienerHopf {
  struct Impl;
  std::unique_ptr<Impl> impl_;
public:
  // An immutable, rotated reference CPI shared by concurrent surveillance
  // paths. Filtering still uses each path's private FFT workspaces.
  class PreparedReference {
    friend class WienerHopf;
    int32_t delayMin_;
    uint32_t taps_, samples_, filterLength_;
    std::vector<std::complex<double>> rotated_, spectrum_, paddedSpectrum_;
    arma::cx_mat cholesky_;
    arma::cx_vec correlation_;
    fftw_plan referencePlan_ = nullptr;
    fftw_plan correlationPlan_ = nullptr;
    fftw_plan paddedPlan_ = nullptr;
    bool valid_ = false;
  public:
    PreparedReference(int32_t delayMin, int32_t delayMax, uint32_t nSamples);
    ~PreparedReference();
    PreparedReference(const PreparedReference&) = delete;
    PreparedReference& operator=(const PreparedReference&) = delete;
    bool prepare(const IqData& reference);
  };

  struct FilteredView {
    const std::complex<double>* rotatedReference;
    const std::complex<double>* filteredSurveillance;
    uint32_t samples;
    int32_t delayMin;
    uint64_t generation;
  };

  enum class Workspace { Pi, SharedReference };
  WienerHopf(int32_t delayMin, int32_t delayMax, uint32_t nSamples,
             Workspace workspace = Workspace::Pi);
  ~WienerHopf();
  WienerHopf(const WienerHopf&) = delete;
  WienerHopf& operator=(const WienerHopf&) = delete;
  uint32_t filter_fft_length() const;
  bool process(IqData* reference, IqData* surveillance, bool borrowOutput = false);
  bool process(const PreparedReference& reference, IqData* surveillance);
  FilteredView filtered_view() const;
};
