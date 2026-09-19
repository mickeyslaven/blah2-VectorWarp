#include "Ambiguity.h"
#include "RangeRowWorker.h"
#include "process/utility/FftwThreads.h"
#ifdef VECTORWARP_GPU_PARTIAL_AMBIGUITY_BENCH
#include "GpuPartialAmbiguity.h"
#endif
#include <complex>
#include <iostream>
#include <deque>
#include <vector>
#include <numeric>
#include <math.h>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <limits>
#include <string>

namespace {
std::mutex& fftw_planner_mutex() { static std::mutex mutex; return mutex; }
bool fftw_threads_ready() {
  static std::once_flag once;
  static bool ready = false;
  std::call_once(once, [] { ready = fftw_init_threads() != 0; });
  return ready;
}
struct RestoreFftwThreads {
  const int saved = blah2::fftw_planner_threads();
  ~RestoreFftwThreads() { blah2::set_fftw_planner_threads(saved); }
};
unsigned fftw_plan_flags() {
  const char* mode = std::getenv("VECTORWARP_FFTW_PLAN");
  if (!mode || std::string(mode) == "estimate") return FFTW_ESTIMATE;
  if (std::string(mode) == "measure") return FFTW_MEASURE;
  throw std::invalid_argument("VECTORWARP_FFTW_PLAN must be estimate or measure");
}
}

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
  if (const char* setting = std::getenv("VECTORWARP_BENCH_RANGE_WORKERS")) {
    const std::string mode(setting);
    if (mode == "0") rangeWorkers = 0;
    else if (mode == "1") rangeWorkers = 1;
    else if (mode == "2") rangeWorkers = 2;
    else throw std::invalid_argument("VECTORWARP_BENCH_RANGE_WORKERS must be 0, 1, or 2");
  }
  if (rangeWorkers == 2 && nDopplerBins < 2)
    throw std::invalid_argument("Two range workers require at least two Doppler rows");
#ifdef VECTORWARP_GPU_PARTIAL_AMBIGUITY_BENCH
  bool usePartialGpu = false;
  if (const char* setting = std::getenv("VECTORWARP_GPU_PARTIAL_AMBIGUITY")) {
    const std::string mode(setting);
    if (mode == "1") usePartialGpu = true;
    else if (mode != "0")
      throw std::invalid_argument("VECTORWARP_GPU_PARTIAL_AMBIGUITY must be 0 or 1");
  }
  if (usePartialGpu && rangeWorkers != 2)
    throw std::invalid_argument("Partial GPU ambiguity requires two CPU range workers");
