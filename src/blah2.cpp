/// @file blah2.cpp
/// @brief A real-time radar.
/// @author 30hours

#include "capture/Capture.h"
#include "capture/PairedCpiQueue.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/meta/Timing.h"
#include "data/Track.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#ifdef VECTORWARP_MIXED_AUTO
#include "process/mixed/MixedProcess.h"
#include "process/mixed/MixedAutoPolicy.h"
#include "process/mixed/MixedMap.h"
#include "process/mixed/MixedEligibility.h"
#endif
#include "process/clutter/WienerHopf.h"
#include "process/utility/FftwThreads.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/fusion/Noncoherent.h"
#include "process/fusion/CoherentBeamBank.h"
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
#include <array>
#include <cmath>
#include <numeric>

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

  std::string surveillanceFusionMode = "noncoherent";
  uint32_t coherentBeamCount = 12;
  std::vector<std::array<double, 3>> coherentPositions;
  double directPathAzimuthRad = 0;
  if (tree["process"].has_child("surveillance_fusion"))
  {
    auto fusion = tree["process"]["surveillance_fusion"];
    if (fusion.has_child("mode")) fusion["mode"] >> surveillanceFusionMode;
    if (fusion.has_child("beams")) fusion["beams"] >> coherentBeamCount;
  }
  if (surveillanceFusionMode != "noncoherent" &&
      surveillanceFusionMode != "coherent_beam_bank")
    throw std::invalid_argument("Unknown surveillance-fusion mode");
  const bool coherentFusion = surveillanceFusionMode != "noncoherent";
  if (coherentFusion)
  {
    if (type != "Kraken" || !tree["capture"]["device"].has_child("array_geometry") ||
        !tree["capture"]["device"]["array_geometry"].has_child("elements"))
      throw std::invalid_argument("Coherent surveillance fusion requires Kraken array geometry");
    std::vector<std::array<double, 3>> positions(captureChannelCount);
    std::vector<bool> found(captureChannelCount, false);
    for (auto element : tree["capture"]["device"]["array_geometry"]["elements"].children())
    {
      uint32_t channel = std::numeric_limits<uint32_t>::max();
      if (!element.has_child("daq_channel") || !element.has_child("position")) continue;
      element["daq_channel"] >> channel;
      if (channel >= captureChannelCount) continue;
      std::array<double, 3> position{};
      std::size_t coordinate = 0;
      for (auto value : element["position"].children())
      {
        if (coordinate >= position.size()) break;
        value >> position[coordinate++];
      }
      if (coordinate == position.size())
      {
        positions[channel] = position;
        found[channel] = true;
      }
    }
    for (uint32_t channel : surveillanceChannels)
    {
      if (!found[channel])
        throw std::invalid_argument("Coherent surveillance fusion geometry is missing a surveillance channel");
      coherentPositions.push_back(positions[channel]);
    }
    {
      if (!arrayReference || referenceChannels != surveillanceChannels)
        throw std::invalid_argument("Coherent fusion requires the array reference to use every surveillance channel in the same order");
      auto geometry = tree["capture"]["device"]["array_geometry"];
      if (!geometry.has_child("x_axis_bearing_deg_true") ||
          !tree["location"].has_child("rx") ||
          !tree["location"].has_child("tx"))
        throw std::invalid_argument("Coherent fusion requires array orientation and receiver/transmitter locations");
      double xAxisBearingDeg = 0, rxLatDeg = 0, rxLonDeg = 0,
        txLatDeg = 0, txLonDeg = 0;
      geometry["x_axis_bearing_deg_true"] >> xAxisBearingDeg;
      tree["location"]["rx"]["latitude"] >> rxLatDeg;
      tree["location"]["rx"]["longitude"] >> rxLonDeg;
      tree["location"]["tx"]["latitude"] >> txLatDeg;
      tree["location"]["tx"]["longitude"] >> txLonDeg;
      const double radians = std::acos(-1.0) / 180.0;
      const double rxLat = rxLatDeg * radians, txLat = txLatDeg * radians;
      const double dLon = (txLonDeg - rxLonDeg) * radians;
      const double trueBearing = std::atan2(std::sin(dLon) * std::cos(txLat),
        std::cos(rxLat) * std::sin(txLat) - std::sin(rxLat) *
        std::cos(txLat) * std::cos(dLon));
      directPathAzimuthRad = std::remainder(xAxisBearingDeg * radians -
        trueBearing, 2 * std::acos(-1.0));
      if (!std::isfinite(directPathAzimuthRad) ||
          (rxLatDeg == txLatDeg && rxLonDeg == txLonDeg))
        throw std::invalid_argument("Coherent fusion has invalid receiver/transmitter geometry");
    }
  }

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
  blah2::set_fftw_planner_threads(fftThreads);
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
  const char* pairedQueueEnv = std::getenv("VECTORWARP_RSPDUO_CPI_QUEUE");
  const bool pairedQueueRequested = !state && type == "RspDuo" &&
    captureChannelCount == 2 && pairedQueueEnv && std::string(pairedQueueEnv) == "1";
  std::unique_ptr<PairedCpiQueue> pairedCpiQueue;
  if (pairedQueueRequested) {
    if (!nSamples || captureBufferSamples < 2 * uint64_t(nSamples))
      throw std::invalid_argument("Invalid paired CPI queue geometry");
    // One CPI may be actively filled; retain the configured bounded capture
    // capacity across ready plus partial data, rather than adding a hidden CPI.
    const size_t cpis = captureBufferSamples / nSamples;
    pairedCpiQueue = std::make_unique<PairedCpiQueue>(nSamples,
      cpis - 1);
    capture->set_paired_cpi_queue(pairedCpiQueue.get());
    std::cout << "RSPduo paired CPI queue requested: " << cpis << " CPI capacity\n";
  }
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
  // The paired live receiver has one reference and one surveillance channel.
  // Keep their completed deques and overwrite the samples next CPI instead of
  // allocating two million deque elements again on every capture handoff.
  const bool directPairedIq = pairedQueueRequested && !arrayReference &&
    surveillanceChannels.size() == 1 &&
    ((referenceChannel == 0 && surveillanceChannels[0] == 1) ||
     (referenceChannel == 1 && surveillanceChannels[0] == 0));
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
  const bool residentMode=accelerationMode=="gpu-resident";
