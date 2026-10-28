#include "GpuResidentBackend.h"
#include "GpuResidentProof.h"
#include "GpuResidentWire.h"
#include "../../core/TrackBeforeDetect.private.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <cstring>

using namespace blah2::resident;

namespace {
Config configuration(const char* device) {
  Config c;
  auto& g = c.geometry;
  g.kind = blah2::GpuWorkKind::radar;
  g.range = 12;
  g.doppler = 7;
  g.delays = 3;
  g.channels = 5;
  g.delayMin = -1;
  g.clutterSamples = 64;
  g.clutterBins = 4;
  c.correlation = 9;
  c.sampleRate = 128000;
  c.epoch = 19;
  c.surveillanceChannels = {0, 1, 2, 3, 4};
  c.delayAxis = {-1, 0, 1};
  const double resolution = 1.0 / (63.0 / c.sampleRate);
  c.dopplerAxis = {-3 * resolution, -2 * resolution, -resolution, 0,
    resolution, 2 * resolution, 3 * resolution};
  c.spectrumBins = 64;
  c.reference.channels = {0, 1, 2, 3, 4};
  c.reference.analysisSamples = 64;
  c.fusion = {FusionMode::coherentRegularPolygon, 12, 539000000,
    .2, 0, -2 * std::acos(-1.0) / 5, 80 * std::acos(-1.0) / 180, 6};
  c.detector.enabled = true;
  c.detector.guard = 1;
  c.detector.train = 2;
  // Keep this tiny synthetic scene intentionally detection-rich so the
  // physical qualification executes the guided shader with live track state,
  // rather than merely validating its descriptor and empty dispatch path.
  c.detector.pfa = .9;
  c.detector.centroidDopplerResolution = resolution;
  c.detector.integrationCpis = 8;
  c.detector.integrationMotionBins = track_before_detect::encodeMotionBins(2, 4);
  c.detector.integrationBeamTolerance = 1;
  c.tracker.enabled = true;
  c.tracker.capacity = 64;
  c.tracker.m = 2;
  c.tracker.n = 3;
  c.tracker.nDelete = 4;
  c.tracker.cpi = 63.0 / c.sampleRate;
  c.tracker.rangeRes = 299792458.0 / c.sampleRate;
  c.tracker.lambda = 299792458.0 / c.fusion.frequencyHz;
  c.tracker.trackGuidedShadow = true;
  c.tracker.trackGuidedPromote = true;
  c.tracker.trackGuidedShadowMaxTracks = 64;
  c.bearing = {true, BearingMethod::covarianceMvdr, BearingReferenceAnchor::fixedSite,
    60, 3, 1, 3, .01,
    {0, 1, 2, 3, 4}};
  for (uint32_t beam = 0; beam < c.bearing.scanBeams; ++beam)
    for (uint32_t channel = 0; channel < g.channels; ++channel)
      c.bearing.manifoldResponses.push_back(std::polar(1.0f,
        float(.01 * double(beam) * double(channel + 1))));
  c.device = device;
  c.presence = wire::mandatoryPresence(c);
  return c;
}
struct ExactFrame {
 std::vector<uint8_t> reference,maps,fused,detections,tracker;
 double noise=0,maximum=0;
};
template<class T>std::vector<uint8_t> copyBytes(const T* p,size_t n){
 const auto* bytes=reinterpret_cast<const uint8_t*>(p);return n?std::vector<uint8_t>(bytes,bytes+n*sizeof(T)):std::vector<uint8_t>{};}
void snapshotParity(const char* device,const blah2::GpuProcessOptions& options){
 namespace rr=blah2::resident_replay;unsigned frames=0;
 for(auto mode:{FusionMode::coherentRegularPolygon,FusionMode::noiseWhitenedRegularPolygon})
 for(uint32_t cpis:{1u,2u,4u,8u}){
  std::vector<ExactFrame> baseline;
  for(bool exporting:{false,true}){
   auto c=configuration(device);c.detector.integrationCpis=cpis;c.fusion.mode=mode;
   c.detector.pfa=.6/double(c.fusion.beams);c.reference.analysisInterval=2;
   if(cpis<=2){c.detector.integrationMotionBins=0;c.detector.integrationBeamTolerance=UINT32_MAX;
    c.tracker.trackGuidedShadow=c.tracker.trackGuidedPromote=false;c.tracker.trackGuidedShadowMaxTracks=0;c.bearing={};}
   if(exporting){c.replay.maxBytes=rr::byteLimit;c.replay.maxFrames=9;c.replay.frequencyHz=c.fusion.frequencyHz;
    c.replay.configuredPfa=.6;c.replay.sourceHash.fill(0x33);c.replay.rfHash.fill(0x44);
    c.replay.actualGainDb.assign(c.geometry.channels,29.7);}
   c.presence=wire::mandatoryPresence(c);auto session=createProcess(c,options);
   for(uint64_t frame=1;frame<=9;++frame){auto input=session->input();
    for(size_t i=0;i<input.size;++i){const double phase=.017*double(i)+.031*double(frame)+.11*double(i%5);input.data[i]={std::cos(phase),std::sin(phase)};}
    session->begin({frame,frame,1000+frame});session->receiveReferenceReady();const auto o=session->receivePrepared();
    ExactFrame current{copyBytes(o.referenceCheckpoint.data,o.referenceCheckpoint.size),
     copyBytes(o.maps.data,o.maps.size),copyBytes(o.fused.data,o.fused.size),
     copyBytes(o.detections.data,o.detections.size),copyBytes(o.trackerCheckpoint.data,o.trackerCheckpoint.size),o.noise,o.maxPower};
    if(exporting){const auto& prior=baseline.at(size_t(frame-1));
     if(current.reference!=prior.reference||current.maps!=prior.maps||current.fused!=prior.fused||
      current.detections!=prior.detections||current.tracker!=prior.tracker||current.noise!=prior.noise||current.maximum!=prior.maximum)
      throw std::runtime_error("Replay opt-in changed exact native output");}
    else baseline.push_back(std::move(current));
    session->commit(o.trackerToken);session->published();
    if(exporting){const auto snapshot=session->snapshot();if(snapshot.size()!=rr::requiredBytes(c))throw std::runtime_error("Native snapshot size differs");}
    ++frames;
   }
  }
 }
 std::cout<<"GPU_RESIDENT_REPLAY_PHYSICAL_PASS frames="<<frames<<" cpis=1,2,4,8 whitening=off,on exact=1\n";
}
}