#endif

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
  // Only the requested signed-lag window must be free of circular aliasing.
  // This is the minimum zero-padded transform length for those lags and is
  // also the geometry reported to the GPU path.
  const uint32_t maxLag = static_cast<uint32_t>(std::max(
    std::abs(static_cast<int64_t>(delayMin)),
    std::abs(static_cast<int64_t>(delayMax))));
  nfft = nCorr + maxLag;
  if (_roundHamming) nfft = next_hamming(nfft);
  // Benchmark overlay only: both the CPU oracle and GPU receive this geometry.
  if (const char* power2 = std::getenv("BLAH2_BENCH_RANGE_POWER2")) {
    if (std::string(power2) != "1")
      throw std::invalid_argument("BLAH2_BENCH_RANGE_POWER2 must be 1 or unset");
    uint32_t padded = 1;
    while (padded < nfft && padded <= UINT16_MAX) padded <<= 1;
    nfft = padded;
  }
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
  // The two forward inputs are contiguous so one plan_many execution replaces
  // the former separate reference and surveillance launches.
  dataXi.resize(static_cast<std::size_t>(nfft) * 2);
  dataZi.resize(nfft);
  // This transform runs across time batches, not range samples. Wide Doppler
  // windows can have more bins than the range FFT and must not overrun storage.
  dataDoppler.resize(nDopplerBins);
  std::unique_lock<std::mutex> plannerLock(fftw_planner_mutex());
  if (!fftw_threads_ready())
    throw std::runtime_error("FFTW thread initialization failed");
  RestoreFftwThreads restoreThreads;
  const unsigned planFlags = fftw_plan_flags();
  try {
    int rangeLength = static_cast<int>(nfft);
    if (nfft <= 4096)
      blah2::set_fftw_planner_threads(std::min(std::max(restoreThreads.saved, 1), 2));
    fftXi = fftw_plan_many_dft(1, &rangeLength, 2,
      reinterpret_cast<fftw_complex *>(dataXi.data()), nullptr, 1, rangeLength,
      reinterpret_cast<fftw_complex *>(dataXi.data()), nullptr, 1, rangeLength,
      FFTW_FORWARD, planFlags);
    fftZi = fftw_plan_dft_1d(nfft, reinterpret_cast<fftw_complex *>(dataZi.data()),
                             reinterpret_cast<fftw_complex *>(dataZi.data()), FFTW_BACKWARD, planFlags);
    blah2::set_fftw_planner_threads(restoreThreads.saved);
    if (!fftXi || !fftZi)
      throw std::runtime_error("Could not create ambiguity range FFT plan");

    if (nDopplerBins <= 1024) blah2::set_fftw_planner_threads(1);
    fftDoppler = fftw_plan_dft_1d(nDopplerBins, reinterpret_cast<fftw_complex *>(dataDoppler.data()),
                                  reinterpret_cast<fftw_complex *>(dataDoppler.data()), FFTW_FORWARD, planFlags);
    blah2::set_fftw_planner_threads(restoreThreads.saved);
    if (!fftDoppler)
      throw std::runtime_error("Could not create ambiguity Doppler FFT plan");
  } catch (...) {
    if (fftXi) { fftw_destroy_plan(fftXi); fftXi = nullptr; }
    if (fftZi) { fftw_destroy_plan(fftZi); fftZi = nullptr; }
    if (fftDoppler) { fftw_destroy_plan(fftDoppler); fftDoppler = nullptr; }
    throw;
  }

  plannerLock.unlock();
  try {
    if (rangeWorkers) {
      rangeRows.resize(uint64_t(nDopplerBins)*nDelayBins);
      rangeCaller = std::make_unique<RangeRowWorker>(nfft, planFlags,
        fftw_planner_mutex(), false);
      if (rangeWorkers == 2)
        rangeThread = std::make_unique<RangeRowWorker>(nfft, planFlags,
          fftw_planner_mutex(), true);
    }
#ifdef VECTORWARP_GPU_PARTIAL_AMBIGUITY_BENCH
    if (usePartialGpu)
      gpuPartial = std::make_unique<GpuPartialAmbiguity>(_n, nDopplerBins, nCorr,
        nfft, nDelayBins, delayMin, dopplerMiddle);
#endif
    if (const char* path = std::getenv("VECTORWARP_BENCH_RANGE_LOG")) {
      rangeLog.open(path, std::ios::app);
      if (!rangeLog) throw std::runtime_error("Ambiguity range timing log open failed");
    }
  } catch (...) {
#ifdef VECTORWARP_GPU_PARTIAL_AMBIGUITY_BENCH
    gpuPartial.reset();
#endif
    rangeThread.reset(); rangeCaller.reset();
    plannerLock.lock();
    if (fftXi) fftw_destroy_plan(fftXi);
    if (fftZi) fftw_destroy_plan(fftZi);
    if (fftDoppler) fftw_destroy_plan(fftDoppler);
    fftXi=fftZi=fftDoppler=nullptr;
    throw;
  }
  // RestoreFftwThreads is destroyed before plannerLock on normal exit.
  // Reacquire the planner mutex so its global thread-budget write is guarded.
  plannerLock.lock();

}

