#include "WienerHopf.h"
#include "process/meta/FftLength.h"
#include <complex>
#include <iostream>
#include <stdexcept>

namespace {
struct Geometry {
  uint32_t bins, filter;
};
Geometry geometry(int32_t delayMin, int32_t delayMax, uint32_t samples) {
  const int64_t taps = int64_t(delayMax) - delayMin;
  if (!samples || taps <= 0 || uint64_t(taps) > samples)
    throw std::invalid_argument("Clutter filter needs a non-empty half-open delay range no longer than the CPI");
  return {static_cast<uint32_t>(taps),
    blah2::nextFastFftLength(uint64_t(samples) + taps + 1)};
}
fftw_complex* fft_data(std::vector<std::complex<double>>& data) {
  return reinterpret_cast<fftw_complex*>(data.data());
}
}

WienerHopf::PreparedReference::PreparedReference(int32_t minimum, int32_t maximum,
                                                 uint32_t samples)
  : delayMin(minimum), nBins(geometry(minimum, maximum, samples).bins),
    nSamples(samples), nFilter(geometry(minimum, maximum, samples).filter),
    rotated(samples), spectrum(samples), paddedSpectrum(nFilter),
    cholesky(nBins, nBins), correlation(nBins)
{
  fftReference = fftw_plan_dft_1d(nSamples, fft_data(rotated), fft_data(spectrum),
                                  FFTW_FORWARD, FFTW_ESTIMATE);
  fftCorrelation = fftw_plan_dft_1d(nSamples, fft_data(rotated), fft_data(rotated),
                                    FFTW_BACKWARD, FFTW_ESTIMATE);
  fftPadded = fftw_plan_dft_1d(nFilter, fft_data(paddedSpectrum),
                                fft_data(paddedSpectrum), FFTW_FORWARD, FFTW_ESTIMATE);
  if (!fftReference || !fftCorrelation || !fftPadded) {
    if (fftReference) fftw_destroy_plan(fftReference);
    if (fftCorrelation) fftw_destroy_plan(fftCorrelation);
    if (fftPadded) fftw_destroy_plan(fftPadded);
    throw std::runtime_error("Could not plan clutter reference FFT");
  }
}

WienerHopf::PreparedReference::~PreparedReference()
{
  fftw_destroy_plan(fftReference);
  fftw_destroy_plan(fftCorrelation);
  fftw_destroy_plan(fftPadded);
}

bool WienerHopf::PreparedReference::prepare(const IqData& reference)
{
  valid = false;
  const auto& data = reference.view_data();
  if (data.size() < nSamples)
    throw std::invalid_argument("Clutter reference is shorter than CPI");
  for (uint32_t i = 0; i < nSamples; ++i) {
    const int64_t shifted = (int64_t(i) - delayMin) % int64_t(nSamples);
    rotated[i] = data[shifted < 0 ? shifted + nSamples : shifted];
    paddedSpectrum[i] = rotated[i];
  }
  for (uint32_t i = nSamples; i < nFilter; ++i) paddedSpectrum[i] = {};
  fftw_execute(fftPadded);
  fftw_execute(fftReference);
  for (uint32_t i = 0; i < nSamples; ++i)
    rotated[i] = spectrum[i] * std::conj(spectrum[i]);
  fftw_execute(fftCorrelation);
  for (uint32_t i = 0; i < nBins; ++i)
    correlation[i] = std::conj(rotated[i]) / double(nSamples);
  cholesky = arma::toeplitz(correlation);
  // Armadillo's Toeplitz constructor does not make the lower half Hermitian.
  for (uint32_t i = 0; i < nBins; ++i)
    for (uint32_t j = 0; j < i; ++j)
      cholesky(i, j) = std::conj(cholesky(i, j));
  valid = arma::chol(cholesky, cholesky);
  if (!valid) std::cerr << "Chol decomposition failed, skip clutter filter" << std::endl;
  return valid;
}

