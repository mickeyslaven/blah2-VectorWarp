#include "GpuResidentProof.h"
#include "../../../core/TrackBeforeDetect.private.h"
#include <algorithm>
namespace blah2::resident {
using wire::require;
using wire::Reader;
using proof_detail::sub;
void HostTimingSnapshot::validate()const{Reader r{{data_.data(),data_.size()}};
const uint64_t begin=r.u64(16),end=r.u64(24),top=r.u64(32);
const uint32_t n=r.u32(48),ns=r.u32(56),nc=r.u32(64);
require(r.u32(0)==1&&r.u32(4)==128&&end>=begin&&top<=end-begin&&r.u64(40)==end-begin-top&&n<=2048&&ns<=128&&nc<=1&&r.u32(52)==2048&&r.u32(60)==128&&r.u32(68)==1&&r.u32(72)==5&&r.u32(76)<=2,"Resident host summary invalid");
r.zeros(88,40);
require(std::isfinite(r.f64(80))&&r.f64(80)>=0,"Resident host envelope timing invalid");
uint64_t sum=0,last=0;
 for(uint32_t i=0;i<2048;++i){const size_t a=evidence::prepared::hostIntervals-start+24*i;
 if(i>=n){r.zeros(a,24);
 continue;}const auto packed=r.u32(a),label=packed&65535,parent=packed>>16;
 const uint64_t b=r.u64(a+8),e=r.u64(a+16);
 require(label&&label<26&&b<=e&&e<=end-begin&&(parent==65535||parent<i),"Resident host interval invalid");
 if(parent==65535){require(b>=last,"Resident host top-level overlap");
 sum=wire::add(sum,e-b);
 last=e;}else{size_t p=evidence::prepared::hostIntervals-start+24*parent;
 require(b>=r.u64(p+8)&&e<=r.u64(p+16),"Resident host child interval outside parent");}}
 require(sum==top,"Resident host coverage differs");
 for(uint32_t i=0;i<128;++i){const size_t a=evidence::prepared::submitThreads-start+64*i;
 if(i>=ns){r.zeros(a,64);
 continue;}const auto p=r.u32(a);
 uint32_t interval=p&65535,supported=(p>>16)&255,valid=p>>24;
 require(interval<n&&supported<=1&&valid<=supported&&(r.u32(evidence::prepared::hostIntervals-start+24*interval)&65535)==22,"Resident thread proof invalid");
 r.zeros(a+4,4);
 if(!valid)r.zeros(a+8,56);}
 const size_t a=evidence::prepared::submitContext-start;
 if(!nc){r.zeros(a,256);
 require(r.u32(76)==0,"Configured context proof missing");
 return;}require(r.u32(a)<n&&r.u32(a+4)<=1&&r.i32(a+8)>=-1&&r.i32(a+8)<=1,"Resident context proof fields invalid");
 const size_t api=evidence::prepared::hostIntervals-start+24*r.u32(a);
 const uint32_t parent=r.u32(api)>>16;
 require(parent<n&&(r.u32(api)&65535)==22&&(r.u32(evidence::prepared::hostIntervals-start+24*parent)&65535)==2,"Resident context proof parent differs");
 const size_t outer=evidence::prepared::hostIntervals-start+24*parent;
 const uint64_t bb=r.u64(a+16),be=r.u64(a+24),ab=r.u64(a+136),ae=r.u64(a+144);
 require(bb<=be&&be<=begin+r.u64(api+8)&&ab>=begin+r.u64(api+16)&&ab<=ae&&bb>=begin+r.u64(outer+8)&&ae<=begin+r.u64(outer+16),"Resident context probe outside boundaries");
 r.zeros(a+132,4);
 r.zeros(a+252,4);
}
void HostTimingSnapshot::write(std::ostream& stream,uint32_t call)const{validate();
Reader r{{data_.data(),data_.size()}};
const uint64_t frame=r.u64(8),begin=r.u64(16),end=r.u64(24);
const auto n=r.u32(48),ns=r.u32(56),nc=r.u32(64);
stream<<"GPU_HOST_FRAME call="<<call<<" frame="<<frame<<" entries="<<n<<" capacity=2048 envelope_ns="<<end-begin<<" top_level_ns="<<r.u64(32)<<" unaccounted_ns="<<r.u64(40)<<" calibrated_gpu=0\n";
 for(uint32_t i=0;i<n;++i){size_t a=evidence::prepared::hostIntervals-start+24*i;
 const auto packed=r.u32(a);
 const uint32_t parent=packed>>16;
 stream<<"GPU_HOST_INTERVAL call="<<call<<" frame="<<frame<<" id="<<i<<" parent="<<(parent==65535?UINT32_MAX:parent)<<" label="<<evidence::labels[packed&65535]<<" tag="<<r.u32(a+4)<<" begin_ns="<<r.u64(a+8)<<" end_ns="<<r.u64(a+16)<<" elapsed_ns="<<r.u64(a+16)-r.u64(a+8)<<'\n';}
 for(uint32_t i=0;i<ns;++i){size_t a=evidence::prepared::submitThreads-start+64*i;
 const auto p=r.u32(a);
 stream<<"GPU_HOST_SUBMIT_THREAD call="<<call<<" frame="<<frame<<" interval="<<(p&65535)<<" supported="<<((p>>16)&255)<<" valid="<<(p>>24)<<" capacity=128 storage_bytes=8192";
 const char* names[]={"thread_cpu_ns","thread_user_us","thread_system_us","minor_faults","major_faults","voluntary_switches","involuntary_switches"};
 for(size_t j=0;j<7;++j)stream<<' '<<names[j]<<'='<<r.u64(a+8+8*j);
 stream<<" sample_boundary_overhead_included=1 driver_helper_threads_included=0\n";}
 if(nc){size_t a=evidence::prepared::submitContext-start,b=a+16,z=a+136;
 std::array<uint64_t,11> delta{};
 bool valid[]={r.u32(b+104)==0&&r.u32(z+104)==0,r.u32(b+108)==0&&r.u32(z+108)==0,r.u32(b+112)==0&&r.u32(z+112)==0};
 for(size_t i=0;i<11;++i){const uint64_t before=r.u64(b+16+8*i),after=r.u64(z+16+8*i);
 size_t group=i<3?0:(i<5?1:2);
 if(after<before)valid[group]=false;
 else delta[i]=after-before;}for(size_t i=0;i<11;++i)if(!valid[i<3?0:(i<5?1:2)])delta[i]=0;
  stream<<"GPU_HOST_SUBMIT_CONTEXT call="<<call<<" frame="<<frame<<" interval="<<r.u32(a)<<" supported="<<r.u32(a+4)<<" scheduler_enabled="<<r.i32(a+8)<<" scheduler_flag_errno="<<r.i32(a+12);
  const char* group[]={"scheduler","process","cgroup"};
  for(size_t i=0;i<3;++i)stream<<' '<<group[i]<<"_valid="<<valid[i]<<' '<<group[i]<<"_before_errno="<<r.i32(b+104+4*i)<<' '<<group[i]<<"_after_errno="<<r.i32(z+104+4*i);
  stream<<" before_begin_ns="<<r.u64(b)<<" before_end_ns="<<r.u64(b+8)<<" after_begin_ns="<<r.u64(z)<<" after_end_ns="<<r.u64(z+8)<<" probe_ns="<<(r.u64(b+8)-r.u64(b))+(r.u64(z+8)-r.u64(z));
  const char* names[]={"scheduler_execution_ns","scheduler_runqueue_ns","scheduler_timeslices","process_user_us","process_system_us","cgroup_usage_us","cgroup_user_us","cgroup_system_us","cgroup_periods","cgroup_throttled_periods","cgroup_throttled_us"};
  for(size_t i=0;i<11;++i)stream<<' '<<names[i]<<'='<<delta[i];
  stream<<" own_membership_verified="<<r.u32(a+4)<<" capacity=1 storage_bytes=256 overhead_in_whole=1 causal_claim=0\n";
 }
}
namespace {
void identity(ConstBytes b,const Config& c,const Packet& p,Phase phase,uint32_t stages){Reader r{b};
const auto hash=wire::configHash(c);
require(r.u32(8)==uint32_t(phase)&&!r.u32(12)&&r.u64(16)==p.session[0]&&r.u64(24)==p.session[1]&&r.u64(32)==p.requestId&&r.u64(40)==p.frame.logicalId&&r.u64(48)==c.epoch&&r.u64(56)==p.frame.consumedSequence&&r.u64(64)==p.frame.signalTimestampMs&&r.u32(88)==c.stages()&&r.u32(92)==stages&&r.u64(104)==outputPresence(c.presence,p.diagnosticPresence)&&std::memcmp(b.data+128,hash.data(),32)==0&&std::memcmp(b.data+192,evidence::schemaHash,32)==0,"Resident proof identity differs");
uint32_t nonzero=0;
for(size_t i=160;i<192;++i)nonzero|=b.data[i];
require(nonzero,"Resident module image identity missing");}
void cpu(ConstBytes b,const Config& c){Reader r{b};
uint64_t required=(uint64_t(1)<<31)-1;
if(!c.detector.enabled)required&=~(uint64_t(15)<<14);
if(!c.tracker.enabled)required&=~(uint64_t(31)<<18);
const auto declared=r.u64(8),source=r.u64(16),observed=r.u64(24);
require(r.u64(0)==required&&!(declared>>31)&&source==required&&!observed,"Resident CPU call-graph proof differs");
r.zeros(32,256);}
}
void validateDevice(const Config& c,ConstBytes device){Reader d{device};
require(device.size==512&&d.u32(0)==1&&d.u32(4)==512&&(d.u32(40)&1)&&!(d.u32(40)&~3u)&&d.u32(44)==1&&d.u32(32)&&d.u32(32)<=64&&d.u32(48)&&d.u32(48)<=16&&d.u32(52)&&d.u32(52)<=32,"Resident selected device proof invalid");
uint32_t bits=d.u32(36);
float period=0;
std::memcpy(&period,&bits,4);
require(std::isfinite(period)&&period>0,"Resident device timestamp period invalid");
require(d.u64(56)==std::min<uint64_t>(2ULL<<30,d.u64(120)/4),"Resident selected device budget differs");
size_t length=0;
while(length<96&&device.data[160+length])++length;
require(length<96&&std::string(reinterpret_cast<const char*>(device.data+160),length)==c.device,"Resident selected device selector differs");
d.zeros(160+length,96-length);
size_t name=0;
while(name<256&&device.data[256+name])++name;
require(name&&name<256,"Resident device name bounds");
d.zeros(256+name,256-name);}
uint64_t validateControl(const Config& c,const Packet& request,const Packet& response,ConstBytes bytes,ConstBytes device,uint64_t previousEvent){
 validateDevice(c,device);
 Reader r{bytes};
 require(bytes.size==controlEvidenceBytes&&r.u32(0)==1&&r.u32(4)==controlEvidenceBytes&&r.u32(8)==uint32_t(response.phase)&&!r.u32(12)&&r.u64(16)==request.session[0]&&r.u64(24)==request.session[1],"Resident control evidence envelope differs");
 const uint64_t fields[]={request.requestId,request.frame.logicalId,request.epoch,request.frame.consumedSequence,request.tracker.transaction,request.tracker.generation,request.tracker.consumedSequence,request.tracker.epoch};
 for(size_t i=0;i<8;++i)require(r.u64(32+8*i)==fields[i],"Resident control evidence identity differs");
 uint64_t expectedMask=32;
 const bool duplicate=request.op==Op::commit&&request.phase==Phase::committed;
 uint32_t capture=duplicate?2:0;
 if(c.tracker.enabled){if(request.op==Op::initialize){expectedMask|=2;
 capture=1;}else if(request.op==Op::restore){expectedMask|=13;
 capture=1;}else if(request.op==Op::commit&&!duplicate){expectedMask|=25;
 capture=1;}else if(duplicate)capture=2;}
 require(r.u64(96)==expectedMask&&r.u64(104)>previousEvent&&r.u32(460)==uint32_t(duplicate)&&r.u32(464)<=1&&r.u32(468)<=uint32_t(c.tracker.enabled)&&r.u32(472)==(request.op==Op::commit?c.stages():0)&&r.u32(476)==capture,"Resident control coverage differs");
 r.zeros(112,16);
 r.zeros(448,8);
 require(r.u32(456)==2,"Resident control CPU proof kind differs");
 r.zeros(480,32);
 if(expectedMask&1){require(!r.u32(128)&&r.u32(128+3*4)==c.tracker.capacity&&r.u32(128+58*4)==1,"Resident tracker control status invalid");
 if(request.op==Op::commit){require(r.u64(128+8*4)==request.tracker.transaction&&r.u64(128+10*4)==request.tracker.generation&&r.u64(128+12*4)==request.tracker.consumedSequence&&r.u64(128+14*4)==request.tracker.epoch,"Resident tracker commit GPU token differs");}}else r.zeros(128,256);
 for(size_t i=0;i<4;++i){const double ms=r.f64(384+8*i);
 require(std::isfinite(ms)&&ms>=0,"Resident control timing invalid");
 if(!(expectedMask&(uint64_t(2)<<i)))require(ms==0,"Resident unobserved control time nonzero");}
 const uint64_t used=r.u64(416),peak=r.u64(424),limit=r.u64(432),live=r.u64(440);
 Reader d{device};
 require(used<=peak&&peak<=limit&&limit==d.u64(56),"Resident control allocation bounds differ");
 if(request.op==Op::quit)require(!used&&!live,"Resident worker cleanup not complete");
 return r.u64(104);
}
void validateEvidence(const Config& c,const Packet& p,ConstBytes ready,ConstBytes prepared,ConstBytes device){
 const auto a=readyProof(ready);
 Reader readyReader{ready};
 require(readyReader.u64(96)==127,"Resident Ready block mask differs");
 readyReader.zeros(120,8);
 readyReader.zeros(1376,32);
 readyReader.zeros(1544,8);
 readyReader.zeros(1584,16);
 readyReader.zeros(1664,2432);
 identity(ready,c,p,Phase::referenceReady,48);
 cpu(sub(ready,1088,288),c);
 validateDevice(c,device);
 require(std::memcmp(ready.data+224,device.data+128,32)==0,"Resident selected device UUID differs");
 const auto& g=c.geometry;
 const auto& s=a.referenceStatus;
 const uint32_t refs=uint32_t(c.reference.channels.size()),stride=std::max(1u,g.clutterSamples/c.reference.analysisSamples),obs=1+(g.clutterSamples-1)/stride;
 require(a.frame==p.frame.logicalId&&a.referenceGeneration==p.frame.logicalId&&a.consumed==p.frame.consumedSequence&&a.gpuTimeMask==15&&a.stages==48&&a.batchCapacity&&a.batchCapacity<=g.channels,"Resident Ready proof envelope invalid");
 require(!s[0]&&s[1]<=1&&s[2]==(s[1]?obs:0)&&s[3]==(s[1]?refs*refs:0)&&s[4]==(s[1]?c.reference.powerIterations:0)&&s[5]==g.clutterSamples&&s[6]==g.clutterSamples&&s[7]==1+(g.clutterSamples-1)/32&&s[8]==g.clutterSamples&&s[9]==c.spectrumBins&&s[10]==1,"Resident Ready device coverage invalid");
 // Parallel covariance derivative retains words11/12 (partial/final cells).
 require(s[11]==(s[1]?refs*refs*(1+(obs-1)/std::max(128u,1+(obs-1)/(65536u/(refs*refs)))):0)&&s[12]==(s[1]?refs*refs:0),"Resident covariance device coverage invalid");
 for(size_t i=13;i<16;++i)require(!s[i],"Resident reference reserved status nonzero");
 require(a.residentStatus[0]==0&&a.residentStatus[1]==g.clutterSamples&&a.residentStatus[2]==c.correlation*g.doppler,"Resident reference packing coverage invalid");
 const uint32_t decimation=uint32_t(std::min<double>(g.clutterSamples,std::max(1.0,std::ceil(c.reference.bandwidthHz/(c.sampleRate/g.clutterSamples)))));
 const bool folded=decimation>=8&&g.clutterSamples%decimation==0;
 require(a.shape==std::array<uint32_t,16>{g.clutterSamples,g.channels,refs,1,c.reference.analysisSamples,c.reference.analysisInterval,c.reference.powerIterations,c.spectrumBins,decimation,uint32_t(folded),folded?c.spectrumBins:g.clutterSamples,stride,obs,1+(g.clutterSamples-1)/32,c.correlation,g.doppler},"Resident reference shape proof differs");
 for(size_t i=3;i<16;++i)require(!a.residentStatus[i],"Resident Ready status includes later work");
 if(p.phase==Phase::referenceReady)return;
 const auto b=preparedProof(prepared);
 Reader preparedReader{prepared};
 const uint64_t blockMask=(((1ULL<<12)-1)&~(c.tracker.enabled?0ULL:(1ULL<<6)))|(preparedReader.u32(evidence::prepared::hostSummary+64)?(1ULL<<12):0);
 require(preparedReader.u64(96)==blockMask,"Resident Prepared block mask differs");
 preparedReader.zeros(120,8);
 preparedReader.zeros(1376,32);
 preparedReader.zeros(1528,8);
 preparedReader.zeros(1992,8);
 identity(prepared,c,p,Phase::prepared,c.stages());
 cpu(sub(prepared,1088,288),c);
 require(std::memcmp(ready.data+160,prepared.data+160,96)==0,"Resident prepared cohort/device identity differs");
 require(b.nativeGeneration==p.frame.logicalId&&b.residentGeneration==p.frame.logicalId&&b.batchCapacity==a.batchCapacity&&b.batchCount==(g.channels+b.batchCapacity-1)/b.batchCapacity,"Resident prepared batch identity differs");
 const uint32_t hop=1024-g.clutterBins+1,blocks=1+(g.clutterSamples-1)/hop;
 require(b.nativeShape==std::array<uint32_t,6>{g.clutterSamples,g.clutterBins,1024,hop,blocks,b.batchCapacity}&&!b.nativeReferenceStatus[0]&&b.nativeReferenceStatus[4]==blocks&&b.nativeReferenceStatus[12]==1,"Resident native reference coverage differs");
 const bool diagnosticResidual=p.diagnosticPresence&(bit(Section::filtered)|bit(Section::tail));
 uint32_t next=0;
 for(uint32_t i=0;i<b.batchCount;++i){const auto& x=b.batches[i];
 const auto active=std::min(b.batchCapacity,g.channels-next);
 require(x.generation==p.frame.logicalId&&x.first==next&&x.active==active&&x.capacity==b.batchCapacity&&!x.nativeStatus[0]&&x.nativeStatus[5]==active*blocks&&x.nativeStatus[6]==active*g.clutterSamples&&x.nativeStatus[7]==active*g.clutterSamples&&x.nativeStatus[8]==2*active&&x.nativeStatus[9]==active&&x.nativeStatus[10]==active*g.clutterSamples&&x.nativeStatus[11]==active,"Resident native batch coverage differs");
 next+=active;
 require(!x.residentStatus[0]&&x.residentStatus[3]==next*g.clutterSamples&&x.residentStatus[4]==next*(diagnosticResidual?g.clutterSamples:c.correlation*g.doppler)&&x.residentStatus[5]==next*c.correlation*g.doppler,"Resident batch packing coverage differs");}const uint64_t cells=uint64_t(g.doppler)*g.delays;
 require(next==g.channels&&!b.residentStatus[0]&&b.residentStatus[6]==cells*g.channels&&b.residentStatus[7]==cells,"Resident map coverage differs");
 const auto& post=b.postprocessStatus;
 require(!post[0]&&post[1]==cells&&post[2]==cells&&post[3]==cells&&post[18]==uint32_t(c.epoch)&&post[19]==uint32_t(c.epoch>>32)&&post[20]==uint32_t(c.detector.enabled)&&post[21]==g.doppler&&post[22]==g.delays&&post[27]==c.detector.integrationCpis,"Resident postprocess coverage differs");
 if(c.detector.enabled){const uint32_t detectorPasses=c.detector.integrationCpis>2?(c.detector.integrationCpis==8?4u:3u):1u;
 require(post[4]==cells*detectorPasses&&post[6]<=cells&&post[7]==post[6]&&post[8]<=post[7]&&post[9]==post[8]&&post[10]<=post[9]&&post[11]==post[9]-post[10]&&post[12]==cells&&post[13]==post[11]&&post[15]==(c.detector.integrationCpis>2?post[6]:0),"Resident postprocess decision coverage differs");
 const uint32_t kinds=1u|((p.diagnosticPresence&bit(Section::cfar))?2u:0u)|((p.diagnosticPresence&bit(Section::centroid))?4u:0u),passes=1u+((kinds>>1)&1u)+((kinds>>2)&1u);
 require(post[26]==kinds&&post[23]==passes*((cells+127)/128)&&post[24]==post[23]&&post[25]==passes*cells&&post[16]==((kinds&2)?post[6]:0)&&post[17]==((kinds&4)?post[8]:0),"Resident stable compaction coverage differs");}else{for(size_t i=4;i<=17;++i)require(!post[i],"Disabled detector status nonzero");
 for(size_t i=23;i<=26;++i)require(!post[i],"Disabled compaction status nonzero");}
 require(post[28]<=1&&(c.detector.integrationCpis>=2||!post[28]),"Resident postprocess integration readiness differs");
 const auto motion=track_before_detect::decodeMotionBins(c.detector.integrationMotionBins);
 require(post[29]==(motion.delay|(motion.doppler<<16u))&&post[30]==c.detector.integrationBeamTolerance&&
  post[31]==(c.fusion.mode==FusionMode::noncoherent?0:c.fusion.beams),
  "Resident postprocess spatial integration echo differs");
 if(c.tracker.enabled){const auto& t=b.trackerStatus;
 auto pair=[&](size_t i){return uint64_t(t[i])|(uint64_t(t[i+1])<<32);};
 require(!t[0]&&t[1]<=1&&t[3]==c.tracker.capacity&&t[4]==cells&&pair(8)==p.tracker.transaction&&pair(10)==p.tracker.generation&&pair(12)==p.tracker.consumedSequence&&pair(14)==c.epoch&&pair(16)==p.frame.signalTimestampMs&&pair(52)==p.tracker.generation&&t[19]==post[11],"Resident tracker generation proof differs");
 require(t[54]<=c.tracker.capacity&&t[24]==trackerHeaderWords+trackerRecordWords(c.tracker)*t[54]&&uint64_t(t[33])+t[34]+t[35]+t[36]==t[54]&&t[32]==t[34]+t[35]+t[36]&&!t[58],"Resident tracker prepared snapshot differs");
 if(t[2]==1){for(size_t i=26;i<=31;++i)require(!t[i],"Resident no-op tracker performed work");
 require(!t[55]&&!t[56]&&!t[57]&&t[54]==t[18],"Resident no-op tracker coverage differs");}else require(t[2]==2&&t[26]==t[18]&&t[27]+t[28]==t[18]&&t[29]<=t[28]&&t[20]==t[18]-t[29]&&t[55]==t[18]&&t[57]==t[19]&&t[56]==(t[19]+127)/128&&t[30]==t[22]&&t[31]==t[23]&&t[23]==t[20]+t[22]&&t[54]==t[23],"Resident tracker executed-work coverage differs");}if(!c.tracker.enabled)preparedReader.zeros(evidence::prepared::trackerPreparedStatus,256);
 const uint64_t expectedTimes=135ULL|(c.detector.enabled?(15ULL<<3):0)|(c.tracker.enabled?(63ULL<<8):0);
 require(b.gpuTimeMask==expectedTimes,"Resident stage timing mask differs");
 for(size_t i=0;i<b.gpuMs.size();++i)if(!(expectedTimes&(1ULL<<i)))require(b.gpuMs[i]==0,"Resident disabled stage time is nonzero");
 HostTimingSnapshot host;
 host.assign(prepared);
}
}
