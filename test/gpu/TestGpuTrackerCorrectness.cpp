// Explicit device test; --software accepts only a CPU Vulkan device. Production
// enumeration still excludes software devices. No capture, services or FFT work.
#include "../../core/VulkanBackend.private.cpp"
#include <map>
#include <sstream>
#include "process/tracker/CausalTraceLayout.h"
#include "process/tracker/SignalProgressClock.h"
#include "process/tracker/Tracker.h"
#include "process/tracker/TrackerCheckpointOracle.h"
#include "process/ambiguity/GpuResultJson.private.h"

namespace blah2 {namespace {namespace gpu_tracker {
struct QualificationSnapshot {
 static std::vector<uint32_t> committed(Stage& stage){
  stage.healthy();
  std::vector<uint32_t> state{uint32_t(stage.acceptedFrames_),uint32_t(stage.acceptedFrames_>>32)};
  if(stage.config_.stationaryClutter){const auto count=persistenceStateWords(stage.cells_);
   const auto offset=persistenceHeaderWords+stage.active_*count;
   stage.clutterPersistence_->invalidate(offset*4,count*4);
   const auto* words=static_cast<const uint32_t*>(stage.clutterPersistence_->mapped)+offset;
   state.insert(state.end(),words,words+count);
  }
  return state;
 }
 static uint32_t rejection(const Stage& stage){return stage.ready_[0];}
};
}}}

namespace {
using namespace blah2;
constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
uint32_t* words(Buffer& buffer){return static_cast<uint32_t*>(buffer.mapped);}
void real(uint32_t* target,uint32_t at,double value){std::memcpy(target+at,&value,8);}
void clear(Buffer& buffer){std::memset(buffer.mapped,0,size_t(buffer.bytes));buffer.flush(0,buffer.bytes);}
void dispatch(Context& context,Kernel& kernel,uint32_t count){
 gpu_reference::CommandOwner command(context);command.allocate();
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 check(vkBeginCommandBuffer(command.command,&begin),"tracker test begin");
 barrier(command.command);kernel.append(command.command,{count,0,0,0,0});barrier(command.command);
 VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
 read.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vkCmdPipelineBarrier(command.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&read,0,nullptr,0,nullptr);
 check(vkEndCommandBuffer(command.command),"tracker test end");
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
 check(vkQueueSubmit(context.queue,1,&submit,context.fence),"tracker test submit");
 check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(10000000000)),"tracker test fence");
 check(vkResetFences(context.device,1,&context.fence),"tracker test reset fence");
}

void thresholdParity(Context& context,double threshold){
 constexpr uint32_t rows=3,columns=16,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,128*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 p[44]=96;p[74]=4;p[75]=0;p[77]=UINT32_MAX;p[78]=1;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,96+2*row,4.0*(int(row)-1));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=8;config.m=2;config.n=3;config.nDelete=3;
 config.adaptiveGate=true;config.cpi=.5;config.rangeRes=1;config.lambda=1;config.minInitiationSnrDb=threshold;
 config.normalizedInnovationGate=true;
 config.epoch=7;
 const auto allocation=gpu_tracker::validate(config,cells);
 require(allocation.stageBytes==uint64_t(gpu_tracker::metadataHeaderWords+
  gpu_tracker::workWords*config.capacity)*4,
  "Ordinary tracker reserved extra shadow workspace");
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};
 uint64_t frame=0;
 auto advance=[&](double snr,uint32_t row,uint32_t column,uint32_t births,uint32_t retained){
  auto* s=words(scratch);p[11]=1;s[layout.indices]=row*columns+column;
  real(s,layout.records,double(column));real(s,layout.records+2,4.0*(int(row)-1));
  real(s,layout.records+4,snr);real(s,layout.records+6,double(column));
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);++frame;
  require(stage.prepare({frame,frame,1000+500*frame}),"tracker prepare rejected threshold fixture");
  const auto& evidence=stage.evidencePrepared();
  require(evidence.size()==66,"public tracker evidence layout changed");
  require(evidence[30]==births&&evidence[54]==retained,"GPU birth admission differs from configured SNR oracle");
  require(stage.admissionRejections()[gpu_tracker::minimumSnr]==uint32_t(snr<threshold),
   "minimum-SNR rejection must be counted exactly once");
  stage.copyPrepared(output);stage.commit(result.token);
 };
 // Empty and populated states used to overwrite the SNR threshold differently.
 advance(threshold-.125,0,4,0,0);
 advance(threshold,0,4,1,1);
 advance(threshold-.125,2,10,0,1);
 // The unmatched seed reaches its third/final tentative frame here.
 advance(threshold+.125,2,10,1,1);
 stage.resetInput();
 advance(threshold-.125,0,4,0,0);
 advance(threshold,0,4,1,1);
 // Restore and a second reset must not replace immutable configuration either.
 stage.restore(config,checkpoint.data(),result.checkpointWords);
 stage.resetInput();advance(threshold-.125,0,4,0,0);
 advance(threshold,0,4,1,1);
 advance(threshold-.125,2,10,0,1);
 require(stage.prepare({frame,frame,1000+500*frame}),"tracker duplicate frame rejected");
 require(stage.evidencePrepared()[2]==1,"duplicate frame must be no-op");
 for(auto count:stage.admissionRejections())require(count==0,"no-op retained admission counters");
 stage.copyPrepared(output);stage.commit(result.token);
 p[32]=rows+1;post.flush(0,post.bytes);bool rejected=false;
 try{gpu_tracker::Stage invalid(context,config,cells,post,scratch,layout);}
 catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"host tracker admitted inconsistent map geometry");
 std::cout<<"PASS tracker threshold="<<threshold<<" empty_nonempty_reset_restore=1\n";
}

void admissionReasons(Context& context){
 constexpr uint32_t rows=33,columns=96,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,256*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 p[44]=128;p[74]=4;p[75]=0;p[77]=UINT32_MAX;p[78]=1;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,128+2*row,4.0*(int(row)-16));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=2;config.m=2;config.n=3;config.nDelete=3;
 config.adaptiveGate=true;config.maxAccInit=8;config.cpi=.5;config.rangeRes=1;config.lambda=1;config.minInitiationSnrDb=5;
 config.stationaryClutter=true;config.stationaryPersistenceCpis=3;
 config.epoch=9;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};
 struct Detection {uint32_t row,column;double snr;};
 using Reasons=std::array<uint32_t,gpu_tracker::rejectionWords>;
 uint64_t frame=0;unsigned cases=0;
 auto advance=[&](const std::vector<Detection>& detections,Reasons expected,uint32_t births,uint32_t associated=0){
  auto* s=words(scratch);p[11]=uint32_t(detections.size());
  for(uint32_t i=0;i<detections.size();++i){const auto& d=detections[i];
   s[layout.indices+i]=d.row*columns+d.column;
   real(s,layout.records+8*i,double(d.column));real(s,layout.records+8*i+2,4.0*(int(d.row)-16));
   real(s,layout.records+8*i+4,d.snr);real(s,layout.records+8*i+6,double(d.column));}
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);++frame;
  context.diagnostics.begin();require(stage.prepare({frame,frame,1000+500*frame}),"admission fixture rejected");
  std::ostringstream log;context.diagnostics.flush();context.diagnostics.drain(log);
  if(stage.admissionRejections()!=expected){std::cerr<<"admission_case="<<cases<<' '<<log.str();
   throw std::runtime_error("exclusive GPU admission reasons differ");}
  const auto& evidence=stage.evidencePrepared();
  require(evidence.size()==66&&evidence[30]==births&&evidence[27]==associated,
   "admission diagnostics changed public evidence or tracking decisions");
  std::map<std::string,uint32_t> fields;std::istringstream tokens(log.str());std::string token;
  while(tokens>>token){const auto at=token.find('=');if(at!=std::string::npos&&at+1<token.size()&&token[at+1]>='0'&&token[at+1]<='9')
   fields[token.substr(0,at)]=uint32_t(std::stoul(token.substr(at+1)));}
  const char* names[]={"min_snr_rejected","doppler_comb_rejected","range_comb_rejected",
   "persistent_occupancy_rejected","boundary_rejected","dropped_capacity_detections","lineage_overlap_rejected","stationary_capacity_rejected"};
  uint32_t rejected=0;
  for(uint32_t i=0;i<expected.size();++i){require(fields.at(names[i])==expected[i],"logged rejection differs from shader counter");rejected+=expected[i];}
  require(fields.at("admission_reason_version")==1&&fields.at("stationary_clutter_suppressed")==expected[gpu_tracker::persistentOccupancy],
   "legacy stationary log key still includes unrelated admission rejects");
  require(rejected+fields.at("admitted_detections")+associated==detections.size(),"log admission reasons do not reconcile");
  stage.copyPrepared(output);stage.commit(result.token);++cases;
 };
 advance({},Reasons{},0);
 advance({{16,10,4.9}},Reasons{1,0,0,0,0,0},0);
 stage.resetInput();advance({{16,95,4.9}},Reasons{1,0,0,0,0,0},0);
 stage.resetInput();advance({{16,10,6},{16,20,7},{16,30,8}},Reasons{0,2,0,0,0,0},1);
 stage.resetInput();advance({{4,10,6},{12,10,7},{20,10,8}},Reasons{0,0,2,0,0,0},1);
 stage.resetInput();advance({{4,10,6},{12,30,7},{20,50,8}},Reasons{0,0,0,0,0,1},2);
 // Overlapping causes use the unchanged predicate order, never double count.
 stage.resetInput();advance({{16,10,4},{16,20,7},{16,30,8}},Reasons{1,1,0,0,0,0},1);
 stage.resetInput();advance({{8,10,6},{8,20,8},{8,30,9},{16,10,10},{24,10,11}},Reasons{0,2,1,0,0,0},2);
 // Low-SNR inputs still paint persistence. On the third frame the high-SNR
 // candidate is rejected solely for occupancy, not the earlier SNR reason.
 stage.resetInput();advance({{16,10,4}},Reasons{1,0,0,0,0,0},0);
 advance({{16,10,4}},Reasons{1,0,0,0,0,0},0);
 advance({{16,10,6}},Reasons{0,0,0,1,0,0},0);
 stage.resetInput();advance({{16,10,6}},Reasons{},1);
 advance({{16,10,4}},Reasons{},0,1); // an existing association is not a rejected birth
 stage.resetInput();advance(std::vector<Detection>(129,{16,10,4}),Reasons{129,0,0,0,0,0},0);
 std::cout<<"PASS tracker exclusive_admission_reasons  cases="<<cases
  <<" production_log_reconciliation=1 repeated_predicate_passes=1 multi_tile=1\n";
}