WienerHopf::WienerHopf(int32_t minimum, int32_t maximum, uint32_t samples)
  : delayMin(minimum), nBins(geometry(minimum, maximum, samples).bins),
    nSamples(samples), nFilter(geometry(minimum, maximum, samples).filter),
    scratch(nFilter), correlation(nBins), weights(nBins)
{
  fftSurveillance = fftw_plan_dft_1d(nSamples, fft_data(scratch), fft_data(scratch),
                                      FFTW_FORWARD, FFTW_ESTIMATE);
  ifftCorrelation = fftw_plan_dft_1d(nSamples, fft_data(scratch), fft_data(scratch),
                                      FFTW_BACKWARD, FFTW_ESTIMATE);
  fftWeights = fftw_plan_dft_1d(nFilter, fft_data(scratch), fft_data(scratch),
                                 FFTW_FORWARD, FFTW_ESTIMATE);
  ifftFiltered = fftw_plan_dft_1d(nFilter, fft_data(scratch), fft_data(scratch),
                                   FFTW_BACKWARD, FFTW_ESTIMATE);
  if (!fftSurveillance || !ifftCorrelation || !fftWeights || !ifftFiltered) {
    if (fftSurveillance) fftw_destroy_plan(fftSurveillance);
    if (ifftCorrelation) fftw_destroy_plan(ifftCorrelation);
    if (fftWeights) fftw_destroy_plan(fftWeights);
    if (ifftFiltered) fftw_destroy_plan(ifftFiltered);
    throw std::runtime_error("Could not plan clutter surveillance FFT");
  }
}

WienerHopf::~WienerHopf()
{
  fftw_destroy_plan(fftSurveillance);
  fftw_destroy_plan(ifftCorrelation);
  fftw_destroy_plan(fftWeights);
  fftw_destroy_plan(ifftFiltered);
}

bool WienerHopf::process(const PreparedReference& reference, IqData *surveillance)
{
  if (!surveillance || surveillance->view_data().size() < nSamples ||
      reference.delayMin != delayMin || reference.nBins != nBins ||
      reference.nSamples != nSamples || reference.nFilter != nFilter)
    throw std::invalid_argument("Clutter surveillance or prepared reference geometry does not match");
  if (!reference.valid) return false;
  const auto& data = surveillance->view_data();
  for (uint32_t i = 0; i < nSamples; ++i) scratch[i] = data[i];
  fftw_execute(fftSurveillance);
  for (uint32_t i = 0; i < nSamples; ++i)
    scratch[i] *= std::conj(reference.spectrum[i]);
  fftw_execute(ifftCorrelation);
  for (uint32_t i = 0; i < nBins; ++i)
    correlation[i] = scratch[i] / double(nSamples);
  const bool success = arma::solve(weights, arma::trimatu(reference.cholesky),
    arma::solve(arma::trimatl(arma::trans(reference.cholesky)), correlation));
  if (!success) {
    std::cerr << "Solve failed, skip clutter filter" << std::endl;
    return false;
  }
  for (uint32_t i = 0; i < nBins; ++i) scratch[i] = weights[i];
  for (uint32_t i = nBins; i < nFilter; ++i) scratch[i] = {};
  fftw_execute(fftWeights);
  for (uint32_t i = 0; i < nFilter; ++i)
    scratch[i] *= reference.paddedSpectrum[i];
  fftw_execute(ifftFiltered);
  surveillance->subtract_clutter(scratch.data(), nSamples, nFilter);
  return true;
}

bool WienerHopf::process(IqData *reference, IqData *surveillance)
{
  if (!reference) throw std::invalid_argument("Clutter reference is null");
  if (!ownedReference)
    ownedReference = std::make_unique<PreparedReference>(delayMin,
      int32_t(int64_t(delayMin) + nBins), nSamples);
  return ownedReference->prepare(*reference) && process(*ownedReference, surveillance);
}
