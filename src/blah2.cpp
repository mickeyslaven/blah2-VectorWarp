/// @file blah2.cpp
/// @brief A real-time radar.
/// @author 30hours

#include "capture/Capture.h"
#include "capture/PairedCpiQueue.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/meta/Timing.h"
#include "data/meta/EventTime.h"
#include "data/Track.h"
#include "process/ambiguity/Ambiguity.h"
#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/ambiguity/BearingManifold.h"
#include "process/ambiguity/ManifoldEvidence.h"
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
#include "process/analysis/ComplexCutoutRecorder.h"
#include "process/analysis/ReplayEchoInjector.h"
#include "process/detection/CfarDetector1D.h"
#include "process/detection/Centroid.h"
#include "process/detection/Interpolate.h"
#include "process/spectrum/SpectrumAnalyser.h"
#include "process/tracker/Tracker.h"
#include "process/utility/Socket.h"
#include "process/utility/ProcessingThreads.h"
#include "data/meta/Constants.h"
#include "../core/TrackBeforeDetect.private.h"

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
#include <optional>

volatile sig_atomic_t STOP_SIGNAL = 0;
std::unique_ptr<Socket> socket_map;
std::unique_ptr<Socket> socket_detection;
std::unique_ptr<Socket> socket_track;
std::unique_ptr<Socket> socket_bearing;
std::unique_ptr<Socket> socket_manifold;
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