void clutterParity(Context& context){
 constexpr uint32_t rows=601,columns=511,cells=rows*columns;
 Buffer metadata(context,512,host,preferred),bankA(context,512,host,preferred),bankB(context,512,host,preferred),
  scratch(context,4096,host,preferred),post(context,8192,host,preferred),clutter(context,gpu_tracker::persistenceBytes(true,cells),host,preferred);
 for(Buffer* b:{&metadata,&bankA,&bankB,&scratch,&post,&clutter})clear(*b);
 auto sources=gpu_tracker::shaderSources();
 const auto commonEnd=sources[0].second.rfind(gpu_tracker::initializeBody);
 require(commonEnd!=std::string::npos,"tracker shader prefix unavailable");
 const std::string probeSource=sources[0].second.substr(0,commonEnd)+R"glsl(
void main(){uint i=indexOf();if(i>=p.count)return;s[128u+i]=clutterRidgeStreak(s[i]);}
)glsl";
 const std::vector<Buffer*> buffers{&metadata,&bankA,&bankB,&scratch,&post,&clutter};
 Kernel probe(context,probeSource.c_str(),buffers),paint(context,sources[13].second.c_str(),buffers);
 auto* m=words(metadata);auto* p=words(post);auto* s=words(scratch);auto* c=words(clutter);
 m[1]=1;m[2]=2;m[4]=cells;m[5]=64;m[10]=20;m[19]=1;m[gpu_tracker::stationaryOrdinalWord]=20;
 p[32]=rows;p[33]=columns;p[41]=0;p[44]=96;p[45]=96+2*rows;
 for(uint32_t row=0;row<rows;++row)real(p,96+2*row,double(row));
 for(uint32_t column=0;column<columns;++column)p[p[45]+column]=column;
 c[0]=1;c[1]=12;c[11]=65536;
 auto flush=[&]{for(Buffer* b:{&metadata,&scratch,&post,&clutter})b->flush(0,b->bytes);};
 auto query=[&](uint32_t cell){s[0]=cell;flush();dispatch(context,probe,1);
  metadata.invalidate(0,metadata.bytes);scratch.invalidate(0,scratch.bytes);return s[128];};
 unsigned cases=0;
 // Compare the actual shader against the independent CPU coordinate oracle,
 // including all corners, both axes and neighborhoods touching row boundaries.
 for(const auto& center:std::vector<std::pair<uint32_t,uint32_t>>{{0,0},{0,510},{600,0},{600,510},{300,255},{1,510}}){
  clear(clutter);c[0]=1;c[1]=12;c[11]=65536;const uint32_t learned=center.first*columns+center.second;
  c[13+3*learned]=12u;c[14+3*learned]=20u;c[12]=1;
  for(int dr=-2;dr<=2;++dr)for(int dc=-5;dc<=5;++dc){
   const int r=int(center.first)+dr,d=int(center.second)+dc;
   if(r<0||d<0||r>=int(rows)||d>=int(columns))continue;
   const uint32_t candidate=uint32_t(r)*columns+uint32_t(d);
   const uint32_t expected=gpu_tracker::persistentRidgeContains(learned,candidate,columns,rows)?12:0;
   require(query(candidate)==expected&&m[0]==0,"non-square clutter lookup differs from coordinate oracle");++cases;
  }
 }
 auto paintAt=[&](uint64_t frame,uint32_t row,uint32_t column){
  m[0]=0;m[10]=uint32_t(frame);m[11]=uint32_t(frame>>32);s[0]=row*columns+column;real(s,64,double(column));real(s,66,double(row));flush();
  m[gpu_tracker::stationaryOrdinalWord]=m[10];m[gpu_tracker::stationaryOrdinalWord+1]=m[11];metadata.flush(0,metadata.bytes);
  dispatch(context,paint,1);metadata.invalidate(0,metadata.bytes);clutter.invalidate(0,clutter.bytes);
  require(m[0]==0,"valid persistence painting rejected");
 };
 clear(clutter);c[0]=1;c[1]=12;c[11]=65536;
 for(uint32_t frame=1;frame<=12;++frame)paintAt(frame,300,250);
 require(query(300*columns+250)==12,"stationary nuisance did not accumulate persistence");
 require(query(300*columns+259)==0,"independent moving candidate inherited distant nuisance persistence");
 require(query(303*columns+250)==0,"persistence painted an unbounded Doppler column");
 clear(clutter);c[0]=1;c[1]=12;c[11]=65536;
 for(uint32_t frame=1;frame<=24;++frame){paintAt(frame,300,100+2*frame);
  require(query(300*columns+100+2*frame)<12,"moving candidate was learned as stationary nuisance");}
 // T04: controlled full-width metadata exercises the production shader without
 // replaying 65536 CPIs. Restoring metadata must not revive an expired cell.
 const uint32_t center=300*columns+250,at=13+3*center;
 for(uint64_t generation:{301ull,65595ull,65596ull,65597ull,131132ull,4294967356ull}){
  clear(clutter);c[0]=1;c[1]=60;c[12]=1;c[11]=65536;c[at]=60;c[at+1]=60;
  m[10]=uint32_t(generation);m[11]=uint32_t(generation>>32);
  m[gpu_tracker::stationaryOrdinalWord]=m[10];m[gpu_tracker::stationaryOrdinalWord+1]=m[11];
  require(query(center)==tracker_policy::persistentEvidence(60,60,generation),"T04 full-width stale lookup differs");
  paintAt(generation,300,250);require(query(center)==1,"T04 stale reappearance inherited evidence");
  paintAt(generation,300,250);require(query(center)==1,"T04 duplicate strengthened evidence");
 }
 for(uint64_t generation:{65536ull,131072ull,4294967296ull})for(uint32_t streak:{60u,65535u}){
  clear(clutter);c[0]=1;c[1]=60;c[12]=1;c[11]=65536;c[at]=streak;
  c[at+1]=uint32_t(generation-1);c[at+2]=uint32_t((generation-1)>>32);
  paintAt(generation,300,250);require(query(center)==std::min(65535u,streak+1),"T04 recurring rollover lost evidence");
 }
 // Reject inconsistent dimensions and out-of-map cells on both read/write paths.
 for(uint32_t badRows:{0u,600u}){
  p[32]=badRows;m[0]=0;query(0);require(m[0]==320,"invalid lookup dimensions were accepted");
  m[0]=0;flush();dispatch(context,paint,1);metadata.invalidate(0,metadata.bytes);
  require(m[0]==320,"invalid painting dimensions were accepted");
 }
 p[32]=rows;
 for(uint32_t badColumns:{0u,510u}){
  p[33]=badColumns;m[0]=0;query(0);require(m[0]==320,"invalid lookup columns were accepted");
  m[0]=0;flush();dispatch(context,paint,1);metadata.invalidate(0,metadata.bytes);
  require(m[0]==320,"invalid painting columns were accepted");
 }
 p[33]=columns;
 p[32]=rows;m[0]=0;query(cells);require(m[0]==320,"out-of-map lookup accepted");
 m[0]=0;flush();dispatch(context,paint,1);metadata.invalidate(0,metadata.bytes);
 require(m[0]==320,"out-of-map painting accepted");
 std::cout<<"PASS T04 tracker clutter rows=601 columns=511 oracle_cases="<<cases
  <<" edges=1 stationary=1 moving=1 invalid_geometry=1 rollover_u64=1 duplicate=1 maximum=1\n";
}