Ambiguity::~Ambiguity()
{
#ifdef VECTORWARP_GPU_PARTIAL_AMBIGUITY_BENCH
  gpuPartial.reset();
#endif
  rangeThread.reset();
  rangeCaller.reset();
  std::lock_guard<std::mutex> plannerLock(fftw_planner_mutex());
  fftw_destroy_plan(fftXi);
  fftw_destroy_plan(fftZi);
  fftw_destroy_plan(fftDoppler);
}

Map<std::complex<double>> *Ambiguity::process(IqData *x, IqData *y)
{
  return process_impl(&x->view_data(), nullptr, 0, y, nullptr, 0,
    nullptr, nullptr, 0, 0, false);
}

Map<std::complex<double>> *Ambiguity::process(
  const std::deque<Complex>& x, IqData *y, bool preserveSurveillance)
{
  return process_impl(&x, nullptr, 0, y, nullptr, 0, nullptr, nullptr,
    0, 0, preserveSurveillance);
}

Map<std::complex<double>> *Ambiguity::process_borrowed(
  const std::deque<Complex>& originalReference, IqData* surveillanceOwner,
  const Complex* rotatedReference, const Complex* filteredSurveillance,
  uint32_t fullSamples, int32_t clutterDelayMin, bool preserveSurveillance)
{
  if (!filteredSurveillance || fullSamples < nDopplerBins * uint64_t(nCorr) ||
      (rotatedReference &&
       (clutterDelayMin <= -int64_t(fullSamples) || clutterDelayMin >= int64_t(fullSamples))))
    throw std::invalid_argument("Invalid borrowed CPI geometry");
  return process_impl(&originalReference, nullptr, 0, surveillanceOwner,
    nullptr, 0, rotatedReference, filteredSurveillance, fullSamples,
    clutterDelayMin, preserveSurveillance);
}

Map<std::complex<double>> *Ambiguity::process_borrowed(
  const Complex* rotatedReference, uint32_t referenceSamples,
  const Complex* filteredSurveillance, uint32_t surveillanceSamples,
  int32_t clutterDelayMin)
{
  if (!rotatedReference || !filteredSurveillance || referenceSamples != surveillanceSamples ||
      referenceSamples < nDopplerBins * uint64_t(nCorr) ||
      clutterDelayMin <= -int64_t(referenceSamples) || clutterDelayMin >= int64_t(referenceSamples))
    throw std::invalid_argument("Invalid borrowed CPI geometry");
  return process_impl(nullptr, rotatedReference, referenceSamples, nullptr,
    filteredSurveillance, surveillanceSamples, rotatedReference,
    filteredSurveillance, referenceSamples, clutterDelayMin, true);
}

