#include "Ambiguity.h"
#include <complex>
#include <iostream>
#include <deque>
#include <vector>
#include <numeric>
#include <math.h>
#include <chrono>
#include <stdexcept>
#include <limits>

// constructor
Ambiguity::Ambiguity(int32_t _delayMin, int32_t _delayMax, 
  int32_t _dopplerMin, int32_t _dopplerMax, uint32_t _fs, 
  uint32_t _n, bool _roundHamming)
{
  const int64_t delayBins = static_cast<int64_t>(_delayMax) - _delayMin + 1;
  if (!_fs || !_n || _dopplerMin > _dopplerMax || delayBins < 1 ||
      delayBins > std::numeric_limits<uint16_t>::max())
    throw std::invalid_argument("Invalid delay-Doppler geometry");
  // init
  delayMin = _delayMin;
  delayMax = _delayMax;
  dopplerMin = _dopplerMin;
  dopplerMax = _dopplerMax;
  fs = _fs;
  nSamples = _n;
  nDelayBins = static_cast<uint16_t>(delayBins);
  dopplerMiddle = (static_cast<int64_t>(_dopplerMin) + _dopplerMax) / 2.0;
  
  // doppler calculations
  std::deque<double> doppler;
  double resolutionDoppler = 1.0 / (static_cast<double>(_n) / static_cast<double>(_fs));
  doppler.push_back(dopplerMiddle);
  int i = 1;
  while (dopplerMiddle + (i * resolutionDoppler) <= dopplerMax)
  {
    if (doppler.size() + 2 > std::numeric_limits<uint16_t>::max())
      throw std::invalid_argument("Doppler bin count exceeds the processor limit");
    doppler.push_back(dopplerMiddle + (i * resolutionDoppler));
    doppler.push_front(dopplerMiddle - (i * resolutionDoppler));
    i++;
  }
  nDopplerBins = doppler.size();

  // batches constants
  const uint32_t correlationSamples = _n / nDopplerBins;
  if (!correlationSamples || correlationSamples > std::numeric_limits<uint16_t>::max())
    throw std::invalid_argument("Correlation length exceeds the processor limit");
  // A zero-padded block correlation represents only -(N-1)..+(N-1).
  // A lag can fit in the FFT allocation yet alias the opposite signed lag.
  if (static_cast<int64_t>(delayMin) <= -static_cast<int64_t>(correlationSamples) ||
      static_cast<int64_t>(delayMax) >= correlationSamples)
    throw std::invalid_argument("Delay limits exceed the correlation block; reduce the delay range or narrow the Doppler span");
  nCorr = correlationSamples;
  nfft = 2 * nCorr - 1;
  if (_roundHamming) nfft = next_hamming(nfft);
  if (nfft > std::numeric_limits<uint16_t>::max())
    throw std::invalid_argument("Correlation FFT exceeds the processor limit; widen the Doppler span");
  cpi = (static_cast<double>(nCorr) * nDopplerBins) / fs;

  // update doppler bins to true cpi time
  resolutionDoppler = 1.0 / cpi;

  // create ambiguity map
  map = std::make_unique<Map<Complex>>(nDopplerBins, nDelayBins);

  // delay calculations
  map->delay.resize(nDelayBins);
  std::iota(map->delay.begin(), map->delay.end(), delayMin);

  map->doppler.push_front(dopplerMiddle);
  i = 1;
  while (map->doppler.size() < nDopplerBins)
  {
    map->doppler.push_back(dopplerMiddle + (i * resolutionDoppler));
    map->doppler.push_front(dopplerMiddle - (i * resolutionDoppler));
    i++;
  }

  // compute FFTW plans in constructor
  dataXi.resize(nfft);
  dataYi.resize(nfft);
  dataZi.resize(nfft);
  // This transform runs across time batches, not range samples. Wide Doppler
  // windows can have more bins than the range FFT and must not overrun storage.
  dataDoppler.resize(nDopplerBins);
  fftXi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataXi.data()),
                           reinterpret_cast<fftw_complex *>(dataXi.data()), FFTW_FORWARD, FFTW_ESTIMATE);
  fftYi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataYi.data()),
                           reinterpret_cast<fftw_complex *>(dataYi.data()), FFTW_FORWARD, FFTW_ESTIMATE);
  fftZi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataZi.data()),
                           reinterpret_cast<fftw_complex *>(dataZi.data()), FFTW_BACKWARD, FFTW_ESTIMATE);
  fftDoppler = fftw_plan_dft_1d(nDopplerBins, reinterpret_cast<fftw_complex *>(dataDoppler.data()),
                                reinterpret_cast<fftw_complex *>(dataDoppler.data()), FFTW_FORWARD, FFTW_ESTIMATE);

}

