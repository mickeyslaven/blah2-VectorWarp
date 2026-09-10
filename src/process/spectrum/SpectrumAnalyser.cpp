#include "SpectrumAnalyser.h"
#include <complex>
#include <iostream>
#include <deque>
#include <vector>
#include <math.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

// constructor
SpectrumAnalyser::SpectrumAnalyser(uint32_t _n, double _bandwidth,
  double _centerFrequency, double sampleRate)
{
  if (_n == 0 || _n > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
      !std::isfinite(_bandwidth) || _bandwidth <= 0 || !std::isfinite(sampleRate) ||
      sampleRate <= 0 || !std::isfinite(_centerFrequency))
    throw std::invalid_argument("Invalid reference spectrum sample rate, size or bandwidth");
  // input
  n = _n;
  bandwidth = _bandwidth;
  centerFrequency = _centerFrequency;

  // compute nfft
  nfft = n;
  resolution = sampleRate / nfft;
  decimation = static_cast<uint32_t>(std::min<double>(nfft,
    std::max(1.0, std::ceil(bandwidth / resolution))));
  nSpectrum = 1 + (nfft - 1) / decimation;

  // compute FFTW plans in constructor
  dataX = new std::complex<double>[nfft];
  fftX = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataX),
                           reinterpret_cast<fftw_complex *>(dataX), FFTW_FORWARD, FFTW_ESTIMATE);
  if (!fftX) {
    delete[] dataX;
    throw std::runtime_error("Could not create reference spectrum FFT plan");
  }
}

SpectrumAnalyser::~SpectrumAnalyser()
{
  fftw_destroy_plan(fftX);
  delete[] dataX;
}

void SpectrumAnalyser::process(IqData *x)
{  
  // load data and FFT
  uint32_t i;
  std::deque<std::complex<double>> data = x->get_data();
  if (data.size() < nfft)
    throw std::invalid_argument("Not enough samples for the reference spectrum");
  for (i = 0; i < nfft; i++)
  {
    dataX[i] = data[i];
  }
  fftw_execute(fftX);

  // fftshift
  std::vector<std::complex<double>> fftshift;
  for (i = 0; i < nfft; i++)
  {
    fftshift.push_back(dataX[(i + (nfft + 1) / 2) % nfft]);
  }
  
  // decimate
  std::vector<std::complex<double>> spectrum;
  for (i = 0; i < nfft; i+=decimation)
  {
    spectrum.push_back(fftshift[i] / static_cast<double>(nfft));
  }
  x->update_spectrum(spectrum);

  // update frequency
  std::vector<double> frequency;
  for (i = 0; i < nfft; i += decimation)
  {
    const double basebandBin = static_cast<double>(i) - nfft / 2;
    frequency.push_back((basebandBin * resolution + centerFrequency) / 1000);
  }
  x->update_frequency(frequency);

  return;
}