void confirmationSemantics(Context& context,uint32_t minimum=4,uint32_t window=6,bool adaptive=true){
 constexpr uint32_t rows=301,columns=96,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,1024*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 p[44]=128;p[74]=4;p[75]=0;p[77]=UINT32_MAX;p[78]=1;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,128+2*row,2.0*(int(row)-150));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=8;config.m=minimum;config.n=window;config.nDelete=3;
 config.adaptiveGate=adaptive;config.maxAccInit=adaptive?10:0;config.cpi=.5;
 config.rangeRes=299792458.0/2400000.0;config.lambda=299792458.0/509000000.0;
 config.minInitiationSnrDb=5;
 config.epoch=19;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};
 uint64_t frame=0;
 auto advance=[&](bool hit,double delay=50,double doppler=0,double snr=8,uint64_t stamp=0){
  auto* s=words(scratch);p[11]=hit?1:0;
  if(hit){const uint32_t row=uint32_t(int32_t(std::round(doppler/2))+150);
   s[layout.indices]=row*columns+uint32_t(std::round(delay));
   real(s,layout.records,delay);real(s,layout.records+2,doppler);
   real(s,layout.records+4,snr);real(s,layout.records+6,delay);}
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);++frame;
  require(stage.prepare({frame,frame,stamp?stamp:1000+500*frame}),"confirmation fixture prepare rejected");
  stage.copyPrepared(output);stage.commit(result.token);
  require(checkpoint[0]==5&&checkpoint[27]==0,"measured timing history requires ordinary schema5");
  return checkpoint[20];
 };
 require(advance(true)==uint32_t(minimum==1),"seed is measured and confirms M=1 immediately");
 require((checkpoint[32+22]&4u)!=0,"seed missing measured-hit flag");
 if(minimum==1){require(advance(false)==0&&checkpoint[22]==1,"confirmed miss must coast");
  require((checkpoint[32+16+8+6]&4u)==0,"coast cannot become a measured hit");
  std::cout<<"PASS tracker confirmation M=1 N="<<window<<" seed=1"<<'\n';return;}
 require(advance(true)==0&&advance(true)==0&&advance(true)==1,
  "four consecutive measured hits must confirm at fourth, not sixth CPI");
 stage.restore(config,checkpoint.data(),result.checkpointWords);
 require(advance(false)==0&&checkpoint[22]==1,"restored confirmed track failed to coast");
 require(advance(false)==0&&advance(false)==0&&checkpoint[4]==1,
  "delete3 must retain the first three misses");
 require(advance(false)==0&&checkpoint[4]==0,"delete3 must remove fourth miss");
 stage.resetInput();
 for(bool hit:{true,false,true,false,true})require(advance(hit)==0,"three hits falsely confirmed");
 require(advance(true)==1,"four nonconsecutive hits within six CPIs failed confirmation");
 stage.resetInput();
 for(bool hit:{true,false,false,true,false})require(advance(hit)==0,"three sparse hits falsely confirmed");
 const uint32_t priorId=checkpoint[32+12]&65535u;
 require(advance(true)==0&&checkpoint[4]==0,"unconfirmed hypothesis survived final frame N");
 require(advance(true)==0&&checkpoint[4]==1&&(checkpoint[32+12]&65535u)!=priorId,
  "expired tentative hypothesis claimed a seventh-frame hit");
 stage.resetInput();
 for(unsigned i=0;i<10;++i)require(advance(true,50,0,4.99)==0&&checkpoint[4]==0,
  "subthreshold detections created or confirmed a track");
 stage.resetInput();
 for(unsigned i=0;i<3;++i)require(advance(true)==0,"pre-outlier three-hit track confirmed");
 require(advance(true,50,10)==0,"out-of-gate fourth Doppler was counted as a hit");
 stage.resetInput();
 for(unsigned i=0;i<3;++i)require(advance(true)==0,"pre-reset three-hit track confirmed");
 stage.resetInput();require(advance(true)==0,"reset retained prior-generation hit votes");
 // Exact observed RPA5731 second-burst centroid/SNR values, not truth inputs.
 stage.resetInput();
 const double km[]{6.38,6.49,6.56,6.62},hz[]{-242.29,-243.98,-244.16,-245.88},snr[]{7.92,8.31,8.33,7.49};
 for(unsigned i=0;i<4;++i)require(advance(true,1000*km[i]/config.rangeRes,hz[i],snr[i])==uint32_t(i==3),
  "RPA5731 observed four-hit burst failed production GPU association/confirmation");
 // Duplicate timestamps must neither create a hit nor age the N-frame budget.
 require(stage.prepare({frame,frame,1000+500*frame}),"duplicate confirmation frame rejected");
 require(stage.evidencePrepared()[2]==1,"duplicate confirmation frame was not no-op");
 stage.copyPrepared(output);stage.commit(result.token);
 require(checkpoint[20]==1,"duplicate frame changed confirmation");
 // Optional recorder metadata can disappear and return with a new source
 // epoch. Every accepted IQ frame must still update the actual device tracker.
 stage.resetInput();tracker_clock::SignalProgressClock progress;
 for(uint64_t sampleFrame=1;sampleFrame<=9;++sampleFrame){
  const bool positioned=sampleFrame<3||sampleFrame>5;
  const auto epoch=sampleFrame<8?41u:42u;
  const uint64_t end=sampleFrame<8?9600000000ULL+sampleFrame*1200000:
    (sampleFrame-7)*1200000;
  const auto stamp=progress.advance(1200000,2400000,positioned,epoch,end);
  require(stamp==sampleFrame*500,"tracker metadata gap changed progression");
  require(advance(true,50,0,8,stamp)==uint32_t(sampleFrame>=minimum),
    "metadata gap changed measured confirmation");
  require(!result.noOp&&stage.evidencePrepared()[27]==uint32_t(sampleFrame>1),
    "metadata gap froze production GPU association");
 }
 const auto duplicateStamp=progress.advance(1200000,2400000,true,42,2400000);
 require(duplicateStamp==4500&&stage.prepare({frame,frame,duplicateStamp}),
   "metadata recovery duplicate contract changed");
 require(stage.evidencePrepared()[2]==1,"exact duplicate became fresh tracker evidence");
 stage.copyPrepared(output);stage.commit(result.token);
 auto oldGuided=checkpoint;const auto packed=oldGuided[32+13];
 oldGuided[32+16+8*((uint32_t(packed&255)+((packed>>8)&255)-1)%oldGuided[3])+6]=2u|8u|(2u<<4u);
 bool rejectedGuided=false;
 try{stage.restore(config,oldGuided.data(),result.checkpointWords);}catch(const std::invalid_argument&){rejectedGuided=true;}
 require(rejectedGuided,"pre-lineage schema3 guided history restored into ordinary policy");
 auto overlappingLegacy=checkpoint;
 const auto newestAt=32+16+8*((uint32_t(packed&255)+((packed>>8)&255)-1)%overlappingLegacy[3])+6;
 overlappingLegacy[newestAt]=(overlappingLegacy[newestAt]&15u)|(8u<<4u);
 bool rejectedTemporal=false;
 try{stage.restore(config,overlappingLegacy.data(),result.checkpointWords);}catch(const std::invalid_argument&){rejectedTemporal=true;}
 require(rejectedTemporal,"overlapping temporal schema3 restored into single-CPI policy");
 auto legacy=checkpoint;legacy[0]=1;bool rejected=false;
 try{stage.restore(config,legacy.data(),result.checkpointWords);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"legacy checkpoint cannot be silently restored as measured-hit history");
 std::cout<<"PASS tracker confirmation M=4 N=6  adaptive="<<adaptive
  <<" seed=1 immediate=1 sparse_negative=1 lifetime=1 deletion=1 restore=1 RPA5731=1 duplicate=1\n";
}

