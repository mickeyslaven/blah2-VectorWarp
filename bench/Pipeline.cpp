// Finite replay instrumentation, compiled against unchanged upstream or fork DSP.
// Network/UI transport and hardware acquisition are deliberately not benchmarked.
#include "BenchmarkReader.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/meta/HammingNumber.h"
#include "process/clutter/WienerHopf.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/tracker/Tracker.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include <rapidjson/document.h>
#include <chrono>
#include <cmath>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <algorithm>
#include <atomic>
#include <sstream>
#include <thread>
#include <cstdlib>
#include <sys/resource.h>
#ifdef BLAH2_BENCH_FAST
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/RangeFft.h"
#ifdef VECTORWARP_MIXED_AUTO
#include "process/mixed/MixedProcess.h"
#include "process/mixed/MixedAutoPolicy.h"
#include "process/mixed/MixedMap.h"
#include "process/mixed/MixedEligibility.h"
#endif
#include "process/utility/ProcessingThreads.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/fusion/Noncoherent.h"
#endif

using Complex = std::complex<double>;
using Clock = std::chrono::steady_clock;
#ifdef BLAH2_BENCH_FAST
constexpr const char* Engine = "fast";
#else
constexpr const char* Engine = "upstream";
#endif

struct Geometry {
  uint32_t fs{};
  uint32_t samples{};
  unsigned channels{};
  unsigned referenceChannel{};
  unsigned surveillanceChannel{};
  unsigned pathCount{};
  unsigned workers{};
  unsigned fftThreads{};
  int32_t delayMin{};
  int32_t delayMax{};
  int32_t dopplerMin{};
  int32_t dopplerMax{};
  uint32_t delayBins{};
  uint32_t dopplerBins{};
  uint32_t nCorr{};
  uint32_t nfft{};
  double fc{};
  double requestedCpi{};
  double effectiveCpi{};
  bool roundHamming{};
  bool upstreamSafe{};
};

double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b-a).count();
}
double percentile(std::vector<double> values, double fraction) {
  if (values.empty()) return 0;
  std::sort(values.begin(), values.end());
  const double position=(values.size()-1)*fraction;
  const auto low=static_cast<size_t>(std::floor(position));
  const auto high=static_cast<size_t>(std::ceil(position));
  return values[low]+(values[high]-values[low])*(position-low);
}
std::string jsonNumberOrNull(const std::vector<double>& values, double number) {
  if (values.empty()) return "null";
  std::ostringstream output; output << std::setprecision(12) << number;
  return output.str();
}

rapidjson::Document loadConfig(const char* path) {
  std::ifstream config(path);
  if (!config) throw std::runtime_error("Cannot open benchmark JSON");
  std::stringstream text; text << config.rdbuf();
  rapidjson::Document cfg;
  cfg.Parse(text.str().c_str());
  if (cfg.HasParseError() || !cfg.IsObject()) throw std::runtime_error("Invalid benchmark JSON");
  return cfg;
}

double finiteNumber(const rapidjson::Document& cfg, const char* key) {
  if (!cfg.HasMember(key) || !cfg[key].IsNumber() || !std::isfinite(cfg[key].GetDouble()))
    throw std::runtime_error(std::string("Missing or non-finite setting: ")+key);
  return cfg[key].GetDouble();
}
int64_t wholeNumber(const rapidjson::Document& cfg, const char* key, int64_t low, int64_t high) {
  if (!cfg.HasMember(key) || !cfg[key].IsInt64())
    throw std::runtime_error(std::string("Setting must be an integer: ")+key);
  const int64_t value=cfg[key].GetInt64();
  if (value < low || value > high) throw std::runtime_error(std::string("Setting is outside its safe range: ")+key);
  return value;
}
bool boolean(const rapidjson::Document& cfg, const char* key) {
  if (!cfg.HasMember(key) || !cfg[key].IsBool())
    throw std::runtime_error(std::string("Setting must be boolean: ")+key);
  return cfg[key].GetBool();
}