#ifndef VECTORWARP_RESIDENT_GPU
  if(residentMode)throw std::invalid_argument("This cohort does not provide the resident ABI5 GPU route");
#endif

#ifdef VECTORWARP_MIXED_AUTO
  const blah2::mixed::Shape mixedShape{fs,nSamples,
    uint32_t(surveillanceChannels.size()),ambiguity.front()->get_n_doppler_bins(),
    ambiguity.front()->get_n_delay_bins(),ambiguity.front()->get_n_corr(),
    ambiguity.front()->get_nfft(),uint32_t(clutterBins),delayMinClutter,
    delayMin,delayMax,dopplerMin,dopplerMax,
    ambiguity.front()->get_doppler_middle(),arrayReference,isClutter};
  const bool commonCpuCpi = blah2::mixed::qualified(mixedShape);
  const bool mixedAuto = blah2::mixed::autoCandidate(accelerationMode,mixedShape,
    blah2::mixed::pi4Host(),gpuDevice ? gpuDevice : "auto");
  const bool cpuBorrowed = commonCpuCpi &&
    (accelerationMode == "cpu" || mixedAuto);
  blah2::mixed::AutoPolicy mixedPolicy;
  std::unique_ptr<blah2::mixed::Process> mixedWorker;
  blah2::AccelerationStatus mixedStatus;
  mixedStatus.requested = "auto";
  mixedStatus.state = "checking";
  mixedStatus.reason = "Checking complete mixed-DSP map and CPI cost";
  if (mixedAuto) {
    try {
      mixedWorker = std::make_unique<blah2::mixed::Process>();
      mixedStatus.device = "V3D mixed worker";
    } catch (const std::exception& error) {
      mixedPolicy.disable(error.what());
      mixedStatus.state = "fallback";
      mixedStatus.reason = error.what();
    }
  }
