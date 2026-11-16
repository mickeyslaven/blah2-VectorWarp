// Explicit device test; --software accepts only a CPU Vulkan device. Production
// enumeration still excludes software devices. No capture, services or FFT work.
#include "../../core/VulkanBackend.private.cpp"
#include <map>
#include <sstream>
#include "process/tracker/CausalTraceLayout.h"

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

void thresholdParity(Context& context,double threshold,bool guided){
 constexpr uint32_t rows=3,columns=16,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,128*4,host,preferred),scratch(context,layout.bytes,host,preferred),
  integration(context,uint64_t(cells)*4*8,host,preferred),power(context,uint64_t(cells)*8,host,preferred);
 for(Buffer* b:{&post,&scratch,&integration,&power})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 p[44]=96;p[74]=4;p[75]=0;p[77]=UINT32_MAX;p[78]=1;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,96+2*row,4.0*(int(row)-1));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=8;config.m=2;config.n=3;config.nDelete=3;
 config.adaptiveGate=true;config.cpi=.5;config.rangeRes=1;config.lambda=1;config.minInitiationSnrDb=threshold;
 config.trackGuidedShadow=guided;
 config.normalizedInnovationGate=true;
 config.trackGuidedShadowMaxTracks=guided?4:0;config.epoch=7;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout,integration,power);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity()),recoveries(guided?24:0);
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),
  guided?recoveries.data():nullptr,recoveries.size(),&result};
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
 try{gpu_tracker::Stage invalid(context,config,cells,post,scratch,layout,integration,power);}
 catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"host tracker admitted inconsistent map geometry");
 std::cout<<"PASS tracker threshold="<<threshold<<" guided="<<guided<<" empty_nonempty_reset_restore=1\n";
}

void admissionReasons(Context& context,bool guided){
 constexpr uint32_t rows=33,columns=96,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,256*4,host,preferred),scratch(context,layout.bytes,host,preferred),
  integration(context,uint64_t(cells)*4*8,host,preferred),power(context,uint64_t(cells)*8,host,preferred);
 for(Buffer* b:{&post,&scratch,&integration,&power})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 p[44]=128;p[74]=4;p[75]=0;p[77]=UINT32_MAX;p[78]=1;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,128+2*row,4.0*(int(row)-16));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=2;config.m=2;config.n=3;config.nDelete=3;
 config.adaptiveGate=true;config.maxAccInit=8;config.cpi=.5;config.rangeRes=1;config.lambda=1;config.minInitiationSnrDb=5;
 config.stationaryClutter=true;config.stationaryPersistenceCpis=3;
 config.trackGuidedShadow=guided;config.trackGuidedShadowMaxTracks=guided?2:0;config.epoch=9;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout,integration,power);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity()),recoveries(guided?12:0);
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),
  guided?recoveries.data():nullptr,recoveries.size(),&result};
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
  std::ostringstream log;context.diagnostics.flush(log);
  if(stage.admissionRejections()!=expected){std::cerr<<"admission_case="<<cases<<' '<<log.str();
   throw std::runtime_error("exclusive GPU admission reasons differ");}
  const auto& evidence=stage.evidencePrepared();
  require(evidence.size()==66&&evidence[30]==births&&evidence[27]==associated,
   "admission diagnostics changed public evidence or tracking decisions");
  std::map<std::string,uint32_t> fields;std::istringstream tokens(log.str());std::string token;
  while(tokens>>token){const auto at=token.find('=');if(at!=std::string::npos&&at+1<token.size()&&token[at+1]>='0'&&token[at+1]<='9')
   fields[token.substr(0,at)]=uint32_t(std::stoul(token.substr(at+1)));}
  const char* names[]={"min_snr_rejected","doppler_comb_rejected","range_comb_rejected",
   "persistent_occupancy_rejected","boundary_rejected","dropped_capacity_detections"};
  uint32_t rejected=0;
  for(uint32_t i=0;i<expected.size();++i){require(fields.at(names[i])==expected[i],"logged rejection differs from shader counter");rejected+=expected[i];}
  require(fields.at("admission_reason_version")==1&&fields.at("stationary_clutter_suppressed")==expected[gpu_tracker::persistentOccupancy],
   "legacy stationary log key still includes unrelated admission rejects");
  require(rejected+fields.at("admitted_detections")+associated==detections.size(),"log admission reasons do not reconcile");
  stage.copyPrepared(output);stage.commit(result.token);++cases;
 };
 advance({},Reasons{},0);
 advance({{16,10,4.9}},Reasons{1,0,0,0,0,0},0);
 stage.resetInput();advance({{16,95,4.9}},Reasons{0,0,0,0,1,0},0);
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
 std::cout<<"PASS tracker exclusive_admission_reasons guided="<<guided<<" cases="<<cases
  <<" production_log_reconciliation=1 repeated_predicate_passes=1 multi_tile=1\n";
}