int main(int argc, char** argv) try {
  if (argc != 3 && !(argc==4&&std::string(argv[3])=="--snapshot-parity")) throw std::invalid_argument(
    "usage: testGpuResidentTbdPhysical WORKER DEVICE_ID [--snapshot-parity]");
  blah2::GpuProcessOptions options;
  options.executable = argv[1];
  options.startupMs = 60000;
  options.frameMs = 10000;
  if(argc==4){snapshotParity(argv[2],options);return 0;}
  auto session = createProcess(configuration(argv[2]), options);
  bool sawActiveTracker = false;
  for (uint64_t frame = 1; frame <= 9; ++frame) {
    auto input = session->input();
    for (size_t i = 0; i < input.size; ++i) {
      const double phase = .017 * double(i) + .031 * double(frame) +
        .11 * double(i % 5);
      input.data[i] = {std::cos(phase), std::sin(phase)};
    }
    session->begin({frame, frame, 1000 + frame});
    session->receiveReferenceReady();
    const auto output = session->receivePrepared();
    const auto proof = preparedProof(output.preparedEvidence);
    sawActiveTracker = sawActiveTracker || proof.trackerStatus[54] != 0;
    const bool ready = proof.postprocessStatus[28] != 0;
    if (ready != (frame >= 8))
      throw std::runtime_error("TBD history readiness changed");
    session->commit(output.trackerToken);
    session->published();
  }
  if (!sawActiveTracker)
    throw std::runtime_error("Track-guided physical test did not create track state");
  std::cout << "GPU_RESIDENT_TBD_PHYSICAL_PASS frames=9 integration_cpis=8"
    " channels=5 beams=12 history_ready_frame=8 delay_bins=2 doppler_bins=4 track_guided_shadow=1"
    " guided_max_tracks=64\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL resident TBD physical initialization: " << error.what() << '\n';
  return 1;
}
