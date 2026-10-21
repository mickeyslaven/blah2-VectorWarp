// Adjacent resident ABI9 module. Older cohorts are separate targets.
#include "GpuResidentWire.h"
#include "GpuResidentHash.h"
#include "GpuResidentEvidenceLayout.h"
#include <dlfcn.h>
#include <fstream>
#include "../../../core/VulkanBackend.private.cpp"
extern "C" blah2::resident::Backend* blah2_gpu_create_resident(unsigned,const blah2::resident::Config*,const blah2::resident::Mapping*);
namespace blah2 { namespace {
namespace rw=resident::wire;
namespace re=resident::evidence;
using resident::Section;
using resident::Packet;
using resident::Bytes;
using resident::ConstBytes;
static resident::Hash moduleImageHash(){
 Dl_info info{};
 if(!dladdr(reinterpret_cast<void*>(&blah2_gpu_create_resident),&info)||!info.dli_fname)throw std::runtime_error("Resident module image identity unavailable");
 std::ifstream file(info.dli_fname,std::ios::binary);
 if(!file)throw std::runtime_error("Resident module image cannot be read");
 resident::Sha256 sha;
 std::array<char,65536> buffer{};
 uint64_t bytes=0;
 while(file){file.read(buffer.data(),buffer.size());
 const auto n=file.gcount();
 if(n<0||uint64_t(n)>(256ULL<<20)-bytes)throw std::runtime_error("Resident module image exceeds identity bound");
 bytes+=uint64_t(n);
 sha.add(buffer.data(),size_t(n));}if(!file.eof()||!bytes)throw std::runtime_error("Resident module image identity read failed");
 return sha.finish();
}
static ResidentFrameConfig privateConfig(const resident::Config& c){
 ResidentFrameConfig v;
 v.geometry=c.geometry;
 v.correlation=c.correlation;
 v.sampleRate=c.sampleRate;
 v.middle=c.middle;
 v.fusion={uint32_t(c.fusion.mode),c.fusion.beams,c.fusion.frequencyHz,
  c.fusion.radiusM,c.fusion.element0Rad,c.fusion.elementStepRad,
  c.fusion.directPathAzimuthRad,c.fusion.directPathNullDb};
 v.stages=c.stages();
 const auto& r=c.reference;
 v.reference={ReferenceMode::capturedArray,r.channels,r.analysisSamples,r.analysisInterval,r.powerIterations,r.covarianceSmoothing,r.diagonalLoading,r.bandwidthHz,r.centreHz,c.epoch};
 const auto& d=c.detector;
 auto& p=v.postprocess;
 p.enabled=true;
 p.detection=d.enabled;
 p.pfa=d.pfa;
 p.minDoppler=d.minDoppler;
 p.centroidDopplerResolution=d.centroidDopplerResolution;
 p.guard=d.guard;
 p.train=d.train;
 p.minDelay=d.minDelay;
 p.centroidDelay=uint16_t(d.centroidDelay);
 p.centroidDoppler=uint16_t(d.centroidDoppler);
 p.interpolateDelay=d.interpolateDelay;
 p.interpolateDoppler=d.interpolateDoppler;
 p.integrationCpis=d.integrationCpis;
 const auto motion=track_before_detect::decodeMotionBins(d.integrationMotionBins);
 p.integrationDelayMotionBins=motion.delay;
 p.integrationDopplerMotionBins=motion.doppler;
 p.integrationBeamTolerance=d.integrationBeamTolerance;
 p.beamCount=c.fusion.mode==resident::FusionMode::noncoherent?0:c.fusion.beams;
 if(c.bearing.enabled){p.bearingMethod=uint32_t(c.bearing.method);
  p.bearingReferenceAnchor=uint32_t(c.bearing.referenceAnchor);p.bearingScanBeams=c.bearing.scanBeams;
  p.bearingCandidatePeaks=c.bearing.candidatePeaks;p.bearingGuardRadius=c.bearing.guardRadius;
  p.bearingTrainingRadius=c.bearing.trainingRadius;p.bearingDiagonalLoading=float(c.bearing.diagonalLoading);
  p.bearingChannelMapping=c.bearing.channelMapping;p.bearingManifoldResponses=c.bearing.manifoldResponses;}
 if(p.beamCount){
  const double pi=std::acos(-1.0),elements=double(c.surveillanceChannels.size());
  const double aperture=2*c.fusion.radiusM*std::sin(pi*std::floor(elements/2)/elements);
  const double halfWidth=std::min(90.0,.443*(299792458.0/c.fusion.frequencyHz)/aperture*180.0/pi);
  const double fineStep=360.0/double(p.bearingScanBeams?p.bearingScanBeams:p.beamCount*4u);
  p.bearingExclusionBins=std::max(1u,uint32_t(std::floor(halfWidth/fineStep)));
 }
 p.delayAxis=c.delayAxis;
 p.dopplerAxis=c.dopplerAxis;
 p.epoch=c.epoch;
 const auto&t=c.tracker;
 v.tracker={t.enabled,t.capacity,t.m,t.n,t.nDelete,
  t.initiationMode==resident::TrackerInitiationMode::adaptiveGate,
  t.cpi,t.maxAccInit,t.rangeRes,t.lambda,t.stationaryClutter,
  t.stationaryPersistenceCpis,t.stationaryMaxDopplerHz,
  t.trackGuidedShadow,t.trackGuidedPromote,t.trackGuidedShadowMaxTracks,c.epoch};
 return v;
}
class ResidentModule final:public resident::Backend {
 const resident::Config config_;
 const resident::Mapping map_;
 const ResidentFrameConfig private_;
 resident::Hash imageHash_{},configHash_{};
 AllocationExit exit_{};
 std::unique_ptr<ResidentFramePipeline> pipeline_;
 ReferenceCheckpoint reference_{};
 PostprocessMetrics metrics_{};
 TrackerResult trackerResult_{};
 uint64_t outputPresence_=0;
 uint32_t committedSlot_=0,preparedSlot_=1;
 uint64_t event_=0;
 bool committed_=false,ready_=false,prepared_=false;
 resident::Token token_{};
 static Bytes sub(Bytes b,size_t offset,size_t size){rw::require(offset<=b.size&&size<=b.size-offset,"Resident evidence slice overflow");
 return {b.data+offset,size};}
 template<class T>T* pointer(Section section){if(!(outputPresence_&resident::bit(section)))return nullptr;
 return reinterpret_cast<T*>(map_.bytes(section).data);}
 template<class T>size_t count(Section section){if(!(outputPresence_&resident::bit(section)))return 0;
 return map_.bytes(section).size/sizeof(T);}
 template<class T,size_t N>static void words(rw::Writer& w,size_t at,const std::array<T,N>& s){for(size_t i=0;i<N;++i)w.u32(at+4*i,uint32_t(s[i]));}
 static void times(rw::Writer& w,size_t at,const gpu_native::Timestamps& t,uint32_t first,uint32_t n){for(uint32_t i=0;i<n;++i)w.f64(at+8*i,t.observed(first+i));}
 void allocation(Bytes output){rw::Writer w{output};
 w.zero();
 const auto& context=pipeline_->context_;
 const auto properties=memoryProperties(context.candidate.physical);
 rw::require(properties.heaps.size()<=16&&properties.types.size()<=32,"Resident physical memory table overflow");
  std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto& state=deviceAllocations.at(context.device);
  std::array<uint64_t,16> used{};
  for(const auto& a:state.allocations){rw::require(a.second.type<properties.types.size(),"Resident allocation type unknown");
  const auto heap=properties.types[a.second.type].heap;
  rw::require(heap<properties.heaps.size(),"Resident allocation heap unknown");
  used[heap]=rw::add(used[heap],a.second.bytes);}
  const auto& b=state.budget;
  rw::require(b.used()<=b.peak()&&b.peak()<=b.limit(),"Resident allocator global invariant");
  w.u32(0,1);
  w.u32(4,832);
  w.u32(8,1);
  w.u32(12,uint32_t(properties.heaps.size()));
  w.u32(16,uint32_t(properties.types.size()));
  w.u64(24,b.limit());
  w.u64(32,b.used());
  w.u64(40,b.peak());
  w.u64(48,state.allocations.size());
  uint64_t sum=0;
  for(size_t i=0;i<properties.heaps.size();++i){const auto& h=properties.heaps[i];
  const auto limit=gpu_memory::heapBudget(h.bytes);
  rw::require(used[i]<=limit,"Resident heap budget invariant");
  sum=rw::add(sum,used[i]);
  size_t a=64+32*i;
  w.u64(a,h.bytes);
  w.u64(a+8,limit);
  w.u64(a+16,used[i]);
  w.u32(a+24,h.device);}
  rw::require(sum==b.used(),"Resident heap/global accounting differs");
  for(size_t i=0;i<properties.types.size();++i){w.u32(576+8*i,properties.types[i].flags);
  w.u32(580+8*i,properties.types[i].heap);}
 }
 void device(){auto bytes=map_.bytes(Section::device);
 rw::Writer w{bytes};
 w.zero();
 const auto& c=pipeline_->context_;
 const auto& p=c.candidate.properties;
 const auto& l=p.limits;
 const auto memory=memoryProperties(c.candidate.physical);
 rw::require(c.candidate.info.id.size()<96&&std::strlen(p.deviceName)<256,"Resident identity string overflow");
  const uint32_t values[]={1,512,p.vendorID,p.deviceID,p.driverVersion,p.apiVersion,uint32_t(p.deviceType),c.candidate.queue,c.timestampValidBits};
  for(size_t i=0;i<9;++i)w.u32(4*i,values[i]);
  uint32_t period;
  std::memcpy(&period,&l.timestampPeriod,4);
  w.u32(36,period);
  w.u32(40,(c.supportedFeatures.shaderFloat64?1u:0u)|(c.supportedFeatures.shaderInt64?2u:0u));
  w.u32(44,1);
  w.u32(48,uint32_t(memory.heaps.size()));
  w.u32(52,uint32_t(memory.types.size()));
  w.u64(56,gpu_memory::heapBudget(c.candidate.info.memoryBytes));
  w.u64(64,l.maxStorageBufferRange);
  w.u32(72,l.maxComputeSharedMemorySize);
  w.u32(76,l.maxComputeWorkGroupInvocations);
  for(uint32_t i=0;i<3;++i){w.u32(80+4*i,l.maxComputeWorkGroupSize[i]);
  w.u32(92+4*i,l.maxComputeWorkGroupCount[i]);}w.u32(104,l.maxPerStageDescriptorStorageBuffers);
  w.u32(108,l.maxDescriptorSetStorageBuffers);
  w.u64(112,l.nonCoherentAtomSize);
  w.u64(120,c.candidate.info.memoryBytes);
  w.copy(128,c.identity.deviceUUID,16);
  w.copy(144,c.identity.driverUUID,16);
  w.copy(160,c.candidate.info.id.data(),c.candidate.info.id.size());
  w.copy(256,p.deviceName,std::strlen(p.deviceName));
 }
 void header(Bytes bytes,const Packet& p,resident::Phase phase,uint32_t computed,uint64_t blocks){rw::Writer w{bytes};
 w.zero();
 w.u32(0,1);
 w.u32(4,uint32_t(bytes.size));
 w.u32(8,uint32_t(phase));
 w.u64(16,p.session[0]);
 w.u64(24,p.session[1]);
 w.u64(32,p.requestId);
 w.u64(40,p.frame.logicalId);
 w.u64(48,p.epoch);
 w.u64(56,p.frame.consumedSequence);
 w.u64(64,p.frame.signalTimestampMs);
 w.u64(72,pipeline_->work_->clutter->generation_);
 w.u64(80,pipeline_->generation_);
 w.u32(88,config_.stages());
 w.u32(92,computed);
 w.u64(96,blocks);
 w.u64(104,outputPresence_);
 w.u32(112,phase==resident::Phase::prepared?pipeline_->work_->clutter->completedBatches_:0);
 w.u32(116,pipeline_->batchCapacity());
 w.copy(128,configHash_.data(),32);
 w.copy(160,imageHash_.data(),32);
 w.copy(192,re::schemaHash,32);
 w.copy(224,pipeline_->context_.identity.deviceUUID,16);
 w.copy(240,pipeline_->context_.identity.driverUUID,16);}
 uint64_t proofMask()const{uint64_t mask=(uint64_t(1)<<31)-1;
 if(!config_.detector.enabled)mask&=~(((uint64_t(1)<<4)-1)<<14);
 if(!config_.tracker.enabled)mask&=~(((uint64_t(1)<<5)-1)<<18);
 return mask;}
 void cpu(Bytes bytes){rw::Writer w{bytes};
 w.zero();
 const uint64_t required=proofMask();
 w.u64(0,required);
 w.u64(8,(uint64_t(1)<<6)|(uint64_t(1)<<7)|(uint64_t(1)<<26)|(uint64_t(1)<<28)|(uint64_t(1)<<29));
 w.u64(16,required);
  // These are sealed call-graph/deleted-method proofs, not fake runtime counters.
  w.u64(24,0);
 }
 void referenceCheckpoint(const Packet& p){rw::Writer w{map_.bytes(Section::referenceCheckpoint)};
 w.zero();
 w.u32(0,1);
 w.u32(4,256);
 w.u64(8,config_.epoch);
 w.u64(16,p.frame.logicalId);
 w.u64(24,reference_.samples);
 w.u64(32,reference_.channels);
 w.u64(40,reference_.frames);
 w.u64(48,reference_.updates);
 w.u64(56,reference_.weightCount);
 for(size_t i=0;i<8;++i){w.f64(64+16*i,reference_.weights[i].real());
 w.f64(72+16*i,reference_.weights[i].imag());}w.f64(192,reference_.coherentFraction);
 w.f64(200,reference_.coherentGainDb);
 w.f64(208,reference_.scale);}
 void readyEvidence(const Packet& p){auto bytes=map_.bytes(Section::readyEvidence);
 header(bytes,p,resident::Phase::referenceReady,48,127);
 allocation(sub(bytes,re::ready::allocation,832));
 cpu(sub(bytes,re::ready::cpuProof,288));
 rw::Writer w{bytes};
 const auto& stage=*pipeline_->work_->referenceStage;
 words(w,re::ready::referenceStatus,stage.evidenceStatus());
 words(w,re::ready::residentStatus,pipeline_->readyEvidence_);
 w.u64(re::ready::times,15);
 times(w,re::ready::times+16,stage.evidenceTimes(),0,3);
 w.f64(re::ready::times+40,pipeline_->work_->timestamps->observed(0));
 const auto shape=gpu_reference::validate(private_.reference,config_.geometry.clutterSamples,config_.geometry.channels,config_.sampleRate);
 const uint32_t v[]={config_.geometry.clutterSamples,config_.geometry.channels,uint32_t(config_.reference.channels.size()),1,config_.reference.analysisSamples,config_.reference.analysisInterval,config_.reference.powerIterations,shape.bins,shape.decimation,shape.folded?1u:0u,shape.transform,shape.stride,shape.observations,shape.chunks,config_.correlation,config_.geometry.doppler};
 for(size_t i=0;i<16;++i)w.u32(re::ready::shape+4*i,v[i]);}
 void hostEvidence(Bytes bytes){rw::Writer w{bytes};
 pipeline_->hostTiming_.exportData([&](uint64_t generation,uint64_t begin,uint64_t end,const auto& entries,uint32_t n,const auto& submissions,uint32_t ns,const auto& contexts,uint32_t nc,bool configured){
  const size_t at=re::prepared::hostSummary;uint64_t covered=0;for(uint32_t i=0;i<n;++i)if(entries[i].parent==UINT32_MAX)covered=rw::add(covered,entries[i].end-entries[i].begin);rw::require(covered<=end-begin,"Resident host envelope inconsistent");w.u32(at,1);w.u32(at+4,128);w.u64(at+8,generation);w.u64(at+16,begin);w.u64(at+24,end);w.u64(at+32,covered);w.u64(at+40,end-begin-covered);w.u32(at+48,n);w.u32(at+52,2048);w.u32(at+56,ns);w.u32(at+60,128);w.u32(at+64,nc);w.u32(at+68,1);w.u32(at+72,5);w.u32(at+76,configured?(nc&&contexts[0].supported?1:2):0);w.f64(at+80,pipeline_->evidenceHostResultMs_);
  for(uint32_t i=0;i<n;++i){const auto& e=entries[i];uint16_t label=0;for(uint16_t j=1;j<26;++j)if(std::strcmp(e.label,re::labels[j])==0){label=j;break;}rw::require((label&&e.parent<2048)||(label&&e.parent==UINT32_MAX),"Resident host evidence dictionary/parent invalid");const size_t a=re::prepared::hostIntervals+24*i;w.u32(a,uint32_t(label)|(uint32_t(e.parent==UINT32_MAX?65535:e.parent)<<16));w.u32(a+4,e.tag);w.u64(a+8,e.begin-begin);w.u64(a+16,e.end-begin);}
  for(uint32_t i=0;i<ns;++i){const auto& x=submissions[i];const auto& d=x.delta;const size_t a=re::prepared::submitThreads+64*i;w.u32(a,x.interval|(uint32_t(gpu_host::ThreadSample::supported())<<16)|(uint32_t(d.valid)<<24));const uint64_t values[]={d.cpu,d.user,d.system,d.minorFaults,d.majorFaults,d.voluntary,d.involuntary};for(size_t j=0;j<7;++j)w.u64(a+8+8*j,values[j]);}
  if(nc){const auto& x=contexts[0];const size_t a=re::prepared::submitContext;w.u32(a,x.interval);w.u32(a+4,x.supported);w.u32(a+8,uint32_t(x.schedulerEnabled));w.u32(a+12,uint32_t(x.schedulerFlagError));auto snapshot=[&](size_t o,const auto& s){w.u64(o,s.beginNs);w.u64(o+8,s.endNs);for(size_t i=0;i<3;++i)w.u64(o+16+8*i,s.scheduler[i]);for(size_t i=0;i<2;++i)w.u64(o+40+8*i,s.process[i]);for(size_t i=0;i<6;++i)w.u64(o+56+8*i,s.cgroup[i]);w.u32(o+104,uint32_t(s.schedulerError));w.u32(o+108,uint32_t(s.processError));w.u32(o+112,uint32_t(s.cgroupError));};snapshot(a+16,x.before);snapshot(a+136,x.after);}
 });}
 void preparedEvidence(const Packet& p){auto bytes=map_.bytes(Section::preparedEvidence);
 header(bytes,p,resident::Phase::prepared,config_.stages(),((uint64_t(1)<<12)-1)&~(config_.tracker.enabled?0ULL:(1ULL<<6)));
 allocation(sub(bytes,re::prepared::allocation,832));
 cpu(sub(bytes,re::prepared::cpuProof,288));
 rw::Writer w{bytes};
 const auto& c=*pipeline_->work_->clutter;
 size_t a=re::prepared::nativeReference;
 w.u64(a,c.generation_);
 const uint32_t shape[]={c.samples_,c.bins_,gpu_native::fft,c.native_->hop(),c.native_->blocks(),c.channels_};
 for(size_t i=0;i<6;++i)w.u32(a+8+4*i,shape[i]);
 words(w,a+32,c.referenceEvidence_);
 times(w,a+96,*c.timestamps_,0,3);
 words(w,re::prepared::residentFinalStatus,pipeline_->finalEvidence_);
 words(w,re::prepared::postprocessStatus,pipeline_->work_->postprocessStage->evidenceStatus());
 if(config_.tracker.enabled)words(w,re::prepared::trackerPreparedStatus,pipeline_->tracker_->evidencePrepared());
  uint64_t mask=3|(uint64_t(1)<<2)|(uint64_t(1)<<7);
  if(config_.detector.enabled)mask|=uint64_t(15)<<3;
  if(config_.tracker.enabled)mask|=uint64_t(63)<<8;
  w.u64(re::prepared::times,mask);
  times(w,re::prepared::times+16,*pipeline_->work_->timestamps,3,2);
  const auto& post=pipeline_->work_->postprocessStage->evidenceTimes();
  w.f64(re::prepared::times+32,post.observed(0));
  w.f64(re::prepared::times+72,post.observed(5));
  if(config_.detector.enabled)times(w,re::prepared::times+40,post,1,4);
  if(config_.tracker.enabled)times(w,re::prepared::times+80,pipeline_->tracker_->evidenceTimes(),0,6);
  for(uint32_t i=0;i<c.completedBatches_;++i){const auto& b=c.batchEvidence_[i];
  const auto& r=pipeline_->batchEvidence_[i];
  a=re::prepared::nativeBatches+256*i;
  w.u64(a,c.generation_);
  w.u32(a+8,b.first);
  w.u32(a+12,b.active);
  w.u32(a+16,c.channels_);
  words(w,a+24,b.status);
  words(w,a+88,r.status);
  for(size_t j=0;j<9;++j)w.f64(a+152+8*j,b.times[j]);
  w.f64(a+224,r.packMs);
  w.f64(a+232,r.subtractMs);
  w.f64(a+240,b.hostMs);}hostEvidence(bytes);
  if(rw::Reader{{bytes.data,bytes.size}}.u32(re::prepared::hostSummary+64))w.u64(96,rw::Reader{{bytes.data,bytes.size}}.u64(96)|(1ULL<<12));
 }
 void control(const Packet& p,resident::Phase phase,uint64_t valid=0,uint32_t capture=0,bool idempotent=false){rw::Writer w{map_.bytes(Section::controlEvidence)};
 w.zero();
 rw::require(event_!=UINT64_MAX,"Resident control evidence sequence exhausted");
 w.u32(0,1);
 w.u32(4,512);
 w.u32(8,uint32_t(phase));
 w.u64(16,p.session[0]);
 w.u64(24,p.session[1]);
 const uint64_t fields[]={p.requestId,p.frame.logicalId,p.epoch,p.frame.consumedSequence,p.tracker.transaction,p.tracker.generation,p.tracker.consumedSequence,p.tracker.epoch};
 for(size_t i=0;i<8;++i)w.u64(32+8*i,fields[i]);
 w.u64(96,valid|32);
 w.u64(104,++event_);
 if((valid&1)&&pipeline_->tracker_){const auto& status=pipeline_->tracker_->evidenceControl();
  // The control envelope reserves exactly 64 words before timing at byte 384.
  // New tracker-only diagnostics remain available in prepared evidence.
  rw::require(status.size()>=64,"Resident tracker control proof is short");
  for(size_t i=0;i<64;++i)w.u32(128+4*i,status[i]);}
 if(pipeline_&&pipeline_->tracker_){const auto&t=pipeline_->tracker_->evidenceTimes();
 if(valid&2)w.f64(384,t.observed(6));
 if(valid&4)w.f64(392,t.observed(5));
 if(valid&8)w.f64(400,t.observed(7));
 if(valid&16)w.f64(408,pipeline_->tracker_->evidenceHostCommitMs());}
  AllocationExit state=exit_;
  if(pipeline_){std::lock_guard<std::mutex>lock(allocationsMutex);
  const auto& a=deviceAllocations.at(pipeline_->context_.device);
  state={true,a.budget.deviceUsed(),a.budget.devicePeak(),a.budget.deviceLimit(),
   a.allocations.size()};}rw::require(state.valid&&state.used<=state.peak&&state.peak<=state.limit,"Resident control allocator evidence invalid");
  w.u64(416,state.used);
  w.u64(424,state.peak);
  w.u64(432,state.limit);
  w.u64(440,state.live);
  w.u32(456,2);
  w.u32(460,idempotent);
  w.u32(464,trackerResult_.noOp);
  w.u32(468,committedSlot_);
  w.u32(472,phase==resident::Phase::committed?config_.stages():0);
  w.u32(476,capture);
 }
public:
 ResidentModule(std::shared_ptr<Instance> instance,Candidate device,const resident::Config& c,const resident::Mapping& m):config_(c),map_(m),private_(privateConfig(c)),imageHash_(moduleImageHash()),configHash_(rw::configHash(c)),pipeline_(std::make_unique<ResidentFramePipeline>(std::move(instance),std::move(device),private_)){pipeline_->context_.releaseEvidence=&exit_;
 outputPresence_=resident::outputPresence(config_.presence,0);
 if(const char* path=std::getenv("BLAH2_GPU_SUBMIT_CONTEXT_CGROUP")){if(!*path)throw std::invalid_argument("Empty resident context probe path");
 pipeline_->configureSubmitContextProbe(path);}}
 void initializeEvidence(const Packet& p)override{device();
 const auto& f=pipeline_->spectrumFrequencies();
 rw::require(f.size()==config_.spectrumBins,"Resident frequency count changed");
 std::memcpy(pointer<double>(Section::frequencies),f.data(),f.size()*8);
 reference_=pipeline_->exportReferenceCheckpoint();
 referenceCheckpoint(p);
 if(config_.tracker.enabled)pipeline_->copyTrackerCommitted({pointer<uint32_t>(Section::tracker0),count<uint32_t>(Section::tracker0),
  config_.tracker.trackGuidedShadow?pointer<uint32_t>(Section::guidedRecoveries):nullptr,
  config_.tracker.trackGuidedShadow?count<uint32_t>(Section::guidedRecoveries):0,&trackerResult_});
 control(p,resident::Phase::idle,config_.tracker.enabled?2:0,config_.tracker.enabled?1:0);}
 bool prepareReference(const Packet& p)override{rw::require(!ready_&&!prepared_,"Resident module already owns a frame");
 outputPresence_=resident::outputPresence(config_.presence,p.diagnosticPresence);
 pipeline_->generation_=p.frame.logicalId-1;
 pipeline_->work_->clutter->generation_=p.frame.logicalId-1;
 committed_=false;
 token_={};
 trackerResult_={};
 ReferenceOutputs out{&reference_,pointer<std::complex<double>>(Section::spectrum),config_.spectrumBins,pointer<double>(Section::spectrumDb),config_.spectrumBins,pointer<std::complex<double>>(Section::fullReference),count<std::complex<double>>(Section::fullReference)};
 if(!pipeline_->prepareCaptured(pointer<std::complex<double>>(Section::canonical),count<std::complex<double>>(Section::canonical),out))return false;
 referenceCheckpoint(p);
 readyEvidence(p);
 ready_=true;
 return true;}
 bool finishRadar(const Packet& p)override{rw::require(ready_&&!prepared_,"Resident module radar phase invalid");
 ResidentFrameOutputs out;
 out.maps=pointer<std::complex<float>>(Section::maps);
 out.mapCount=count<std::complex<float>>(Section::maps);
 out.fused=pointer<std::complex<double>>(Section::fused);
 out.fusedCount=count<std::complex<double>>(Section::fused);
 out.filtered=pointer<std::complex<double>>(Section::filtered);
 out.filteredCount=count<std::complex<double>>(Section::filtered);
 out.tail=pointer<std::complex<double>>(Section::tail);
 out.tailCount=count<std::complex<double>>(Section::tail);
 auto& o=out.postprocess;
 o.metrics=&metrics_;
 o.display=pointer<double>(Section::display);
 o.displayCount=count<double>(Section::display);
 o.detections=pointer<PostprocessDetection>(Section::detections);
 o.detectionCapacity=count<PostprocessDetection>(Section::detections);
 o.levels=pointer<double>(Section::levels);
 o.levelCount=count<double>(Section::levels);
 o.unflooredSnr=pointer<double>(Section::unflooredSnr);
 o.unflooredSnrCount=count<double>(Section::unflooredSnr);
 o.cfar=pointer<PostprocessDetection>(Section::cfar);
 o.cfarCapacity=count<PostprocessDetection>(Section::cfar);
 o.centroid=pointer<PostprocessDetection>(Section::centroid);
 o.centroidCapacity=count<PostprocessDetection>(Section::centroid);
 o.decisionFlags=pointer<uint32_t>(Section::flags);
 o.decisionFlagCount=count<uint32_t>(Section::flags);
 o.detectionCells=pointer<uint32_t>(Section::detectionCells);
 o.detectionCellCapacity=count<uint32_t>(Section::detectionCells);
  if(config_.tracker.enabled){preparedSlot_=1-committedSlot_;
  const auto slot=preparedSlot_?Section::tracker1:Section::tracker0;
  out.tracker={pointer<uint32_t>(slot),count<uint32_t>(slot),
   config_.tracker.trackGuidedShadow?pointer<uint32_t>(Section::guidedRecoveries):nullptr,
   config_.tracker.trackGuidedShadow?count<uint32_t>(Section::guidedRecoveries):0,&trackerResult_};
  out.trackerTimestamp=p.frame.signalTimestampMs;
  out.consumedInputSequence=p.frame.consumedSequence;}
  if(!pipeline_->finishResidentRadar(out)){return false;}
   token_={trackerResult_.token.epoch,trackerResult_.token.transaction,trackerResult_.token.generation,trackerResult_.token.consumedSequence};
   rw::Writer w{map_.bytes(Section::metrics)};
   w.zero();
   w.u32(0,1);
   w.u32(4,128);
   w.u64(8,p.frame.logicalId);
   w.u64(16,p.frame.consumedSequence);
   w.f64(24,metrics_.noisePower);
   w.f64(32,metrics_.maxPower);
   w.u64(40,metrics_.cfarCount);
   w.u64(48,metrics_.centroidCount);
   w.u64(56,metrics_.detectionCount);
   w.u64(64,trackerResult_.checkpointWords);
   w.u32(72,config_.tracker.enabled?preparedSlot_:0);
   w.u32(76,trackerResult_.noOp);
   w.u32(80,config_.stages());
   w.u32(84,config_.stages());
   w.u32(88,trackerResult_.guidedRecoveries);
   w.u32(92,config_.tracker.trackGuidedShadow?6:0);
   preparedEvidence(p);
   prepared_=true;
   return true;
 }
 resident::Status rejectionStatus()const override{return config_.tracker.enabled&&pipeline_->tracker_->evidencePrepared()[0]==307?resident::Status::capacity:resident::Status::numeric;}
 resident::Token preparedToken()const override{return token_;}
 void commit(const Packet& p)override{rw::require(prepared_&&rw::same(p.tracker,token_),"Resident module commit mismatch");
 const bool duplicate=committed_;
 if(config_.tracker.enabled)pipeline_->commitTracker({token_.epoch,token_.transaction,token_.generation,token_.consumedSequence});
 committed_=true;
 if(config_.tracker.enabled)committedSlot_=preparedSlot_;
 control(p,resident::Phase::committed,config_.tracker.enabled&&!duplicate?25:0,duplicate?2:(config_.tracker.enabled?1:0),duplicate);}
 void abort(const Packet& p)override{rw::require(prepared_&&!committed_&&rw::same(p.tracker,token_),"Resident module abort mismatch");
 if(config_.tracker.enabled)pipeline_->abortTracker({token_.epoch,token_.transaction,token_.generation,token_.consumedSequence});
 prepared_=ready_=false;
 control(p,resident::Phase::rejected);}
 void settle(const Packet& p)override{rw::require(prepared_&&committed_&&rw::same(p.tracker,token_),"Resident module settlement mismatch");
 control(p,resident::Phase::settled);
 prepared_=ready_=false;}
 void restore(const Packet& p)override{rw::require(!ready_&&!prepared_,"Resident module restore while pending");
 rw::Reader r{map_.read(Section::referenceCheckpoint)};
 rw::require(r.u32(0)==1&&r.u32(4)==256&&r.u64(8)==config_.epoch&&r.u64(24)==config_.geometry.clutterSamples&&r.u64(32)==config_.geometry.channels&&r.u64(56)<=8,"Resident reference checkpoint envelope mismatch");
 r.zeros(216,40);
 ReferenceCheckpoint cp;
 cp.config=private_.reference;
 cp.samples=config_.geometry.clutterSamples;
 cp.channels=config_.geometry.channels;
 cp.sampleRate=config_.sampleRate;
 cp.frames=r.u64(40);
 cp.updates=r.u64(48);
 cp.weightCount=uint32_t(r.u64(56));
 for(size_t i=0;i<8;++i)cp.weights[i]={r.f64(64+16*i),r.f64(72+16*i)};
 cp.coherentFraction=r.f64(192);
 cp.coherentGainDb=r.f64(200);
 cp.scale=r.f64(208);
 pipeline_->restoreReferenceCheckpoint(cp);
 reference_=cp;
  if(config_.tracker.enabled){const auto slot=p.restoreTrackerSlot?Section::tracker1:Section::tracker0;
  rw::Reader t{map_.read(slot)};
  const uint64_t tracks=t.u64(16),words=rw::add(resident::trackerHeaderWords,
   rw::mul(tracks,resident::trackerRecordWords(config_.tracker)));
  rw::require(tracks<=config_.tracker.capacity&&words<=count<uint32_t>(slot)&&t.u64(48)==p.tracker.generation,"Resident tracker restore packet differs");
  pipeline_->restoreTracker(private_.tracker,pointer<uint32_t>(slot),size_t(words));
  committedSlot_=p.restoreTrackerSlot;}
  control(p,resident::Phase::idle,config_.tracker.enabled?13:0,config_.tracker.enabled?1:0);
  committed_=false;
  token_={};
 }
 void shutdown(const Packet& p)override{rw::require(!ready_&&!prepared_,"Resident module shutdown while pending");
 pipeline_.reset();
 rw::require(exit_.valid&&!exit_.used&&!exit_.live,"Resident module cleanup incomplete");
 control(p,resident::Phase::settled);}
};
} }
extern "C" blah2::resident::Backend* blah2_gpu_create_resident(unsigned abi,const blah2::resident::Config* config,const blah2::resident::Mapping* map){
 if(abi!=blah2::resident::moduleAbi||!config||!map){throw std::runtime_error("Resident module ABI9 mismatch");}
 blah2::resident::validateConfig(*config);
 auto instance=std::make_shared<blah2::Instance>();
 for(auto candidate:blah2::enumerate(*instance))if(candidate.info.id==config->device)return new blah2::ResidentModule(instance,std::move(candidate),*config,*map);
 throw std::runtime_error("Requested resident GPU is unavailable; no CPU fallback");
}