Ambiguity::~Ambiguity()
{
  fftw_destroy_plan(fftXi);
  fftw_destroy_plan(fftYi);
  fftw_destroy_plan(fftZi);
  fftw_destroy_plan(fftDoppler);
}

Map<std::complex<double>> *Ambiguity::process(IqData *x, IqData *y)
{
  return process(x->view_data(), y);
}

Map<std::complex<double>> *Ambiguity::process(
  const std::deque<Complex>& x, IqData *y)
{
  if (x.size() < nDopplerBins * nCorr)
    throw std::runtime_error("Reference CPI is shorter than ambiguity input");

  // range processing
  nSamples = nDopplerBins * nCorr;
  uint32_t referenceIndex = 0;
  const std::complex<double> imaginary = {0, 1};
  for (uint16_t i = 0; i < nDopplerBins; i++)
  {
    for (uint16_t j = 0; j < nCorr; j++)
    {
      dataXi[j] = x[referenceIndex];
      if (dopplerMiddle != 0)
        dataXi[j] *= std::exp(imaginary * 2.0 * M_PI * dopplerMiddle *
          (static_cast<double>(referenceIndex) / fs));
      referenceIndex++;
      dataYi[j] = y->pop_front();
    }

    for (uint16_t j = nCorr; j < nfft; j++)
    {
      dataXi[j] = {0, 0};
      dataYi[j] = {0, 0};
    }

    fftw_execute(fftXi);
    fftw_execute(fftYi);

    // compute correlation
    for (uint32_t j = 0; j < nfft; j++)
    {
      dataZi[j] = (dataYi[j] * std::conj(dataXi[j])) / (double)nfft;
    }

    fftw_execute(fftZi);

    // Extract signed lags directly, including the full unrounded FFT window.
    corr.clear();
    for (uint16_t j = 0; j < nDelayBins; j++)
    {
      const int32_t lag = delayMin + j;
      const uint32_t index = lag < 0 ? static_cast<int64_t>(nfft) + lag : lag;
      corr.push_back(dataZi[index]);
    }

    map->set_row(i, corr);
  }

  // doppler processing
  for (uint16_t i = 0; i < nDelayBins; i++)
  {
    delayProfile = map->get_col(i);
    for (uint16_t j = 0; j < nDopplerBins; j++)
    {
      dataDoppler[j] = {delayProfile[j].real(), delayProfile[j].imag()};
    }

    fftw_execute(fftDoppler);

    corr.clear();
    for (uint16_t j = 0; j < nDopplerBins; j++)
    {
      corr.push_back(dataDoppler[(j + int(nDopplerBins / 2) + 1) % nDopplerBins]);
    }

    map->set_col(i, corr);
  }

  return map.get();
}

double Ambiguity::get_doppler_middle() const {
  return dopplerMiddle;
}

uint16_t Ambiguity::get_n_delay_bins() const {
  return nDelayBins;
}

uint16_t Ambiguity::get_n_doppler_bins() const {
  return nDopplerBins;
}

uint16_t Ambiguity::get_n_corr() const {
  return nCorr;
}

double Ambiguity::get_cpi() const {
  return cpi;
}

uint32_t Ambiguity::get_nfft() const {
  return nfft;
}

uint32_t Ambiguity::get_n_samples() const {
  return nSamples;
}

Map<Ambiguity::Complex>* Ambiguity::import_gpu(const std::complex<float>* output) {
  for (uint16_t delay = 0; delay < nDelayBins; ++delay) {
    corr.resize(nDopplerBins);
    for (uint16_t d = 0; d < nDopplerBins; ++d)
      corr[d] = output[delay * nDopplerBins +
        (d + nDopplerBins / 2 + 1) % nDopplerBins];
    map->set_col(delay, corr);
  }
  return map.get();
}