// The same causal detections drive the actual portable tracker and compiled
// The generated production owner is executed as GLSL, including nonzero
// acceleration fits and three-peak spatial association. Exact decision and
// FP64-distance words are compared with the C++ owner, not a shape surrogate.
void associationContractParity(Context& context){
 struct Case {double delay,doppler,cpi,elapsed,span,q,nis;bool adaptive,normalized;uint32_t left,right,beams,factor,tolerance;};
 std::vector<Case> cases;
 for(bool adaptive:{false,true})for(bool normalized:{false,true})
  for(int d=-16;d<=16;++d)for(int v=-12;v<=12;++v)
   cases.push_back({double(d)/8,double(v)/2,.5,.5,8,.125,9.210340371976184,
    adaptive,normalized,1,12,12,4,1});
 for(uint32_t left:{1u,12u,0x80000000u|1u|(17u<<7u)|(33u<<14u)})
  for(uint32_t right=0;right<=12;++right)for(uint32_t tolerance:{0u,1u,2u,UINT32_MAX})
   cases.push_back({0,0,.5,.5,0,.125,9.210340371976184,true,true,left,right,12,4,tolerance});
 cases.push_back({1,0,.5,.5,0,.125,9.210340371976184,true,false,0,0,0,1,UINT32_MAX});
 cases.push_back({2,0,1,1,0,0,4,false,true,0,0,0,1,UINT32_MAX});
 cases.push_back({std::nextafter(2.,3.),0,1,1,0,0,4,false,true,0,0,0,1,UINT32_MAX});
 Buffer metadata(context,512,host,preferred),bankA(context,512,host,preferred),bankB(context,512,host,preferred),
  scratch(context,uint64_t(cases.size())*64*4,host,preferred),post(context,512,host,preferred),clutter(context,512,host,preferred);
 for(Buffer* b:{&metadata,&bankA,&bankB,&scratch,&post,&clutter})clear(*b);
 const auto sources=gpu_tracker::shaderSources();
 const auto end=sources[0].second.rfind(gpu_tracker::initializeBody);
 require(end!=std::string::npos,"association shader prefix absent");
 const std::string shader=sources[0].second.substr(0,end)+R"glsl(
void main(){uint i=indexOf();if(i>=p.count)return;uint at=64u*i;
 AssociationDecision value=associationDecision(sd(at),sd(at+2u),0.0LF,0.0LF,
  sd(at+4u),s[at+14u]!=0u,sd(at+6u),sd(at+8u),sd(at+10u),s[at+15u]!=0u,sd(at+12u));
 s[at+32u]=uint(value.valid);s[at+33u]=uint(value.accepted);s[at+34u]=value.rejection;
 uvec2 distance=unpackDouble2x32(value.distance);s[at+35u]=distance.x;s[at+36u]=distance.y;
 s[at+37u]=uint(spatialCompatible(s[at+16u],s[at+17u],s[at+18u],s[at+19u],s[at+20u]));}
)glsl";
 Kernel probe(context,shader.c_str(),{&metadata,&bankA,&bankB,&scratch,&post,&clutter});
 auto* data=words(scratch);
 for(size_t i=0;i<cases.size();++i){const auto& c=cases[i];auto* at=data+64*i;
  real(at,0,c.delay);real(at,2,c.doppler);real(at,4,c.cpi);real(at,6,c.elapsed);
  real(at,8,c.span);real(at,10,c.q);real(at,12,c.nis);at[14]=c.adaptive;at[15]=c.normalized;
  at[16]=c.left;at[17]=c.right;at[18]=c.beams;at[19]=c.factor;at[20]=c.tolerance;}
 scratch.flush(0,scratch.bytes);dispatch(context,probe,uint32_t(cases.size()));scratch.invalidate(0,scratch.bytes);
 for(size_t i=0;i<cases.size();++i){const auto& c=cases[i];const auto* actual=data+64*i+32;
  const auto expected=tracker_association::associationDecision(c.delay,c.doppler,0,0,c.cpi,
   c.adaptive,c.elapsed,c.span,c.q,c.normalized,c.nis);
  require(actual[0]==expected.valid&&actual[1]==expected.accepted&&actual[2]==expected.rejection,
   "association contract decision bytes differ");
  require(std::memcmp(actual+3,&expected.distance,8)==0,"association fitted distance bytes differ");
  require(actual[5]==tracker_association::spatialCompatible(c.left,c.right,c.beams,c.factor,c.tolerance),
   "multi-peak spatial decision bytes differ");}
 std::cout<<"PASS association shared_cpp_glsl byte_parity="<<cases.size()<<" acceleration=1 multipeak=1 boundaries=1\n";
}

// resident shader stages. This is backend parity, not an RF false-alarm claim.
void temporalBackendParity(Context& context,uint32_t depth,bool spatialMotion=false){
 constexpr uint32_t rows=3,columns=16,cells=rows*columns,cell=columns+6;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,128*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[34]=cells;p[39]=layout.flags;
 p[41]=layout.indices;p[44]=96;p[74]=depth;p[77]=UINT32_MAX;p[78]=depth>2;p[86]=layout.direction;
 if(spatialMotion){p[75]=12;p[77]=0;p[81]=4;p[83]=48;}
 for(uint32_t row=0;row<rows;++row)real(p,96+2*row,2.0*(int(row)-1));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=4;config.m=4;
 config.n=tracker_confirmation::confirmationMinimumWindow(config.m,depth);config.nDelete=std::max(3u,depth-1);
 config.adaptiveGate=true;config.cpi=.5;config.rangeRes=100;config.lambda=2;
 config.maxAccInit=spatialMotion?8:0;config.normalizedInnovationGate=true;
 config.minInitiationSnrDb=5;config.epoch=23;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout,depth);
 ::Tracker portable(config.m,config.n,config.nDelete,config.cpi,config.maxAccInit,config.rangeRes,config.lambda,5,true);
 if(spatialMotion)portable.configureSpatialAssociation(12,4,0);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};
 uint64_t frame=0,stamp=1000,seedStamp=0;std::unique_ptr<::Track> tracks;
 auto advance=[&](bool hit,uint32_t support,double snr=8.0,bool duplicate=false){
  if(!duplicate){++frame;stamp+=spatialMotion?(frame%2?200:800):500;}
  auto* s=words(scratch);p[11]=hit;p[79]=depth>2?uint32_t(frame):0;
  ::Detection detected(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  if(hit){s[layout.indices]=cell;s[layout.flags+cell]=1u|(support==8?64u:support==4?32u:support==2?8u:0u);
   if(!seedStamp)seedStamp=stamp;
   const double t=spatialMotion?double(stamp-seedStamp)/1000:0.,dv=4*t;
   const double delay=6-config.lambda*.5*4*t*t/config.rangeRes;
   const uint32_t spatial=spatialMotion?(frame==depth?(0x80000000u|1u|(17u<<7u)):5u):0u;
   s[layout.flags+cell]|=spatialMotion?5u<<8u:0u;
   s[layout.direction+cell]=(spatial&0x80000000u)?spatial:0u;
   real(s,layout.records,delay);real(s,layout.records+2,dv);real(s,layout.records+4,snr);real(s,layout.records+6,delay);
   detected=::Detection(delay,dv,snr);if(spatialMotion)detected.set_spatial({spatial});::Detection::Evidence evidence;
   evidence.epoch=config.epoch;evidence.newestSequence=frame;evidence.depth=uint8_t(support);
   for(uint32_t i=0;i<support;++i)evidence.sequences[i]=frame-i;
   detected.set_evidence({evidence});}
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);
  require(stage.prepare({frame,frame,stamp}),"temporal backend prepare rejected");
  stage.copyPrepared(output);stage.commit(result.token);
  tracks=portable.process(&detected,stamp);
  tracker_qualification::compareCheckpoint(portable,checkpoint.data(),result.checkpointWords);
  const auto residentJson=gpu_result_json::trackerCheckpoint(stamp,{checkpoint.data(),size_t(result.checkpointWords)},
    {config.capacity,config.epoch,frame,frame,spatialMotion?12u:0u,spatialMotion?48u:0u});
  require(residentJson==tracks->to_json(stamp),"temporal public JSON byte parity differs");
  require(checkpoint[4]==tracks->get_n(),"temporal backend track count differs");
  const std::array<std::string,4> names{"TENTATIVE","ASSOCIATED","ACTIVE","COASTING"};
  for(uint32_t i=0;i<checkpoint[4];++i){const uint32_t r=32+i*(checkpoint[1]/4);
   require(names.at(checkpoint[r+14])==tracks->get_state(i),"temporal backend state differs");
   require(checkpoint[r+10]==tracks->get_nInactive(i),"temporal backend coast age differs");
   uint32_t measured=0;const uint32_t packed=checkpoint[r+13],head=packed&255u,length=(packed>>8)&255u;
   if(spatialMotion){const uint32_t latest=r+16+8*((head+length-1)%checkpoint[3]);
    require(checkpoint[latest+7]==portable.spatialForTrack(tracks->get_id(i)),"spatial history bytes differ");}
   for(uint32_t j=0;j<length;++j)measured+=(checkpoint[r+16+8*((head+j)%checkpoint[3])+6]&4u)!=0;
   require(measured==tracks->get_nAssociated(i),"overlap changed measured history");}
  require(result.noOp==portable.frame_diagnostics().noOp,"duplicate disposition differs");
 };
 auto reset=[&]{stage.resetInput();portable.reset();frame=0;stamp=1000;seedStamp=0;for(uint32_t i=1;i<depth;++i)advance(false,depth);};
 reset();
 for(uint32_t i=0;i<config.n;++i){advance(true,depth);
  require(tracks->get_n()==1,"independent sequence fragmented");
  require((tracks->get_state(0)=="ACTIVE")== (i==config.n-1),"early or missing independent confirmation");
  if(i==0)advance(true,depth,8,true);}
 for(uint32_t i=1;i<=config.nDelete;++i){advance(false,depth);require(tracks->get_n()==1&&tracks->get_state(0)=="COASTING","coast horizon shortened");}
 advance(false,depth);require(tracks->get_n()==0,"coast exceeded deletion horizon");
 // A single temporal impulse can never supply the four independent votes.
 reset();advance(true,depth);
 for(uint32_t i=1;i<config.n;++i){advance(i<depth,depth);require(checkpoint[20]==0,"one source impulse manufactured confirmation");}
 require(tracks->get_n()==0,"unconfirmed impulse escaped its bounded lifetime");
 reset();for(uint32_t i=0;i<config.n+depth;++i){advance(true,depth,4.99);require(tracks->get_n()==0,"subthreshold temporal evidence created a false track");}
 if(depth>1){auto impossible=config;impossible.n=tracker_confirmation::confirmationMinimumWindow(4,depth)-1;
  bool refused=false;try{gpu_tracker::validate(impossible,cells,depth);}catch(const std::invalid_argument&){refused=true;}
  require(refused,"resident accepted infeasible temporal initiation");
  ::Tracker invalid(4,impossible.n,config.nDelete,.5,0,100,2);
  ::Detection d(6,0,8);::Detection::Evidence e;e.newestSequence=depth;e.depth=uint8_t(depth);
  for(uint32_t i=0;i<depth;++i){e.sequences[i]=depth-i;}
  d.set_evidence({e});
  refused=false;try{invalid.process(&d,1000);}catch(const std::invalid_argument&){refused=true;}
  require(refused,"portable accepted infeasible temporal initiation");}
 std::cout<<"PASS tracker cross_backend depth="<<depth<<" spatial_motion="<<spatialMotion<<" initiation=1 overlap=1 duplicate=1 coast=1 impulse=1 threshold=1 feasibility=1\n";
}