Geometry inspectGeometry(const rapidjson::Document& cfg, bool array) {
  Geometry value;
  value.fs=wholeNumber(cfg,"sample_rate",1,100000000);
  value.channels=wholeNumber(cfg,"channels",2,8);
  value.referenceChannel=wholeNumber(cfg,"reference_channel",0,value.channels-1);
  value.surveillanceChannel=wholeNumber(cfg,"surveillance_channel",0,value.channels-1);
  if (value.referenceChannel == value.surveillanceChannel)
    throw std::invalid_argument("Reference and surveillance channels must differ");
  value.fc=finiteNumber(cfg,"frequency");
  if (value.fc <= 0) throw std::invalid_argument("Frequency must be positive");
  value.requestedCpi=finiteNumber(cfg,"cpi");
  if (value.requestedCpi <= 0 || value.requestedCpi > 10)
    throw std::invalid_argument("CPI is outside the bounded benchmark range");
  const long double exactSamples=static_cast<long double>(value.fs)*value.requestedCpi;
  if (exactSamples < 1 || exactSamples > 10000000)
    throw std::invalid_argument("CPI sample count is outside the bounded benchmark range");
  value.samples=std::llround(exactSamples);
  value.delayMin=wholeNumber(cfg,"delay_min",-1000000,1000000);
  value.delayMax=wholeNumber(cfg,"delay_max",-1000000,1000000);
  value.dopplerMin=wholeNumber(cfg,"doppler_min",INT32_MIN,INT32_MAX);
  value.dopplerMax=wholeNumber(cfg,"doppler_max",INT32_MIN,INT32_MAX);
  if (value.delayMin > 0 || value.delayMax < 0 || value.delayMin >= value.delayMax)
    throw std::invalid_argument("Delay geometry must be ordered and include zero");
  if (value.dopplerMin >= value.dopplerMax)
    throw std::invalid_argument("Doppler geometry must be ordered");
  const int64_t delayBins=static_cast<int64_t>(value.delayMax)-value.delayMin+1;
  const long double halfSpan=(static_cast<long double>(value.dopplerMax)-value.dopplerMin)/2;
  const long double resolution=static_cast<long double>(value.fs)/value.samples;
  const uint64_t sideBins=std::floor(halfSpan/resolution+1e-12L);
  const uint64_t dopplerBins=2*sideBins+1;
  if (delayBins < 1 || delayBins > UINT16_MAX || dopplerBins > UINT16_MAX)
    throw std::invalid_argument("Delay or Doppler bin count exceeds the DSP representation");
  value.delayBins=delayBins; value.dopplerBins=dopplerBins;
  value.nCorr=value.samples/value.dopplerBins;
  if (!value.nCorr || value.nCorr > UINT16_MAX)
    throw std::invalid_argument("Correlation length exceeds the DSP representation");
  if (value.delayMin <= -static_cast<int64_t>(value.nCorr) ||
      value.delayMax >= static_cast<int64_t>(value.nCorr))
    throw std::invalid_argument("Delay limits exceed the correlation block; reduce the delay range or narrow the Doppler span");
  value.roundHamming=boolean(cfg,"round_hamming");
#ifdef BLAH2_BENCH_FAST
  const uint32_t maxLag=static_cast<uint32_t>(std::max(
    std::abs(static_cast<int64_t>(value.delayMin)),
    std::abs(static_cast<int64_t>(value.delayMax))));
  value.nfft=value.nCorr+maxLag;
#else
  value.nfft=2*value.nCorr-1;
#endif
  if (value.roundHamming) value.nfft=next_hamming(value.nfft);
#ifdef BLAH2_BENCH_FAST
  value.nfft=blah2::selectedRangeFftLength(value.nfft);
#ifdef VECTORWARP_MIXED_AUTO
  if(!array && value.samples==1000000 && value.dopplerBins==301 &&
      value.nCorr==3322 && value.delayMin==-10 && value.delayBins==411 &&
      value.dopplerMin==-300 && value.dopplerMax==300)
    value.nfft=4096;
#endif
#endif
  if (!value.nfft || value.delayBins > value.nfft)
    throw std::invalid_argument("Delay geometry exceeds the range FFT");
  value.effectiveCpi=static_cast<double>(value.nCorr)*value.dopplerBins/value.fs;
  value.pathCount=array ? value.channels : 1;
  value.workers=wholeNumber(cfg,"benchmark_workers",1,8);
  value.fftThreads=wholeNumber(cfg,"benchmark_fft_threads",1,256);
  if ((!array && value.workers != 1) || value.workers > value.pathCount)
    throw std::invalid_argument("Worker count must be one for pair or no greater than array paths");
  // Upstream also copies one extra positive-lag sample when the requested
  // delay window occupies the entire unrounded range FFT.
  value.upstreamSafe=value.dopplerBins <= value.nfft && value.delayBins < value.nfft;
  return value;
}

void printGeometry(const Geometry& value, const std::string& profile) {
  bool supported=true;
#ifndef BLAH2_BENCH_FAST
  supported=profile == "pair" && value.upstreamSafe;
#endif
  std::cout << std::setprecision(12) << "{\"schemaVersion\":1,\"engine\":\"" << Engine
    << "\",\"profile\":\"" << profile << "\",\"supported\":" << (supported?"true":"false")
    << ",\"upstreamSafe\":" << (value.upstreamSafe?"true":"false")
    << ",\"requestedCpiMs\":" << value.requestedCpi*1000
    << ",\"effectiveCpiMs\":" << value.effectiveCpi*1000
    << ",\"sampleRate\":" << value.fs << ",\"samples\":" << value.samples
    << ",\"delayBins\":" << value.delayBins << ",\"dopplerBins\":" << value.dopplerBins
    << ",\"nCorr\":" << value.nCorr << ",\"rangeFft\":" << value.nfft
    << ",\"workers\":" << value.workers << ",\"fftThreads\":" << value.fftThreads
    << ",\"roundHamming\":" << (value.roundHamming?"true":"false")
    << ",\"reason\":\""
#ifndef BLAH2_BENCH_FAST
    << (profile != "pair" ? "upstream supports physical pair only" :
        (!value.upstreamSafe ? "upstream Doppler scratch is smaller than requested Doppler bins" : ""))
#endif
    << "\"}\n";
}
void fill(IqData& destination, const std::deque<Complex>& values) {
  destination.clear();
  for (const auto& value : values) destination.push_back(value);
}
template<class Work> void paths(unsigned count, unsigned workers, Work work) {
  if (workers == 1) { for (unsigned i=0; i<count; ++i) work(i); return; }
  for (unsigned first=0; first<count; first+=workers) {
    std::vector<std::future<void>> tasks;
    for (unsigned i=first; i<std::min(count, first+workers); ++i)
      tasks.push_back(std::async(std::launch::async, work, i));
    for (auto& task : tasks) task.get();
  }
}

