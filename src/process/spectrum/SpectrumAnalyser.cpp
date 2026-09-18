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

  // For a regularly sampled subset of full-FFT bins, folding decimation-sized
  // input blocks produces exactly the same selected DFT values.  Preserve the
  // existing full-CPI transform for a partial last group: its output shape and
  // frequency labels intentionally include that final bin.
  folded = decimation >= 8 && nfft % decimation == 0;
  const uint32_t transformLength = folded ? nSpectrum : nfft;
  if (folded)
  {
    const uint32_t shift = (nfft + 1) / 2;
    const uint32_t remainder = shift % decimation;
    const double phase = -2.0 * std::acos(-1.0) * remainder;
    blockPhase.resize(decimation);
    binPhase.resize(nSpectrum);
    for (uint32_t block = 0; block < decimation; ++block)
      blockPhase[block] = std::polar(1.0, phase * block / decimation);
    for (uint32_t bin = 0; bin < nSpectrum; ++bin)
      binPhase[bin] = std::polar(1.0, phase * bin / nfft);
  }

  // compute FFTW plans in constructor
  dataX = new std::complex<double>[transformLength];
  fftX = fftw_plan_dft_1d(transformLength, reinterpret_cast<fftw_complex *>(dataX),
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
  const auto& data = x->view_data();
  if (data.size() < nfft)
    throw std::invalid_argument("Not enough samples for the reference spectrum");
  if (folded)
  {
    std::fill(dataX, dataX + nSpectrum, std::complex<double>{});
    for (uint32_t block = 0; block < decimation; ++block)
    {
      const uint64_t offset = uint64_t(block) * nSpectrum;
      for (uint32_t bin = 0; bin < nSpectrum; ++bin)
        dataX[bin] += data[offset + bin] * blockPhase[block];
    }
    for (uint32_t bin = 0; bin < nSpectrum; ++bin)
      dataX[bin] *= binPhase[bin];
  }
  else
    for (i = 0; i < nfft; i++) dataX[i] = data[i];
  fftw_execute(fftX);

  // Select the shifted bins directly; no full-size shifted copy is needed.
  std::vector<std::complex<double>> spectrum;
  spectrum.reserve(nSpectrum);
  if (folded)
  {
    const uint32_t shift = ((nfft + 1) / 2) / decimation;
    for (i = 0; i < nSpectrum; ++i)
      spectrum.push_back(dataX[(shift + i) % nSpectrum] / static_cast<double>(nfft));
  }
  else
    for (i = 0; i < nfft; i += decimation)
      spectrum.push_back(dataX[(i + (nfft + 1) / 2) % nfft] / static_cast<double>(nfft));
  x->update_spectrum(spectrum);

  // update frequency
  std::vector<double> frequency;
  frequency.reserve(nSpectrum);
  for (i = 0; i < nfft; i += decimation)
  {
    const double basebandBin = static_cast<double>(i) - nfft / 2;
    frequency.push_back((basebandBin * resolution + centerFrequency) / 1000);
  }
  x->update_frequency(frequency);

  return;
}