Map<std::complex<double>> *Ambiguity::process_impl(
  const std::deque<Complex>* x, const Complex* rawReference, uint32_t rawReferenceSamples,
  IqData *y, const Complex* rawSurveillance, uint32_t rawSurveillanceSamples,
  const Complex* rotatedReference, const Complex* filteredSurveillance,
  uint32_t fullSamples, int32_t clutterDelayMin, bool preserveSurveillance)
{
  if ((x ? x->size() : rawReferenceSamples) < nDopplerBins * uint64_t(nCorr))
    throw std::runtime_error("Reference CPI is shorter than ambiguity input");

  // range processing
  nSamples = nDopplerBins * nCorr;
  if ((y ? y->view_data().size() : rawSurveillanceSamples) < nSamples)
    throw std::runtime_error("Surveillance CPI is shorter than ambiguity input");
  auto surveillanceIt = y ? y->view_data().cbegin() : std::deque<Complex>::const_iterator{};
  uint32_t referenceIndex = 0;
  const std::complex<double> imaginary = {0, 1};
  const auto rangePhaseStarted = std::chrono::steady_clock::now();
  if (rangeWorkers) {
#ifdef VECTORWARP_GPU_PARTIAL_AMBIGUITY_BENCH
    if (gpuPartial) {
      const auto rangeStarted = std::chrono::steady_clock::now();
      if (rawReference)
        gpuPartial->start_borrowed(rotatedReference, filteredSurveillance,
          fullSamples, clutterDelayMin);
      else
        gpuPartial->start(*x, y->view_data(), rotatedReference, filteredSurveillance,
          fullSamples, clutterDelayMin);
      const uint32_t first = GpuPartialAmbiguity::gpuRows;
      const uint32_t split = first + (nDopplerBins-first)/2;
      RangeRowWorker::Job caller{x,y ? &y->view_data() : nullptr,rawReference,rawSurveillance,
        rawReferenceSamples,rawSurveillanceSamples,&rangeRows,first,split,
        nCorr,nfft,nDelayBins,fs,delayMin,dopplerMiddle};
      caller.rotatedX=rotatedReference; caller.filteredY=filteredSurveillance;
      caller.fullSamples=fullSamples; caller.rotation=clutterDelayMin;
      auto second=caller; second.first=split; second.last=nDopplerBins;
      bool started=false;
      GpuPartialAmbiguityTiming gpuTiming{};
      std::chrono::steady_clock::time_point submitted, callerFinished, joined;
      try {
        rangeThread->start(second); started=true;
        submitted=std::chrono::steady_clock::now();
        rangeCaller->executeCaller(caller);
        callerFinished=std::chrono::steady_clock::now();
        rangeThread->finish(); started=false;
        joined=std::chrono::steady_clock::now();
        gpuTiming=gpuPartial->finish(rangeRows);
      } catch (...) {
        if (started) try { rangeThread->finish(); } catch (...) {}
        gpuPartial->abort();
        throw;
      }
      for (uint32_t row=0; row<nDopplerBins; ++row) {
        corr.assign(rangeRows.begin()+uint64_t(row)*nDelayBins,
          rangeRows.begin()+uint64_t(row+1)*nDelayBins);
        map->set_row(row,corr);
      }
      if (rangeLog) {
        auto milliseconds=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
        rangeLog << "{\"frame\":" << ++rangeFrame << ",\"workers\":2,\"gpu_rows\":" << first
          << ",\"cpu_rows\":" << nDopplerBins-first << ",\"split\":" << split
          << ",\"fft\":" << nfft << ",\"backend\":\"vulkan+cpu\""
          << ",\"borrowed_y\":" << (filteredSurveillance?"true":"false")
          << ",\"borrowed_x\":" << (rotatedReference?"true":"false")
          << ",\"gpu_pack_submit_ms\":" << gpuTiming.packSubmitMs
          << ",\"gpu_submit_to_fence_ms\":" << gpuTiming.submitToFenceMs
          << ",\"gpu_fence_wait_ms\":" << gpuTiming.fenceWaitMs
          << ",\"gpu_readback_ms\":" << gpuTiming.readbackMs
          << ",\"cpu_caller_ms\":" << milliseconds(submitted,callerFinished)
          << ",\"cpu_join_wait_ms\":" << milliseconds(callerFinished,joined)
          << ",\"range_total_ms\":" << milliseconds(rangeStarted,std::chrono::steady_clock::now())
          << "}\n" << std::flush;
      }
    } else
#endif
    {
    const auto rangeStarted = std::chrono::steady_clock::now();
    const uint32_t split = rangeWorkers == 2 ? nDopplerBins/2 : nDopplerBins;
    RangeRowWorker::Job job{x,y ? &y->view_data() : nullptr,rawReference,rawSurveillance,
      rawReferenceSamples,rawSurveillanceSamples,&rangeRows,0,split,nCorr,nfft,
      nDelayBins,fs,delayMin,dopplerMiddle};
    job.rotatedX=rotatedReference; job.filteredY=filteredSurveillance;
    job.fullSamples=fullSamples; job.rotation=clutterDelayMin;
    bool started = false;
    if (rangeThread) {
      auto second = job; second.first = split; second.last = nDopplerBins;
      rangeThread->start(second); started = true;
    }
    const auto submitted = std::chrono::steady_clock::now();
    try { rangeCaller->executeCaller(job); }
    catch (...) {
      if (started) try { rangeThread->finish(); } catch (...) {}
      throw;
    }
    const auto callerFinished = std::chrono::steady_clock::now();
    if (started) rangeThread->finish();
    const auto joined = std::chrono::steady_clock::now();
    for (uint32_t row=0;row<nDopplerBins;++row) {
      corr.assign(rangeRows.begin()+uint64_t(row)*nDelayBins,
        rangeRows.begin()+uint64_t(row+1)*nDelayBins);
      map->set_row(row,corr);
    }
    if (rangeLog) {
      auto milliseconds=[](auto a,auto b){return std::chrono::duration<double,std::milli>(b-a).count();};
      rangeLog << "{\"frame\":" << ++rangeFrame << ",\"workers\":" << rangeWorkers
        << ",\"rows\":" << nDopplerBins << ",\"split\":" << split
        << ",\"fft\":" << nfft << ",\"submit_ms\":" << milliseconds(rangeStarted,submitted)
        << ",\"caller_ms\":" << milliseconds(submitted,callerFinished)
        << ",\"join_wait_ms\":" << milliseconds(callerFinished,joined)
        << ",\"range_total_ms\":" << milliseconds(rangeStarted,std::chrono::steady_clock::now())
        << "}\n" << std::flush;
    }
    }
  } else for (uint16_t i = 0; i < nDopplerBins; i++)
  {
    uint32_t rotatedIndex = 0;
    if (rotatedReference) {
      const int64_t at = (int64_t(referenceIndex) + clutterDelayMin) % fullSamples;
      rotatedIndex = uint32_t(at < 0 ? at + fullSamples : at);
    }
    for (uint16_t j = 0; j < nCorr; j++)
    {
      dataXi[j] = rotatedReference ? rotatedReference[rotatedIndex] :
        (rawReference ? rawReference[referenceIndex] : (*x)[referenceIndex]);
      if (rotatedReference && ++rotatedIndex == fullSamples) rotatedIndex = 0;
      if (dopplerMiddle != 0)
        dataXi[j] *= std::exp(imaginary * 2.0 * M_PI * dopplerMiddle *
          (static_cast<double>(referenceIndex) / fs));
      referenceIndex++;
      dataXi[nfft + j] = filteredSurveillance ? filteredSurveillance[referenceIndex-1] :
        (rawSurveillance ? rawSurveillance[referenceIndex-1] : *surveillanceIt++);
    }

    for (uint16_t j = nCorr; j < nfft; j++)
    {
      dataXi[j] = {0, 0};
      dataXi[nfft + j] = {0, 0};
    }

    fftw_execute(fftXi);

    // compute correlation
    for (uint32_t j = 0; j < nfft; j++)
    {
      dataZi[j] = (dataXi[nfft + j] * std::conj(dataXi[j])) / (double)nfft;
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
  if (!rangeWorkers && rangeLog) {
    const double total = std::chrono::duration<double,std::milli>(
      std::chrono::steady_clock::now()-rangePhaseStarted).count();
    rangeLog << "{\"frame\":" << ++rangeFrame << ",\"workers\":0"
      << ",\"rows\":" << nDopplerBins << ",\"fft\":" << nfft
      << ",\"range_total_ms\":" << total << "}\n" << std::flush;
  }

  if (!preserveSurveillance && y) {
    y->discard_front(nSamples);
    // The legacy path retains the filtered short tail after consuming the
    // ambiguity rows. Copy only that tail; the next CPI replaces this deque.
    if (filteredSurveillance)
      y->assign_complex(filteredSurveillance+nSamples, fullSamples-nSamples);
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