void clutterParity(Context& context){
 constexpr uint32_t rows=601,columns=511,cells=rows*columns;
 Buffer metadata(context,512,host,preferred),bankA(context,512,host,preferred),bankB(context,512,host,preferred),
  scratch(context,4096,host,preferred),post(context,512,host,preferred),clutter(context,uint64_t(cells+10)*4,host,preferred);
 for(Buffer* b:{&metadata,&bankA,&bankB,&scratch,&post,&clutter})clear(*b);
 auto sources=gpu_tracker::shaderSources(false);
 const auto commonEnd=sources[0].second.rfind(gpu_tracker::initializeBody);
 require(commonEnd!=std::string::npos,"tracker shader prefix unavailable");
 const std::string probeSource=sources[0].second.substr(0,commonEnd)+R"glsl(
void main(){uint i=indexOf();if(i>=p.count)return;s[128u+i]=clutterRidgeStreak(s[i]);}
)glsl";
 const std::vector<Buffer*> buffers{&metadata,&bankA,&bankB,&scratch,&post,&clutter};
 Kernel probe(context,probeSource.c_str(),buffers),paint(context,sources[13].second.c_str(),buffers);
 auto* m=words(metadata);auto* p=words(post);auto* s=words(scratch);auto* c=words(clutter);
 m[2]=2;m[4]=cells;m[5]=64;m[10]=20;m[19]=1;
 p[32]=rows;p[33]=columns;p[41]=0;c[0]=1;c[1]=12;
 auto flush=[&]{for(Buffer* b:{&metadata,&scratch,&post,&clutter})b->flush(0,b->bytes);};
 auto query=[&](uint32_t cell){s[0]=cell;flush();dispatch(context,probe,1);
  metadata.invalidate(0,metadata.bytes);scratch.invalidate(0,scratch.bytes);return s[128];};
 unsigned cases=0;
 // Compare the actual shader against the independent CPU coordinate oracle,
 // including all corners, both axes and neighborhoods touching row boundaries.
 for(const auto center:std::vector<std::pair<uint32_t,uint32_t>>{{0,0},{0,510},{600,0},{600,510},{300,255},{1,510}}){
  clear(clutter);c[0]=1;c[1]=12;const uint32_t learned=center.first*columns+center.second;
  c[10+learned]=(20u<<16u)|12u;
  for(int dr=-2;dr<=2;++dr)for(int dc=-5;dc<=5;++dc){
   const int r=int(center.first)+dr,d=int(center.second)+dc;
   if(r<0||d<0||r>=int(rows)||d>=int(columns))continue;
   const uint32_t candidate=uint32_t(r)*columns+uint32_t(d);
   const uint32_t expected=gpu_tracker::persistentRidgeContains(learned,candidate,columns,rows)?12:0;
   require(query(candidate)==expected&&m[0]==0,"non-square clutter lookup differs from coordinate oracle");++cases;
  }
 }
 auto paintAt=[&](uint32_t frame,uint32_t row,uint32_t column){
  m[0]=0;m[10]=frame;s[0]=row*columns+column;real(s,66,100.0);flush();
  dispatch(context,paint,1);metadata.invalidate(0,metadata.bytes);clutter.invalidate(0,clutter.bytes);
  require(m[0]==0,"valid persistence painting rejected");
 };
 clear(clutter);c[0]=1;c[1]=12;
 for(uint32_t frame=1;frame<=12;++frame)paintAt(frame,300,250);
 require(query(300*columns+250)==12,"stationary nuisance did not accumulate persistence");
 require(query(300*columns+259)==0,"independent moving candidate inherited distant nuisance persistence");
 require(query(303*columns+250)==0,"persistence painted an unbounded Doppler column");
 clear(clutter);c[0]=1;c[1]=12;
 for(uint32_t frame=1;frame<=24;++frame){paintAt(frame,300,100+2*frame);
  require(query(300*columns+100+2*frame)<12,"moving candidate was learned as stationary nuisance");}
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
 std::cout<<"PASS tracker clutter rows=601 columns=511 oracle_cases="<<cases
  <<" edges=1 stationary=1 moving=1 invalid_geometry=1\n";
}