std::optional<std::complex<double>> direct_path_pilot(
  const IqData& reference, const std::vector<std::unique_ptr<IqData>>& surveillance,
  const std::vector<uint32_t>& surveillanceChannels,
  const std::vector<uint32_t>& referenceChannels,
  const std::vector<std::complex<double>>& referenceWeights)
{
  const auto& referenceSamples = reference.view_data();
  if (referenceSamples.empty() || surveillance.size() != surveillanceChannels.size())
    return std::nullopt;
  std::complex<double> pilot{};
  for (std::size_t path = 0; path < surveillance.size(); ++path)
  {
    const auto& samples = surveillance[path]->view_data();
    if (samples.size() != referenceSamples.size()) return std::nullopt;
    std::complex<double> correlation{};
    double referenceEnergy = 0, surveillanceEnergy = 0;
    for (std::size_t sample = 0; sample < samples.size(); ++sample)
    {
      correlation += samples[sample] * std::conj(referenceSamples[sample]);
      referenceEnergy += std::norm(referenceSamples[sample]);
      surveillanceEnergy += std::norm(samples[sample]);
    }
    const double normalization = std::sqrt(referenceEnergy * surveillanceEnergy);
    if (!(normalization > 1e-30) || !std::isfinite(normalization)) continue;
    auto weight = std::complex<double>{1, 0};
    const auto found = std::find(referenceChannels.begin(), referenceChannels.end(),
      surveillanceChannels[path]);
    if (found != referenceChannels.end())
    {
      const std::size_t index = std::distance(referenceChannels.begin(), found);
      if (index < referenceWeights.size()) weight = std::conj(referenceWeights[index]);
    }
    pilot += weight * correlation / normalization;
  }
  if (!std::isfinite(pilot.real()) || !std::isfinite(pilot.imag()) ||
      std::abs(pilot) <= 1e-30) return std::nullopt;
  return pilot;
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
  std::string replayProvenanceMode = "legacy-unqualified";
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
  if (tree["capture"]["replay"].has_child("provenance"))
    tree["capture"]["replay"]["provenance"] >> replayProvenanceMode;
  tree["network"]["ip"] >> ip_capture;
  tree["network"]["ports"]["api"] >> port_capture;
  if (fs == 0 || fc == 0 || port_capture == 0)
  {
    std::cerr << "Invalid configuration: sample rate, centre frequency and API port must be positive.\n";
    return 1;
  }

  // set up socket
  sleep(2);
  bool bearingEnabled=false,manifoldRecorderEnabled=false;
  uint32_t bearingMinConsistent=3,bearingMinPeakSeparationDb=3;
  std::string bearingMethod="coherent_beam_bank",bearingReferenceAnchor="live_eigenbeam";uint32_t bearingScanBeams=0,
    bearingCandidatePeaks=3,bearingGuardRadius=1,bearingTrainingRadius=3;
  double bearingDiagonalLoading=.01,bearingMaxRateDegPerSecond=0;std::vector<uint32_t>bearingChannelMapping;
  std::string bearingManifoldFile;
  if(tree["process"].has_child("bearing")){
    auto bearing=tree["process"]["bearing"];
    if(bearing.has_child("enable"))bearing["enable"]>>bearingEnabled;
    if(bearing.has_child("min_consistent"))bearing["min_consistent"]>>bearingMinConsistent;
    if(bearing.has_child("min_peak_separation_db"))
      bearing["min_peak_separation_db"]>>bearingMinPeakSeparationDb;
    if(bearing.has_child("method"))bearing["method"]>>bearingMethod;
    if(bearing.has_child("reference_anchor"))bearing["reference_anchor"]>>bearingReferenceAnchor;
    if(bearing.has_child("scan_beams"))bearing["scan_beams"]>>bearingScanBeams;
    if(bearing.has_child("candidate_peaks"))bearing["candidate_peaks"]>>bearingCandidatePeaks;
    if(bearing.has_child("guard_radius"))bearing["guard_radius"]>>bearingGuardRadius;
    if(bearing.has_child("training_radius"))bearing["training_radius"]>>bearingTrainingRadius;
    if(bearing.has_child("diagonal_loading"))bearing["diagonal_loading"]>>bearingDiagonalLoading;
    if(bearing.has_child("max_rate_deg_s"))bearing["max_rate_deg_s"]>>bearingMaxRateDegPerSecond;
    if(bearing.has_child("manifold_file"))bearing["manifold_file"]>>bearingManifoldFile;
    if(bearing.has_child("channel_mapping"))for(auto value:bearing["channel_mapping"].children()){
      uint32_t channel=0;value>>channel;bearingChannelMapping.push_back(channel);}
    if(bearing.has_child("manifold_recorder")){
      auto recorder=bearing["manifold_recorder"];
      bool automatic=!recorder.has_child("enabled");
      if(recorder.has_child("enabled")){
        std::string mode;recorder["enabled"]>>mode;
        if(mode=="true"||mode=="on")manifoldRecorderEnabled=true;
        else if(mode=="auto")automatic=true;
        else if(mode!="false"&&mode!="off")throw std::invalid_argument("Manifold recorder enabled must be auto, on or off");
      }
      if(automatic){
        std::string fusion="noncoherent";bool detection=false,adsb=false;
        if(tree["process"].has_child("surveillance_fusion")&&tree["process"]["surveillance_fusion"].has_child("mode"))
          tree["process"]["surveillance_fusion"]["mode"]>>fusion;
        if(tree["process"].has_child("detection")&&tree["process"]["detection"].has_child("enable"))
          tree["process"]["detection"]["enable"]>>detection;
        if(tree.rootref().has_child("truth")&&tree["truth"].has_child("adsb")&&tree["truth"]["adsb"].has_child("enabled"))
          tree["truth"]["adsb"]["enabled"]>>adsb;
        bool mapping=false,geometry=false,orientation=false;
        if(tree["capture"]["device"].has_child("array_geometry")){
          auto array=tree["capture"]["device"]["array_geometry"];
          if(array.has_child("mapping_confirmed"))array["mapping_confirmed"]>>mapping;
          if(array.has_child("geometry_confirmed"))array["geometry_confirmed"]>>geometry;
          if(array.has_child("orientation_confirmed"))array["orientation_confirmed"]>>orientation;
        }
        manifoldRecorderEnabled=type=="Kraken"&&fusion!="noncoherent"&&detection&&adsb&&mapping&&geometry&&orientation;
      }
    }
  }
  // Fail closed before opening output sockets or allocating/opening capture.
  // This anchor is an offline experiment; native IQ is the only admitted input.
  if(bearingReferenceAnchor=="fixed_site_projected"){
    if(!state||replayFormat!="blah2")
      throw std::invalid_argument("fixed_site_projected is experimental and requires explicit native blah2 replay");
    auto process=tree["process"],device=tree["capture"]["device"];
    const auto allFive=[](ryml::NodeRef values){
      uint32_t index=0;for(auto child:values.children()){uint32_t value=UINT32_MAX;child>>value;
        if(value!=index++)return false;}return index==5;
    };
    uint32_t channels=0;bool clutter=false;std::string reference,fusion,acceleration;double nullDb=0;
    if(device.has_child("channel_count"))device["channel_count"]>>channels;
    if(process.has_child("reference_synthesis")&&process["reference_synthesis"].has_child("mode"))
      process["reference_synthesis"]["mode"]>>reference;
    if(process.has_child("clutter")&&process["clutter"].has_child("enable"))process["clutter"]["enable"]>>clutter;
    if(process.has_child("performance")&&process["performance"].has_child("acceleration"))
      process["performance"]["acceleration"]>>acceleration;
    if(process.has_child("surveillance_fusion")){
      auto node=process["surveillance_fusion"];
      if(node.has_child("mode"))node["mode"]>>fusion;
      if(node.has_child("direct_path_null_db"))node["direct_path_null_db"]>>nullDb;
    }
    if(type!="Kraken"||channels!=5||!device.has_child("surveillance_channels")||
       !allFive(device["surveillance_channels"])||reference!="array_eigenbeam"||
       !process["reference_synthesis"].has_child("channels")||!allFive(process["reference_synthesis"]["channels"])||
       !clutter||acceleration!="gpu-resident"||!bearingEnabled||bearingMethod!="covariance_mvdr"||
       tree["process"]["bearing"].has_child("manifold_file")||fusion!="coherent_beam_bank"||nullDb!=0)
      throw std::invalid_argument("fixed_site_projected requires enabled five-channel GPU-resident array-eigenbeam clutter cancellation, ideal covariance_mvdr bearing and coherent_beam_bank with direct_path_null_db: 0");
  }
  uint16_t port_map, port_detection, port_timestamp, 
    port_timing, port_iqdata, port_track;
  uint16_t port_bearing=0,port_manifold=0;
  std::string ip;
  tree["network"]["ports"]["map"] >> port_map;
  tree["network"]["ports"]["detection"] >> port_detection;
  tree["network"]["ports"]["track"] >> port_track;
  if(bearingEnabled){
    if(!tree["network"]["ports"].has_child("bearing"))
      throw std::invalid_argument("Bearing output requires network.ports.bearing");
    tree["network"]["ports"]["bearing"]>>port_bearing;
  }
  if(manifoldRecorderEnabled){
    if(!tree["network"]["ports"].has_child("manifold"))
      throw std::invalid_argument("Manifold recorder requires network.ports.manifold");
    tree["network"]["ports"]["manifold"]>>port_manifold;
  }
  tree["network"]["ports"]["timestamp"] >> port_timestamp;
  tree["network"]["ports"]["timing"] >> port_timing;
  tree["network"]["ports"]["iqdata"] >> port_iqdata;
  tree["network"]["ip"] >> ip;
  
  try {
    socket_map = std::make_unique<Socket>(ip, port_map);
    socket_detection = std::make_unique<Socket>(ip, port_detection);
    socket_track = std::make_unique<Socket>(ip, port_track);
    if(bearingEnabled)socket_bearing = std::make_unique<Socket>(ip, port_bearing);
    if(manifoldRecorderEnabled)socket_manifold = std::make_unique<Socket>(ip, port_manifold);
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
  double effectiveBeamTrials = 0;
  double directPathNullDb = 0;
  std::string surveillanceManifoldFile;
  bool surveillanceManifoldConfigured = false;
  std::vector<std::complex<float>> surveillanceManifoldResponses;
  std::optional<blah2::bearing_manifold::Calibration> surveillanceManifoldCalibration,
    bearingManifoldCalibration;
  std::vector<std::array<double, 3>> coherentPositions;
  double directPathAzimuthRad = 0,xAxisBearingDeg = 0,bearingUncertaintyFloorDeg = 0;
  if (tree["process"].has_child("surveillance_fusion"))
  {
    auto fusion = tree["process"]["surveillance_fusion"];
    if (fusion.has_child("mode")) fusion["mode"] >> surveillanceFusionMode;
    if (fusion.has_child("beams")) fusion["beams"] >> coherentBeamCount;
    if (fusion.has_child("effective_beam_trials"))
      fusion["effective_beam_trials"] >> effectiveBeamTrials;
    if (fusion.has_child("direct_path_null_db"))
      fusion["direct_path_null_db"] >> directPathNullDb;
    if (fusion.has_child("manifold_file")) {
      surveillanceManifoldConfigured = true;
      fusion["manifold_file"] >> surveillanceManifoldFile;
    }
  }
  if (surveillanceFusionMode != "noncoherent" &&
      surveillanceFusionMode != "coherent_beam_bank" &&
      surveillanceFusionMode != "noise_whitened_beam_bank")
    throw std::invalid_argument("Unknown surveillance-fusion mode");
  const bool coherentFusion = surveillanceFusionMode != "noncoherent";
  if (!effectiveBeamTrials) effectiveBeamTrials = coherentBeamCount;
  if (!std::isfinite(effectiveBeamTrials) || effectiveBeamTrials < 1 ||
      effectiveBeamTrials > coherentBeamCount)
    throw std::invalid_argument("Effective beam trials must be from 1 through the beam count");
  if (!std::isfinite(directPathNullDb) || directPathNullDb < 0 || directPathNullDb > 40)
    throw std::invalid_argument("Direct-path null depth must be from 0 through 40 dB");
  if (!coherentFusion && (effectiveBeamTrials != coherentBeamCount || directPathNullDb != 0 ||
      surveillanceManifoldConfigured))
    throw std::invalid_argument("Beam trial/null controls require coherent surveillance fusion");
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
    double apertureM = 0;
    for (size_t a = 0; a < coherentPositions.size(); ++a)
      for (size_t b = a + 1; b < coherentPositions.size(); ++b)
      {
        const double dx = coherentPositions[a][0] - coherentPositions[b][0];
        const double dy = coherentPositions[a][1] - coherentPositions[b][1];
        const double dz = coherentPositions[a][2] - coherentPositions[b][2];
        apertureM = std::max(apertureM, std::sqrt(dx * dx + dy * dy + dz * dz));
      }
    if (!(apertureM > 0) || !std::isfinite(apertureM))
      throw std::invalid_argument("Coherent fusion has invalid array aperture");
    // Fine scan spacing interpolates the beam maximum; it does not defeat the
    // physical aperture limit. Publish at least the half-power half-width.
    const double wavelengthM = 299792458.0 / double(fc);
    bearingUncertaintyFloorDeg = std::min(90.0,
      0.443 * wavelengthM / apertureM * 180.0 / std::acos(-1.0));
    {
      if (!arrayReference || referenceChannels != surveillanceChannels)
        throw std::invalid_argument("Coherent fusion requires the array reference to use every surveillance channel in the same order");
      auto geometry = tree["capture"]["device"]["array_geometry"];
      if (!geometry.has_child("x_axis_bearing_deg_true") ||
          !tree["location"].has_child("rx") ||
          !tree["location"].has_child("tx"))
        throw std::invalid_argument("Coherent fusion requires array orientation and receiver/transmitter locations");
      double rxLatDeg = 0, rxLonDeg = 0,
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
    legacyReplayBlockSamples, replayProvenanceMode);

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
  const auto& clutterConfig = tree["process"]["clutter"];
  bool followAmbiguity = false;
  if (clutterConfig.has_child("followAmbiguity"))
    clutterConfig["followAmbiguity"] >> followAmbiguity;
  if (followAmbiguity) {
    // Ambiguity limits are inclusive; Wiener-Hopf consumes a half-open span.
    // Calculate in 64 bits so a malformed boundary cannot wrap into a huge
    // allocation before the startup checks reject it.
    const int64_t derivedDelayMax = int64_t(delayMax) + 1;
    if (derivedDelayMax > INT32_MAX)
      throw std::invalid_argument("Clutter followAmbiguity range exceeds the processor delay limit");
    delayMinClutter = delayMin;
    delayMaxClutter = int32_t(derivedDelayMax);
  } else {
    clutterConfig["delayMin"] >> delayMinClutter;
    clutterConfig["delayMax"] >> delayMaxClutter;
  }
  clutterConfig["enable"] >> isClutter;
  const int64_t clutterBins = int64_t(delayMaxClutter) - delayMinClutter;
  if (isClutter && (clutterBins <= 0 || clutterBins > nSamples || clutterBins > UINT32_MAX))
    throw std::invalid_argument("Clutter delay range must be a non-empty half-open interval no longer than the CPI");

  std::string accelerationMode = "auto";
  if (tree["process"].has_child("performance") &&
      tree["process"]["performance"].has_child("acceleration"))
    tree["process"]["performance"]["acceleration"] >> accelerationMode;
  const char* gpuDevice = std::getenv("BLAH2_GPU_DEVICE");
  const bool residentMode=accelerationMode=="gpu-resident";
  if (surveillanceManifoldConfigured &&
      surveillanceManifoldFile.find_first_not_of(" \t\r\n") == std::string::npos)
    throw std::invalid_argument("Measured surveillance manifold_file must be a non-empty path");
  if (surveillanceManifoldConfigured &&
      surveillanceFusionMode != "coherent_beam_bank" &&
      surveillanceFusionMode != "noise_whitened_beam_bank")
    throw std::invalid_argument("Measured surveillance manifold requires coherent surveillance fusion");
  if (surveillanceFusionMode == "noise_whitened_beam_bank" && !residentMode)
    throw std::invalid_argument("Noise-whitened surveillance fusion requires gpu-resident acceleration");
#ifndef VECTORWARP_RESIDENT_GPU
  if(residentMode)throw std::invalid_argument("This cohort does not provide the resident ABI10 GPU route");
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
    // Offline target-protected reference experiment.  A caller selects a
    // member of a fixed, complete azimuth bank; it never consumes ADS-B truth
    // at runtime.  The chosen direction is nulled in the reference before
    // ECA/CAF, while the measured eigenbeam remains the direct-path anchor.
    if (node.has_child("target_protection_beams") ||
        node.has_child("target_protection_beam_index"))
    {
      if (!state)
        throw std::invalid_argument("Target-protection reference is replay-only");
      uint32_t beams = 0, index = 0;
      if (node.has_child("target_protection_beams"))
        node["target_protection_beams"] >> beams;
      if (node.has_child("target_protection_beam_index"))
        node["target_protection_beam_index"] >> index;
      if (beams < 4 || beams > 360 || index >= beams ||
          coherentPositions.empty())
        throw std::invalid_argument("Invalid target-protection beam-bank configuration");
      const double targetAzimuth = 2 * std::acos(-1.0) * double(index) / double(beams);
      const double directX = std::cos(directPathAzimuthRad);
      const double directY = std::sin(directPathAzimuthRad);
      const double targetX = std::cos(targetAzimuth);
      const double targetY = std::sin(targetAzimuth);
      const double waveNumber = 2 * std::acos(-1.0) * double(fc) / 299792458.0;
      referenceConfig.protectionRelativeSteering.reserve(coherentPositions.size());
      for (const auto& position : coherentPositions)
      {
        const double relativePhase = waveNumber *
          (position[0] * (targetX - directX) +
           position[1] * (targetY - directY));
        referenceConfig.protectionRelativeSteering.push_back(
          std::polar(1.0, -relativePhase));
      }
      std::cout << "[ReferenceSynthesis] target_protection_beam=" << index
        << "/" << beams << " azimuth_deg=" <<
        targetAzimuth * 180.0 / std::acos(-1.0) << "\n";
    }
  }
  ArrayReferenceSynthesizer referenceSynthesizer(referenceConfig);
  uint64_t reportedReferenceUpdate = 0;
  uint64_t replayProcessedFrames = 0;
  Noncoherent mapFusion;
  std::unique_ptr<CoherentBeamBank> coherentMapFusion;
  blah2::bearing_manifold::Identity manifoldIdentity;
  if (coherentFusion && (surveillanceManifoldConfigured || !bearingManifoldFile.empty()))
  {
    auto device=tree["capture"]["device"];
    auto geometry=device["array_geometry"];
    bool mappingConfirmed=false,geometryConfirmed=false,orientationConfirmed=false;
    if(geometry.has_child("mapping_confirmed"))geometry["mapping_confirmed"]>>mappingConfirmed;
    if(geometry.has_child("geometry_confirmed"))geometry["geometry_confirmed"]>>geometryConfirmed;
    if(geometry.has_child("orientation_confirmed"))geometry["orientation_confirmed"]>>orientationConfirmed;
    if(!mappingConfirmed||!geometryConfirmed||!orientationConfirmed)
      throw std::invalid_argument("Measured manifold requires confirmed channel mapping, geometry, and orientation");
    manifoldIdentity.channelMapping = surveillanceChannels;
    manifoldIdentity.positionsM = coherentPositions;
    manifoldIdentity.frequencyHz = double(fc);
    manifoldIdentity.sampleRateHz = double(fs);
    manifoldIdentity.xAxisBearingDeg = xAxisBearingDeg;
    manifoldIdentity.directPathAzimuthDeg=std::fmod(std::fmod(
      directPathAzimuthRad*180.0/std::acos(-1.0),360.0)+360.0,360.0);
    auto receiver=tree["location"]["rx"],transmitter=tree["location"]["tx"];
    receiver["latitude"]>>manifoldIdentity.receiverLocation[0];
    receiver["longitude"]>>manifoldIdentity.receiverLocation[1];
    receiver["altitude"]>>manifoldIdentity.receiverLocation[2];
    transmitter["latitude"]>>manifoldIdentity.transmitterLocation[0];
    transmitter["longitude"]>>manifoldIdentity.transmitterLocation[1];
    transmitter["altitude"]>>manifoldIdentity.transmitterLocation[2];
    auto heimdall=device["heimdall"];
    if(heimdall.has_child("gain_control")){
      std::string gainControl;heimdall["gain_control"]>>gainControl;
      if(gainControl!="fixed")
        throw std::invalid_argument("Measured manifold requires fixed gain control");
    }
    std::vector<double> captureGains;
    if(heimdall.has_child("channel_gains"))
      for(auto value:heimdall["channel_gains"].children()){double gain=0;value>>gain;captureGains.push_back(gain);}
    else if(heimdall.has_child("gain")){
      std::string gainText;heimdall["gain"]>>gainText;size_t consumed=0;double gain=0;
      try{gain=std::stod(gainText,&consumed);}catch(...){consumed=0;}
      if(consumed!=gainText.size()||!std::isfinite(gain)||gain<0)
        throw std::invalid_argument("Measured manifold requires an explicit fixed requested gain");
      captureGains.assign(captureChannelCount,gain);
    }
    if(captureGains.size()!=captureChannelCount)
      throw std::invalid_argument("Measured manifold requested gain vector differs from capture channels");
    for(uint32_t channel:surveillanceChannels){
      const double gain=captureGains[channel];
      if(!std::isfinite(gain)||gain<0||gain>50)
        throw std::invalid_argument("Measured manifold requested gain is invalid");
      manifoldIdentity.requestedGainDb.push_back(gain);
    }
  }
  if (coherentFusion)
  {
    if (surveillanceManifoldConfigured)
    {
      surveillanceManifoldCalibration=blah2::bearing_manifold::load(surveillanceManifoldFile,
        double(fc),coherentBeamCount,uint32_t(coherentPositions.size()),manifoldIdentity);
      surveillanceManifoldResponses = surveillanceManifoldCalibration->responses;
    }
    const std::vector<std::complex<double>> surveillanceManifold(
      surveillanceManifoldResponses.begin(), surveillanceManifoldResponses.end());
    coherentMapFusion = std::make_unique<CoherentBeamBank>(
      CoherentBeamBank::Config{double(fc), coherentBeamCount, coherentPositions,
        directPathAzimuthRad, directPathNullDb, surveillanceManifold});
    std::cout << "[SurveillanceFusion] mode=" << surveillanceFusionMode << " beams="
      << coherentBeamCount << " channels=" << coherentPositions.size();
    std::cout << " direct_path_azimuth_deg=" <<
      directPathAzimuthRad * 180.0 / std::acos(-1.0);
    std::cout << " effective_beam_trials=" << effectiveBeamTrials
      << " direct_path_null_db=" << directPathNullDb;
    if (surveillanceManifoldConfigured)
      std::cout << " measured_manifold=" << surveillanceManifoldFile;
    std::cout << "\n";
  }
  else
    std::cout << "[SurveillanceFusion] mode=noncoherent\n";

  blah2::analysis::ReplayEchoInjector::Config echoConfig;
  if (tree["capture"]["replay"].has_child("inject_echo")) {
    auto echo = tree["capture"]["replay"]["inject_echo"];
    if (echo.has_child("state")) echo["state"] >> echoConfig.enabled;
    if (echo.has_child("amplitude_db")) echo["amplitude_db"] >> echoConfig.amplitudeDb;
    if (echo.has_child("delay_km")) echo["delay_km"] >> echoConfig.delayKm;
    if (echo.has_child("doppler_hz")) echo["doppler_hz"] >> echoConfig.dopplerHz;
    double targetAzimuthDeg = 0;
    if (echo.has_child("target_azimuth_deg_array"))
      echo["target_azimuth_deg_array"] >> targetAzimuthDeg;
    echoConfig.targetAzimuthRad = targetAzimuthDeg * std::acos(-1.0) / 180.0;
  }
  blah2::analysis::ReplayEchoInjector echoInjector(echoConfig, fs, fc,
    directPathAzimuthRad, coherentPositions, state);

  // set up process detection
  double pfa, minDoppler;
  int8_t nGuard, nTrain;
  int8_t minDelay;
  uint32_t integrationCpis = 1;
  uint32_t integrationMotionBins = 0;
  bool trackBeforeDetect = false;
  uint32_t trackBeforeDetectCpis = 8;
  uint32_t trackBeforeDetectMotionBins = 1;
  uint32_t trackBeforeDetectDelayMotionBins = 1;
  uint32_t trackBeforeDetectDopplerMotionBins = 1;
  uint32_t integrationBeamTolerance = UINT32_MAX;
  tree["process"]["detection"]["pfa"] >> pfa;
  tree["process"]["detection"]["nGuard"] >> nGuard;
  tree["process"]["detection"]["nTrain"] >> nTrain;
  tree["process"]["detection"]["minDelay"] >> minDelay;
  tree["process"]["detection"]["minDoppler"] >> minDoppler;
  if (tree["process"]["detection"].has_child("integration_cpis"))
    tree["process"]["detection"]["integration_cpis"] >> integrationCpis;
  if (tree["process"]["detection"].has_child("integration_motion_bins"))
    tree["process"]["detection"]["integration_motion_bins"] >> integrationMotionBins;
  if (tree["process"]["detection"].has_child("track_before_detect"))
  {
    auto node = tree["process"]["detection"]["track_before_detect"];
    if (node.has_child("enable")) node["enable"] >> trackBeforeDetect;
    if (node.has_child("max_cpis")) node["max_cpis"] >> trackBeforeDetectCpis;
    if (node.has_child("motion_bins")) {
      node["motion_bins"] >> trackBeforeDetectMotionBins;
      trackBeforeDetectDelayMotionBins=trackBeforeDetectMotionBins;
      trackBeforeDetectDopplerMotionBins=trackBeforeDetectMotionBins;
    }
    if(node.has_child("delay_motion_bins"))node["delay_motion_bins"]>>trackBeforeDetectDelayMotionBins;
    if(node.has_child("doppler_motion_bins"))node["doppler_motion_bins"]>>trackBeforeDetectDopplerMotionBins;
  }
  if (tree["process"]["tracker"].has_child("beam_tolerance"))
    tree["process"]["tracker"]["beam_tolerance"] >> integrationBeamTolerance;
  if (integrationCpis < 1 || integrationCpis > 2)
    throw std::invalid_argument("Detection integration_cpis must be 1 or 2");
  if (integrationCpis > 1 && !residentMode)
    throw std::invalid_argument("Multi-CPI detection requires gpu-resident acceleration");
  if (integrationMotionBins > 1 || (integrationMotionBins && integrationCpis != 2))
    throw std::invalid_argument("Detection integration_motion_bins requires two-CPI integration and must be 0 or 1");
  if (trackBeforeDetect && (!residentMode || !coherentFusion))
    throw std::invalid_argument("Track-before-detect requires gpu-resident coherent surveillance fusion");
  if(bearingEnabled&&(!residentMode||!coherentFusion))
    throw std::invalid_argument("Bearing output requires GPU-resident coherent tracking");
  if(manifoldRecorderEnabled&&!coherentFusion)
    throw std::invalid_argument("Manifold recording requires coherent detection");
  if(bearingEnabled&&!trackBeforeDetect)
    throw std::invalid_argument("Bearing output requires track-before-detect candidate maps");
  if(bearingMethod!="coherent_beam_bank"&&bearingMethod!="covariance_mvdr")
    throw std::invalid_argument("Bearing method must be coherent_beam_bank or covariance_mvdr");
  if(bearingReferenceAnchor!="live_eigenbeam"&&bearingReferenceAnchor!="fixed_site"&&bearingReferenceAnchor!="fixed_site_projected")
    throw std::invalid_argument("Bearing reference_anchor must be live_eigenbeam, fixed_site or fixed_site_projected");
  if(!bearingScanBeams)bearingScanBeams=coherentBeamCount*4u;
  if(bearingEnabled&&(bearingScanBeams<coherentBeamCount||bearingScanBeams>127||
      bearingScanBeams%coherentBeamCount))
    throw std::invalid_argument("Bearing scan_beams must be a beam-count multiple from the beam count through 127");
  if(bearingCandidatePeaks!=3||bearingGuardRadius>3||bearingTrainingRadius<=bearingGuardRadius||
      bearingTrainingRadius>6||!std::isfinite(bearingDiagonalLoading)||
      bearingDiagonalLoading<=0||bearingDiagonalLoading>1)
    throw std::invalid_argument("Invalid bearing covariance/candidate configuration");
  if(bearingMinConsistent<2||bearingMinConsistent>100)
    throw std::invalid_argument("Bearing min_consistent must be from 2 through 100");
  if(bearingMinPeakSeparationDb<1||bearingMinPeakSeparationDb>15)
    throw std::invalid_argument("Bearing min_peak_separation_db must be from 1 through 15");
  if(!std::isfinite(bearingMaxRateDegPerSecond)||bearingMaxRateDegPerSecond<0||bearingMaxRateDegPerSecond>360)
    throw std::invalid_argument("Bearing max_rate_deg_s must be from 0 through 360");
  if (trackBeforeDetect && (!track_before_detect::validMaximum(trackBeforeDetectCpis) ||
      trackBeforeDetectDelayMotionBins > 2 || trackBeforeDetectDopplerMotionBins > 16))
    throw std::invalid_argument("Track-before-detect requires max_cpis 4 or 8, delay_motion_bins 0 through 2, and doppler_motion_bins 0 through 16");
  if (integrationBeamTolerance != UINT32_MAX &&
      (!coherentFusion || integrationBeamTolerance > coherentBeamCount / 2))
    throw std::invalid_argument("Tracker beam_tolerance requires coherent fusion and at most half the beam count");
  // A beam bank reports the maximum of several trials per cell. Apply a
  // Bonferroni correction so its configured false-alarm probability retains
  // the same upper bound as the single noncoherent statistic.
  const double motionTrials = trackBeforeDetect
    ? track_before_detect::trialCount(trackBeforeDetectCpis,
      trackBeforeDetectDelayMotionBins,trackBeforeDetectDopplerMotionBins)
    : (integrationMotionBins ? 9.0 : 1.0);
  // TBD adds its motion-bank correction inside the resident detector while
  // retaining the proven two-CPI baseline at this beam-corrected PFA.
  const double detectorPfa = coherentFusion
    ? pfa / (effectiveBeamTrials * (trackBeforeDetect ? 1.0 : motionTrials))
    : pfa / (trackBeforeDetect ? 1.0 : motionTrials);
  if (coherentFusion)
    std::cout << "[SurveillanceFusion] configured_pfa=" << pfa
      << " effective_pfa=" << detectorPfa << " effective_beam_trials="
      << effectiveBeamTrials << " motion_trials=" << motionTrials;
  if (coherentFusion && trackBeforeDetect)
    std::cout << " tbd_effective_pfa=" << detectorPfa / motionTrials;
  if (coherentFusion) std::cout << "\n";
  if (trackBeforeDetect)
    std::cout << "[TrackBeforeDetect] enabled=1 banks="
      << track_before_detect::bankCount(trackBeforeDetectCpis)
      << " max_cpis=" << trackBeforeDetectCpis
      << " delay_motion_bins=" << trackBeforeDetectDelayMotionBins
      << " doppler_motion_bins=" << trackBeforeDetectDopplerMotionBins
      << " coherent_refinement_factor="
      << (bearingEnabled ? bearingScanBeams / coherentBeamCount : 0u)
      << " bearing_method=" << (bearingEnabled ? bearingMethod : "disabled") << "\n";
  CfarDetector1D *cfarDetector1D = new CfarDetector1D(detectorPfa, nGuard, nTrain, minDelay, minDoppler);
  Interpolate *interpolate = new Interpolate(true, true);

  // set up process centroid
  uint16_t nCentroid;
  tree["process"]["detection"]["nCentroid"] >> nCentroid;
  Centroid *centroid = new Centroid(nCentroid, nCentroid, 1/tCpi);

  // set up process tracker
  uint8_t m, n, nDelete;
  double maxAcc, rangeRes, lambda;
  std::string smooth,trackerAccelerationModel="hypothesis_bank";
  bool stationaryClutter=residentMode;
  uint32_t stationaryPersistenceCpis=residentMode?60u:0u;
  // Zero means the full configured ambiguity Doppler span. A positive legacy
  // value remains an optional safety bound for installations that need one.
  double stationaryMaxDopplerHz=0.0;
  bool trackGuidedShadow=false;
  bool trackGuidedPromote=false;
  uint32_t trackGuidedShadowMaxTracks=0;
  tree["process"]["tracker"]["initiate"]["M"] >> m;
  tree["process"]["tracker"]["initiate"]["N"] >> n;
  tree["process"]["tracker"]["delete"] >> nDelete;
  tree["process"]["tracker"]["initiate"]["maxAcc"] >> maxAcc;
  if(tree["process"]["tracker"]["initiate"].has_child("acceleration_model"))
    tree["process"]["tracker"]["initiate"]["acceleration_model"] >> trackerAccelerationModel;
  if(tree["process"]["tracker"]["initiate"].has_child("stationary_clutter")){
    const auto& stationary=tree["process"]["tracker"]["initiate"]["stationary_clutter"];
    stationary["enable"] >> stationaryClutter;
    if(stationaryClutter){stationary["persistence_cpis"] >> stationaryPersistenceCpis;
      if(stationary.has_child("max_doppler_hz"))stationary["max_doppler_hz"] >> stationaryMaxDopplerHz;}
    else {stationaryPersistenceCpis=0;stationaryMaxDopplerHz=0;}
  }
  if(tree["process"]["tracker"].has_child("track_guided_shadow")){
    const auto& guided=tree["process"]["tracker"]["track_guided_shadow"];
    guided["enable"] >> trackGuidedShadow;
    if(trackGuidedShadow){guided["max_tracks"] >> trackGuidedShadowMaxTracks;
      if(guided.has_child("promote"))guided["promote"] >> trackGuidedPromote;}
  }
  if(trackerAccelerationModel!="hypothesis_bank"&&trackerAccelerationModel!="adaptive_gate")
    throw std::invalid_argument("Tracker acceleration_model must be hypothesis_bank or adaptive_gate");
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
  if(manifoldRecorderEnabled&&!isDetection)
    throw std::invalid_argument("Manifold recording requires GPU-resident coherent detection");
  if(bearingEnabled&&!isTracker)
    throw std::invalid_argument("Bearing output requires tracking to be enabled");
  if (integrationBeamTolerance != UINT32_MAX && !isTracker)
    throw std::invalid_argument("Tracker beam_tolerance requires tracking to be enabled");
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
      const auto fusionMode=surveillanceFusionMode == "noise_whitened_beam_bank"
        ? blah2::resident::FusionMode::noiseWhitenedRegularPolygon
        : blah2::resident::FusionMode::coherentRegularPolygon;
      residentConfig.fusion={fusionMode,
        coherentBeamCount,double(fc),radius,angles[0],step,directPathAzimuthRad,
        directPathNullDb,surveillanceManifoldResponses};
      if(bearingEnabled){if(bearingChannelMapping.empty())for(uint32_t i=0;i<surveillanceChannels.size();++i)bearingChannelMapping.push_back(i);
       std::vector<std::complex<float>> manifold;
       if(!bearingManifoldFile.empty()){bearingManifoldCalibration=blah2::bearing_manifold::load(
        bearingManifoldFile,double(fc),bearingScanBeams,uint32_t(surveillanceChannels.size()),manifoldIdentity);
        manifold=bearingManifoldCalibration->responses;}
       residentConfig.bearing={true,bearingMethod=="covariance_mvdr"?blah2::resident::BearingMethod::covarianceMvdr:blah2::resident::BearingMethod::coherentBeamBank,
        bearingReferenceAnchor=="fixed_site_projected"?blah2::resident::BearingReferenceAnchor::fixedSiteProjected:
        bearingReferenceAnchor=="fixed_site"?blah2::resident::BearingReferenceAnchor::fixedSite:blah2::resident::BearingReferenceAnchor::liveEigenbeam,
        bearingScanBeams,bearingCandidatePeaks,bearingGuardRadius,bearingTrainingRadius,bearingDiagonalLoading,bearingChannelMapping,manifold};}
    }
    const double spectrumResolution=double(fs)/nSamples;
    const uint32_t decimation=uint32_t(std::min<double>(nSamples,std::max(1.0,std::ceil(spectrumBandwidth/spectrumResolution))));
    residentConfig.spectrumBins=1+(nSamples-1)/decimation;
    residentConfig.detector={isDetection,true,true,nGuard,nTrain,minDelay,nCentroid,nCentroid,detectorPfa,minDoppler,1/tCpi,
      trackBeforeDetect?trackBeforeDetectCpis:integrationCpis,
      trackBeforeDetect?track_before_detect::encodeMotionBins(trackBeforeDetectDelayMotionBins,
        trackBeforeDetectDopplerMotionBins):integrationMotionBins,
      integrationBeamTolerance};
    if(isTracker){
      uint32_t capacity=0;
      if(!tree["process"]["tracker"].has_child("gpu_capacity"))throw std::invalid_argument("Resident tracker requires an explicit gpu_capacity; capacity is never inferred or shrunk");
      tree["process"]["tracker"]["gpu_capacity"] >> capacity;
      residentConfig.tracker={true,capacity,m,n,nDelete,
        trackerAccelerationModel=="adaptive_gate"?blah2::resident::TrackerInitiationMode::adaptiveGate:blah2::resident::TrackerInitiationMode::hypothesisBank,
        ambiguity.front()->get_cpi(),maxAcc,rangeRes,lambda,stationaryClutter,
        stationaryPersistenceCpis,stationaryMaxDopplerHz,
        trackGuidedShadow,trackGuidedPromote,trackGuidedShadowMaxTracks};
    }
    residentConfig.presence=blah2::resident::wire::mandatoryPresence(residentConfig)|blah2::resident::bit(blah2::resident::Section::filtered)|blah2::resident::bit(blah2::resident::Section::fullReference)|
      (manifoldRecorderEnabled?blah2::resident::bit(blah2::resident::Section::detectionCells):0);
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
  blah2::analysis::ComplexCutoutRecorder::Config complexCutoutConfig;
  if (tree["save"].has_child("complex_cutout")) {
    auto cutout = tree["save"]["complex_cutout"];
    if (cutout.has_child("state")) cutout["state"] >> complexCutoutConfig.enabled;
    if (cutout.has_child("path")) cutout["path"] >> complexCutoutConfig.path;
    if (cutout.has_child("delay_km")) cutout["delay_km"] >> complexCutoutConfig.delayKm;
    if (cutout.has_child("doppler_hz")) cutout["doppler_hz"] >> complexCutoutConfig.dopplerHz;
    if (cutout.has_child("delay_radius_bins"))
      cutout["delay_radius_bins"] >> complexCutoutConfig.delayRadiusBins;
    if (cutout.has_child("doppler_radius_bins"))
      cutout["doppler_radius_bins"] >> complexCutoutConfig.dopplerRadiusBins;
    if (cutout.has_child("max_frames")) cutout["max_frames"] >> complexCutoutConfig.maxFrames;
  }
  blah2::analysis::ComplexCutoutRecorder complexCutout(complexCutoutConfig, fs, state);

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
        bool captureSamplePositionAvailable = false;
        uint64_t captureEpoch = 0, captureFirstEpochSample = 0,
          captureEndEpochSample = 0;
        int64_t captureEpochEstimateNanoseconds = 0;
        int64_t captureDeviceTimeEpochTick = 0;
        std::string captureClockSource, captureTimeSource,
          captureMotherboard, captureSubdevices;
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
              // PairedCpiQueue is the RSPduo capture-epoch authority. Its
              // generation advances on gain/reset/gap boundaries and its
              // first offset is delivered-sample-relative, never UTC.
              if (block.epoch != UINT64_MAX) {
                captureSamplePositionAvailable = true;
                captureEpoch = block.epoch + 1;
                captureFirstEpochSample = block.first;
                captureEndEpochSample = uint64_t(block.first) + nSamples;
              }
              ready = true;
            } catch (...) { pairedCpiQueue->release(slot); throw; }
            if (!pairedCpiQueue->release(slot)) ready = false;
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
            const auto position = capture->live_sample_position();
            if (position.available && !captureBacklogSamples.empty() &&
                std::all_of(captureBacklogSamples.begin(),
                  captureBacklogSamples.end(), [&](uint64_t value) {
                    return value == captureBacklogSamples.front();
                  }) &&
                position.nextSample >= captureBacklogSamples.front() &&
                captureBacklogSamples.front() >= nSamples) {
              captureSamplePositionAvailable = true;
              captureEpoch = position.epoch;
              captureEpochEstimateNanoseconds =
                position.epochEstimateNanoseconds;
              captureDeviceTimeEpochTick = position.deviceTimeEpochTick;
              captureClockSource = position.clockSource;
              captureTimeSource = position.timeSource;
              captureMotherboard = position.motherboard;
              captureSubdevices = position.subdevices;
              captureFirstEpochSample = position.nextSample -
                captureBacklogSamples.front();
              captureEndEpochSample = captureFirstEpochSample + nSamples;
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
          const auto captureGainDb=capture->input_gain_db();
          std::vector<double> surveillanceGainDb;
          if(manifoldRecorderEnabled||surveillanceManifoldCalibration||bearingManifoldCalibration){
            if(captureGainDb.size()!=captureChannelCount)
              throw std::runtime_error("Measured manifold requires per-channel actual gain metadata");
            for(uint32_t channel:surveillanceChannels)surveillanceGainDb.push_back(captureGainDb[channel]);
            if(surveillanceManifoldCalibration)
              blah2::bearing_manifold::validateActualGains(*surveillanceManifoldCalibration,surveillanceGainDb);
            if(bearingManifoldCalibration)
              blah2::bearing_manifold::validateActualGains(*bearingManifoldCalibration,surveillanceGainDb);
          }
          time.push_back(captureStartUs ? captureStartUs : current_time_us());
          auto eventTime = capture->requires_kraken_time_provenance() ?
            blah2::kraken_event_time_unavailable() :
            capture->requires_usrp_time_provenance() ?
              blah2::usrp_event_time_unavailable() :
            capture->requires_hackrf_time_provenance() ?
              blah2::hackrf_event_time_unavailable() :
            capture->requires_rspduo_time_provenance() ?
              blah2::rspduo_event_time_unavailable() :
              blah2::EventTimeProvenance{};
          if (captureSamplePositionAvailable &&
              capture->requires_usrp_time_provenance())
            eventTime = blah2::usrp_cpi_midpoint_time(
              captureDeviceTimeEpochTick, captureFirstEpochSample,
              captureEndEpochSample, fs, captureClockSource,
              captureTimeSource, captureMotherboard, captureSubdevices);
          else if (captureSamplePositionAvailable &&
                   capture->requires_hackrf_time_provenance())
            eventTime = blah2::hackrf_cpi_sample_epoch(captureEpoch,
              captureFirstEpochSample, captureEndEpochSample, fs);
          else if (captureSamplePositionAvailable &&
                   capture->requires_rspduo_time_provenance())
            eventTime = blah2::rspduo_cpi_sample_epoch(captureEpoch,
              captureFirstEpochSample, captureEndEpochSample, fs);
          else if (captureSamplePositionAvailable)
            eventTime = blah2::kraken_cpi_midpoint_time(
              captureEpochEstimateNanoseconds, captureEpoch,
              captureFirstEpochSample, captureEndEpochSample, fs);
          const uint64_t frameTimestampMs = blah2::event_time_milliseconds(
            eventTime, time[0] / 1000);
          const auto attachEventTime = [&](std::string json) {
            return blah2::annotate_event_time(std::move(json), eventTime);
          };
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
          std::vector<IqData*> physicalChannels;
          physicalChannels.reserve(captureData.size());
          for (auto& channel : captureData) physicalChannels.push_back(channel.get());
          echoInjector.apply(trackerFrame * uint64_t(nSamples), physicalChannels);
          const bool residentOracle=resident&&resident->qualifying();
          if(resident){
            if(residentLogical==UINT64_MAX||residentConsumed==UINT64_MAX)throw std::overflow_error("Resident frame sequence exhausted");
            const uint64_t signalTime=state?tStart+(trackerFrame*uint64_t(nSamples)*1000)/fs:frameTimestampMs;
            resident->begin(captureData,{++residentLogical,++residentConsumed,signalTime});
          }
          // This block is the unchanged CPU route, or its bounded startup oracle.
          // Accepted resident frames cannot enter CPU computational stages.
          #ifdef VECTORWARP_MIXED_AUTO
          bool mixedCandidateReady=false,mixedPublished=false;
          #endif
          std::vector<Map<std::complex<double>> *> channelMaps(ambiguity.size());
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
            if (coherentMapFusion)
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
          const auto cutoutPilot = complexCutout.enabled() ? direct_path_pilot(
            *referenceData, surveillanceData, surveillanceChannels, referenceChannels,
            referenceSynthesizer.get_metrics().weights) : std::nullopt;
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
          complexCutout.append(frameTimestampMs, channelMaps,
            referenceSynthesizer.get_metrics().weights, cutoutPilot);
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
              (trackerFrame * uint64_t(nSamples) * 1000) / fs : frameTimestampMs;
            track = tracker->process(detection.get(), trackerTime);
            timing_helper(timing_name, timing_time, time, "tracker");
          }

          } // legacy computation or explicitly labeled startup oracle
          blah2::resident::Coordinator::Publication residentPublication;
          std::string cpuManifoldEvidence;
          if(resident){
            auto evidenceMapping=bearingChannelMapping;
            if(manifoldRecorderEnabled&&evidenceMapping.empty())
              for(uint32_t i=0;i<surveillanceChannels.size();++i)evidenceMapping.push_back(i);
            const blah2::resident::BearingConfig evidenceBearing{
              true,
              bearingMethod=="covariance_mvdr"?blah2::resident::BearingMethod::covarianceMvdr:
                blah2::resident::BearingMethod::coherentBeamBank,
              bearingReferenceAnchor=="fixed_site_projected"?
                blah2::resident::BearingReferenceAnchor::fixedSiteProjected:
                bearingReferenceAnchor=="fixed_site"?blah2::resident::BearingReferenceAnchor::fixedSite:
                blah2::resident::BearingReferenceAnchor::liveEigenbeam,
              bearingScanBeams,bearingCandidatePeaks,bearingGuardRadius,bearingTrainingRadius,
              bearingDiagonalLoading,std::move(evidenceMapping),{}};
            resident->receivePrepared();
            residentPublication=resident->format(frameTimestampMs,
              {bearingEnabled,coherentBeamCount,bearingScanBeams/coherentBeamCount,bearingMinConsistent,xAxisBearingDeg,
               bearingUncertaintyFloorDeg,bearingMinPeakSeparationDb,
               bearingMethod=="covariance_mvdr"?1u:0u,bearingScanBeams,
               ambiguity.front()->get_cpi(),bearingMaxRateDegPerSecond,
               stationaryClutter?stationaryPersistenceCpis:0u,
               stationaryClutter?stationaryMaxDopplerHz:0.0},manifoldRecorderEnabled,
               evidenceBearing,{surveillanceGainDb.data(),surveillanceGainDb.size()});
            resident->commit();
            timing_helper(timing_name,timing_time,time,"gpu_resident_ready");
          }
          else if(manifoldRecorderEnabled)
            cpuManifoldEvidence=blah2::manifold_evidence::cpu(frameTimestampMs,double(fc),double(fs),
              *detection,channelMaps,referenceSynthesizer.get_metrics().weights,surveillanceGainDb);

          // output IqData meta data
          jsonIqData = attachEventTime(resident ?
            std::move(residentPublication.spectrum) :
            referenceData->to_json(frameTimestampMs));
          socket_iqdata->sendData(jsonIqData);

          // output map data
          mapJson = attachEventTime(resident ? std::move(residentPublication.map) :
            map->to_json_km(frameTimestampMs, fs));
          if (saveMap)
          {
            map->save(mapJson, saveMapPath);
          }
          socket_map->sendData(mapJson);

          // output detection data
          if (isDetection)
          {
            detectionJson = attachEventTime(resident ?
              std::move(residentPublication.detections) :
              detection->to_json_km(frameTimestampMs, fs));
            socket_detection->sendData(detectionJson);
            if(manifoldRecorderEnabled)socket_manifold->sendData(attachEventTime(
              resident ? std::move(residentPublication.manifoldEvidence) :
              std::move(cpuManifoldEvidence)));
          }
          if (saveDetection && isDetection)
          {
            detection->save(detectionJson, saveDetectionPath);
          }

          // output tracker data
          if (isTracker)
          {
            jsonTracker = attachEventTime(resident ?
              std::move(residentPublication.tracks) :
              track->to_json(frameTimestampMs));
            socket_track->sendData(jsonTracker);
            if(bearingEnabled)socket_bearing->sendData(attachEventTime(
              std::move(residentPublication.bearing)));
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
          timing->update(frameTimestampMs, timing_time, timing_name);
          timing->set_capture_queues(captureBacklogSamples,captureDroppedSamples);
          timing->set_capture_sample_position(captureSamplePositionAvailable,
            captureEpoch, captureFirstEpochSample, captureEndEpochSample);
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
          jsonTiming = attachEventTime(timing->to_json());
          socket_timing->sendData(jsonTiming);
          timing_time.clear();
          timing_name.clear();

          // output CPI timestamp for updating data
          std::string t0_string = std::to_string(frameTimestampMs);
          socket_timestamp->sendData(t0_string);
          if(resident){
            // sendData is a completed local TCP write, not a browser/application
            // acknowledgement. Any exception stops; there is no blind replay.
            resident->published();
            const auto residentWholeEnd=std::chrono::steady_clock::now();
            const double wholeMs=std::chrono::duration<double,std::milli>(residentWholeEnd-residentWholeStart).count();
            std::cout<<"GPU_RESIDENT_MAIN frame="<<residentLogical<<" consumed="<<residentConsumed<<" timestamp_ms="<<frameTimestampMs
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