int main(int argc, char** argv) try {
  if (argc == 4 && std::string(argv[1]) == "--inspect-geometry") {
    const std::string profile=argv[3];
    if (profile != "pair" && profile != "array")
      throw std::invalid_argument("Geometry profile must be pair or array");
    const auto cfg=loadConfig(argv[2]);
    printGeometry(inspectGeometry(cfg,profile == "array"),profile);
    return 0;
  }
  if (argc != 9) throw std::invalid_argument(
    "Usage: bench-{upstream,fast} input.mchq profile.json output-prefix pair|array cpu|auto|gpu|mixed max-frames write|compare|none golden.maps; or --inspect-geometry profile.json pair|array");
  const auto started = Clock::now();
  const std::string prefix=argv[3], profile=argv[4], mode=argv[5], goldenMode=argv[7];
  const bool array = profile == "array";
  if ((!array && profile != "pair") || (mode != "cpu" && mode != "auto" && mode != "gpu" && mode != "mixed") ||
      (goldenMode != "write" && goldenMode != "compare" && goldenMode != "none"))
    throw std::invalid_argument("Invalid benchmark mode");
#ifndef BLAH2_BENCH_FAST
  if (array || mode != "cpu") throw std::invalid_argument("Upstream supports only the physical two-channel CPU comparison");
#endif
#ifndef VECTORWARP_MIXED_AUTO
  if (mode == "mixed") throw std::invalid_argument("Mixed benchmark requires the Pi mixed worker build");
#endif
  const auto cfg=loadConfig(argv[2]);
  auto number = [&](const char* key) -> double {
    return finiteNumber(cfg,key);
  };
  const Geometry geometry=inspectGeometry(cfg,array);
  const unsigned fs=geometry.fs, channels=geometry.channels;
  const unsigned referenceChannel=geometry.referenceChannel, surveillanceChannel=geometry.surveillanceChannel;
  const double fc=geometry.fc, cpi=geometry.requestedCpi;
  const unsigned samples=geometry.samples, pathCount=geometry.pathCount;
  const int32_t clutterMin=number("clutter_min"), clutterMax=number("clutter_max");
  const int64_t clutterBins=int64_t(clutterMax)-clutterMin;
  if (clutterBins <= 0 || clutterBins > UINT32_MAX)
    throw std::invalid_argument("Clutter delay range must be a non-empty half-open interval");
#ifndef BLAH2_BENCH_FAST
  if (!geometry.upstreamSafe)
    throw std::invalid_argument("UNSAFE_UPSTREAM_GEOMETRY: Doppler bins exceed the upstream scratch FFT; case must be reported, not run");
#endif
  const std::string limitText=argv[6];
  size_t limitEnd=0;
  const unsigned long parsedLimit=std::stoul(limitText,&limitEnd);
  if (limitEnd != limitText.size() || parsedLimit > UINT32_MAX)
    throw std::invalid_argument("Frame limit must be a bounded non-negative integer");
  const unsigned limit=parsedLimit;
  const unsigned workers=geometry.workers, fftThreads=geometry.fftThreads;
  if (!fftw_init_threads()) throw std::runtime_error("FFTW thread initialization failed");
  fftw_plan_with_nthreads(fftThreads);
  std::vector<std::unique_ptr<IqData>> capture, surveillance;
  std::vector<std::unique_ptr<Ambiguity>> ambiguity;
  std::vector<std::unique_ptr<WienerHopf>> filters;
  std::vector<Ambiguity*> ambPointers;
  std::vector<IqData*> surPointers, capPointers;
  auto reference=std::make_unique<IqData>(samples);
  for (unsigned i=0; i<channels; ++i) { capture.push_back(std::make_unique<IqData>(samples)); capPointers.push_back(capture.back().get()); }
  for (unsigned i=0; i<pathCount; ++i) {
    surveillance.push_back(std::make_unique<IqData>(samples));
    ambiguity.push_back(std::make_unique<Ambiguity>(geometry.delayMin, geometry.delayMax,
      geometry.dopplerMin, geometry.dopplerMax, fs, samples, geometry.roundHamming));
    filters.push_back(std::make_unique<WienerHopf>(clutterMin, clutterMax, samples));
    ambPointers.push_back(ambiguity.back().get()); surPointers.push_back(surveillance.back().get());
  }
  if (ambiguity[0]->get_nfft() != geometry.nfft ||
      ambiguity[0]->get_n_doppler_bins() != geometry.dopplerBins ||
      ambiguity[0]->get_n_delay_bins() != geometry.delayBins ||
      ambiguity[0]->get_n_corr() != geometry.nCorr)
    throw std::runtime_error("DSP geometry differs from the inspected benchmark contract");
#ifdef BLAH2_BENCH_FAST
#ifdef VECTORWARP_MIXED_AUTO
  const blah2::mixed::Shape mixedShape{fs,samples,pathCount,
    geometry.dopplerBins,geometry.delayBins,geometry.nCorr,geometry.nfft,
    uint32_t(clutterBins),clutterMin,geometry.delayMin,geometry.delayMax,
    geometry.dopplerMin,geometry.dopplerMax,
    ambiguity[0]->get_doppler_middle(),array,true};
  const bool commonCpuCpi=blah2::mixed::qualified(mixedShape);
  const char* mixedDevice=std::getenv("BLAH2_GPU_DEVICE");
  const bool forcedMixed=mode=="mixed";
  const char* pairedSetting=std::getenv("VECTORWARP_BENCH_PAIRED_INPUT");
  if(pairedSetting && std::string(pairedSetting)!="0" && std::string(pairedSetting)!="1")
    throw std::invalid_argument("VECTORWARP_BENCH_PAIRED_INPUT must be 0 or 1");
  const bool pairedInput=pairedSetting && std::string(pairedSetting)=="1";
  if(pairedInput && (!forcedMixed || channels!=2 || surveillanceChannel!=1-referenceChannel))
    throw std::invalid_argument("Paired input verification requires forced mixed and two distinct channels");
  const bool mixedAuto=blah2::mixed::autoCandidate(forcedMixed?"auto":mode,mixedShape,
    blah2::mixed::pi4Host(),mixedDevice?mixedDevice:"auto");
  if(forcedMixed&&!mixedAuto)throw std::invalid_argument("Mixed benchmark requires qualified Pi 4 geometry/device");
  const bool cpuBorrowed=commonCpuCpi && (mode=="cpu" || mixedAuto);
  blah2::mixed::AutoPolicy mixedPolicy;
  std::unique_ptr<blah2::mixed::Process> mixedWorker;
  if(mixedAuto)try{mixedWorker=std::make_unique<blah2::mixed::Process>();}
    catch(const std::exception& error){if(forcedMixed)throw;mixedPolicy.disable(error.what());}
#endif
  blah2::Acceleration acceleration(
#ifdef VECTORWARP_MIXED_AUTO
    mixedAuto?"cpu":mode,
#else
    mode,
#endif
    {ambiguity[0]->get_nfft(), ambiguity[0]->get_n_doppler_bins(),
      ambiguity[0]->get_n_delay_bins(), pathCount, int(number("delay_min")),
      samples, uint32_t(clutterBins), clutterMin},
    ambiguity[0]->get_n_corr(), fs, ambiguity[0]->get_doppler_middle(),
    std::getenv("BLAH2_GPU_DEVICE") ? std::getenv("BLAH2_GPU_DEVICE") : "auto");
  ArrayReferenceSynthesizer::Config refConfig;
  refConfig.analysisSamples=number("reference_analysis_samples"); refConfig.analysisInterval=number("reference_analysis_interval");
  refConfig.powerIterations=number("reference_power_iterations"); refConfig.covarianceSmoothing=number("reference_covariance_smoothing");
  refConfig.diagonalLoading=number("reference_diagonal_loading");
  ArrayReferenceSynthesizer synthesizer(refConfig);
  Noncoherent fusion;
  SpectrumAnalyser spectrum(samples, 2000, fc, fs);
#else
  SpectrumAnalyser spectrum(samples, 2000);
#endif
  CfarDetector1D detector(number("pfa"), number("guard_cells"), number("training_cells"), number("min_delay"), number("min_doppler"));
  Centroid centroid(number("centroid_cells"), number("centroid_cells"), 1/cpi);
  Interpolate interpolate(true, true);
  Tracker tracker(number("tracker_m"), number("tracker_n"), number("tracker_delete"),
    ambiguity[0]->get_cpi(), number("tracker_max_acceleration"), 299792458.0/fs, 299792458.0/fc);
  if (cfg.HasMember("recording_format") && !cfg["recording_format"].IsString())
    throw std::invalid_argument("recording_format must be a string");
  const std::string recordingFormat = cfg.HasMember("recording_format") ?
    cfg["recording_format"].GetString() : "mchq";
  BenchmarkReader reader(argv[1], channels, fc, recordingFormat);
  std::ofstream frames(prefix+".frames.csv"), outputs(prefix+".outputs.jsonl");
  if (!frames || !outputs) throw std::runtime_error("Cannot open benchmark outputs");
  std::fstream golden;
  if (goldenMode != "none") {
    golden.open(argv[8], std::ios::binary | (goldenMode == "write" ? std::ios::out | std::ios::trunc : std::ios::in));
    if (!golden) throw std::runtime_error("Cannot open correctness map file");
  }
  frames << "frame,phase,sample_start,sample_count,read_ms,extract_ms,reference_ms,spectrum_ms,clutter_ms,ambiguity_ms,fusion_ms,detection_ms,tracker_ms,json_ms,clutter_prepare_ms,clutter_dispatch_ms,clutter_accept_ms,clutter_gpu_executed,clutter_cpu_executed,clutter_backend,clutter_state,pipeline_ms,dsp_ms,validation_ms,backend,state,detections,tracks,map_rms_relative,map_peak_relative,fusion_rms_relative,fusion_peak_relative\n";
  frames << std::setprecision(10);
  unsigned frame=0; double totalPipeline=0, totalRead=0, totalValidation=0, worstRms=0, worstPeak=0;
  double worstFusionRms=0, worstFusionPeak=0;
  unsigned gpuFrames=0, cpuFrames=0; uint64_t jsonBytes=0;
  unsigned clutterGpuFrames=0, clutterCpuFrames=0;
  double totalClutterPrepare=0, totalClutterDispatch=0, totalClutterAccept=0;
  std::vector<double> pipelineValues;
  constexpr unsigned SteadyStartFrame=8;
#ifdef BLAH2_BENCH_FAST
  static_assert(SteadyStartFrame == blah2::Acceleration::TotalQualificationFrames);
#endif
  const double startupMs=ms(started, Clock::now());
  const char* paceSetting = std::getenv("BLAH2_BENCH_PACE");
  if (paceSetting && std::string(paceSetting) != "0" && std::string(paceSetting) != "1")
    throw std::invalid_argument("BLAH2_BENCH_PACE must be 0 or 1");
  const bool paced = paceSetting && std::string(paceSetting) == "1";
  const auto paceStart = Clock::now();
  double finalScheduleLagMs = 0;
  double peakScheduleLagMs = 0;
  std::vector<std::deque<Complex>> decoded;
  while (!limit || frame < limit) {
    // Lossless scheduled replay, not a live queue/drop simulation. Inputs are
    // released on the original sample clock; slow processing falls behind it.
    const auto release = paceStart + std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>((frame + 1) * geometry.requestedCpi));
    if (paced) std::this_thread::sleep_until(release);
    const auto beforeRead=Clock::now();
    if (!reader.read(samples, decoded)) break;
#ifdef VECTORWARP_MIXED_AUTO
    // Verification fixture only: reconstruct signed16 capture input before
    // timing, then exercise the production packed decode/IPC entry point.
    std::vector<int16_t> paired;
    if(pairedInput){
      paired.resize(uint64_t(samples)*4);
      for(uint32_t i=0;i<samples;++i)for(unsigned channel=0;channel<2;++channel){
        const auto v=decoded[channel][i];
        const double parts[]={v.real(),v.imag()};
        for(unsigned component=0;component<2;++component){
          const double value=parts[component];
          if(!std::isfinite(value)||value<-32768||value>32767||std::trunc(value)!=value)
            throw std::runtime_error("Paired verification input is not lossless signed16 IQ");
          paired[uint64_t(i)*4+channel*2+component]=static_cast<int16_t>(value);
        }
      }
    }
#endif
    const auto begin=Clock::now(); auto mark=begin;
    std::vector<double> times;
    auto tick=[&] { const auto now=Clock::now(); times.push_back(ms(mark, now)); mark=now; };
#ifdef BLAH2_BENCH_FAST
    // Keep decoded IQ immutable for the independent oracle. Model the native
    // consumer's block ownership, including synthesis's final read before the
    // same surveillance blocks pass to conditioning without another copy.
#ifdef VECTORWARP_MIXED_AUTO
    if(pairedInput)
      mixedWorker->prepare_paired_i16(paired.data(),samples,*reference,
        *surveillance[0],referenceChannel);
    else
#endif
    if (array) for (unsigned i=0; i<channels; ++i)
      capture[i]->replace(std::deque<Complex>(decoded[i]));
    else {
      reference->replace(std::deque<Complex>(decoded[referenceChannel]));
      surveillance[0]->replace(std::deque<Complex>(decoded[surveillanceChannel]));
    }
#else
    if (array) for (unsigned i=0; i<channels; ++i) fill(*capture[i], decoded[i]);
    else fill(*reference, decoded[referenceChannel]);
    for (unsigned i=0; i<pathCount; ++i) fill(*surveillance[i], decoded[array ? i : surveillanceChannel]);
#endif
    tick();
#ifdef BLAH2_BENCH_FAST
    if (array) {
      reference=synthesizer.process(capPointers);
      for (unsigned i=0; i<pathCount; ++i)
        surveillance[i]->replace(capture[i]->drain_front(samples));
    }
#endif
    tick(); spectrum.process(reference.get()); tick();
#ifdef VECTORWARP_MIXED_AUTO
    blah2::mixed::Choice mixedChoice=blah2::mixed::Choice::cpu;
    std::vector<Complex> mixedMap,mixedTail;
    bool mixedCandidateReady=false;
    if(mixedAuto && mixedWorker && !mixedPolicy.disabled()){
      mixedChoice=forcedMixed?blah2::mixed::Choice::mixed:mixedPolicy.choose(0,2*samples,0);
      if(mixedChoice!=blah2::mixed::Choice::cpu)try{
        if(pairedInput)mixedWorker->run_prepared(mixedMap,mixedTail);
        else mixedWorker->run(reference->view_data(),surveillance[0]->view_data(),
          mixedMap,mixedTail);
        mixedCandidateReady=true;
      }catch(const std::exception& error){
        if(forcedMixed)throw;
        mixedPolicy.disable(error.what());mixedWorker.reset();
        mixedChoice=blah2::mixed::Choice::cpu;
      }
    }
    bool mixedPublished=mixedChoice==blah2::mixed::Choice::mixed && mixedCandidateReady;
#endif
    bool clutterGpuExecuted=false, clutterCpuExecuted=true;
    double clutterPrepare=0, clutterDispatch=0, clutterAccept=0;
    std::string clutterBackend="cpu", clutterState="upstream";
#ifdef BLAH2_BENCH_FAST
    if(
#ifdef VECTORWARP_MIXED_AUTO
      !mixedPublished
#else
      true
#endif
      ){
    const bool clutterSuccess=acceleration.processClutter(*reference, surPointers, [&] {
      std::atomic<bool> success{true};
      paths(pathCount, workers, [&](unsigned i) {
        if (!filters[i]->process(reference.get(), surveillance[i].get()
#ifdef VECTORWARP_MIXED_AUTO
          ,cpuBorrowed
#endif
          )) success.store(false);
      });
      return success.load();
    });
    if (!clutterSuccess) throw std::runtime_error("Clutter filter rejected a frame");
    const auto& clutterTiming=acceleration.clutterTiming();
    clutterGpuExecuted=clutterTiming.gpuExecuted;
    clutterCpuExecuted=clutterTiming.cpuExecuted;
    clutterPrepare=clutterTiming.prepareMs;
    clutterDispatch=clutterTiming.dispatchMs;
    clutterAccept=clutterTiming.acceptMs;
    clutterBackend=clutterGpuExecuted ?
      (clutterCpuExecuted ? "vulkan_fft+cpu_solve+cpu_oracle" :
        "vulkan_fft+cpu_solve") : "cpu";
    clutterState=acceleration.clutterStatus().state;
#ifdef VECTORWARP_MIXED_AUTO
    if(mixedChoice==blah2::mixed::Choice::shadow&&mixedCandidateReady){
      clutterGpuExecuted=true;clutterCpuExecuted=true;
      clutterBackend="mixed_oracle+cpu";clutterState="checking";
    }
#endif
    if (mode == "gpu" && frame >= SteadyStartFrame &&
        (!clutterGpuExecuted || acceleration.clutterStatus().active != "vulkan"))
      throw std::runtime_error("FORCED_GPU_CLUTTER_FALLBACK: explicit GPU case did not execute clutter on Vulkan");
    }
#ifdef VECTORWARP_MIXED_AUTO
    else{clutterGpuExecuted=true;clutterCpuExecuted=true;
      clutterBackend="vulkan_corr68+fir50";clutterState="ready";}
#endif
#else
    paths(pathCount, workers, [&](unsigned i) {
      if (!filters[i]->process(reference.get(), surveillance[i].get()))
        throw std::runtime_error("Clutter filter rejected a frame");
    });
#endif
    tick();
    std::vector<Map<Complex>*> maps(pathCount);
    Map<Complex>* map=nullptr;
    std::string active="cpu", state="upstream";
#ifdef BLAH2_BENCH_FAST
    bool cpuExecuted=false;
    if(
#ifdef VECTORWARP_MIXED_AUTO
      mixedPublished
#else
      false
#endif
      ){
#ifdef VECTORWARP_MIXED_AUTO
      try{
        blah2::mixed::commitMap(std::move(mixedMap),*ambiguity[0]->result());
        surveillance[0]->discard_front(blah2::mixed::usedSamples);
        surveillance[0]->assign_complex(mixedTail.data(),blah2::mixed::tailSamples);
        active="vulkan+cpu";state="ready";
      }catch(const std::exception& error){
        if(forcedMixed)throw;
        if(surveillance[0]->get_length()!=samples)throw;
        mixedPolicy.disable(error.what());mixedWorker.reset();
        mixedPublished=false;mixedChoice=blah2::mixed::Choice::cpu;
        if(!filters[0]->process(reference.get(),surveillance[0].get(),cpuBorrowed))
          throw std::runtime_error("CPU clutter fallback rejected CPI");
      }
#endif
    }
    if(
#ifdef VECTORWARP_MIXED_AUTO
      !mixedPublished
#else
      true
#endif
      ){
    acceleration.process(reference->view_data(), surPointers, ambPointers, [&] {
      cpuExecuted=true;
      paths(pathCount, workers, [&](unsigned i) {
#ifdef VECTORWARP_MIXED_AUTO
        if(cpuBorrowed){
          const auto view=filters[i]->filtered_view();
          ambiguity[i]->process_borrowed(reference->view_data(),surveillance[i].get(),
            view.rotatedReference,view.filteredSurveillance,view.samples,view.delayMin);
        }else
#endif
        ambiguity[i]->process(reference->view_data(), surveillance[i].get());
      });
    });
    // The AUTO policy can transition to CPU after accepting a GPU result. The
    // callback, not the post-frame policy status, identifies this frame's DSP.
    active=cpuExecuted ? "cpu" : "vulkan"; state=acceleration.status().state;
    }
#ifdef VECTORWARP_MIXED_AUTO
    if(mixedChoice==blah2::mixed::Choice::shadow&&mixedCandidateReady&&
        !mixedPolicy.disabled()){
      const auto error=blah2::mixed::compareMap(mixedMap,*ambiguity[0]->result());
      mixedPolicy.shadow(error.rms,error.peak);
      if(mixedPolicy.disabled())mixedWorker.reset();
    }
#endif
    if (mode == "gpu" && frame >= SteadyStartFrame && active != "vulkan")
      throw std::runtime_error("FORCED_GPU_FALLBACK: explicit GPU case did not execute on Vulkan");
    for (unsigned i=0; i<pathCount; ++i) maps[i]=ambiguity[i]->result();
    tick();
    auto fused=fusion.process(maps); map=fused.get();
    map->set_metrics(); tick();
#else
    map=ambiguity[0]->process(reference.get(), surveillance[0].get()); maps[0]=map;
    map->set_metrics(); tick(); tick();
#endif
    auto first=detector.process(map); auto second=centroid.process(first.get());
    auto detections=interpolate.process(second.get(), map); tick();
    // Fixed sample-derived clock keeps tracking independent of replay speed.
    const uint64_t timestamp=1700000000000ULL+std::llround(frame*cpi*1000);
    auto tracks=tracker.process(detections.get(), timestamp); tick();
    auto iqJson=reference->to_json(timestamp);
#ifdef BLAH2_BENCH_FAST
    auto mapJson=map->to_json_km(timestamp, fs);
#else
    auto mapJson=map->delay_bin_to_km(map->to_json(timestamp), fs);
#endif
    auto detectionJson=detections->to_json(timestamp); auto trackJson=tracks->to_json(timestamp);
    jsonBytes+=iqJson.size()+mapJson.size()+detectionJson.size()+trackJson.size(); tick();
    const double pipeline=ms(begin, mark), readMs=ms(beforeRead, begin);
#ifdef VECTORWARP_MIXED_AUTO
    if(mixedAuto&&!forcedMixed){
      if(!mixedPolicy.disabled()&&mixedChoice!=blah2::mixed::Choice::shadow)
        mixedPolicy.complete(mixedChoice,pipeline);
      if(mixedPolicy.disabled() || (mixedPolicy.trials()==6&&!mixedPolicy.selected()))
        mixedWorker.reset();
      state=mixedPolicy.disabled()?"fallback":
        mixedPolicy.trials()==6?"ready":"checking";
    }
#endif
    double errorPower=0, signalPower=0, maxError=0, maxSignal=0;
    for (unsigned ch=0; ch<pathCount; ++ch) {
      const uint32_t shape[4]={frame,ch,maps[ch]->get_nRows(),maps[ch]->get_nCols()};
      if (goldenMode == "write") golden.write(reinterpret_cast<const char*>(shape), sizeof(shape));
      if (goldenMode == "compare") {
        uint32_t expected[4]{}; golden.read(reinterpret_cast<char*>(expected), sizeof(expected));
        if (!golden || !std::equal(shape, shape+4, expected)) throw std::runtime_error("Correctness map dimensions/frame differ");
      }
      for (const auto& row : maps[ch]->data) {
        std::vector<std::complex<float>> values(row.begin(), row.end());
        if (goldenMode == "write") golden.write(reinterpret_cast<const char*>(values.data()), values.size()*sizeof(values[0]));
        if (goldenMode == "compare") {
          std::vector<std::complex<float>> expected(values.size());
          golden.read(reinterpret_cast<char*>(expected.data()), expected.size()*sizeof(expected[0]));
          if (!golden) throw std::runtime_error("Truncated correctness maps");
          for (size_t i=0; i<values.size(); ++i) {
            const Complex v=values[i], e=expected[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag()) || !std::isfinite(e.real()) || !std::isfinite(e.imag()))
              throw std::runtime_error("Non-finite correctness map");
            errorPower+=std::norm(v-e); signalPower+=std::norm(e);
            maxError=std::max(maxError, std::abs(v-e)); maxSignal=std::max(maxSignal,std::abs(e));
          }
        }
      }
    }
    const double rms=std::sqrt(errorPower/std::max(signalPower,1e-30)), peak=maxError/std::max(maxSignal,1e-30);
    worstRms=std::max(worstRms,rms); worstPeak=std::max(worstPeak,peak);
    if (rms > 1e-4 || peak > 1e-4) {
      std::ostringstream reason;
      reason << "CPU/GPU complex map agreement failed frame=" << frame
        << " rms=" << rms << " peak=" << peak << " ambiguity_backend=" << active
        << " ambiguity_state=" << state << " clutter_backend=" << clutterBackend
        << " clutter_state=" << clutterState;
      throw std::runtime_error(reason.str());
    }
    double fusionErrorPower=0, fusionSignalPower=0, fusionMaxError=0, fusionMaxSignal=0;
