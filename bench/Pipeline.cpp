// Finite replay instrumentation, compiled against unchanged upstream or fork DSP.
// Network/UI transport and hardware acquisition are deliberately not benchmarked.
#include "MchqReader.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/clutter/WienerHopf.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/tracker/Tracker.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include <rapidjson/document.h>
#include <chrono>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <sys/resource.h>
#ifdef BLAH2_BENCH_FAST
#include "process/ambiguity/Acceleration.h"
#include "process/utility/ProcessingThreads.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/fusion/Noncoherent.h"
#endif

using Complex = std::complex<double>;
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b-a).count();
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
  if (argc != 9) throw std::invalid_argument(
    "Usage: bench-{upstream,fast} input.mchq profile.json output-prefix pair|array cpu|auto|gpu max-frames write|compare|none golden.maps");
  const auto started = Clock::now();
  const std::string prefix=argv[3], profile=argv[4], mode=argv[5], goldenMode=argv[7];
  const bool array = profile == "array";
  if ((!array && profile != "pair") || (mode != "cpu" && mode != "auto" && mode != "gpu") ||
      (goldenMode != "write" && goldenMode != "compare" && goldenMode != "none"))
    throw std::invalid_argument("Invalid benchmark mode");
#ifndef BLAH2_BENCH_FAST
  if (array || mode != "cpu") throw std::invalid_argument("Upstream supports only the physical two-channel CPU comparison");
#endif
  std::ifstream config(argv[2]);
  std::stringstream text; text << config.rdbuf();
  rapidjson::Document cfg;
  cfg.Parse(text.str().c_str());
  if (cfg.HasParseError() || !cfg.IsObject()) throw std::runtime_error("Invalid benchmark JSON");
  auto number = [&](const char* key) -> double {
    if (!cfg.HasMember(key) || !cfg[key].IsNumber()) throw std::runtime_error(std::string("Missing setting: ")+key);
    return cfg[key].GetDouble();
  };
  const unsigned fs=number("sample_rate"), channels=number("channels");
  const unsigned referenceChannel=number("reference_channel"), surveillanceChannel=number("surveillance_channel");
  const double fc=number("frequency"), cpi=number("cpi");
  const unsigned samples=std::llround(fs*cpi), pathCount=array ? channels : 1;
  if (channels < 2 || channels > 8 || referenceChannel >= channels || surveillanceChannel >= channels ||
      referenceChannel == surveillanceChannel || !samples || samples > 10000000)
    throw std::invalid_argument("Invalid benchmark input geometry");
  const unsigned limit=std::stoul(argv[6]);
  unsigned workers=1, fftThreads=4;
#ifdef BLAH2_BENCH_FAST
  const auto threads=blah2::choose_processing_threads(pathCount, 0, 0, blah2::available_cpu_threads());
  workers=threads.workers; fftThreads=threads.fftThreads;
#endif
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
    ambiguity.push_back(std::make_unique<Ambiguity>(number("delay_min"), number("delay_max"),
      number("doppler_min"), number("doppler_max"), fs, samples, true));
    filters.push_back(std::make_unique<WienerHopf>(number("clutter_min"), number("clutter_max"), samples));
    ambPointers.push_back(ambiguity.back().get()); surPointers.push_back(surveillance.back().get());
  }