void confirmationSemantics(Context& context,bool guided,uint32_t minimum=4,uint32_t window=6,bool adaptive=true){
 constexpr uint32_t rows=301,columns=96,cells=rows*columns;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,1024*4,host,preferred),scratch(context,layout.bytes,host,preferred),
  integration(context,uint64_t(cells)*4*8,host,preferred),power(context,uint64_t(cells)*8,host,preferred);
 for(Buffer* b:{&post,&scratch,&integration,&power})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 p[44]=128;p[74]=4;p[75]=0;p[77]=UINT32_MAX;p[78]=1;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,128+2*row,2.0*(int(row)-150));
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=8;config.m=minimum;config.n=window;config.nDelete=3;
 config.adaptiveGate=adaptive;config.maxAccInit=adaptive?10:0;config.cpi=.5;
 config.rangeRes=299792458.0/2400000.0;config.lambda=299792458.0/509000000.0;
 config.minInitiationSnrDb=5;config.trackGuidedShadow=guided;
 config.trackGuidedShadowMaxTracks=guided?8:0;config.epoch=19;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout,integration,power);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity()),recoveries(guided?48:0);
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),
  guided?recoveries.data():nullptr,recoveries.size(),&result};
 uint64_t frame=0;
 auto advance=[&](bool hit,double delay=50,double doppler=0,double snr=8){
  auto* s=words(scratch);p[11]=hit?1:0;
  if(hit){const uint32_t row=uint32_t(int32_t(std::round(doppler/2))+150);
   s[layout.indices]=row*columns+uint32_t(std::round(delay));
   real(s,layout.records,delay);real(s,layout.records+2,doppler);
   real(s,layout.records+4,snr);real(s,layout.records+6,delay);}
  post.flush(0,post.bytes);scratch.flush(0,scratch.bytes);++frame;
  require(stage.prepare({frame,frame,1000+500*frame}),"confirmation fixture prepare rejected");
  stage.copyPrepared(output);stage.commit(result.token);
  require(checkpoint[0]==3,"measured history requires checkpoint schema v3");
  return checkpoint[20];
 };
 require(advance(true)==uint32_t(minimum==1),"seed is measured and confirms M=1 immediately");
 require((checkpoint[32+22]&4u)!=0,"seed missing measured-hit flag");
 if(minimum==1){require(advance(false)==0&&checkpoint[22]==1,"confirmed miss must coast");
  require((checkpoint[32+16+8+6]&4u)==0,"coast cannot become a measured hit");
  std::cout<<"PASS tracker confirmation M=1 N="<<window<<" seed=1 guided="<<guided<<'\n';return;}
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
 auto oldGuided=checkpoint;const auto packed=oldGuided[32+13];
 oldGuided[32+16+8*((uint32_t(packed&255)+((packed>>8)&255)-1)%oldGuided[3])+6]=2u|8u|(2u<<4u);
 bool rejectedGuided=false;
 try{stage.restore(config,oldGuided.data(),result.checkpointWords);}catch(const std::invalid_argument&){rejectedGuided=true;}
 require(rejectedGuided,"pre-lineage schema3 guided history restored into ordinary policy");
 auto legacy=checkpoint;legacy[0]=1;bool rejected=false;
 try{stage.restore(config,legacy.data(),result.checkpointWords);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"legacy checkpoint cannot be silently restored as measured-hit history");
 std::cout<<"PASS tracker confirmation M=4 N=6 guided="<<guided<<" adaptive="<<adaptive
  <<" seed=1 immediate=1 sparse_negative=1 lifetime=1 deletion=1 restore=1 RPA5731=1 duplicate=1\n";
}

