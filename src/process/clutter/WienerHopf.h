/// @file WienerHopf.h
/// @brief Wiener-Hopf clutter filter with a reusable, shared CPI reference.
#ifndef WIENERHOPF_H
#define WIENERHOPF_H

#include "data/IqData.h"
#include <armadillo>
#include <fftw3.h>
#include <complex>
#include <cstdint>
#include <memory>
#include <vector>

class WienerHopf
{
public:
  class PreparedReference
  {
    friend class WienerHopf;
    int32_t delayMin;
    uint32_t nBins, nSamples, nFilter;
    std::vector<std::complex<double>> rotated, spectrum, paddedSpectrum;
    arma::cx_mat cholesky;
    arma::cx_vec correlation;
    fftw_plan fftReference = nullptr, fftCorrelation = nullptr, fftPadded = nullptr;
    bool valid = false;
  public:
    PreparedReference(int32_t delayMin, int32_t delayMax, uint32_t nSamples);
    ~PreparedReference();
    PreparedReference(const PreparedReference&) = delete;
    PreparedReference& operator=(const PreparedReference&) = delete;
    bool prepare(const IqData& reference);
  };

  WienerHopf(int32_t delayMin, int32_t delayMax, uint32_t nSamples);
  ~WienerHopf();
  WienerHopf(const WienerHopf&) = delete;
  WienerHopf& operator=(const WienerHopf&) = delete;

  uint32_t filter_fft_length() const { return nFilter; }
  // A shared reference is prepared once before parallel surveillance paths.
  bool process(const PreparedReference& reference, IqData *surveillance);
  // Compatibility path for independent users of one filter.
  bool process(IqData *reference, IqData *surveillance);

private:
  int32_t delayMin;
  uint32_t nBins, nSamples, nFilter;
  std::vector<std::complex<double>> scratch;
  arma::cx_vec correlation, weights;
  fftw_plan fftSurveillance = nullptr, ifftCorrelation = nullptr;
  fftw_plan fftWeights = nullptr, ifftFiltered = nullptr;
  std::unique_ptr<PreparedReference> ownedReference;
};

#endif