void stationaryBackendParity(Context& context,double maximum=0,double sign=0,double outside=2.01){
 constexpr uint32_t rows=65,columns=64,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,256*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[34]=cells;p[39]=layout.flags;
 p[41]=layout.indices;p[44]=96;p[74]=1;p[77]=UINT32_MAX;p[86]=layout.direction;
 for(uint32_t r=0;r<rows;++r)real(p,96+2*r,2.0*(int(r)-32));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=4;config.m=2;config.n=6;config.nDelete=4;
 config.adaptiveGate=true;config.cpi=.5;config.rangeRes=100;config.lambda=2;
 config.maxAccInit=8;config.normalizedInnovationGate=true;config.minInitiationSnrDb=5;config.epoch=23;
 config.stationaryClutter=true;config.stationaryPersistenceCpis=3;
 config.stationaryMaxDopplerHz=maximum;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout);
 TrackerRecoveryConfig recovery;recovery.stationaryClutter=true;recovery.persistenceCpis=3;
 recovery.maximumDopplerHz=maximum;
 ::Tracker portable(2,6,4,.5,8,100,2,5,true,{},true,9.210340371976184,{},recovery);
 std::deque<int> delayAxis;std::deque<double> dopplerAxis;
 for(uint32_t column=0;column<columns;++column)delayAxis.push_back(int(column));
 for(uint32_t row=0;row<rows;++row)dopplerAxis.push_back(2.0*(int(row)-32));
 portable.configureGrid(delayAxis,dopplerAxis);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());TrackerResult result;
 TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};
 uint64_t frame=0,stamp=1000;std::unique_ptr<::Track> tracks;
 auto advance=[&](bool hit,double delay=20,double frequency=std::numeric_limits<double>::quiet_NaN()){
  if(std::isnan(frequency))frequency=sign*2;
  ++frame;stamp+=frame%2?200:800;
  p[11]=hit;auto* s=words(scratch);::Detection detected(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  if(hit){const auto cell=uint32_t(std::ceil(frequency/2-.5)+32)*columns+uint32_t(std::ceil(delay-.5));s[layout.indices]=cell;s[layout.flags+cell]=1;
   real(s,layout.records,delay);real(s,layout.records+2,frequency);real(s,layout.records+4,8);real(s,layout.records+6,delay);
   detected=::Detection(delay,frequency,8);}
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);
  require(stage.prepare({frame,frame,stamp}),"stationary parity prepare rejected");
  stage.copyPrepared(output);stage.commit(result.token);tracks=portable.process(&detected,stamp);
  tracker_qualification::compareCheckpoint(portable,checkpoint.data(),result.checkpointWords);
  require(gpu_result_json::trackerCheckpoint(stamp,{checkpoint.data(),size_t(result.checkpointWords)},
    {config.capacity,config.epoch,frame,frame})==tracks->to_json(stamp),"stationary publication byte parity differs");
 };
 advance(true);advance(true);require(tracks->get_state(0)=="ACTIVE","stationary fixture never confirmed");
 advance(true);require(tracks->get_state(0)=="ACTIVE","association used current rather than prior occupancy");
 stage.restore(config,checkpoint.data(),result.checkpointWords);
 for(unsigned i=0;i<8;++i){advance(true);require(tracks->get_n()==1&&tracks->get_state(0)=="TENTATIVE",
   "stationary quarantine did not hold measured confirmed track");}
 if(sign!=0){advance(true,20,sign*outside);
  require(tracks->get_n()==1&&tracks->get_state(0)==(maximum==0?"TENTATIVE":"ACTIVE"),"T05 finite Doppler cutoff differs");
  stage.resetInput();portable.reset();frame=0;stamp=1000;advance(true);
  require(tracks->get_n()==1&&tracks->get_nAssociated(0)==1,"T05 reset retained occupancy");
  std::cout<<std::setprecision(17)<<"PASS T05 stationary maximum="<<maximum<<" sign="<<sign<<" outside="<<outside<<" checkpoint=1 reset=1 restore=1\n";return;}
 for(unsigned i=1;i<=4;++i)advance(true,20+.6*i);
 require(tracks->get_n()==1&&tracks->get_state(0)=="ACTIVE","moving target did not exit stationary quarantine");
 advance(false);require(tracks->get_state(0)=="COASTING","stationary exit did not preserve confirmed coast");
 stage.resetInput();portable.reset();frame=0;stamp=1000;
 advance(true);require(tracks->get_n()==1&&tracks->get_nAssociated(0)==1,"reset retained elapsed/quarantine state");
 std::cout<<"PASS tracker stationary cross_backend enter=1 hold=1 exit=1 measured=1 irregular_coast=1 reset=1 json_bytes=1\n";
}