#ifdef BLAH2_BENCH_FAST
    for (size_t d=0; d<map->data.size(); ++d)
      for (size_t r=0; r<map->data[d].size(); ++r) {
        double power=0;
        for (const auto* channelMap : maps) power+=std::norm(channelMap->data[d][r]);
        const Complex expected={std::sqrt(power/maps.size()),0};
        const Complex actual=map->data[d][r];
        fusionErrorPower+=std::norm(actual-expected); fusionSignalPower+=std::norm(expected);
        fusionMaxError=std::max(fusionMaxError,std::abs(actual-expected));
        fusionMaxSignal=std::max(fusionMaxSignal,std::abs(expected));
      }
#endif
    const double fusionRms=std::sqrt(fusionErrorPower/std::max(fusionSignalPower,1e-30));
    const double fusionPeak=fusionMaxError/std::max(fusionMaxSignal,1e-30);
    worstFusionRms=std::max(worstFusionRms,fusionRms);
    worstFusionPeak=std::max(worstFusionPeak,fusionPeak);
    if (fusionRms > 1e-12 || fusionPeak > 1e-12)
      throw std::runtime_error("Post-fusion magnitude map is inconsistent with complex channel maps");
    const double validationMs=ms(mark,Clock::now());
    const char* phase=frame >= SteadyStartFrame ? "steady" :
      (mode == "cpu" ? (frame == 0 ? "cold" : "warmup") :
       mode == "mixed" ? "mixed_warmup" : "gpu_qualification");
    frames << frame << ',' << phase << ',' << uint64_t(frame)*samples << ',' << samples << ',' << readMs;
    for (double value : times) frames << ',' << value;
    frames << ',' << clutterPrepare << ',' << clutterDispatch << ',' << clutterAccept
      << ',' << (clutterGpuExecuted?1:0) << ',' << (clutterCpuExecuted?1:0)
      << ',' << clutterBackend << ',' << clutterState;
    frames << ',' << pipeline << ',' << pipeline << ',' << validationMs << ',' << active << ',' << state << ','
      << detections->get_nDetections() << ',' << tracks->get_n() << ',' << rms << ',' << peak
      << ',' << fusionRms << ',' << fusionPeak << '\n';
    outputs << "{\"frame\":" << frame << ",\"detections\":" << detectionJson << ",\"tracks\":" << trackJson << ",\"iq\":" << iqJson << "}\n";
    if (!frames || !outputs || (goldenMode == "write" && !golden)) throw std::runtime_error("Benchmark output write failed");
    if (active == "vulkan" || active == "vulkan+cpu") ++gpuFrames;
    else if (active == "cpu") ++cpuFrames;
    else throw std::runtime_error("Benchmark reported an unknown processing backend");
    if (clutterGpuExecuted) ++clutterGpuFrames;
    if (clutterCpuExecuted) ++clutterCpuFrames;
    totalClutterPrepare += clutterPrepare;
    totalClutterDispatch += clutterDispatch;
    totalClutterAccept += clutterAccept;
    pipelineValues.push_back(pipeline);
    if (paced) {
      finalScheduleLagMs = std::max(0.0, ms(release, Clock::now()) - geometry.requestedCpi * 1000);
      peakScheduleLagMs = std::max(peakScheduleLagMs, finalScheduleLagMs);
    }
    totalPipeline+=pipeline; totalRead+=readMs; totalValidation+=validationMs; ++frame;
    if (frame % 50 == 0) std::cerr << "frames=" << frame << " backend=" << active << " processing_ms=" << totalPipeline/frame << '\n';
  }
  if (!frame) throw std::runtime_error("Recording has no complete frames");