void guidedLineageSemantics(Context& context){
 constexpr uint32_t rows=3,columns=16,cells=rows*columns,cell=columns+6;
 const auto layout=gpu_postprocess::layout(rows,columns);
 Buffer post(context,1024*4,host,preferred),scratch(context,layout.bytes,host,preferred),
  integration(context,uint64_t(cells)*8*8,host,preferred),power(context,uint64_t(cells)*8,host,preferred);
 for(Buffer* b:{&post,&scratch,&integration,&power})clear(*b);
 auto* p=words(post);p[32]=rows;p[33]=columns;p[34]=cells;p[39]=layout.flags;
 p[41]=layout.indices;p[44]=96;p[45]=104;p[46]=128;p[74]=8;p[75]=0;
 p[77]=UINT32_MAX;p[78]=1;p[84]=8;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,96+2*row,2.0*(int(row)-1));
 for(uint32_t column=0;column<columns;++column){p[104+column]=column;
  const auto at=128+8*column;p[at]=0;p[at+1]=1;p[at+2]=14;p[at+3]=15;p[at+4]=2;real(p,at+6,5);}
 post.flush(0,post.bytes);
 TrackerConfig config;config.enabled=true;config.capacity=4;config.m=2;config.n=3;config.nDelete=3;
 config.adaptiveGate=true;config.maxAccInit=8;config.cpi=.5;config.rangeRes=1;config.lambda=1;
 config.minInitiationSnrDb=5;config.trackGuidedShadow=config.trackGuidedPromote=true;
 config.trackGuidedSustain=true;config.normalizedInnovationGate=true;
 config.causalDiagnostics=true;
 config.trackGuidedShadowMaxTracks=4;config.epoch=23;
 gpu_tracker::Stage stage(context,config,cells,post,scratch,layout,integration,power);
 std::vector<uint32_t> checkpoint(stage.checkpointWordCapacity()),recoveries(24);
 TrackerResult result;TrackerOutputs output{checkpoint.data(),checkpoint.size(),recoveries.data(),recoveries.size(),&result};
 uint64_t frame=0;uint32_t temporal=0;
 auto advance=[&](bool hit,uint32_t depth=2,double cut=1.0,bool accepted=true){
  auto* s=words(scratch);p[11]=hit?1u:0u;
  if(hit){s[layout.indices]=cell;s[layout.flags+cell]=1u|(depth==8?64u:depth==4?32u:8u);
   real(s,layout.records,6);real(s,layout.records+2,0);real(s,layout.records+4,8);real(s,layout.records+6,6);}
  auto* current=static_cast<double*>(power.mapped);std::fill(current,current+cells,1.0);current[cell]=cut;
  auto* history=static_cast<double*>(integration.mapped);std::copy(current,current+cells,history+(temporal%8)*cells);
  p[79]=++temporal;++frame;
  for(Buffer* b:{&post,&scratch,&integration,&power})b->flush(0,b->bytes);
  const bool prepared=stage.prepare({frame,frame,1000+500*frame});
  require(prepared==accepted,"experimental lineage prepare disposition differs");
  if(!prepared){require(stage.evidencePrepared()[0]==322,"gap failed for an unexpected reason");return;}
  stage.copyPrepared(output);stage.commit(result.token);require(checkpoint[0]==4,"experimental schema must be v4");
 };
 auto newest=[&]{const uint32_t packed=checkpoint[32+13],h=checkpoint[3];
  return checkpoint[32+16+8*((uint32_t(packed&255)+((packed>>8)&255)-1)%h)+6];};
 auto reset=[&]{stage.resetInput();temporal=0;p[79]=0;
  for(unsigned i=0;i<8;++i)advance(false);};
 reset();advance(true);require((newest()&16u)!=0&&checkpoint[20]==0,"seed credit differs");
 advance(true);require((newest()&4u)!=0&&(newest()&16u)==0&&checkpoint[20]==0,
  "overlapping ordinary window double-confirmed");
 advance(false,2,20);require((newest()&24u)==24u&&checkpoint[20]==1,
  "disjoint guided boundary did not confirm feasible 2-of-3");
 const auto stale=checkpoint;const auto staleWords=result.checkpointWords;
 advance(true);require((newest()&4u)!=0&&(newest()&16u)==0,
  "ordinary window overlapping prior guided credit received credit");
 advance(true,4);require((newest()&16u)==0,"four-CPI support reused guided credit");
 advance(true);require((newest()&16u)!=0,"nonoverlapping two-CPI support lost credit");
 advance(false,2,20);require((newest()&16u)==0&&checkpoint[22]==1,
  "overlapping guided impulse sustained track");
 advance(false);require((newest()&24u)==24u&&checkpoint[20]==1,
  "same impulse did not receive its one nonoverlapping credit");
 stage.restore(config,checkpoint.data(),result.checkpointWords);
 bool rejected=false;try{stage.restore(config,stale.data(),staleWords);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"stale tracker checkpoint paired with newer temporal ring");
 auto legacy=checkpoint;legacy[0]=3;rejected=false;
 try{stage.restore(config,legacy.data(),result.checkpointWords);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"ordinary schema restored into experimental policy");
 require(stage.prepare({frame,frame,1000+500*frame}),"exact experimental duplicate rejected");
 require(stage.evidencePrepared()[2]==1,"experimental duplicate was not no-op");
 stage.copyPrepared(output);stage.commit(result.token);
 for(unsigned i=0;i<110;++i)advance(true);
 require(checkpoint[20]==1&&((checkpoint[32+13]>>8)&255u)==100,
  "experimental ring wrap lost confirmation");
 ++frame;advance(true,2,1,false); // sequence/generation gap: require explicit reset
 reset();advance(true,2,20);advance(false);
 require(checkpoint[20]==0&&(newest()&16u)==0,"single impulse duplicated its seed credit");
 advance(false);require(checkpoint[4]==0,"unconfirmed experiment escaped unchanged N-CPI lifetime");
 reset();advance(true);const auto beforeReset=checkpoint;const auto beforeResetWords=result.checkpointWords;
 stage.resetInput();rejected=false;
 try{stage.restore(config,beforeReset.data(),beforeResetWords);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"cross-reset lineage resurrected a prior seed");
 auto impossible=config;impossible.m=4;impossible.n=6;rejected=false;
 try{gpu_tracker::validate(impossible,cells);}catch(const std::invalid_argument&){rejected=true;}
 require(rejected,"experimental impossible 4-of-6 accepted");
 impossible.trackGuidedPromote=false;gpu_tracker::validate(impossible,cells);
 std::cout<<"PASS tracker experimental lineage mixed_windows=1 impulse=1 boundary=1 reset=1 restore=1 gap=1 ring_wrap=1 legacy_4of6=1\n";
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
 Buffer post(context,128*4,host,preferred),scratch(context,layout.bytes,host,preferred),
  integration(context,uint64_t(cells)*4*8,host,preferred),power(context,uint64_t(cells)*8,host,preferred);
 for(Buffer* b:{&post,&scratch,&integration,&power})clear(*b);
 auto*p=words(post);p[32]=rows;p[33]=columns;p[39]=layout.flags;p[41]=layout.indices;
 p[44]=96;p[74]=2;p[75]=0;p[77]=UINT32_MAX;p[86]=layout.direction;
 for(uint32_t row=0;row<rows;++row)real(p,96+2*row,4.0*(int(row)-1));
 post.flush(0,post.bytes);
 TrackerConfig cfg;cfg.enabled=true;cfg.capacity=64;cfg.m=4;cfg.n=6;cfg.nDelete=3;
 cfg.adaptiveGate=true;cfg.maxAccInit=maximumAcceleration;cfg.cpi=.5;cfg.rangeRes=1;cfg.lambda=1;cfg.minInitiationSnrDb=5;cfg.epoch=99;
 auto observed=cfg;observed.causalDiagnostics=true;
 gpu_tracker::Stage baseline(context,cfg,cells,post,scratch,layout,integration,power),
  traced(context,observed,cells,post,scratch,layout,integration,power);
 std::vector<uint32_t> a(baseline.checkpointWordCapacity()),b(traced.checkpointWordCapacity());
 std::array<uint32_t,causal_trace::words> trace{};TrackerResult ra,rb;
 TrackerOutputs oa{a.data(),a.size(),nullptr,0,&ra},ob{b.data(),b.size(),nullptr,0,&rb};
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
 require(argc==2&&(std::string(argv[1])=="--software"||std::string(argv[1])=="--lineage-only"||std::string(argv[1])=="--confirmation-only"),
  "Usage: testGpuTrackerCorrectness --software|--lineage-only|--confirmation-only (CPU Vulkan only)");
 auto instance=std::make_shared<::blah2::Instance>();auto candidate=softwareCandidate(*instance);
 std::cout<<"device="<<candidate.info.name<<" software_test_only=1"<<std::endl;
 ::blah2::Context context(instance,candidate);
 if(std::string(argv[1])=="--lineage-only"){guidedLineageSemantics(context);return 0;}
 if(std::string(argv[1])=="--confirmation-only"){
  confirmationSemantics(context,false);confirmationSemantics(context,true);return 0;}
 for(double threshold:{-5.0,5.0,12.0})for(bool guided:{false,true})thresholdParity(context,threshold,guided);
 for(bool guided:{false,true})admissionReasons(context,guided);
 causalTraceParity(context,8);causalTraceParity(context,0);
 for(bool guided:{false,true}){confirmationSemantics(context,guided);confirmationSemantics(context,guided,4,6,false);confirmationSemantics(context,guided,1);confirmationSemantics(context,guided,1,1);}
 clutterParity(context);guidedLineageSemantics(context);std::cout<<"COMPLETE tracker correctness production_shader_execution=1\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL tracker correctness: "<<e.what()<<'\n';return 1;}}
