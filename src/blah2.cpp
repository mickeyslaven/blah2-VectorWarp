/// @file blah2.cpp
/// @brief A real-time radar.
/// @author 30hours

#include "capture/Capture.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/meta/Timing.h"
#include "data/Track.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/ambiguity/Acceleration.h"
#include "process/clutter/WienerHopf.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/fusion/Noncoherent.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include "process/tracker/Tracker.h"
#include "process/utility/Socket.h"
#include "process/utility/ProcessingThreads.h"
#include "data/meta/Constants.h"

#include "data/Yaml.h"
#include <c4/format.hpp> // needed for the examples below
#include <sys/types.h>
#include <getopt.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <sys/time.h>
#include <signal.h>
#include <atomic>
#include <memory>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <future>
#include <limits>

volatile sig_atomic_t STOP_SIGNAL = 0;
std::unique_ptr<Socket> socket_map;
std::unique_ptr<Socket> socket_detection;
std::unique_ptr<Socket> socket_track;
std::unique_ptr<Socket> socket_timestamp;
std::unique_ptr<Socket> socket_timing;
std::unique_ptr<Socket> socket_iqdata;

void signal_callback_handler(int signum);
void getopt_print_help();
std::string getopt_process(int argc, char **argv);
std::string ryml_get_file(const char *filename);
uint64_t current_time_ms();
uint64_t current_time_us();
void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name);

class AtomicFlagGuard
{
  std::atomic<bool>& flag;
public:
  explicit AtomicFlagGuard(std::atomic<bool>& value) : flag(value) { flag.store(true); }
  ~AtomicFlagGuard() { flag.store(false); }
};

class BufferLocks
{
  const std::vector<IqData*>& buffers;
  bool locked = true;
public:
  explicit BufferLocks(const std::vector<IqData*>& value) : buffers(value)
  {
    for (auto* buffer : buffers) buffer->lock();
  }
  ~BufferLocks() { unlock(); }
  void unlock()
  {
    if (!locked) return;
    for (auto it = buffers.rbegin(); it != buffers.rend(); ++it) (*it)->unlock();
    locked = false;
  }
};

class ReplayFrameGuard
{
  bool replay;
  uint64_t& frames;
public:
  ReplayFrameGuard(bool isReplay, uint64_t& value) : replay(isReplay), frames(value) {}
  ~ReplayFrameGuard() { if (replay) ++frames; }
};

template <typename Work>
bool process_paths(std::size_t pathCount, std::size_t workerCount, Work work)
{
  bool success = true;
  workerCount = std::max<std::size_t>(1,
    std::min(workerCount, pathCount));
  if (workerCount == 1)
  {
    for (std::size_t path = 0; path < pathCount; path++)
      success = work(path) && success;
    return success;
  }
  for (std::size_t first = 0; first < pathCount; first += workerCount)
  {
    std::vector<std::future<bool>> tasks;
    const std::size_t last = std::min(first + workerCount, pathCount);
    for (std::size_t path = first; path < last; path++)
      tasks.push_back(std::async(std::launch::async, work, path));
    for (auto& task : tasks) success = task.get() && success;
  }
  return success;
}