// T01--T03: detector records (including signed map edges) enter both actual
// tracker implementations unchanged. Compare full checkpoints, IDs, votes,
// associations and exclusive rejection counters across every supported depth.
void admissionBackendParity(Context& context,uint32_t depth,bool adaptive,uint32_t minimum){
 constexpr uint32_t rows=65,columns=64,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,512*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[34]=cells;p[39]=layout.flags;
 p[41]=layout.indices;p[44]=96;p[45]=226;p[74]=depth;p[77]=UINT32_MAX;p[78]=depth>2;p[86]=layout.direction;
 std::deque<int> delays;std::deque<double> dopplers;
 for(uint32_t row=0;row<rows;++row){dopplers.push_back(2.*(int(row)-32));real(p,96+2*row,dopplers.back());}
 for(uint32_t col=0;col<columns;++col){delays.push_back(int(col)-16);p[226+col]=uint32_t(delays.back());}
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=64;config.m=minimum;
 config.n=std::max(3u,tracker_confirmation::confirmationMinimumWindow(minimum,depth));
 config.nDelete=std::max(3u,depth-1);config.adaptiveGate=adaptive;
 config.cpi=.5;config.maxAccInit=4;config.rangeRes=1e12;config.lambda=1;config.minInitiationSnrDb=5;config.epoch=19;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout,depth);
 ::Tracker portable(config.m,config.n,config.nDelete,.5,config.maxAccInit,1e12,1,5,adaptive,{},false);
 portable.configureGrid(delays,dopplers);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());TrackerResult result;
 TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};uint64_t frame=0;
 using Point=std::array<double,3>;
 const auto advance=[&](const std::vector<Point>& points){++frame;p[11]=uint32_t(points.size());p[79]=uint32_t(frame+depth);
  auto* s=words(scratch);std::vector<double> ds,vs,snrs;std::vector<::Detection::Evidence> evidence;
  for(uint32_t i=0;i<points.size();++i){const auto& x=points[i];ds.push_back(x[0]);vs.push_back(x[1]);snrs.push_back(x[2]);
   const uint32_t row=uint32_t(std::clamp(std::ceil(x[1]/2-.5)+32,0.,64.));
   const uint32_t col=uint32_t(std::clamp(std::ceil(x[0]-.5)+16,0.,63.)),cell=row*columns+col;
   s[layout.indices+i]=cell;s[layout.flags+cell]=1u|(depth==8?64u:depth==4?32u:depth==2?8u:0u);
   real(s,layout.records+8*i,x[0]);real(s,layout.records+8*i+2,x[1]);real(s,layout.records+8*i+4,x[2]);
   if(depth>1){::Detection::Evidence e;e.epoch=config.epoch;e.depth=uint8_t(depth);e.newestSequence=frame+depth;
    for(uint32_t k=0;k<depth;++k)e.sequences[k]=e.newestSequence-k;
    evidence.push_back(e);}}
  ::Detection detection(ds,vs,snrs);detection.set_evidence(evidence);
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);
  require(stage.prepare({frame,frame+depth,1000+500*frame}),"T01-T03 resident admission rejected");
  stage.copyPrepared(output);stage.commit(result.token);portable.process(&detection,1000+500*frame);
  tracker_qualification::compareCheckpoint(portable,checkpoint.data(),result.checkpointWords);
  const auto& d=portable.frame_diagnostics();const auto& r=stage.admissionRejections();
  require(d.birthDopplerCombRejected==r[gpu_tracker::dopplerComb]&&d.birthRangeCombRejected==r[gpu_tracker::rangeComb]&&
   d.associations==stage.evidencePrepared()[27]&&d.initiated==stage.evidencePrepared()[30],"T01-T03 counters differ");
 };
 const auto reset=[&]{stage.resetInput();portable.reset();frame=0;};
 for(auto points:std::vector<std::vector<Point>>{
   {{-8,0,8},{8,2,12},{24,-2,10}},{{8,-16,8},{8,0,12},{8,16,10}},
   {{-8,0,10},{8,2,10},{24,-2,10}},{{-8,-20,10},{8,0,10},{24,20,10}}}) {
  for(unsigned permutation=0;permutation<3;++permutation){reset();advance(points);std::rotate(points.begin(),points.begin()+1,points.end());}
 }
 reset();advance({{-8,0,10},{8,2,10}});advance({{-8,0,10},{8,2,10},{24,0,10}});
 reset();for(uint32_t i=0;i<config.n;++i)advance({{8,-8,10},{8,8,10}});
 reset();for(uint32_t i=1;i<config.n;++i)advance({{8,-8,10},{8,8,10}});
 advance({{8,-8,10}});for(uint32_t i=0;i<=config.nDelete;++i)advance({});
 for(double delay:{-16.,8.,47.})for(double doppler:{-64.,0.,64.}){reset();advance({{delay,doppler,10}});advance({{delay,doppler,10}});}
 std::cout<<"PASS T01_T02_T03 tracker admission depth="<<depth<<" adaptive="<<adaptive<<" M="<<minimum<<" max_acceleration="<<config.maxAccInit<<" checkpoint=1\n";
}

// T06 uses a map larger than 65536 cells. Capacity limits learned occupancy,
// never map admission; the same ordered painting/decay must admit the same
// independent target after space becomes available.
void stationaryCapacityBackendParity(Context& context,uint32_t capacity){
 constexpr uint32_t rows=257,columns=257,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,2048*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[34]=cells;p[39]=layout.flags;
 p[41]=layout.indices;p[44]=96;p[45]=610;p[74]=1;p[77]=UINT32_MAX;p[86]=layout.direction;
 std::deque<int> delays;std::deque<double> dopplers;
 for(uint32_t row=0;row<rows;++row){dopplers.push_back(2.*(int(row)-128));real(p,96+2*row,dopplers.back());}
 for(uint32_t col=0;col<columns;++col){delays.push_back(int(col));p[610+col]=col;}
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=8;config.m=1;config.n=4;config.nDelete=4;
 config.adaptiveGate=true;config.cpi=.5;config.rangeRes=1e12;config.lambda=1;config.minInitiationSnrDb=5;
 config.stationaryClutter=true;config.stationaryPersistenceCpis=3;config.maximumStationaryCells=capacity;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout);
 TrackerRecoveryConfig recovery{true,3,0,capacity};
 ::Tracker portable(1,4,4,.5,0,1e12,1,5,true,{},false,9.210340371976184,{},recovery);
 portable.configureGrid(delays,dopplers);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());TrackerResult result;
 TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};uint64_t frame=0;
 using Point=std::array<double,3>;
 const auto advance=[&](const std::vector<Point>& points){++frame;p[11]=uint32_t(points.size());auto* s=words(scratch);
  std::vector<double> ds,vs,snrs;
  for(uint32_t i=0;i<points.size();++i){const auto& x=points[i];ds.push_back(x[0]);vs.push_back(x[1]);snrs.push_back(x[2]);
   const uint32_t cell=uint32_t(x[1]/2+128)*columns+uint32_t(x[0]);s[layout.indices+i]=cell;s[layout.flags+cell]=1;
   real(s,layout.records+8*i,x[0]);real(s,layout.records+8*i+2,x[1]);real(s,layout.records+8*i+4,x[2]);}
  ::Detection input(ds,vs,snrs);post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);
  require(stage.prepare({frame,frame,500*frame}),"T06 resident capacity refused valid map");
  stage.copyPrepared(output);stage.commit(result.token);portable.process(&input,500*frame);
  tracker_qualification::compareCheckpoint(portable,checkpoint.data(),result.checkpointWords);
  require(portable.frame_diagnostics().stationaryBirthCapacityRejected==stage.admissionRejections()[gpu_tracker::stationaryCapacity],
   "T06 stationary birth capacity differs");
 };
 advance({{10,0,4}});require(portable.frame_diagnostics().stationaryCells==std::min(27u,capacity),"T06 initial occupancy differs");
 advance({{11,0,4}});advance({{100,40,10}});
 for(unsigned i=0;i<10;++i)advance({});
 advance({{100,40,10}});require(portable.frame_diagnostics().initiated==1,"T06 decay did not free independent birth");
 if(capacity==65536){stage.resetInput();portable.reset();frame=0;std::vector<Point> cover;
  for(uint32_t row=1;row<rows;row+=3)for(uint32_t col=4;col<columns;col+=9)cover.push_back({double(col),dopplers[row],4});
  advance(cover);require(portable.frame_diagnostics().stationaryCells==capacity,"T06 operational cap never saturated");
  advance({{256,256,10}});
 }
 std::cout<<"PASS T06 occupancy capacity="<<capacity<<" map_cells="<<cells<<" checkpoint=1 decay=1\n";
}

