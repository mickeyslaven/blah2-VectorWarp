#pragma once
#include "GpuResidentBackend.h"
#include "GpuResidentProof.h"
#include "GpuResidentQualification.h"
#include "GpuResidentInput.h"
#include "GpuResultJson.private.h"
#include "Acceleration.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include "process/detection/DetectorIntegration.h"
#include "process/pipeline/RadarPipeline.h"
#include "process/fusion/BearingRefinement.h"
#include "process/tracker/TrackerCheckpointOracle.h"
#include "ResidentFusionWeights.h"
#include "process/analysis/StartupIq.h"
#include <algorithm>
#include <iostream>
namespace blah2::resident {
// Parent control/byte-copy/serializer route. CPU numeric loops below are guarded
// startup oracles only; no accepted steady frame can invoke them.
class Coordinator {
 std::unique_ptr<Session> session_;
 GpuProcessOptions options_;
 Qualification qualification_;
 const std::complex<double>* canonical_=nullptr;
 OutputViews ready_{},prepared_{};
 AccelerationStatus status_;
 std::array<uint8_t,256> referenceCheckpoint_{};
 bool haveReferenceCheckpoint_=false;
 bool replayObserver_=false,replayClutterObserver_=false;
 Frame pendingFrame_{};
 uint64_t temporalEpoch_=1;
 std::vector<uint64_t> temporalSequences_;
 std::optional<startup::Target> startupTarget_;
 bool privateStartup_=false,privatePositive_=false,privateMature_=false,privateTrackMature_=false;
 void clearBorrowed()noexcept{canonical_=nullptr;
 ready_={};
 prepared_={};}
 template<class F>decltype(auto) sessionCall(F&& f){try{return f();}catch(...){clearBorrowed();
 throw;}}
 void oracle()const{wire::require(qualifying(),"CPU resident oracle called after qualification");}
 static bool finite(std::complex<double> v){return std::isfinite(v.real())&&std::isfinite(v.imag());}
 static void compareJson(const rapidjson::Value& expected,const rapidjson::Value& actual,
  const char* label){
  wire::require(expected.GetType()==actual.GetType(),label);
  if(expected.IsObject()){
   wire::require(expected.MemberCount()==actual.MemberCount(),label);
   for(auto member=expected.MemberBegin();member!=expected.MemberEnd();++member){
    wire::require(actual.HasMember(member->name.GetString()),label);
    compareJson(member->value,actual[member->name.GetString()],label);
   }
  }else if(expected.IsArray()){
   wire::require(expected.Size()==actual.Size(),label);
   for(rapidjson::SizeType i=0;i<expected.Size();++i)
    compareJson(expected[i],actual[i],label);
  }else if(expected.IsNumber()){
   const double left=expected.GetDouble(),right=actual.GetDouble();
   if(expected.IsInt64()&&actual.IsInt64())wire::require(expected.GetInt64()==actual.GetInt64(),label);
   else if(expected.IsUint64()&&actual.IsUint64())wire::require(expected.GetUint64()==actual.GetUint64(),label);
   else wire::require(std::isfinite(left)&&std::isfinite(right)&&
    std::abs(left-right)<=0.010001+1e-7*std::max({1.0,std::abs(left),std::abs(right)}),label);
  }else if(expected.IsString())wire::require(std::string(expected.GetString())==actual.GetString(),label);
  else if(expected.IsBool())wire::require(expected.GetBool()==actual.GetBool(),label);
  else wire::require(expected.IsNull()&&actual.IsNull(),label);
 }
 static void compareJson(const std::string& expected,const std::string& actual,
  const char* label){rapidjson::Document left,right;
  left.Parse<rapidjson::kParseFullPrecisionFlag>(expected.c_str());
  right.Parse<rapidjson::kParseFullPrecisionFlag>(actual.c_str());
  wire::require(!left.HasParseError()&&!right.HasParseError(),label);
  compareJson(left,right,label);
 }
 static rapidjson::Document productFrame(const std::string& value,const char* label){
  rapidjson::Document document;
  document.Parse<rapidjson::kParseFullPrecisionFlag>(value.c_str());
  wire::require(!document.HasParseError()&&document.IsObject()&&
   document.HasMember("timestamp")&&document["timestamp"].IsUint64(),label);
  return document;
 }
public:
 explicit Coordinator(Config c,GpuProcessOptions options={}):session_(createProcess(c,options)),options_(options),
  qualification_(Qualification::startupCount(Acceleration::TotalQualificationFrames,
   c.detector.enabled?c.detector.integrationCpis:1,
   c.tracker.enabled?c.tracker.m:0,c.tracker.n,c.tracker.nDelete),
   c.detector.enabled?c.detector.integrationCpis:1){status_.requested="gpu-resident";
 status_.active="vulkan-resident";
 status_.device=c.device;
 status_.state="checking";
 status_.reason="Strict startup CPU oracle; failures stop this route";}
 const Config& config()const{return session_->config();}
 void fillStartupIq(const std::vector<IqData*>& input,uint32_t samples,unsigned frame){
  oracle();wire::require(privateStartup_,"Qualification IQ requested outside private instance");
  const auto& c=config();const auto& d=c.detector;
  if(!d.enabled){startup::fillIq(input,samples,frame);return;}
  if(!startupTarget_){
   startup::Target target;target.sampleRate=c.sampleRate;
   uint32_t column=0,training=0;const int columns=int(c.delayAxis.size());
   for(int offset=0;offset<columns;++offset){const int i=(columns/2+offset)%columns;
    const int delay=c.delayAxis[i];
    if(delay<std::max(d.minDelay,d.guard+1)||delay<=0||uint32_t(delay)>=samples/4)continue;
    const int count=std::max(0,i-d.guard)-std::max(0,i-d.guard-d.train)+
     std::min(columns,i+d.guard+d.train+1)-std::min(columns,i+d.guard+1);
    if(count>0){target.delay=uint32_t(delay);column=uint32_t(i);training=uint32_t(count);break;}
   }
   wire::require(target.delay&&training,"Private qualification cannot place a delayed target with valid CFAR training cells");
   double minimum=std::max(d.minDoppler,c.tracker.stationaryClutter?c.tracker.stationaryMaxDopplerHz:0.0);
   for(double hz:c.dopplerAxis)if(std::abs(hz)>minimum&&
     (!target.dopplerHz||std::abs(hz)<std::abs(target.dopplerHz)))target.dopplerHz=hz;
   wire::require(target.dopplerHz!=0,"Private qualification has no nonstationary Doppler cell above configured gates");
   const double trials=d.integrationCpis>2?2*track_before_detect::trialCount(d.integrationCpis,d.integrationMotionBins):
    (d.integrationCpis==2?2.0:1.0);
   const double alpha=detection::integrated_cfar::alpha(d.pfa/trials,training,d.integrationCpis);
   const double snr=c.tracker.enabled?std::pow(10.0,c.tracker.minInitiationSnrDb/10.0):0;
   target.amplitude=16*std::sqrt(std::max({1.0,alpha,snr}));
   wire::require(std::isfinite(target.amplitude)&&target.amplitude<=4096,
    "Private qualification target exceeds bounded amplitude for configured CFAR/trial/SNR gates");
   target.response.assign(c.inputChannels,{});
   const double pi=std::acos(-1.0),azimuth=c.fusion.directPathAzimuthRad+pi/2;
   const double wrapped=std::fmod(std::fmod(azimuth,2*pi)+2*pi,2*pi);
   const uint32_t beam=c.fusion.beams?uint32_t(std::llround(wrapped/(2*pi)*c.fusion.beams))%c.fusion.beams:0;
   double largest=1;
   for(size_t channel=0;channel<c.surveillanceChannels.size();++channel){
    std::complex<double> response{1,0};
    if(c.fusion.mode==FusionMode::noncoherent&&c.reference.mode==ReferenceMode::capturedArray)
     response=std::polar(1.0,2*pi*channel/c.surveillanceChannels.size());
    if(c.fusion.mode!=FusionMode::noncoherent){
     if(!c.fusion.manifoldResponses.empty())response=
      std::complex<double>(c.fusion.manifoldResponses[beam*c.geometry.channels+channel])/
      std::complex<double>(c.fusion.directResponse[channel]);
     else {const double element=c.fusion.element0Rad+channel*c.fusion.elementStepRad;
      const double phase=-2*pi*c.fusion.frequencyHz*c.fusion.radiusM/299792458.0*
       (std::cos(element)*(std::cos(azimuth)-std::cos(c.fusion.directPathAzimuthRad))+
        std::sin(element)*(std::sin(azimuth)-std::sin(c.fusion.directPathAzimuthRad)));
      response=std::polar(1.0,phase);}
    }
    target.response[c.surveillanceChannels[channel]]=response;largest=std::max(largest,std::abs(response));
   }
   for(auto& value:target.response)value/=largest;
   startupTarget_=std::move(target);
   std::cout<<"[Startup] private controls depth="<<d.integrationCpis<<" delay_cell="<<column
    <<" doppler_hz="<<startupTarget_->dopplerHz<<" publish=0\n";
  }
  privatePositive_=frame>=d.integrationCpis;
  privateMature_=privatePositive_?frame+1>=2*d.integrationCpis:frame+1>=d.integrationCpis;
  privateTrackMature_=!c.tracker.enabled||frame+1>=
   Qualification::confirmationFrame(d.integrationCpis,c.tracker.m);
  startup::fillTargetIq(input,samples,frame,*startupTarget_,privatePositive_);
 }
 struct OracleFusion {
  std::vector<uint32_t> beams;
  std::function<uint32_t(uint32_t,uint32_t)> spatialAt;
 };
 // A bounded adapter into the canonical fusion/detection owners. Inputs are
 // independently computed CPU reference statistics and channel CAFs only.
 template<class MapOwner>
 OracleFusion fuseOracle(const std::vector<Map<std::complex<double>>*>& maps,
  Noncoherent& noncoherent,CoherentBeamBank* coherent,MapOwner& output,
  const ArrayReferenceSynthesizer::Metrics& metrics)const{
  oracle();OracleFusion result;
  const auto& c=config();const uint32_t channels=c.geometry.channels;
  const bool whiten=c.fusion.mode==FusionMode::noiseWhitenedRegularPolygon;
  auto scales=resident_fusion::whiteningScales(metrics.covariance.data(),
   metrics.directPathWeights.data(),channels,c.reference.diagonalLoading,
   whiten&&!metrics.covariance.empty());
  if(coherent)coherent->calibrate(metrics.directPathWeights,whiten?scales.data():nullptr);
  pipeline::fuseMaps(maps,noncoherent,coherent,output,coherent?&result.beams:nullptr);
  if(!coherent)return result;
  std::shared_ptr<fusion::BearingRefinement> refinement;
  if(c.detector.integrationCpis>2){
   fusion::BearingRefinement::Config b;
   b.channels=channels;b.beams=c.bearing.enabled?c.bearing.scanBeams:c.fusion.beams*4u;
   b.frequencyHz=c.fusion.frequencyHz;b.radiusM=c.fusion.radiusM;
   b.element0Rad=c.fusion.element0Rad;b.elementStepRad=c.fusion.elementStepRad;
   b.directPathAzimuthRad=c.fusion.directPathAzimuthRad;b.directPathNullDb=c.fusion.directPathNullDb;
   const double pi=std::acos(-1.0),elements=double(channels);
   const double aperture=2*c.fusion.radiusM*std::sin(pi*std::floor(elements/2)/elements);
   const double halfWidth=std::min(90.0,.443*(299792458.0/c.fusion.frequencyHz)/aperture*180.0/pi);
   b.exclusionBins=std::max(1u,uint32_t(std::floor(halfWidth/(360.0/b.beams))));
   if(c.bearing.enabled){b.method=uint32_t(c.bearing.method);b.referenceAnchor=uint32_t(c.bearing.referenceAnchor);
    b.guardRadius=c.bearing.guardRadius;b.trainingRadius=c.bearing.trainingRadius;
    b.diagonalLoading=float(c.bearing.diagonalLoading);b.channelMapping=c.bearing.channelMapping;
    b.measuredResponses=c.bearing.manifoldResponses;}
   refinement=std::make_shared<fusion::BearingRefinement>(std::move(b),metrics.directPathWeights,scales,whiten);
  }
  result.spatialAt=[maps,beams=result.beams,refinement,columns=output->get_nCols()](uint32_t row,uint32_t column){
   const uint32_t coarse=beams.at(uint64_t(row)*columns+column);
   if(!refinement)return coarse;
   const auto value=refinement->refineAt(maps,row,column);
   wire::require(value.outcome!=fusion::BearingRefinement::Outcome::invalidInput,
    "Resident spatial qualification oracle rejected nonfinite input");
   return value.spatial?value.spatial:coarse;
  };
  return result;
 }
 bool qualifying()const{return qualification_.pending();}
 uint32_t qualificationChecks()const{return qualification_.completed();}
 const AccelerationStatus& status()const{return status_;}
 void enableReplayObserver(bool replay,bool confirmed,bool clutter=false){wire::require(replay&&confirmed&&!haveReferenceCheckpoint_,
  "Resident observer requires confirmed replay before input");replayObserver_=true;replayClutterObserver_=clutter;}
 Span<const std::complex<double>> observedReference()const{wire::require(replayObserver_,"Resident observer is not enabled");return ready_.reference;}
 uint64_t observedSharedReservationBytes()const{wire::require(replayObserver_,"Resident observer is not enabled");return session_->layout().bytes;}
 void begin(const std::vector<std::unique_ptr<IqData>>& capture,Frame frame){sessionCall([&]{clearBorrowed();pendingFrame_=frame;auto input=session_->input();copyCanonicalInput(config(),input,capture,[](const auto& channel){wire::require(bool(channel),"Resident captured channel absent");return channel->sample_view();});canonical_=input.data;qualification_.begin();uint64_t diagnostics=qualifying()?(bit(Section::fullReference)|bit(Section::filtered)):0;if(replayObserver_)diagnostics|=bit(Section::fullReference);if(replayClutterObserver_)diagnostics|=bit(Section::filtered);if(config().presence&bit(Section::detectionCells))diagnostics|=bit(Section::detectionCells);session_->begin(frame,diagnostics);ready_=session_->receiveReferenceReady();wire::require(ready_.referenceCheckpoint.size==referenceCheckpoint_.size(),"Resident acknowledged reference checkpoint shape");std::memcpy(referenceCheckpoint_.data(),ready_.referenceCheckpoint.data,referenceCheckpoint_.size());haveReferenceCheckpoint_=true;ready_.referenceCheckpoint={referenceCheckpoint_.data(),referenceCheckpoint_.size()};});}
 const OutputViews& receivePrepared(){sessionCall([&]{if(!prepared_.preparedEvidence.data){prepared_=session_->receivePrepared();
  if(config().detector.integrationCpis>1){wire::require(pendingFrame_.consumedSequence&&
   (temporalSequences_.empty()||pendingFrame_.consumedSequence>temporalSequences_.back()),"Resident temporal input sequence did not advance");
   temporalSequences_.push_back(pendingFrame_.consumedSequence);
   if(temporalSequences_.size()>config().detector.integrationCpis)temporalSequences_.erase(temporalSequences_.begin());
   wire::require(prepared_.availableCpis==temporalSequences_.size(),"Resident acknowledged temporal history differs from input lineage");}}});
 return prepared_;}
 void checkReference(const IqData& reference,const ArrayReferenceSynthesizer::Metrics& metrics){oracle();
 wire::require(ready_.reference.size==reference.sample_view().size(),"Resident reference oracle shape differs");
 size_t i=0;
 for(auto expected:reference.sample_view()){auto value=ready_.reference.data[i++];
 wire::require(finite(value)&&finite(expected)&&std::abs(value-expected)<1e-12,"Resident reference oracle rejected frame");}const auto cp=referenceState(ready_.referenceCheckpoint);
 wire::require(cp.updates==metrics.updates&&cp.weightCount==metrics.weights.size()&&std::abs(cp.coherentFraction-metrics.coherentFraction)<=1e-12&&std::abs(cp.coherentGainDb-metrics.coherentGainDb)<=1e-10,"Resident reference state oracle rejected frame");
 for(size_t c=0;c<metrics.weights.size();++c)wire::require(std::abs(cp.weights[c]-metrics.weights[c])<=1e-12,"Resident reference weight oracle rejected frame");
 qualification_.checked(Qualification::reference);}
 void checkFiltered(const std::vector<IqData*>& cpu){oracle();
 const auto& out=receivePrepared();
 const size_t n=config().geometry.clutterSamples;
 wire::require(cpu.size()==config().geometry.channels&&out.filtered.size==n*cpu.size(),"Resident sample oracle shape differs");
 for(size_t c=0;c<cpu.size();++c){const auto& expected=cpu[c]->sample_view();
 wire::require(expected.size()==n,"Resident sample oracle was consumed");
 double error=0,filtered=0,input=0,maxError=0,maxFiltered=0,maxInput=0;
 for(size_t i=0;i<n;++i){const auto actual=out.filtered.data[c*n+i];
 wire::require(finite(actual)&&finite(expected[i]),"Resident sample oracle nonfinite");
 const double delta=std::norm(actual-expected[i]);
 const double original=std::norm(std::complex<float>(canonical_[config().surveillanceChannels[c]*n+i]));
 error+=delta;
 filtered+=std::norm(expected[i]);
 input+=original;
 maxError=std::max(maxError,delta);
 maxFiltered=std::max(maxFiltered,std::norm(expected[i]));
 maxInput=std::max(maxInput,original);}const double filteredFloor=std::max(filtered,input*1e-8),peakFloor=std::max(maxFiltered,maxInput*1e-8);
 wire::require(std::isfinite(error)&&std::isfinite(filtered)&&std::isfinite(input)&&error<=std::max(input*1e-8,1e-20)&&error<=std::max(filteredFloor*1e-8,1e-20)&&maxError<=std::max(maxInput*1e-8,1e-20)&&maxError<=std::max(peakFloor*1e-8,1e-20),"Resident clutter sample oracle rejected frame");}qualification_.checked(Qualification::filtered);}
 void checkMaps(const std::vector<Ambiguity*>& cpu){oracle();
 const auto& out=receivePrepared();
 const auto& g=config().geometry;
 const size_t cells=size_t(g.doppler)*g.delays;
 wire::require(cpu.size()==g.channels&&out.maps.size==cells*g.channels,"Resident map oracle shape differs");
 for(size_t c=0;c<cpu.size();++c){const auto& data=cpu[c]->result()->data;
 double error=0,signal=0,peak=0,maxError=0;
 for(uint32_t d=0;d<g.doppler;++d){for(uint32_t r=0;r<g.delays;++r){const auto expected=data[d][r];
 const std::complex<double> actual=out.maps.data[c*cells+size_t(r)*g.doppler+(d+g.doppler/2+1)%g.doppler];
 wire::require(finite(expected)&&finite(actual),"Resident composed map nonfinite");
 const double delta=std::norm(actual-expected);
 error+=delta;
 signal+=std::norm(expected);
 peak=std::max(peak,std::norm(expected));
 maxError=std::max(maxError,delta);}}
 wire::require(std::isfinite(error)&&std::isfinite(signal)&&error<=std::max(signal*1e-8,1e-20)&&maxError<=std::max(peak*1e-8,1e-20),"Resident composed map oracle rejected frame");}qualification_.checked(Qualification::maps);}
 struct Publication {std::string spectrum,map,detections,tracks,bearing,manifoldEvidence;
  uint32_t availableCpis=0;bool detectorReady=false;uint64_t temporalEpoch=0,newestSequence=0;
  std::array<uint64_t,8> supportSequences{};};
 Publication format(uint64_t timestamp,const gpu_result_json::BearingConfig& bearing={},
  bool publishManifoldEvidence=false,
  const resident::BearingConfig& evidenceBearing={},
  gpu_result_json::View<double> actualGainDb={}){const auto& v=receivePrepared();
 qualification_.requireNumeric();
 namespace json=gpu_result_json;
 Publication result;
 result.availableCpis=v.availableCpis;
 result.detectorReady=config().detector.integrationCpis==1||v.availableCpis>=2;
 result.temporalEpoch=temporalEpoch_;
 result.newestSequence=temporalSequences_.empty()?0:temporalSequences_.back();
 for(size_t age=0;age<temporalSequences_.size();++age)
  result.supportSequences[age]=temporalSequences_[temporalSequences_.size()-1-age];
 result.spectrum=json::spectrum({timestamp,{v.frequencies.data,v.frequencies.size},{v.spectrumDb.data,v.spectrumDb.size}});
 result.map=json::map({timestamp,config().geometry.doppler,config().geometry.delays,v.noise,v.maxPower,{v.delayKm.data,v.delayKm.size},{v.doppler.data,v.doppler.size},{v.display.data,v.display.size}});
 if(config().detector.enabled){result.detections=json::detectionRecords(timestamp,json::View<DetectionRecord>{v.detections.data,v.detections.size});
  if(config().detector.integrationCpis>1)result.detections=json::temporalEvidence(
   std::move(result.detections),{v.flags.data,v.flags.size},{v.detectionCells.data,v.detectionCells.size},
   temporalEpoch_,{temporalSequences_.data(),temporalSequences_.size()},config().detector.integrationCpis);
  if(publishManifoldEvidence){const auto reference=referenceState(v.referenceCheckpoint);
   const auto sourceCells=json::temporalSourceCells({v.flags.data,v.flags.size},
    {v.detectionCells.data,v.detectionCells.size},config().geometry.doppler,
    config().geometry.delays,config().detector.integrationCpis);
   result.manifoldEvidence=json::manifoldEvidence(timestamp,config().fusion.frequencyHz,
    config().sampleRate,config().geometry.doppler,config().geometry.delays,
    config().geometry.channels,json::View<DetectionRecord>{v.detections.data,v.detections.size},
    {sourceCells.data(),sourceCells.size()},{v.maps.data,v.maps.size},
    {reference.weights.data(),reference.weightCount},reference.frames,reference.updates,
    reference.coherentFraction,reference.coherentGainDb,reference.scale,
    uint32_t(evidenceBearing.method),uint32_t(evidenceBearing.referenceAnchor),
    evidenceBearing.scanBeams,
    evidenceBearing.guardRadius,evidenceBearing.trainingRadius,evidenceBearing.diagonalLoading,
    {evidenceBearing.channelMapping.data(),evidenceBearing.channelMapping.size()},actualGainDb);}}
 if(config().tracker.enabled){const auto* w=v.trackerCheckpoint.data;
 wire::require(v.trackerCheckpoint.size>=32,"Resident tracker checkpoint missing");
 const json::CheckpointBinding binding{config().tracker.capacity,config().epoch,json::wordPair(w,12),json::wordPair(w,14),config().fusion.mode==FusionMode::noncoherent?0u:config().fusion.beams,config().bearing.scanBeams};
 result.tracks=json::trackerCheckpoint(timestamp,{w,v.trackerCheckpoint.size},binding);
 if(bearing.enabled)result.bearing=json::bearingCheckpoint(timestamp,{w,v.trackerCheckpoint.size},binding,bearing);}return result;}
 void checkProducts(const Publication& resident,const std::string& cpuMap,
  const std::string& cpuDetections,const std::string& cpuTracks,
  const detection::IntegrationMetrics* cpuIntegration=nullptr,Tracker* cpuTracker=nullptr){oracle();
  // Recursive equality includes keys, types and every array dimension; the
  // wire boundary already validates configured shapes before serialization.
  compareJson(cpuMap,resident.map,"Resident fused-map startup oracle rejected frame");
  const bool temporal=config().detector.enabled&&config().detector.integrationCpis>1;
  if(temporal){
   wire::require(cpuIntegration&&cpuIntegration->cpis==config().detector.integrationCpis&&
    cpuIntegration->availableCpis==resident.availableCpis&&
    cpuIntegration->ready==resident.detectorReady&&
    resident.newestSequence==pendingFrame_.consumedSequence&&
    resident.temporalEpoch==temporalEpoch_,"Resident temporal startup readiness/lineage differs");
   for(size_t age=0;age<temporalSequences_.size();++age)
    wire::require(resident.supportSequences[age]==temporalSequences_[temporalSequences_.size()-1-age],
     "Resident temporal startup source window differs");
  }
  if(config().detector.enabled){
   auto expected=productFrame(cpuDetections,"CPU detector startup product invalid");
   if(temporal&&expected.HasMember("evidence")){
    wire::require(expected["evidence"].IsArray(),"CPU temporal evidence invalid");
    auto& allocator=expected.GetAllocator();
    for(auto& candidate:expected["evidence"].GetArray()){
     wire::require(candidate.IsObject()&&candidate.HasMember("support_sequences_exact")&&
     candidate["support_sequences_exact"].IsArray()&&candidate.HasMember("cpis")&&
      candidate["cpis"].IsUint()&&candidate.HasMember("epoch_exact")&&candidate["epoch_exact"].IsString()&&
      candidate.HasMember("newest_sequence_exact")&&candidate["newest_sequence_exact"].IsString()&&
      std::to_string(cpuIntegration->window.epoch)==candidate["epoch_exact"].GetString()&&
      std::to_string(cpuIntegration->window.newestSequence)==candidate["newest_sequence_exact"].GetString(),
      "CPU temporal candidate lineage absent");
     auto& support=candidate["support_sequences_exact"];
     wire::require(support.Size()==candidate["cpis"].GetUint()&&
      support.Size()<=temporalSequences_.size(),"CPU temporal candidate support invalid");
     for(rapidjson::SizeType age=0;age<support.Size();++age){
      const auto local=std::to_string(cpuIntegration->window.newestSequence-age);
      wire::require(support[age].IsString()&&local==support[age].GetString(),
       "CPU temporal candidate has noncontiguous support");
      const auto submitted=std::to_string(temporalSequences_[temporalSequences_.size()-1-age]);
      support[age].SetString(submitted.c_str(),allocator);
     }
     const auto epoch=std::to_string(temporalEpoch_),sequence=std::to_string(pendingFrame_.consumedSequence);
     candidate["epoch_exact"].SetString(epoch.c_str(),allocator);
     candidate["newest_sequence_exact"].SetString(sequence.c_str(),allocator);
    }
   }
   const auto actual=productFrame(resident.detections,"Resident detector startup product invalid");
   compareJson(expected,actual,"Resident detector startup oracle rejected frame");
  }
  if(config().tracker.enabled){
   compareJson(cpuTracks,resident.tracks,
    "Resident tracker startup oracle rejected frame");
   wire::require(cpuTracker,"Resident startup qualification requires canonical hidden tracker state");
   const tracker_qualification::ClockBinding binding{
    cpuIntegration?cpuIntegration->window.epoch:0,config().epoch,
    cpuIntegration?cpuIntegration->window.newestSequence:0,pendingFrame_.consumedSequence};
   tracker_qualification::compareCheckpoint(*cpuTracker,prepared_.trackerCheckpoint.data,
    prepared_.trackerCheckpoint.size,&binding);
  }
  if(privateStartup_&&config().detector.enabled){
   uint64_t tracks=0,confirmedTracks=0;
   if(config().tracker.enabled){
    const auto product=productFrame(resident.tracks,"Private qualification tracker control invalid");
    wire::require(product.HasMember("n")&&product["n"].IsUint64()&&
     product.HasMember("data")&&product["data"].IsArray(),"Private qualification tracker fields invalid");
    tracks=product["n"].GetUint64();
    for(const auto& item:product["data"].GetArray()){
     wire::require(item.IsObject()&&item.HasMember("state")&&item["state"].IsString(),
      "Private qualification public track state missing");
     const std::string state=item["state"].GetString();
     if(state=="ACTIVE"||state=="COASTING")++confirmedTracks;
    }
   }
   qualification_.control(privatePositive_,privateMature_,prepared_.detectionCount,tracks,
    confirmedTracks,config().tracker.enabled,privateTrackMature_);
  }
  qualification_.checkedProducts(temporal?resident.availableCpis:1);
 }
 // Lane consumers use only the numerically qualified complex CAFs; fusion,
 // temporal evidence, CFAR and tracking are the identical host contract.
 void checkFrontendProducts(){oracle();
  wire::require(!config().detector.enabled&&!config().tracker.enabled,
   "Frontend qualification cannot bypass resident detector/tracker products");
  qualification_.requireNumeric();
  qualification_.checked(Qualification::products);
 }
 void commit(){wire::require(prepared_.preparedEvidence.data,"Resident commit missing prepared frame");
 qualification_.requireComplete();
 sessionCall([&]{session_->commit(prepared_.trackerToken);});}
 void published(){sessionCall([&]{session_->published();});
 if(qualifying()){qualification_.published();
 if(!qualifying()){status_.state="ready";
 status_.reason="GPU-resident stages qualified; no steady CPU DSP oracle";}}}
 template<class Prepare,class ResetCpu>
 void prepareStartup(Prepare&& prepare,ResetCpu&& resetCpu){
  try{
   privateStartup_=true;
   qualification_.prepareStartup(prepare,
    [&]{sessionCall([&]{session_->published();});},
    [&]{if(config().detector.enabled)qualification_.requireControls();resetInput();resetCpu();});
   const auto identity=session_->qualificationIdentity();
   const auto productionConfig=config();
   // The old instance's full reset is acknowledged before orderly destruction.
   // Fresh ownership starts public frame1; private samples/IDs never escape.
   session_.reset();
   session_=createProcess(productionConfig,options_);
   wire::require(session_->qualificationIdentity()==identity,
    "Resident production instance differs from privately qualified config/module/device");
   temporalEpoch_=1;temporalSequences_.clear();pendingFrame_={};
   privateStartup_=false;startupTarget_.reset();
   status_.state="ready";
   status_.reason="GPU-resident stages qualified before capture; synthetic state reset";
  }catch(...){
   stop();status_.state="failed";
   status_.reason="Pre-capture resident qualification or adaptive-state reset failed";
   throw;
  }
 }
 std::vector<uint8_t> snapshot(){return sessionCall([&]{return session_->snapshot();});}
 void resetInput(){sessionCall([&]{session_->resetInput();});
  clearBorrowed();qualification_.resetInput();
  temporalSequences_.clear();if(++temporalEpoch_==0)throw std::overflow_error("Resident temporal epoch exhausted");
  referenceCheckpoint_={};haveReferenceCheckpoint_=false;
 }
 const OutputViews& prepared()const{return prepared_;}
 // The acknowledged reference state survives any later radar/control failure.
 // Tracker views are borrowed only: after failure none is exposed or retried.
 ConstBytes acknowledgedReferenceCheckpoint()const noexcept{return haveReferenceCheckpoint_?ConstBytes{referenceCheckpoint_.data(),referenceCheckpoint_.size()}:ConstBytes{};}
 void stop()noexcept{clearBorrowed();
 if(session_)session_->stop();}
};
}