int main(int argc, char **argv)
try
{
  // This read-only command checks adapter/runtime ABI without creating a
  // receiver, opening hardware, reading a configuration or starting a service.
  if (argc == 2 && std::string(argv[1]) == "--receiver-status") {
    std::cout << blah2::receiver_module_status_json() << '\n';
    return 0;
  }
  // input handling
  signal(SIGTERM, signal_callback_handler);
  signal(SIGINT, signal_callback_handler);
  std::string file = getopt_process(argc, argv);
  std::ifstream filePath(file);
  if (!filePath.is_open())
  {
    std::cout << "Error: Config file does not exist." << "\n";
    exit(1);
  }

  // config handling
  std::string contents = ryml_get_file(file.c_str());
  ryml::Tree tree = ryml::parse_in_arena(ryml::to_csubstr(contents));

  // set up capture
  uint32_t fs = 0, fc = 0;
  uint16_t port_capture = 0;
  std::string type, path, replayFile, ip_capture;
  bool saveIq, state, loop;
  std::string replayFormat = "auto";
  uint32_t legacyReplayBlockSamples = 0;
  tree["capture"]["fs"] >> fs;
  tree["capture"]["fc"] >> fc;
  tree["capture"]["device"]["type"] >> type;
  tree["save"]["iq"] >> saveIq;
  tree["save"]["path"] >> path;
  tree["capture"]["replay"]["state"] >> state;
  tree["capture"]["replay"]["loop"] >> loop;
  tree["capture"]["replay"]["file"] >> replayFile;
  if (tree["capture"]["replay"].has_child("format"))
    tree["capture"]["replay"]["format"] >> replayFormat;
  if (tree["capture"]["replay"].has_child("legacy_block_samples"))
    tree["capture"]["replay"]["legacy_block_samples"] >> legacyReplayBlockSamples;
  tree["network"]["ip"] >> ip_capture;
  tree["network"]["ports"]["api"] >> port_capture;
  if (fs == 0 || fc == 0 || port_capture == 0)
  {
    std::cerr << "Invalid configuration: sample rate, centre frequency and API port must be positive.\n";
    return 1;
  }

  // set up socket
  sleep(2);
  uint16_t port_map, port_detection, port_timestamp, 
    port_timing, port_iqdata, port_track;
  std::string ip;
  tree["network"]["ports"]["map"] >> port_map;
  tree["network"]["ports"]["detection"] >> port_detection;
  tree["network"]["ports"]["track"] >> port_track;
  tree["network"]["ports"]["timestamp"] >> port_timestamp;
  tree["network"]["ports"]["timing"] >> port_timing;
  tree["network"]["ports"]["iqdata"] >> port_iqdata;
  tree["network"]["ip"] >> ip;
  
  try {
    socket_map = std::make_unique<Socket>(ip, port_map);
    socket_detection = std::make_unique<Socket>(ip, port_detection);
    socket_track = std::make_unique<Socket>(ip, port_track);
    socket_timestamp = std::make_unique<Socket>(ip, port_timestamp);
    socket_timing = std::make_unique<Socket>(ip, port_timing);
    socket_iqdata = std::make_unique<Socket>(ip, port_iqdata);
  } catch (const std::exception& e) {
    std::cerr << "Failed to initialize socket connections: " << e.what() << "\n";
    std::cerr << "Make sure the server at " << ip << " is reachable." << "\n";
    return 1;
  }

  // set up FFTW multithreading
  if (fftw_init_threads() == 0)
  {
    std::cout << "Error in FFTW multithreading." << "\n";
    return -1;
  }
  // Configure channel roles. Other devices keep their usual two-channel path.
  uint32_t referenceChannel = 0;
  std::vector<uint32_t> surveillanceChannels{1};
  std::vector<uint32_t> referenceChannels;
  std::size_t captureChannelCount = 2;
  std::string referenceMode = "dedicated";
  if (type == "Kraken")
  {
    auto device = tree["capture"]["device"];
    if (!device.has_child("channel_count"))
      throw std::invalid_argument("Kraken channel_count is required");
    device["channel_count"] >> captureChannelCount;
    if (device.has_child("reference_channel"))
      device["reference_channel"] >> referenceChannel;
    if (tree["process"].has_child("reference_synthesis"))
    {
      auto reference = tree["process"]["reference_synthesis"];
      if (reference.has_child("mode")) reference["mode"] >> referenceMode;
      if (reference.has_child("channels"))
        for (auto child : reference["channels"].children())
        {
          uint32_t channel = std::numeric_limits<uint32_t>::max();
          child >> channel;
          referenceChannels.push_back(channel);
        }
    }
    surveillanceChannels.clear();
    if (device.has_child("surveillance_channels"))
      for (auto child : device["surveillance_channels"].children())
      {
        uint32_t channel = std::numeric_limits<uint32_t>::max();
        child >> channel;
        surveillanceChannels.push_back(channel);
      }
  }
  if (referenceMode != "dedicated" && referenceMode != "array_eigenbeam")
    throw std::invalid_argument("Unknown reference-synthesis mode");
  const bool arrayReference = referenceMode == "array_eigenbeam";
  if (captureChannelCount < 2 || captureChannelCount > 8 ||
      referenceChannel >= captureChannelCount)
    throw std::invalid_argument("Invalid capture channel configuration");
  if (referenceChannels.empty() && arrayReference)
    for (std::size_t channel = 0; channel < captureChannelCount; channel++)
      referenceChannels.push_back(static_cast<uint32_t>(channel));
  if (surveillanceChannels.empty())
    for (std::size_t channel = 0; channel < captureChannelCount; channel++)
      if (arrayReference || channel != referenceChannel)
        surveillanceChannels.push_back(static_cast<uint32_t>(channel));
  std::sort(surveillanceChannels.begin(), surveillanceChannels.end());
  if (std::adjacent_find(surveillanceChannels.begin(),
      surveillanceChannels.end()) != surveillanceChannels.end())
    throw std::invalid_argument("Surveillance channels must be unique");
  for (uint32_t channel : surveillanceChannels)
    if (channel >= captureChannelCount ||
        (!arrayReference && channel == referenceChannel))
      throw std::invalid_argument("Invalid surveillance channel");
  std::sort(referenceChannels.begin(), referenceChannels.end());
  if (arrayReference && (referenceChannels.size() < 2 ||
      std::adjacent_find(referenceChannels.begin(), referenceChannels.end()) !=
        referenceChannels.end()))
    throw std::invalid_argument("Array-reference channels must be unique");
  for (uint32_t channel : referenceChannels)
    if (channel >= captureChannelCount)
      throw std::invalid_argument("Invalid array-reference channel");

  // Each surveillance path owns its processing state and working buffers.
  // Zero selects automatic sizing; omitted values retain legacy behavior.
  uint32_t configuredWorkers = 1;
  uint32_t fftThreads = type == "Kraken" ? 1 : 4;
  if (tree["process"].has_child("performance") &&
      tree["process"]["performance"].has_child("surveillance_workers"))
    tree["process"]["performance"]["surveillance_workers"] >>
      configuredWorkers;
  if (tree["process"].has_child("performance") &&
      tree["process"]["performance"].has_child("fft_threads"))
    tree["process"]["performance"]["fft_threads"] >> fftThreads;
  const auto threadPlan = blah2::choose_processing_threads(surveillanceChannels.size(),
    configuredWorkers, fftThreads, blah2::available_cpu_threads());
  const std::size_t surveillanceWorkers = threadPlan.workers;
  fftThreads = static_cast<uint32_t>(threadPlan.fftThreads);
  fftw_plan_with_nthreads(fftThreads);
  std::cout << "Surveillance paths=" << surveillanceChannels.size()
    << " workers=" << surveillanceWorkers << " fft_threads=" << fftThreads
    << " available_cpus=" << threadPlan.availableCpus
    << "\n";

  Capture *capture = new Capture(type, fs, fc, path);
  if (state) capture->set_replay(loop, replayFile, replayFormat,
    legacyReplayBlockSamples);

  // Keep one bounded capture queue per coherent input.
  double tCpi, tBuffer;
  tree["process"]["data"]["cpi"] >> tCpi;
  tree["process"]["data"]["buffer"] >> tBuffer;
  const uint32_t captureBufferSamples = static_cast<uint32_t>(tCpi*tBuffer*fs);
  std::vector<std::unique_ptr<IqData>> captureStorage;
  std::vector<IqData *> captureBuffers;
  for (std::size_t channel = 0; channel < captureChannelCount; channel++)
  {
    captureStorage.push_back(std::make_unique<IqData>(captureBufferSamples));
    captureBuffers.push_back(captureStorage.back().get());
  }

  // set up process CPI
  uint32_t nSamples = fs * tCpi;
  auto referenceData = std::make_unique<IqData>(nSamples);
  std::vector<std::unique_ptr<IqData>> captureData;
  for (std::size_t channel = 0; channel < captureChannelCount; channel++)
    captureData.push_back(std::make_unique<IqData>(nSamples));

  std::vector<std::unique_ptr<IqData>> surveillanceData;
  for (std::size_t pathIndex = 0;
       pathIndex < surveillanceChannels.size(); pathIndex++)
  {
    surveillanceData.push_back(std::make_unique<IqData>(nSamples));
  }
  std::unique_ptr<Map<std::complex<double>>> map;
  std::unique_ptr<Detection> detection;
  std::unique_ptr<Detection> detection1;
  std::unique_ptr<Detection> detection2;
  std::unique_ptr<Track> track;

  // set up process ambiguity
  int32_t delayMin, delayMax;
  int32_t dopplerMin, dopplerMax;
  bool roundHamming = true;
  tree["process"]["ambiguity"]["delayMin"] >> delayMin;
  tree["process"]["ambiguity"]["delayMax"] >> delayMax;
  tree["process"]["ambiguity"]["dopplerMin"] >> dopplerMin;
  tree["process"]["ambiguity"]["dopplerMax"] >> dopplerMax;
  std::vector<std::unique_ptr<Ambiguity>> ambiguity;
  for (std::size_t pathIndex = 0;
       pathIndex < surveillanceChannels.size(); pathIndex++)
    ambiguity.push_back(std::make_unique<Ambiguity>(delayMin, delayMax,
      dopplerMin, dopplerMax, fs, nSamples, roundHamming));

  // Set up clutter before the shared accelerator so its worker can allocate a
  // bounded batched pipeline only when filtering is enabled.
  int32_t delayMinClutter, delayMaxClutter;
  bool isClutter;
  tree["process"]["clutter"]["delayMin"] >> delayMinClutter;
  tree["process"]["clutter"]["delayMax"] >> delayMaxClutter;
  tree["process"]["clutter"]["enable"] >> isClutter;
  const int64_t clutterBins = int64_t(delayMaxClutter) - delayMinClutter;
  if (isClutter && (clutterBins <= 0 || clutterBins > UINT32_MAX))
    throw std::invalid_argument("Clutter delay range must be a non-empty half-open interval");

  std::string accelerationMode = "auto";
  if (tree["process"].has_child("performance") &&
      tree["process"]["performance"].has_child("acceleration"))
    tree["process"]["performance"]["acceleration"] >> accelerationMode;
  const char* gpuDevice = std::getenv("BLAH2_GPU_DEVICE");
  blah2::Acceleration acceleration(accelerationMode,
    {ambiguity.front()->get_nfft(), ambiguity.front()->get_n_doppler_bins(),
     ambiguity.front()->get_n_delay_bins(), uint32_t(ambiguity.size()), delayMin,
     isClutter ? nSamples : 0u,
     isClutter ? uint32_t(clutterBins) : 0u,
     delayMinClutter},
    ambiguity.front()->get_n_corr(), fs, ambiguity.front()->get_doppler_middle(),
    gpuDevice ? gpuDevice : "auto");
  std::vector<Ambiguity*> ambiguityPointers;
  std::vector<IqData*> surveillancePointers;
  for (size_t i = 0; i < ambiguity.size(); ++i) {
    ambiguityPointers.push_back(ambiguity[i].get());
    surveillancePointers.push_back(surveillanceData[i].get());
  }

  // set up process clutter
  std::vector<std::unique_ptr<WienerHopf>> filter;
  for (std::size_t pathIndex = 0;
       pathIndex < surveillanceChannels.size(); pathIndex++)
    filter.push_back(std::make_unique<WienerHopf>(delayMinClutter,
      delayMaxClutter, nSamples));

  ArrayReferenceSynthesizer::Config referenceConfig;
  if (tree["process"].has_child("reference_synthesis"))
  {
    auto node = tree["process"]["reference_synthesis"];
    if (node.has_child("analysis_samples"))
      node["analysis_samples"] >> referenceConfig.analysisSamples;
    if (node.has_child("analysis_interval"))
      node["analysis_interval"] >> referenceConfig.analysisInterval;
    if (node.has_child("power_iterations"))
      node["power_iterations"] >> referenceConfig.powerIterations;
    if (node.has_child("covariance_smoothing"))
      node["covariance_smoothing"] >> referenceConfig.covarianceSmoothing;
    if (node.has_child("diagonal_loading"))
      node["diagonal_loading"] >> referenceConfig.diagonalLoading;
  }
  ArrayReferenceSynthesizer referenceSynthesizer(referenceConfig);
  uint64_t reportedReferenceUpdate = 0;
  uint64_t replayProcessedFrames = 0;
  Noncoherent mapFusion;

  // set up process detection
  double pfa, minDoppler;
  int8_t nGuard, nTrain;
  int8_t minDelay;
  tree["process"]["detection"]["pfa"] >> pfa;
  tree["process"]["detection"]["nGuard"] >> nGuard;
  tree["process"]["detection"]["nTrain"] >> nTrain;
  tree["process"]["detection"]["minDelay"] >> minDelay;
  tree["process"]["detection"]["minDoppler"] >> minDoppler;
  CfarDetector1D *cfarDetector1D = new CfarDetector1D(pfa, nGuard, nTrain, minDelay, minDoppler);
  Interpolate *interpolate = new Interpolate(true, true);

  // set up process centroid
  uint16_t nCentroid;
  tree["process"]["detection"]["nCentroid"] >> nCentroid;
  Centroid *centroid = new Centroid(nCentroid, nCentroid, 1/tCpi);

  // set up process tracker
  uint8_t m, n, nDelete;
  double maxAcc, rangeRes, lambda;
  std::string smooth;
  tree["process"]["tracker"]["initiate"]["M"] >> m;
  tree["process"]["tracker"]["initiate"]["N"] >> n;
  tree["process"]["tracker"]["delete"] >> nDelete;
  tree["process"]["tracker"]["initiate"]["maxAcc"] >> maxAcc;
  rangeRes = (double)Constants::c/fs;
  lambda = (double)Constants::c/fc;
  Tracker *tracker = new Tracker(m, n, nDelete,
    ambiguity.front()->get_cpi(), maxAcc, rangeRes, lambda);

  // set up process spectrum analyser
  double spectrumBandwidth = 2000;
  SpectrumAnalyser *spectrumAnalyser = new SpectrumAnalyser(nSamples,
    spectrumBandwidth, fc, fs);

  // process options
  bool isDetection, isTracker;
  tree["process"]["detection"]["enable"] >> isDetection;
  tree["process"]["tracker"]["enable"] >> isTracker;
  if (!isDetection)
  {
    isTracker = false;
  }

  // set up output data
  bool saveMap, saveDetection;
  tree["save"]["map"] >> saveMap;
  tree["save"]["detection"] >> saveDetection;
  std::string savePath, saveMapPath, saveDetectionPath;
  if (saveIq || saveMap || saveDetection)
  {
    char startTimeStr[16];
    struct timeval currentTime = {0, 0};
    gettimeofday(&currentTime, NULL);
    strftime(startTimeStr, 16, "%Y%m%d-%H%M%S", localtime(&currentTime.tv_sec));
    savePath = path + startTimeStr;
  }
  if (saveMap)
  {
    saveMapPath = savePath + ".map";
  }
  if (saveDetection)
  {
    saveDetectionPath = savePath + ".detection";
  }

  // set up output timing
  uint64_t tStart = current_time_ms();
  Timing *timing = new Timing(tStart);
  std::vector<std::string> timing_name;
  std::vector<double> timing_time;
  std::string jsonTiming;
  std::vector<uint64_t> time;

  // set up output json
  std::string mapJson, detectionJson, jsonTracker, jsonIqData;

  // run process.  Keep capture dormant until all later configuration/DSP setup
  // has succeeded, otherwise a startup exception would strand a joinable thread.
  std::thread t1([&]{capture->process(captureBuffers,
    tree["capture"]["device"], ip_capture, port_capture, nSamples);
  });
  std::thread t2([&]{
    try {
      uint64_t replayGeneration = capture->replayGeneration.load();
      while (!capture->stopping.load())
      {
        if (STOP_SIGNAL) { capture->request_stop(); break; }
        BufferLocks captureLocks(captureBuffers);
        bool ready = true;
        for (auto *buffer : captureBuffers)
          ready = ready && buffer->get_length() >= nSamples;
        if (ready)
        {
          std::vector<uint64_t> captureBacklogSamples, captureDroppedSamples;
          captureBacklogSamples.reserve(captureBuffers.size());
          captureDroppedSamples.reserve(captureBuffers.size());
          for (auto* buffer : captureBuffers) {
            captureBacklogSamples.push_back(buffer->get_length());
            captureDroppedSamples.push_back(buffer->get_dropped_samples());
          }
          // Keep ReplayPlayer's EOF/loop drain from observing an empty queue
          // between extraction and setting the consumer-busy state.
          AtomicFlagGuard processing(capture->processingBusy);
          time.push_back(current_time_us());
          for (std::size_t channel = 0; channel < captureChannelCount; channel++)
            captureData[channel]->replace(
              captureBuffers[channel]->drain_front(nSamples));
          captureLocks.unlock();
          const uint64_t nextReplayGeneration = capture->replayGeneration.load();
          if (nextReplayGeneration != replayGeneration)
          {
            replayGeneration = nextReplayGeneration;
            referenceSynthesizer = ArrayReferenceSynthesizer(referenceConfig);
            reportedReferenceUpdate = 0;
            replayProcessedFrames = 0;
            delete tracker;
            tracker = new Tracker(m, n, nDelete, ambiguity.front()->get_cpi(),
              maxAcc, rangeRes, lambda);
          }
          // Advance signal time for every accepted input CPI, even if clutter
          // later rejects it. UI publication timestamps remain wall-clock.
          const uint64_t trackerFrame = replayProcessedFrames;
          ReplayFrameGuard replayFrame(state, replayProcessedFrames);
          if (!arrayReference)
            referenceData->replace(captureData[referenceChannel]->drain_front(nSamples));
          timing_helper(timing_name, timing_time, time, "extract_buffer");

          std::vector<IqData *> surveillancePointers;
          for (auto& channel : surveillanceData)
            surveillancePointers.push_back(channel.get());
          if (arrayReference)
          {
            std::vector<IqData *> referencePointers;
            for (uint32_t channel : referenceChannels)
              referencePointers.push_back(captureData[channel].get());
            referenceData = referenceSynthesizer.process(referencePointers);
            const auto& metrics = referenceSynthesizer.get_metrics();
            if (metrics.updates != reportedReferenceUpdate)
            {
              std::cout << "[ArrayReference] channels="
                << referencePointers.size() << " update="
                << metrics.updates << " coherent_fraction="
                << metrics.coherentFraction << " coherent_gain_db="
                << metrics.coherentGainDb << " weights=";
              for (std::size_t channel = 0;
                   channel < metrics.weights.size(); channel++)
              {
                if (channel) std::cout << ",";
                std::cout << referenceChannels[channel] << ":"
                  << std::abs(metrics.weights[channel])
                  << "@" << std::arg(metrics.weights[channel]);
              }
              std::cout << "\n";
              reportedReferenceUpdate = metrics.updates;
            }
          }
          // Synthesis is the last reader of the captured channels. Transfer
          // complete sample blocks to conditioning instead of copying a CPI.
          for (std::size_t pathIndex = 0;
               pathIndex < surveillanceChannels.size(); pathIndex++)
            surveillanceData[pathIndex]->replace(
              captureData[surveillanceChannels[pathIndex]]->drain_front(nSamples));
          timing_helper(timing_name, timing_time, time,
            "reference_synthesis");
          
          // spectrum
          spectrumAnalyser->process(referenceData.get());
          timing_helper(timing_name, timing_time, time, "spectrum");
          
          // Filter each surveillance channel independently.
          if (isClutter)
          {
            const bool success = acceleration.processClutter(*referenceData,
              surveillancePointers, [&] {
                return process_paths(surveillanceData.size(),
                  surveillanceWorkers, [&](std::size_t pathIndex) {
                    return filter[pathIndex]->process(referenceData.get(),
                      surveillanceData[pathIndex].get());
                  });
              });
            if (!success)
            {
              time.clear();
              timing_name.clear();
              timing_time.clear();
              continue;
            }
            timing_helper(timing_name, timing_time, time, "clutter_filter");
          }
          
          // Produce one map per surveillance channel, then combine their power.
          std::vector<Map<std::complex<double>> *> channelMaps(
            surveillanceData.size());
          acceleration.process(referenceData->view_data(), surveillancePointers,
            ambiguityPointers, [&] { process_paths(surveillanceData.size(), surveillanceWorkers,
            [&](std::size_t pathIndex) {
              channelMaps[pathIndex] = ambiguity[pathIndex]->process(
                referenceData->view_data(),
                surveillanceData[pathIndex].get());
              return true;
            }); });
          for (size_t channel = 0; channel < ambiguity.size(); ++channel) {
            channelMaps[channel] = ambiguity[channel]->result();
          }
          // Only the fused map is detected/published. Fusion consumes complex
          // samples, so per-channel display metrics would be unused log passes.
          map = mapFusion.process(channelMaps);
          map->set_metrics();
          timing_helper(timing_name, timing_time, time, "ambiguity_processing");
          
          // detection process
          if (isDetection)
          {
            detection1 = cfarDetector1D->process(map.get());
            detection2 = centroid->process(detection1.get());
            detection = interpolate->process(detection2.get(), map.get());
            timing_helper(timing_name, timing_time, time, "detector");
          }

          // tracker process
          if (isTracker)
          {
            const uint64_t trackerTime = state ? tStart +
              (trackerFrame * uint64_t(nSamples) * 1000) / fs : time[0]/1000;
            track = tracker->process(detection.get(), trackerTime);
            timing_helper(timing_name, timing_time, time, "tracker");
          }

          // output IqData meta data
          jsonIqData = referenceData->to_json(time[0]/1000);
          socket_iqdata->sendData(jsonIqData);

          // output map data
          mapJson = map->to_json_km(time[0]/1000, fs);
          if (saveMap)
          {
            map->save(mapJson, saveMapPath);
          }
          socket_map->sendData(mapJson);

          // output detection data
          if (isDetection)
          {
            detectionJson = detection->to_json_km(time[0]/1000, fs);
            socket_detection->sendData(detectionJson);
          }
          if (saveDetection)
          {
            detection->save(detectionJson, saveDetectionPath);
          }

          // output tracker data
          if (isTracker)
          {
            jsonTracker = track->to_json(time[0]/1000);
            socket_track->sendData(jsonTracker);
          }

          // output radar data timer
          timing_helper(timing_name, timing_time, time, "output_radar_data");

          // cpi timer
          time.push_back(current_time_us());
          double delta_ms = (double)(time.back()-time[0]) / 1000;
          timing_name.push_back("cpi");
          timing_time.push_back(delta_ms);
          std::cout << "CPI time (ms): " << delta_ms << "\n";

          // output timing data
          timing->update(time[0]/1000, timing_time, timing_name);
          timing->set_capture_queues(std::move(captureBacklogSamples),
            std::move(captureDroppedSamples));
          timing->set_acceleration(acceleration.status());
          if (isClutter)
            timing->set_clutter_acceleration(acceleration.clutterStatus(),
              acceleration.clutterTiming().gpuExecuted,
              acceleration.clutterTiming().cpuExecuted);
          else {
            blah2::AccelerationStatus disabled;
            disabled.requested = accelerationMode;
            disabled.state = "disabled";
            disabled.reason = "Clutter filtering is disabled";
            timing->set_clutter_acceleration(disabled, false, false);
          }
          jsonTiming = timing->to_json();
          socket_timing->sendData(jsonTiming);
          timing_time.clear();
          timing_name.clear();

          // output CPI timestamp for updating data
          std::string t0_string = std::to_string(time[0]/1000);
          socket_timestamp->sendData(t0_string);
          time.clear();

        }
        else
        {
          captureLocks.unlock();
          // short delay to prevent tight looping
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    } catch (const std::exception& error) {
      capture->processing_error(error.what());
      capture->request_input_stop();
    } catch (...) {
      capture->processing_error("Unknown processing failure");
      capture->request_input_stop();
    }
    });
  t2.join();
  t1.join();

  return 0;
}
catch (const std::exception& error) {
  std::cerr << "Radar startup failed: " << error.what() << '\n';
  return 1;
}

void signal_callback_handler(int signum) {
  (void)signum;
  // Signal context cannot safely touch a Source (replay has none) or do I/O.
  STOP_SIGNAL = 1;
}

void getopt_print_help()
{
  std::cout << "--config <file.yml>: 	Configuration file\n"
               "--help:              	Show help\n";
  exit(1);
}

std::string getopt_process(int argc, char **argv)
{
  const char *const short_opts = "c:h";
  const option long_opts[] = {
      {"config", required_argument, nullptr, 'c'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, no_argument, nullptr, 0}};

  if (argc == 1)
  {
    std::cout << "Error: No arguments provided." << "\n";
    exit(1);
  }

  std::string file;

  while (true)
  {
    const auto opt = getopt_long(argc, argv, short_opts, long_opts, nullptr);

    // handle input "-", ":", etc
    if ((argc == 2) && (-1 == opt))
    {
      std::cout << "Error: No arguments provided." << "\n";
      exit(1);
    }

    if (-1 == opt)
      break;

    switch (opt)
    {
    case 'c':
      file = std::string(optarg);
      break;

    case 'h':
      getopt_print_help();

    // unrecognised option
    case '?':
      exit(1);

    default:
      break;
    }
  }

  return file;
}

std::string ryml_get_file(const char *filename)
{
  std::ifstream in(filename, std::ios::in | std::ios::binary);
  if (!in)
  {
    std::cerr << "could not open " << filename << "\n";
    exit(1);
  }
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

uint64_t current_time_ms()
{
  // current time in POSIX ms
  return std::chrono::duration_cast<std::chrono::milliseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t current_time_us()
{
  // current time in POSIX us
  return std::chrono::duration_cast<std::chrono::microseconds>
  (std::chrono::system_clock::now().time_since_epoch()).count();
}

void timing_helper(std::vector<std::string>& timing_name, 
  std::vector<double>& timing_time, std::vector<uint64_t>& time_us, 
  std::string name)
{
  time_us.push_back(current_time_us());
  double delta_ms = (double)(time_us.back()-time_us[time_us.size()-2]) / 1000;
  timing_name.push_back(name);
  timing_time.push_back(delta_ms);
}