void stationaryLifecycleParity(Context& context,uint32_t depth,uint32_t capacity){
 constexpr uint32_t rows=9,columns=64,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,256*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[34]=cells;p[39]=layout.flags;
 p[41]=layout.indices;p[44]=96;p[45]=128;p[74]=depth;p[77]=UINT32_MAX;p[78]=depth>2;p[86]=layout.direction;
 std::deque<int> delays;std::deque<double> dopplers;
 for(uint32_t r=0;r<rows;++r){dopplers.push_back(2.*(int(r)-4));real(p,96+2*r,dopplers.back());}
 for(uint32_t c=0;c<columns;++c){delays.push_back(int(c));p[128+c]=c;}
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=8;config.m=1;config.n=3;
 config.nDelete=std::max(3u,depth-1);config.adaptiveGate=true;config.cpi=.5;
 config.rangeRes=1e12;config.lambda=1;config.minInitiationSnrDb=5;config.epoch=71;
 config.stationaryClutter=true;config.stationaryPersistenceCpis=3;config.maximumStationaryCells=capacity;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout,depth);
 TrackerRecoveryConfig recovery{true,3,0,capacity};
 ::Tracker portable(1,3,config.nDelete,.5,0,1e12,1,5,true,{},false,9.210340371976184,{},recovery);
 portable.configureGrid(delays,dopplers);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity());TrackerResult result;
 TrackerOutputs output{checkpoint.data(),checkpoint.size(),&result};
 ::Detection detected(std::vector<double>{},std::vector<double>{},std::vector<double>{});
 auto state=[&]{return gpu_tracker::QualificationSnapshot::committed(stage);};
 auto committed=[&]{std::vector<uint32_t> snapshot(stage.checkpointWordCapacity());TrackerResult r;
  stage.copyCommitted({snapshot.data(),snapshot.size(),&r});snapshot.resize(r.checkpointWords);return snapshot;};
 const auto input=[&](TrackerFrame frame,double delay,double snr,bool hit,bool malformed=false){
  p[11]=hit;p[79]=uint32_t(frame.consumedSequence);auto* s=words(scratch);
  detected=::Detection(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  if(hit){const uint32_t cell=4*columns+uint32_t(delay);s[layout.indices]=cell;
   s[layout.flags+cell]=1u|(depth==8?64u:depth==4?32u:depth==2?8u:0u);
   s[layout.direction+cell]=malformed?0x80000000u:0u;
   real(s,layout.records,delay);real(s,layout.records+2,0);real(s,layout.records+4,snr);
   detected=::Detection(delay,0,snr);::Detection::Evidence e;e.epoch=config.epoch;
   e.depth=uint8_t(depth);e.newestSequence=frame.consumedSequence;
   for(uint32_t i=0;i<depth;++i)e.sequences[i]=e.newestSequence-i;
   detected.set_evidence({e});}
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);
 };
 const auto frame=[&](uint64_t generation,uint64_t timestamp){return TrackerFrame{generation,generation+depth,timestamp};};
 const auto advance=[&](TrackerFrame f,double delay,double snr,bool hit=true,bool noOp=false){
  const auto before=state(),oldCheckpoint=committed();input(f,delay,snr,hit);
  require(stage.prepare(f),"LC resident clock rejected host-admitted frame");
  require((stage.evidencePrepared()[2]==1)==noOp,"LC resident no-op disposition differs");
  stage.copyPrepared(output);stage.commit(result.token);portable.process(&detected,f.timestamp);
  require(portable.frame_diagnostics().noOp==noOp,"LC host no-op disposition differs");
  tracker_qualification::compareCheckpoint(portable,checkpoint.data(),result.checkpointWords);
  if(noOp)require(state()==before&&committed()==oldCheckpoint,"LC no-op changed committed occupancy/clock/checkpoint");
 };
 const auto reset=[&]{stage.resetInput();portable.reset();};
 // LC-01: a first timestamp of zero is a real accepted frame, even when weak
 // evidence leaves no tracks. No-op probes deliberately advance generation.
 advance(frame(1,0),10,4);require(checkpoint[4]==0&&state()[0]==1,"LC first empty timestamp-zero frame lost");
 advance(frame(1,0),10,4,true,true);advance(frame(2,0),10,4,true,true);
 advance(frame(65537,0),10,4,true,true);
 advance(frame(2,1000),10,10);require(checkpoint[4]==1,"LC empty duplicate overlearned and vetoed birth");
 advance(frame(3,1000),10,4,true,true);advance(frame(3,999),10,4,true,true);
 auto stale=frame(3,2000);--stale.consumedSequence;advance(stale,10,4,true,true);
 advance(frame(3,2000),40,4);
 uint64_t generation=3;
 for(uint32_t i=0;i<=config.nDelete;++i){++generation;advance(frame(generation,1000*generation),0,0,false);}
 require(checkpoint[4]==0,"LC deletion did not produce empty retained population");
 advance(frame(generation+1,1000*generation),40,4,true,true);
 advance(frame(generation+1,1000*generation-1),40,4,true,true);
 ++generation;advance(frame(generation,1000*generation),10,10);
 require(checkpoint[4]==1,"LC post-deletion stale frame vetoed eligible birth");
 reset();advance(frame(1,1000),10,4);advance(frame(2,999),10,4,true,true);
 auto emptyStale=frame(2,2000);--emptyStale.consumedSequence;advance(emptyStale,10,4,true,true);
 advance(frame(2,2000),10,10);require(checkpoint[4]==1,"LC empty stale clock overlearned occupancy");
 if(depth==1){reset();advance(frame(1,1000),10,4);advance(frame(UINT64_C(4294967297),2000),10,4);
  advance(frame(UINT64_C(4294967298),3000),10,10);
  require(checkpoint[4]==0&&state()[0]==3,"LC transport generation incorrectly aged accepted-frame occupancy");}
 // LC-02: prepare/abort/reuse and late device validation failure must leave
 // both the occupied count/cells and accepted clock byte-identical.
 for(bool fail:{false,true}){reset();advance(frame(1,1000),10,4);
  const auto before=state(),oldCheckpoint=committed();input(frame(2,2000),10,fail?10:4,true,fail);
  if(fail){require(!stage.prepare(frame(2,2000)),"LC injected post-paint failure was accepted");
   require(gpu_tracker::QualificationSnapshot::rejection(stage)==3162,"LC failure did not reach post-paint checkpoint validation");}
  else{require(stage.prepare(frame(2,2000)),"LC abort fixture prepare failed");stage.abort(stage.preparedToken());}
  require(state()==before&&committed()==oldCheckpoint,"LC rejected/aborted painting mutated committed state");
  advance(frame(2,2000),40,4);advance(frame(3,3000),10,10);
  require(checkpoint[4]==1,"LC discarded learning vetoed later accepted birth");
 }
 reset();advance(frame(1,1000),10,4);
 for(uint64_t i=2;i<=5;++i)advance(frame(i,1000*i),0,0,false);
 const auto beforeExpiry=state(),beforeExpiryCheckpoint=committed();
 require(beforeExpiry[2]==27,"LC expiry fixture lacked occupied capacity");
 input(frame(6,6000),40,10,true);require(stage.prepare(frame(6,6000)),"LC prune abort fixture failed");stage.abort(stage.preparedToken());
 require(state()==beforeExpiry&&committed()==beforeExpiryCheckpoint,"LC aborted prune reclaimed committed capacity");
 input(frame(6,6000),40,10,true,true);require(!stage.prepare(frame(6,6000)),"LC post-prune failure accepted");
 require(gpu_tracker::QualificationSnapshot::rejection(stage)==3162&&state()==beforeExpiry&&committed()==beforeExpiryCheckpoint,
  "LC rejected expiry changed occupancy or checkpoint");
 advance(frame(6,6000),10,10);require(checkpoint[4]==1&&state()[2]==27,"LC accepted expiry failed to release capacity");
 // LC-03: a kinematic-only checkpoint can preserve, never reconstruct, the
 // current warm occupancy lifecycle at any depth or occupied-cell limit.
 reset();advance(frame(1,1000),10,4);const auto old=committed();advance(frame(2,2000),10,4);
 const auto warm=committed(),warmState=state();
 const auto refuse=[&](gpu_tracker::Stage& target,const std::vector<uint32_t>& saved){bool refused=false;
  try{target.restore(config,saved.data(),saved.size());}catch(const std::invalid_argument&){refused=true;}
  require(refused,"LC stale/foreign/cross-reset stationary restore admitted missing occupancy lifecycle");};
 refuse(stage,old);require(state()==warmState&&committed()==warm,"LC stale restore mutated state");
 {gpu_tracker::Stage foreign(context,config,cells,post,scratch,layout,depth);
  const auto cold=gpu_tracker::QualificationSnapshot::committed(foreign);refuse(foreign,warm);
  require(gpu_tracker::QualificationSnapshot::committed(foreign)==cold,"LC foreign restore mutated occupancy");}
 stage.restore(config,warm.data(),warm.size());
 require(state()==warmState&&committed()==warm,"LC current restore changed occupancy/clock/checkpoint");
 advance(frame(3,3000),10,10);require(checkpoint[4]==0,"LC current restore forgot learned veto");
 const auto preReset=committed();reset();refuse(stage,preReset);
 advance(frame(1,0),10,10);require(checkpoint[4]==1,"LC cold reset retained prior lifecycle");
 std::cout<<"PASS LC01-LC03 depth="<<depth<<" capacity="<<capacity
  <<" empty_nonempty_duplicate_stale=1 first_zero=1 post_deletion=1 abort_reuse=1 post_paint_failure=1 expiry_rollback=1 occupancy_bytes=1 restore_current_stale_foreign_reset=1\n";
}