#ifdef BLAH2_BENCH_FAST
  blah2::Acceleration acceleration(mode,
    {ambiguity[0]->get_nfft(), ambiguity[0]->get_n_doppler_bins(), ambiguity[0]->get_n_delay_bins(), pathCount, int(number("delay_min"))},
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
  MchqReader reader(argv[1], channels, fc);
  std::ofstream frames(prefix+".frames.csv"), outputs(prefix+".outputs.jsonl");
  if (!frames || !outputs) throw std::runtime_error("Cannot open benchmark outputs");
  std::fstream golden;
  if (goldenMode != "none") {
    golden.open(argv[8], std::ios::binary | (goldenMode == "write" ? std::ios::out | std::ios::trunc : std::ios::in));
    if (!golden) throw std::runtime_error("Cannot open correctness map file");
  }
  frames << "frame,sample_start,sample_count,read_ms,extract_ms,reference_ms,spectrum_ms,clutter_ms,ambiguity_ms,detection_ms,tracker_ms,json_ms,pipeline_ms,validation_ms,backend,state,detections,tracks,map_rms_relative,map_peak_relative\n";
  frames << std::setprecision(10);
  unsigned frame=0; double totalPipeline=0, totalRead=0, totalValidation=0, worstRms=0, worstPeak=0;
  unsigned gpuFrames=0; uint64_t jsonBytes=0;
  const double startupMs=ms(started, Clock::now());
  std::vector<std::deque<Complex>> decoded;
  while (!limit || frame < limit) {
    const auto beforeRead=Clock::now();
    if (!reader.read(samples, decoded)) break;
    const auto begin=Clock::now(); auto mark=begin;
    std::vector<double> times;
    auto tick=[&] { const auto now=Clock::now(); times.push_back(ms(mark, now)); mark=now; };
    if (array) for (unsigned i=0; i<channels; ++i) fill(*capture[i], decoded[i]);
    else fill(*reference, decoded[referenceChannel]);
    for (unsigned i=0; i<pathCount; ++i) fill(*surveillance[i], decoded[array ? i : surveillanceChannel]);
    tick();
#ifdef BLAH2_BENCH_FAST
    if (array) reference=synthesizer.process(capPointers);
#endif
    tick(); spectrum.process(reference.get()); tick();
    paths(pathCount, workers, [&](unsigned i) {
      if (!filters[i]->process(reference.get(), surveillance[i].get())) throw std::runtime_error("Clutter filter rejected a frame");
    });
    tick();
    std::vector<Map<Complex>*> maps(pathCount);
    Map<Complex>* map=nullptr;
    std::string active="cpu", state="upstream";
#ifdef BLAH2_BENCH_FAST
    acceleration.process(reference->view_data(), surPointers, ambPointers, [&] {
      paths(pathCount, workers, [&](unsigned i) { ambiguity[i]->process(reference->view_data(), surveillance[i].get()); });
    });
    active=frame < 3 ? "cpu" : acceleration.status().active; state=acceleration.status().state;
    for (unsigned i=0; i<pathCount; ++i) { maps[i]=ambiguity[i]->result(); maps[i]->set_metrics(); }
    auto fused=fusion.process(maps); map=fused.get();
#else
    map=ambiguity[0]->process(reference.get(), surveillance[0].get()); maps[0]=map;
#endif
    map->set_metrics(); tick();
    auto first=detector.process(map); auto second=centroid.process(first.get());
    auto detections=interpolate.process(second.get(), map); tick();
    // Fixed sample-derived clock keeps tracking independent of replay speed.
    const uint64_t timestamp=1700000000000ULL+std::llround(frame*cpi*1000);
    auto tracks=tracker.process(detections.get(), timestamp); tick();
    auto iqJson=reference->to_json(timestamp);
    auto mapJson=map->delay_bin_to_km(map->to_json(timestamp), fs);
    auto detectionJson=detections->to_json(timestamp); auto trackJson=tracks->to_json(timestamp);
    jsonBytes+=iqJson.size()+mapJson.size()+detectionJson.size()+trackJson.size(); tick();
    const double pipeline=ms(begin, mark), readMs=ms(beforeRead, begin);
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
    if (rms > 1e-4 || peak > 1e-4) throw std::runtime_error("CPU/GPU complex map agreement failed");
    const double validationMs=ms(mark,Clock::now());
    frames << frame << ',' << uint64_t(frame)*samples << ',' << samples << ',' << readMs;
    for (double value : times) frames << ',' << value;
    frames << ',' << pipeline << ',' << validationMs << ',' << active << ',' << state << ','
      << detections->get_nDetections() << ',' << tracks->get_n() << ',' << rms << ',' << peak << '\n';
    outputs << "{\"frame\":" << frame << ",\"detections\":" << detectionJson << ",\"tracks\":" << trackJson << "}\n";
    if (!frames || !outputs || (goldenMode == "write" && !golden)) throw std::runtime_error("Benchmark output write failed");
    if (active == "vulkan") ++gpuFrames;
    totalPipeline+=pipeline; totalRead+=readMs; totalValidation+=validationMs; ++frame;
    if (frame % 50 == 0) std::cerr << "frames=" << frame << " backend=" << active << " processing_ms=" << totalPipeline/frame << '\n';
  }
  if (!frame) throw std::runtime_error("Recording has no complete frames");
  if (goldenMode == "compare" && !limit && golden.peek() != std::char_traits<char>::eof())
    throw std::runtime_error("Correctness file has extra frames");
  rusage usage{}; getrusage(RUSAGE_SELF, &usage);
  std::ofstream summary(prefix+".summary.json");
  summary << std::setprecision(12) << "{\"frames\":" << frame << ",\"samples_per_channel\":" << reader.samples
    << ",\"tail_samples\":" << reader.tailSamples << ",\"startup_ms\":" << startupMs
    << ",\"pipeline_ms\":" << totalPipeline << ",\"read_ms\":" << totalRead
    << ",\"validation_ms\":" << totalValidation << ",\"wall_ms\":" << ms(started,Clock::now())
    << ",\"gpu_frames\":" << gpuFrames << ",\"workers\":" << workers << ",\"fft_threads\":" << fftThreads
    << ",\"peak_parent_rss_kib\":" << usage.ru_maxrss << ",\"json_bytes\":" << jsonBytes
    << ",\"map_rms_relative\":" << worstRms << ",\"map_peak_relative\":" << worstPeak << "}\n";
  if (!summary) throw std::runtime_error("Cannot write benchmark summary");
  return 0;
} catch (const std::exception& error) { std::cerr << "BENCHMARK FAILED: " << error.what() << '\n'; return 1; }