#ifdef VECTORWARP_MIXED_AUTO
  if(forcedMixed && (gpuFrames!=frame || cpuFrames))
    throw std::runtime_error("FORCED_MIXED_FALLBACK: mixed benchmark must publish every frame through the worker");
#endif
  if (mode == "gpu" && (frame <= SteadyStartFrame || gpuFrames != frame-SteadyStartFrame))
    throw std::runtime_error("FORCED_GPU_UNQUALIFIED: explicit GPU case has no complete post-qualification Vulkan result");
  if (goldenMode == "compare" && !limit && golden.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Correctness file has extra frames");
  rusage usage{}; getrusage(RUSAGE_SELF, &usage);
  const std::vector<double> steady(pipelineValues.begin()+std::min<size_t>(SteadyStartFrame,pipelineValues.size()),
    pipelineValues.end());
  const auto mean=[](const std::vector<double>& values) {
    return values.empty() ? 0 : std::accumulate(values.begin(),values.end(),0.0)/values.size();
  };
  const auto countDeadline=[&](const std::vector<double>& values) {
    return std::count_if(values.begin(),values.end(),[&](double value) { return value > 1000*geometry.requestedCpi; });
  };
  std::ofstream summary(prefix+".summary.json");
  summary << std::setprecision(12) << "{\"schema_version\":2,\"engine\":\"" << Engine
    << "\",\"profile\":\"" << profile << "\",\"requested_mode\":\"" << mode
    << "\",\"frames\":" << frame << ",\"samples_per_channel\":" << reader.samples
    << ",\"tail_samples\":" << reader.tailSamples << ",\"startup_ms\":" << startupMs
    << ",\"initialization_ms\":" << startupMs
    << ",\"pipeline_ms\":" << totalPipeline << ",\"dsp_ms\":" << totalPipeline
    << ",\"first_dsp_ms\":" << pipelineValues.front()
    << ",\"dsp_mean_ms\":" << mean(pipelineValues)
    << ",\"dsp_p95_ms\":" << percentile(pipelineValues,.95)
    << ",\"dsp_p99_ms\":" << percentile(pipelineValues,.99)
    << ",\"dsp_max_ms\":" << *std::max_element(pipelineValues.begin(),pipelineValues.end())
    << ",\"dsp_deadline_misses\":" << countDeadline(pipelineValues)
    << ",\"steady_start_frame\":" << SteadyStartFrame
    << ",\"steady_frames\":" << steady.size()
    << ",\"steady_dsp_mean_ms\":" << jsonNumberOrNull(steady,mean(steady))
    << ",\"steady_dsp_p95_ms\":" << jsonNumberOrNull(steady,percentile(steady,.95))
    << ",\"steady_dsp_p99_ms\":" << jsonNumberOrNull(steady,percentile(steady,.99))
    << ",\"steady_dsp_max_ms\":" << jsonNumberOrNull(steady,steady.empty()?0:*std::max_element(steady.begin(),steady.end()))
    << ",\"steady_dsp_deadline_misses\":" << countDeadline(steady)
    << ",\"read_ms\":" << totalRead
    << ",\"sample_clock_paced\":" << (paced ? "true" : "false")
    << ",\"final_schedule_lag_ms\":" << finalScheduleLagMs
    << ",\"peak_schedule_lag_ms\":" << peakScheduleLagMs
    << ",\"validation_ms\":" << totalValidation << ",\"wall_ms\":" << ms(started,Clock::now())
    << ",\"gpu_frames\":" << gpuFrames << ",\"cpu_frames\":" << cpuFrames
    << ",\"clutter_gpu_frames\":" << clutterGpuFrames
    << ",\"clutter_cpu_solve_frames\":" << clutterGpuFrames
    << ",\"clutter_cpu_frames\":" << clutterCpuFrames
    << ",\"clutter_prepare_ms\":" << totalClutterPrepare
    << ",\"clutter_dispatch_ms\":" << totalClutterDispatch
    << ",\"clutter_accept_ms\":" << totalClutterAccept
    << ",\"forced_gpu_verified\":" << (mode=="gpu"?"true":"null")
    << ",\"forced_mixed_verified\":" << (mode=="mixed"?"true":"null")
    << ",\"workers\":" << workers << ",\"fft_threads\":" << fftThreads
    << ",\"round_hamming\":" << (geometry.roundHamming?"true":"false")
    << ",\"requested_cpi_ms\":" << geometry.requestedCpi*1000
    << ",\"effective_cpi_ms\":" << geometry.effectiveCpi*1000
    << ",\"input_samples_per_frame\":" << geometry.samples
    << ",\"delay_bins\":" << geometry.delayBins << ",\"doppler_bins\":" << geometry.dopplerBins
    << ",\"n_corr\":" << geometry.nCorr << ",\"range_fft\":" << geometry.nfft
    << ",\"peak_parent_rss_kib\":" << usage.ru_maxrss << ",\"json_bytes\":" << jsonBytes
    << ",\"detection_input_map\":\"" << (std::string(Engine)=="fast"?"noncoherent_magnitude":"complex_upstream") << "\""
    << ",\"map_rms_relative\":" << worstRms << ",\"map_peak_relative\":" << worstPeak
    << ",\"fusion_rms_relative\":" << worstFusionRms
    << ",\"fusion_peak_relative\":" << worstFusionPeak
#ifdef VECTORWARP_MIXED_AUTO
    << ",\"paired_input_verified\":" << (pairedInput?"true":"false")
    << ",\"mixed_auto_eligible\":" << (mixedAuto?"true":"false")
    << ",\"mixed_auto_selected\":" << (mixedAuto&&mixedPolicy.selected()?"true":"false")
    << ",\"mixed_auto_shadows\":" << (mixedAuto?mixedPolicy.shadowPasses():0)
    << ",\"mixed_auto_trials\":" << (mixedAuto?mixedPolicy.trials():0)
    << ",\"mixed_cpu_cpi_ms\":" << (mixedAuto?mixedPolicy.cpuMs():0)
    << ",\"mixed_gpu_cpi_ms\":" << (mixedAuto?mixedPolicy.mixedMs():0)
#endif
    << "}\n";
  if (!summary) throw std::runtime_error("Cannot write benchmark summary");
  return 0;
} catch (const std::exception& error) { std::cerr << "BENCHMARK FAILED: " << error.what() << '\n'; return 1; }