Candidate softwareCandidate(Instance& instance){
 uint32_t count=0;check(vkEnumeratePhysicalDevices(instance.handle,&count,nullptr),"test device discovery");
 std::vector<VkPhysicalDevice> devices(count);
 check(vkEnumeratePhysicalDevices(instance.handle,&count,devices.data()),"test device discovery");
 for(auto device:devices){Candidate candidate{};candidate.physical=device;
  vkGetPhysicalDeviceProperties(device,&candidate.properties);
  if(candidate.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
  VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(device,&memory);
  uint64_t bytes=0;for(uint32_t i=0;i<memory.memoryHeapCount;++i)bytes=std::max(bytes,uint64_t(memory.memoryHeaps[i].size));
  candidate.info={"test-only-software",candidate.properties.deviceName,bytes};
  uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(device,&queues,nullptr);
  std::vector<VkQueueFamilyProperties> families(queues);vkGetPhysicalDeviceQueueFamilyProperties(device,&queues,families.data());
  for(uint32_t q=0;q<queues;++q)if(families[q].queueCount&&(families[q].queueFlags&VK_QUEUE_COMPUTE_BIT)){
   candidate.queue=q;return candidate;}
 }
 throw std::runtime_error("Explicit --software test requires a CPU Vulkan device; physical devices will not be used");
}
}
void causalTraceParity(Context& context,double maximumAcceleration){
 constexpr uint32_t rows=3,columns=128,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,128*4,host,preferred),scratch(context,layout.bytes,host,preferred);
 for(Buffer* b:{&post,&scratch})clear(*b);
 auto*p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 // This is ordinary 4-of-6, with a measured vote every CPI. Declaring a
 // two-CPI producer here would encode temporal evidence in schema3.
 p[44]=96;p[74]=1;p[75]=0;p[77]=UINT32_MAX;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,96+2*row,4.0*(int(row)-1));
 post.flush(0,post.bytes);
 TrackerConfig cfg;cfg.enabled=true;cfg.capacity=64;cfg.m=4;cfg.n=6;cfg.nDelete=3;
 cfg.adaptiveGate=true;cfg.maxAccInit=maximumAcceleration;cfg.cpi=.5;cfg.rangeRes=1;cfg.lambda=1;cfg.minInitiationSnrDb=5;cfg.epoch=99;
 auto observed=cfg;observed.causalDiagnostics=true;
 gpu_tracker::Stage baseline(context,cfg,cells,post,scratch,layout),
  traced(context,observed,cells,post,scratch,layout);
 std::vector<uint32_t> a(baseline.checkpointWordCapacity()),b(traced.checkpointWordCapacity());
 std::array<uint32_t,causal_trace::words> trace{};TrackerResult ra,rb;
 TrackerOutputs oa{a.data(),a.size(),&ra},ob{b.data(),b.size(),&rb};
 for(uint64_t frame=1;frame<=120;++frame){
  if(frame==20){baseline.restore(cfg,a.data(),ra.checkpointWords);traced.restore(observed,b.data(),rb.checkpointWords);}
  if(frame==25){baseline.resetInput();traced.resetInput();}
  const bool miss=frame==5;
  p[11]=40;auto*s=words(scratch);
  for(uint32_t i=0;i<40;++i){const double delay=i==0?(miss?7:4):20+i;
   s[layout.indices+i]=columns+uint32_t(delay);real(s,layout.records+8*i,delay);
   real(s,layout.records+8*i+2,i==0&&frame==8?2:0);real(s,layout.records+8*i+4,i==0?8:1);real(s,layout.records+8*i+6,delay);}
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);
  require(baseline.prepare({frame,frame,1000+500*frame}),"causal baseline prepare");baseline.copyPrepared(oa);
  if(frame==1)require(a[0]==5u&&a[27]==0u&&(a[32u+22u]>>4u)==1u,
   "causal fixture must retain ordinary single-CPI lineage");
  require(traced.prepare({frame,frame,1000+500*frame}),"causal observed prepare");traced.copyPrepared(ob);
  traced.copyCausalTrace(trace.data(),trace.size());
  require(ra.checkpointWords==rb.checkpointWords&&std::equal(a.begin(),a.begin()+ra.checkpointWords,b.begin()),
   "diagnostics changed native checkpoint bytes");
  require(trace[0]==1&&trace[1]==40&&trace[4]==frame,"causal frame identity/coverage");
  require(trace[causal_trace::headerWords+causal_trace::recordWords+1]==2,"causal exclusive SNR reason");
  if(frame==1||frame==25){const auto* seed=trace.data()+causal_trace::headerWords;
   require(seed[1]==8&&(seed[5]&2u)&&seed[27]==1&&seed[26]<65535,"causal seed credit/track linkage");}
  if(frame==4)require(trace[causal_trace::headerWords+32*causal_trace::recordWords+1]==12,
   "causal promotion missing");
  if(frame==5||frame==8){const auto* record=trace.data()+causal_trace::headerWords+32*causal_trace::recordWords;
   require(record[1]==9&&(record[24]&(frame==5?2u:4u)),"strict gate negative control or residual missing");}
  baseline.commit(ra.token);traced.commit(rb.token);
 }
 require(traced.prepare({120,120,61000}),"causal duplicate prepare");traced.copyPrepared(ob);
 traced.copyCausalTrace(trace.data(),trace.size());require(trace[3]==1&&!trace[1]&&!trace[2],"duplicate trace retained events");traced.commit(rb.token);
 if(maximumAcceleration==0){uint64_t sequence=120;
  for(unsigned axis=0;axis<2;++axis){traced.resetInput();
   for(unsigned step=0;step<2;++step){auto*s=words(scratch);p[11]=1;
    const double delay=4+(step&&axis==0?1:0),doppler=step&&axis==1?2:0;
    s[layout.indices]=columns+uint32_t(delay);real(s,layout.records,delay);
    real(s,layout.records+2,doppler);real(s,layout.records+4,step?1:8);real(s,layout.records+6,delay);
    post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);++sequence;
    require(traced.prepare({sequence,sequence,1000+500*sequence}),"zero-span boundary prepare");
    if(step)require(traced.evidencePrepared()[27]==0,"zero-span first-association boundary widened");
    traced.copyPrepared(ob);traced.commit(rb.token);}}}
 std::cout<<"PASS causal GPU diagnostics maxAcc="<<maximumAcceleration<<" 120-CPI byte-parity=1 bounded-prefix=32 reset_restore=1 wrap=1 promotion=1 duplicate=1 strict_gate_negatives=1\n";
}
int main(int argc,char** argv){try{
 require(argc==2&&(std::string(argv[1])=="--software"||std::string(argv[1])=="--lineage-only"||std::string(argv[1])=="--confirmation-only"||std::string(argv[1])=="--temporal-parity"||std::string(argv[1])=="--policy-parity"),
  "Usage: testGpuTrackerCorrectness --software|--lineage-only|--confirmation-only|--temporal-parity|--policy-parity (CPU Vulkan only)");
 // This explicit offline fixture inspects diagnostic text as well as numerical
 // evidence. Ordinary workers receive this recipe only from an offline parent.
 const auto parent=std::to_string(getppid());
 setenv(::blah2::gpu_worker_lifetime::parentVariable,parent.c_str(),1);
 setenv(::blah2::gpu_offline_diagnostics::recipeVariable,(parent+" 64").c_str(),1);
 auto instance=std::make_shared<::blah2::Instance>();auto candidate=softwareCandidate(*instance);
 std::cout<<"device="<<candidate.info.name<<" software_test_only=1"<<std::endl;
 ::blah2::Context context(instance,candidate);
 std::cout<<"TRACKER_POLICY_BACKEND software-vulkan device="<<candidate.properties.deviceName
  <<" actual_shader_execution_required=1 policy="<<tracker_policy::identity<<'\n';
 if(std::string(argv[1])=="--policy-parity"||std::string(argv[1])=="--software"){
  for(uint32_t depth:{1u,2u,4u,8u})for(bool adaptive:{false,true})for(uint32_t minimum:{1u,2u})
   admissionBackendParity(context,depth,adaptive,minimum);
  for(double maximum:{0.,2.})for(double sign:{-1.,1.})for(double outside:{std::nextafter(2.,3.),2.01})
   stationaryBackendParity(context,maximum,sign,outside);
  for(uint32_t capacity:{26u,27u,28u,65536u})stationaryCapacityBackendParity(context,capacity);
  for(uint32_t depth:{1u,2u,4u,8u})for(uint32_t capacity:{27u,65536u})stationaryLifecycleParity(context,depth,capacity);
  if(std::string(argv[1])=="--policy-parity"){clutterParity(context);
   std::cout<<"COMPLETE tracker policy parity production_shader_execution=1 lifecycle=1\n";return 0;}
 }
 if(std::string(argv[1])=="--temporal-parity"){associationContractParity(context);
  stationaryBackendParity(context);
  for(uint32_t depth:{1u,2u,4u,8u})temporalBackendParity(context,depth);
  for(uint32_t depth:{4u,8u}){temporalBackendParity(context,depth,true);}
  return 0;}
 if(std::string(argv[1])=="--confirmation-only"){
  confirmationSemantics(context);confirmationSemantics(context,4,6,false);return 0;}
 for(double threshold:{-5.0,5.0,12.0})thresholdParity(context,threshold);
 admissionReasons(context);
 causalTraceParity(context,8);causalTraceParity(context,0);
 confirmationSemantics(context);confirmationSemantics(context,4,6,false);confirmationSemantics(context,1);confirmationSemantics(context,1,1);
 clutterParity(context);for(uint32_t depth:{1u,2u,4u,8u})temporalBackendParity(context,depth);
 associationContractParity(context);stationaryBackendParity(context);for(uint32_t depth:{4u,8u})temporalBackendParity(context,depth,true);
 std::cout<<"COMPLETE tracker correctness production_shader_execution=1\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL tracker correctness: "<<e.what()<<'\n';return 1;}}