#endif
  blah2::Acceleration acceleration(
#ifdef VECTORWARP_MIXED_AUTO
    residentMode ? "cpu" : (mixedAuto ? "cpu" : accelerationMode),
#else
    residentMode ? "cpu" : accelerationMode,
#endif
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

  // Allocate on the first CPU execution, including GPU qualification's CPU
  // oracle, and prepare the common reference once per CPU-processed CPI.
  std::vector<std::unique_ptr<WienerHopf>> filter(surveillanceChannels.size());
  std::unique_ptr<WienerHopf::PreparedReference> cpuClutterReference;
#ifdef VECTORWARP_MIXED_AUTO
  // The qualified Pi path plans before capture starts. Delaying its measured
  // FFT plans until the first CPU oracle would consume live queue headroom.
  if (cpuBorrowed)
    for (auto& pathFilter : filter)
      pathFilter = std::make_unique<WienerHopf>(delayMinClutter, delayMaxClutter, nSamples);
#endif

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
  std::unique_ptr<CoherentBeamBank> coherentMapFusion;
  if (coherentFusion)
  {
    coherentMapFusion = std::make_unique<CoherentBeamBank>(
      CoherentBeamBank::Config{double(fc), coherentBeamCount, coherentPositions,
        directPathAzimuthRad});
    std::cout << "[SurveillanceFusion] mode=" << surveillanceFusionMode << " beams="
      << coherentBeamCount << " channels=" << coherentPositions.size();
    std::cout << " direct_path_azimuth_deg=" <<
      directPathAzimuthRad * 180.0 / std::acos(-1.0);
    std::cout << "\n";
  }
  else
    std::cout << "[SurveillanceFusion] mode=noncoherent\n";

  // set up process detection
  double pfa, minDoppler;
  int8_t nGuard, nTrain;
  int8_t minDelay;
  tree["process"]["detection"]["pfa"] >> pfa;
  tree["process"]["detection"]["nGuard"] >> nGuard;
  tree["process"]["detection"]["nTrain"] >> nTrain;
  tree["process"]["detection"]["minDelay"] >> minDelay;
  tree["process"]["detection"]["minDoppler"] >> minDoppler;
  // A beam bank reports the maximum of several trials per cell. Apply a
  // Bonferroni correction so its configured false-alarm probability retains
  // the same upper bound as the single noncoherent statistic.
  const double detectorPfa = coherentFusion
    ? pfa / coherentBeamCount : pfa;
  if (coherentFusion)
    std::cout << "[SurveillanceFusion] configured_pfa=" << pfa
      << " effective_pfa=" << detectorPfa << "\n";
  CfarDetector1D *cfarDetector1D = new CfarDetector1D(detectorPfa, nGuard, nTrain, minDelay, minDoppler);
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

  // Resident mode is explicit and first admits the reviewed all-capture identity
  // route. Existing CPU/AUTO/FIR and other-platform routes keep their behavior.
  blah2::resident::Config residentConfig;
  std::unique_ptr<blah2::resident::Coordinator> resident;
  uint64_t residentLogical=0,residentConsumed=0;
  if(residentMode){
    if(!arrayReference||!isClutter||pairedQueueRequested||surveillanceChannels.size()!=captureChannelCount||!gpuDevice||std::string(gpuDevice)=="auto")
      throw std::invalid_argument("Resident GPU requires array reference, clutter, identity all-capture surveillance and an explicit GPU device");
    residentConfig.geometry={ambiguity.front()->get_nfft(),ambiguity.front()->get_n_doppler_bins(),ambiguity.front()->get_n_delay_bins(),uint32_t(surveillanceChannels.size()),delayMin,nSamples,uint32_t(clutterBins),delayMinClutter};
    residentConfig.correlation=ambiguity.front()->get_n_corr();
    residentConfig.sampleRate=fs;
    residentConfig.middle=ambiguity.front()->get_doppler_middle();
    residentConfig.epoch=1;
    residentConfig.device=gpuDevice;
    residentConfig.surveillanceChannels=surveillanceChannels;
    residentConfig.delayAxis.assign(ambiguity.front()->result()->delay.begin(),ambiguity.front()->result()->delay.end());
    residentConfig.dopplerAxis.assign(ambiguity.front()->result()->doppler.begin(),ambiguity.front()->result()->doppler.end());
    residentConfig.reference={referenceChannels,referenceConfig.analysisSamples,referenceConfig.analysisInterval,referenceConfig.powerIterations,referenceConfig.covarianceSmoothing,referenceConfig.diagonalLoading,spectrumBandwidth,double(fc)};
    if(coherentFusion){
      std::string shape;
      auto geometry=tree["capture"]["device"]["array_geometry"];
      if(geometry.has_child("shape"))geometry["shape"]>>shape;
      if(shape!="regular_polygon"||coherentPositions.size()<3)
        throw std::invalid_argument("Resident coherent fusion requires regular-polygon geometry");
      double centreX=0,centreY=0;
      for(const auto& position:coherentPositions){centreX+=position[0];centreY+=position[1];}
      centreX/=coherentPositions.size();centreY/=coherentPositions.size();
      std::vector<double> angles,radii;
      for(const auto& position:coherentPositions){const double x=position[0]-centreX,y=position[1]-centreY;
        angles.push_back(std::atan2(y,x));radii.push_back(std::hypot(x,y));}
      const double radius=std::accumulate(radii.begin(),radii.end(),0.0)/radii.size();
      const double step=std::remainder(angles[1]-angles[0],2*std::acos(-1.0));
      const double tolerance=std::max(.001,radius*.05);
      for(size_t channel=0;channel<radii.size();++channel)
        if(std::abs(radii[channel]-radius)>tolerance||
           std::abs(std::remainder(angles[channel]-angles[0]-channel*step,2*std::acos(-1.0)))>.0873)
          throw std::invalid_argument("Resident coherent fusion geometry/order is not a regular polygon");
      residentConfig.fusion={blah2::resident::FusionMode::coherentRegularPolygon,
        coherentBeamCount,double(fc),radius,angles[0],step,directPathAzimuthRad};
    }
    const double spectrumResolution=double(fs)/nSamples;
    const uint32_t decimation=uint32_t(std::min<double>(nSamples,std::max(1.0,std::ceil(spectrumBandwidth/spectrumResolution))));
    residentConfig.spectrumBins=1+(nSamples-1)/decimation;
    residentConfig.detector={isDetection,true,true,nGuard,nTrain,minDelay,nCentroid,nCentroid,detectorPfa,minDoppler,1/tCpi};
    if(isTracker){
      uint32_t capacity=0;
      if(!tree["process"]["tracker"].has_child("gpu_capacity"))throw std::invalid_argument("Resident tracker requires an explicit gpu_capacity; capacity is never inferred or shrunk");
      tree["process"]["tracker"]["gpu_capacity"] >> capacity;
      residentConfig.tracker={true,capacity,m,n,nDelete,ambiguity.front()->get_cpi(),maxAcc,rangeRes,lambda};
    }
    residentConfig.presence=blah2::resident::wire::mandatoryPresence(residentConfig)|blah2::resident::bit(blah2::resident::Section::filtered)|blah2::resident::bit(blah2::resident::Section::fullReference);
    resident=std::make_unique<blah2::resident::Coordinator>(residentConfig);
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
      bool pairedQueueWasActive = false;
      while (!capture->stopping.load() || (pairedQueueWasActive &&
        (capture->pairedCpiQueueActive.load() || pairedCpiQueue->backlog_samples() >= nSamples)))
      {
        if (STOP_SIGNAL && !capture->stopping.load()) capture->request_stop();
        pairedQueueWasActive = pairedQueueWasActive ||
          (pairedCpiQueue && capture->pairedCpiQueueActive.load());
        const bool usePairedQueue = pairedQueueWasActive;
        const bool pairedDirectFrame = usePairedQueue && directPairedIq;
        const auto residentWholeStart=std::chrono::steady_clock::now();
        bool ready = false;
        uint64_t captureStartUs = 0;
        std::vector<uint64_t> captureBacklogSamples, captureDroppedSamples;
#ifdef VECTORWARP_MIXED_AUTO
        blah2::mixed::Choice mixedChoice = blah2::mixed::Choice::cpu;
        bool mixedChoiceMade = false, mixedInputPrepared = false;
#endif
        if (usePairedQueue) {
          size_t slot = 0;
          if (pairedCpiQueue->acquire(slot)) {
            try {
              const auto& block = pairedCpiQueue->block(slot);
              captureStartUs = current_time_us();
              if (pairedDirectFrame) {
#ifdef VECTORWARP_MIXED_AUTO
                if (mixedAuto && mixedWorker && !mixedPolicy.disabled()) {
                  // Decide while the packed CPI is borrowed, so shared input
                  // can retain signed16 IQ. CPU choices keep the
                  // original decode path, and the queue loan ends as before.
                  const auto queue = pairedCpiQueue->stats();
                  mixedChoice = mixedPolicy.choose(static_cast<uint32_t>(
                    std::min<uint64_t>(pairedCpiQueue->backlog_samples(), UINT32_MAX)),
                    captureBufferSamples, queue.discardedSamples);
                  mixedChoiceMade = true;
                  if (mixedChoice != blah2::mixed::Choice::cpu) {
                    try {
                      mixedWorker->prepare_paired_i16(block.iq.data(), nSamples,
                        *referenceData, *surveillanceData[0], referenceChannel);
                      mixedInputPrepared = true;
                    } catch (const std::exception& error) {
                      mixedPolicy.disable(error.what());
                      mixedWorker.reset();
                      mixedChoice = blah2::mixed::Choice::cpu;
                    }
                  }
                }
                if (!mixedInputPrepared) {
#endif
                if (referenceChannel == 0)
                  referenceData->assign_paired_i16(block.iq.data(), nSamples,
                    *surveillanceData[0]);
                else
                  surveillanceData[0]->assign_paired_i16(block.iq.data(), nSamples,
                    *referenceData);
#ifdef VECTORWARP_MIXED_AUTO
                }
#endif
              } else {
                captureData[0]->assign_paired_i16(block.iq.data(), nSamples,
                  *captureData[1]);
              }
              ready = true;
            } catch (...) { pairedCpiQueue->release(slot); throw; }
            pairedCpiQueue->release(slot);
          }
          const auto queue = pairedCpiQueue->stats();
          captureBacklogSamples.resize(2);
          captureDroppedSamples.resize(2);
          const uint64_t queued = uint64_t(pairedCpiQueue->backlog_samples());
          captureBacklogSamples[0] = captureBacklogSamples[1] = queued;
          captureDroppedSamples[0] = captureDroppedSamples[1] = queue.discardedSamples;
        } else {
          BufferLocks captureLocks(captureBuffers);
          ready = true;
          for (auto *buffer : captureBuffers)
            ready = ready && buffer->get_length() >= nSamples;
          if (ready) {
            captureBacklogSamples.reserve(captureBuffers.size());
            captureDroppedSamples.reserve(captureBuffers.size());
            for (auto* buffer : captureBuffers) {
              captureBacklogSamples.push_back(buffer->get_length());
              captureDroppedSamples.push_back(buffer->get_dropped_samples());
            }
            for (std::size_t channel = 0; channel < captureChannelCount; channel++)
              captureData[channel]->replace(captureBuffers[channel]->drain_front(nSamples));
          }
          captureLocks.unlock();
        }
        if (ready)
        {
          // Keep ReplayPlayer's EOF/loop drain from observing an empty queue
          // between extraction and setting the consumer-busy state.
          AtomicFlagGuard processing(capture->processingBusy);
          time.push_back(captureStartUs ? captureStartUs : current_time_us());
          const uint64_t nextReplayGeneration = capture->replayGeneration.load();
          if (nextReplayGeneration != replayGeneration)
          {
            replayGeneration = nextReplayGeneration;
            referenceSynthesizer = ArrayReferenceSynthesizer(referenceConfig);
            reportedReferenceUpdate = 0;
            replayProcessedFrames = 0;
            if(resident){
              if(residentConfig.epoch==UINT64_MAX)throw std::overflow_error("Resident configuration epoch exhausted");
              resident.reset();++residentConfig.epoch;residentLogical=residentConsumed=0;
              resident=std::make_unique<blah2::resident::Coordinator>(residentConfig);
            }
            delete tracker;
            tracker = new Tracker(m, n, nDelete, ambiguity.front()->get_cpi(),
              maxAcc, rangeRes, lambda);
          }
          // Advance signal time for every accepted input CPI, even if clutter
          // later rejects it. UI publication timestamps remain wall-clock.
          const uint64_t trackerFrame = replayProcessedFrames;
          ReplayFrameGuard replayFrame(state, replayProcessedFrames);
          const bool residentOracle=resident&&resident->qualifying();
          if(resident){
            if(residentLogical==UINT64_MAX||residentConsumed==UINT64_MAX)throw std::overflow_error("Resident frame sequence exhausted");
            const uint64_t signalTime=state?tStart+(trackerFrame*uint64_t(nSamples)*1000)/fs:time[0]/1000;
            resident->begin(captureData,{++residentLogical,++residentConsumed,signalTime});
          }
          // This block is the unchanged CPU route, or its bounded startup oracle.
          // Accepted resident frames cannot enter CPU computational stages.
          #ifdef VECTORWARP_MIXED_AUTO
          bool mixedCandidateReady=false,mixedPublished=false;
          #endif
          if(!resident||residentOracle){
          if (!arrayReference && !pairedDirectFrame)
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
            referenceSynthesizer.process_into(referencePointers, *referenceData);
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
            coherentMapFusion->calibrate(metrics.weights);
          }
          if(resident)resident->checkReference(*referenceData,referenceSynthesizer.get_metrics());
          // Synthesis is the last reader of the captured channels. Transfer
          // complete sample blocks to conditioning instead of copying a CPI.
          if (!pairedDirectFrame)
            for (std::size_t pathIndex = 0;
                 pathIndex < surveillanceChannels.size(); pathIndex++)
              surveillanceData[pathIndex]->replace(
                captureData[surveillanceChannels[pathIndex]]->drain_front(nSamples));
          timing_helper(timing_name, timing_time, time,
            "reference_synthesis");
          
          // spectrum
          spectrumAnalyser->process(referenceData.get());
          timing_helper(timing_name, timing_time, time, "spectrum");

#ifdef VECTORWARP_MIXED_AUTO
          std::vector<std::complex<double>> mixedMap, mixedTail;
          mixedCandidateReady = false;
          if (mixedAuto && mixedWorker && !mixedPolicy.disabled()) {
            const uint64_t backlog = captureBacklogSamples.empty() ? 0 :
              *std::max_element(captureBacklogSamples.begin(), captureBacklogSamples.end());
            const uint64_t drops = captureDroppedSamples.empty() ? 0 :
              *std::max_element(captureDroppedSamples.begin(), captureDroppedSamples.end());
            if (!mixedChoiceMade)
              mixedChoice = mixedPolicy.choose(static_cast<uint32_t>(std::min<uint64_t>(
                backlog, UINT32_MAX)), captureBufferSamples, drops);
            if (mixedChoice != blah2::mixed::Choice::cpu) {
              try {
                if (mixedInputPrepared)
                  mixedWorker->run_prepared(mixedMap, mixedTail);
                else
                  mixedWorker->run(referenceData->view_data(),
                    surveillanceData[0]->view_data(), mixedMap, mixedTail);
                mixedCandidateReady = true;
              } catch (const std::exception& error) {
                mixedPolicy.disable(error.what());
                mixedStatus.state = "fallback";
                mixedStatus.reason = error.what();
                mixedWorker.reset();
                mixedChoice = blah2::mixed::Choice::cpu;
              }
            }
          }
          mixedPublished = mixedChoice == blah2::mixed::Choice::mixed &&
            mixedCandidateReady;
#endif
          
          // Filter each surveillance channel independently.
#ifdef VECTORWARP_MIXED_AUTO
          if (!mixedPublished)
#endif
          if (isClutter)
          {
            const bool success = acceleration.processClutter(*referenceData,
              surveillancePointers, [&] {
#ifdef VECTORWARP_MIXED_AUTO
                if (cpuBorrowed)
                  return process_paths(surveillanceData.size(), surveillanceWorkers,
                    [&](std::size_t pathIndex) {
                      return filter[pathIndex]->process(referenceData.get(),
                        surveillanceData[pathIndex].get(), true);
                    });
#endif
                if (!cpuClutterReference) {
                  auto pendingReference = std::make_unique<WienerHopf::PreparedReference>(
                    delayMinClutter, delayMaxClutter, nSamples);
                  std::vector<std::unique_ptr<WienerHopf>> pendingFilters;
                  pendingFilters.reserve(filter.size());
                  for (std::size_t pathIndex = 0; pathIndex < filter.size(); ++pathIndex)
                    pendingFilters.push_back(std::make_unique<WienerHopf>(
                      delayMinClutter, delayMaxClutter, nSamples,
                      WienerHopf::Workspace::SharedReference));
                  // A plan constructor may throw during GPU qualification.
                  // Publish only a complete set so a later CPU fallback can
                  // safely retry initialization.
                  filter = std::move(pendingFilters);
                  cpuClutterReference = std::move(pendingReference);
                }
                if (!cpuClutterReference->prepare(*referenceData)) return false;
                return process_paths(surveillanceData.size(),
                  surveillanceWorkers, [&](std::size_t pathIndex) {
                    return filter[pathIndex]->process(*cpuClutterReference,
                      surveillanceData[pathIndex].get());
                  });
              });
            if (!success)
            {
              if(resident)throw std::runtime_error("Resident startup CPU clutter oracle rejected the CPI");
              time.clear();
              timing_name.clear();
              timing_time.clear();
              continue;
            }
            timing_helper(timing_name, timing_time, time, "clutter_filter");
          }
          
          if(resident)resident->checkFiltered(surveillancePointers);
          // Produce one map per surveillance channel, then combine their power.
          std::vector<Map<std::complex<double>> *> channelMaps(
            surveillanceData.size());
#ifdef VECTORWARP_MIXED_AUTO
          if (mixedPublished) {
            try {
              blah2::mixed::commitMap(std::move(mixedMap),
                *ambiguity[0]->result());
              if (!pairedDirectFrame) {
                surveillanceData[0]->discard_front(blah2::mixed::usedSamples);
                surveillanceData[0]->assign_complex(mixedTail.data(),
                  blah2::mixed::tailSamples);
              }
            } catch (const std::exception& error) {
              // Original input is still authoritative until commit. A map
              // assignment failure can be recomputed from the raw CPI.
              if (!pairedDirectFrame && surveillanceData[0]->get_length() != nSamples)
                throw;
              mixedPolicy.disable(error.what());
              mixedStatus.state = "fallback";
              mixedStatus.reason = error.what();
              mixedWorker.reset();
              mixedPublished = false;
              mixedChoice = blah2::mixed::Choice::cpu;
              if (!filter[0]->process(referenceData.get(),
                    surveillanceData[0].get(), cpuBorrowed))
                throw std::runtime_error("CPU clutter fallback rejected CPI");
            }
          }
          if (!mixedPublished)
#endif
          acceleration.process(referenceData->view_data(), surveillancePointers,
            ambiguityPointers, [&] { process_paths(surveillanceData.size(), surveillanceWorkers,
            [&](std::size_t pathIndex) {
#ifdef VECTORWARP_MIXED_AUTO
              if (cpuBorrowed) {
                const auto view = filter[pathIndex]->filtered_view();
                channelMaps[pathIndex] = ambiguity[pathIndex]->process_borrowed(
                  referenceData->view_data(), surveillanceData[pathIndex].get(),
                  view.rotatedReference, view.filteredSurveillance,
                  view.samples, view.delayMin, pairedDirectFrame);
              } else
#endif
              channelMaps[pathIndex] = ambiguity[pathIndex]->process(
                referenceData->view_data(),
                surveillanceData[pathIndex].get(), pairedDirectFrame);
              return true;
            }); });
#ifdef VECTORWARP_MIXED_AUTO
          if (mixedChoice == blah2::mixed::Choice::shadow &&
              mixedCandidateReady && !mixedPolicy.disabled()) {
            const auto error = blah2::mixed::compareMap(mixedMap,
              *ambiguity[0]->result());
            mixedPolicy.shadow(error.rms, error.peak);
            if (mixedPolicy.disabled()) {
              mixedStatus.state = "fallback";
              mixedStatus.reason = mixedPolicy.reason();
              mixedWorker.reset();
            }
          }
#endif
          for (size_t channel = 0; channel < ambiguity.size(); ++channel) {
            channelMaps[channel] = ambiguity[channel]->result();
          }
          if(resident)resident->checkMaps(ambiguityPointers);
          // Only the fused map is detected/published. Fusion consumes complex
          // samples, so per-channel display metrics would be unused log passes.
          map = coherentMapFusion ? coherentMapFusion->process(channelMaps) :
            mapFusion.process(channelMaps);
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

          } // legacy computation or explicitly labeled startup oracle
          blah2::resident::Coordinator::Publication residentPublication;
          if(resident){
            resident->receivePrepared();
            residentPublication=resident->format(time[0]/1000);
            resident->commit();
            timing_helper(timing_name,timing_time,time,"gpu_resident_ready");
          }

          // output IqData meta data
          jsonIqData = resident ? std::move(residentPublication.spectrum) : referenceData->to_json(time[0]/1000);
          socket_iqdata->sendData(jsonIqData);

          // output map data
          mapJson = resident ? std::move(residentPublication.map) : map->to_json_km(time[0]/1000, fs);
          if (saveMap)
          {
            map->save(mapJson, saveMapPath);
          }
          socket_map->sendData(mapJson);

          // output detection data
          if (isDetection)
          {
            detectionJson = resident ? std::move(residentPublication.detections) : detection->to_json_km(time[0]/1000, fs);
            socket_detection->sendData(detectionJson);
          }
          if (saveDetection && isDetection)
          {
            detection->save(detectionJson, saveDetectionPath);
          }

          // output tracker data
          if (isTracker)
          {
            jsonTracker = resident ? std::move(residentPublication.tracks) : track->to_json(time[0]/1000);
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
#ifdef VECTORWARP_MIXED_AUTO
          if (mixedAuto) {
            if (!mixedPolicy.disabled() &&
                mixedChoice != blah2::mixed::Choice::shadow)
              mixedPolicy.complete(mixedChoice, delta_ms);
            if (mixedPolicy.disabled() ||
                (mixedPolicy.trials() == 6 && !mixedPolicy.selected()))
              mixedWorker.reset();
            mixedStatus.active = mixedPublished ? "vulkan+cpu" : "cpu";
            mixedStatus.cpuMs = mixedPolicy.cpuMs();
            mixedStatus.gpuMs = mixedPolicy.mixedMs();
            mixedStatus.reason = mixedPolicy.reason();
            mixedStatus.state = mixedPolicy.disabled() ? "fallback" :
              mixedPolicy.trials() == 6 ? "ready" : "checking";
            std::cout << "Mixed AUTO active=" << mixedStatus.active
              << " state=" << mixedStatus.state
              << " cpu_cpi_ms=" << mixedStatus.cpuMs
              << " mixed_cpi_ms=" << mixedStatus.gpuMs
              << " shadow_passes=" << mixedPolicy.shadowPasses()
              << " oracle_worker_executed="
              << (mixedChoice == blah2::mixed::Choice::shadow && mixedCandidateReady)
              << " trials=" << mixedPolicy.trials()
              << " reason=" << mixedStatus.reason << "\n";
          }
#endif

          // output timing data
          timing->update(time[0]/1000, timing_time, timing_name);
          timing->set_capture_queues(captureBacklogSamples,captureDroppedSamples);
          timing->set_acceleration(
#ifdef VECTORWARP_MIXED_AUTO
            mixedAuto ? mixedStatus :
#endif
            resident ? resident->status() : acceleration.status());
          if (isClutter)
            timing->set_clutter_acceleration(
#ifdef VECTORWARP_MIXED_AUTO
              mixedAuto ? mixedStatus :
#endif
              resident ? resident->status() : acceleration.clutterStatus(),
#ifdef VECTORWARP_MIXED_AUTO
              mixedAuto ? mixedCandidateReady :
#endif
              resident ? true : acceleration.clutterTiming().gpuExecuted,
#ifdef VECTORWARP_MIXED_AUTO
              // Mixed frames execute CPU correlation/solve/FIR/range work
              // inside the child, even when no parent CPU oracle runs.
              mixedAuto ? true :
#endif
              resident ? residentOracle : acceleration.clutterTiming().cpuExecuted);
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
          if(resident){
            // sendData is a completed local TCP write, not a browser/application
            // acknowledgement. Any exception stops; there is no blind replay.
            resident->published();
            const auto residentWholeEnd=std::chrono::steady_clock::now();
            const double wholeMs=std::chrono::duration<double,std::milli>(residentWholeEnd-residentWholeStart).count();
            std::cout<<"GPU_RESIDENT_MAIN frame="<<residentLogical<<" consumed="<<residentConsumed<<" timestamp_ms="<<time[0]/1000
              <<" startup_cpu_oracle="<<residentOracle<<" whole_ms="<<wholeMs
              <<" whole_start_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(residentWholeStart.time_since_epoch()).count()
              <<" whole_end_ns="<<std::chrono::duration_cast<std::chrono::nanoseconds>(residentWholeEnd.time_since_epoch()).count()<<" includes_fifo=1 includes_pack_ipc=1 includes_json=1 includes_all_socket_writes=1 backlog=";
            for(size_t i=0;i<captureBacklogSamples.size();++i)std::cout<<(i?",":"")<<captureBacklogSamples[i];
            std::cout<<" drops=";
            for(size_t i=0;i<captureDroppedSamples.size();++i)std::cout<<(i?",":"")<<captureDroppedSamples[i];
            std::cout<<'\n';
          }
          time.clear();

        }
        else
        {
          // short delay to prevent tight looping
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    } catch (const std::exception& error) {
      if(resident)resident->stop();
      capture->processing_error(error.what());
      capture->request_input_stop();
    } catch (...) {
      if(resident)resident->stop();
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
