// Explicit CPU Vulkan only. Executes the production levels/TBD/CFAR sources;
// no worker, receiver, service, physical GPU, or live configuration.
// A small resident/legacy clutter parity case also checks scratch lifetimes.
#include "../../core/VulkanBackend.private.cpp"
#include <chrono>
#include <random>

namespace {
using namespace blah2;
unsigned checks=0;
void require(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
void put(uint32_t* p,uint32_t at,double value){std::memcpy(p+at,&value,8);}
constexpr uint32_t rows=17,columns=65,cells=rows*columns;
using History=std::vector<std::vector<double>>;
const std::vector<double> axes=[] {std::vector<double> out(rows);
 for(uint32_t row=0;row<rows;++row)out[row]=int(row)-8;
 return out;}();

Candidate softwareCandidate(Instance& instance){
 uint32_t count=0;check(vkEnumeratePhysicalDevices(instance.handle,&count,nullptr),"software device count");
 std::vector<VkPhysicalDevice> devices(count);
 check(vkEnumeratePhysicalDevices(instance.handle,&count,devices.data()),"software device discovery");
 for(auto device:devices){Candidate candidate{};candidate.physical=device;
  vkGetPhysicalDeviceProperties(device,&candidate.properties);
  if(candidate.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
  candidate.info={"test-only-software",candidate.properties.deviceName,UINT64_C(1)<<30};
  uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(device,&count,nullptr);
  std::vector<VkQueueFamilyProperties> queues(count);
  vkGetPhysicalDeviceQueueFamilyProperties(device,&count,queues.data());
  for(uint32_t i=0;i<count;++i)if(queues[i].queueCount&&(queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT)){
   candidate.queue=i;return candidate;}
 }
 throw std::runtime_error("--software requires a CPU Vulkan device; physical devices forbidden");
}

struct Fixture {
 static constexpr auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
 static constexpr auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
 static constexpr uint32_t windows=(96+2*rows+columns+1)&~1u;
 static constexpr uint32_t offsetBase=windows+14*columns;
 Context& context;
 gpu_postprocess::Layout layout=gpu_postprocess::layout(rows,columns);
 Buffer metadata,fused,scratch,history,power,banks,maps,weights,angles;
 std::unique_ptr<Kernel> levels,tbd,legacyTbd,cfar;
 std::vector<double> observed;
 std::vector<uint32_t> flags;
 double elapsedMs=0;
 Fixture(Context& c):context(c),metadata(c,(offsetBase+rows*33*8)*4,host,preferred),
  fused(c,cells*16,host,preferred),scratch(c,layout.bytes,host,preferred),
  history(c,cells*8*8,host,preferred),power(c,cells*8,host,preferred),
  banks(c,cells*4*8,host,preferred),maps(c,8,host,preferred),weights(c,8,host,preferred),
  angles(c,cells*8*4,host,preferred){
  const std::vector<Buffer*> buffers{&metadata,&fused,&scratch,&history,&power,&banks,&maps,&weights,&angles};
  const auto sources=gpu_postprocess::shaderSources();
  levels=std::make_unique<Kernel>(c,sources[0].second.c_str(),buffers);
  tbd=std::make_unique<Kernel>(c,sources[10].second.c_str(),buffers);
  // Keep the original per-cell GPU arithmetic as an independent bitwise
  // comparator. Only the lookup is replaced; all trials, banks and CFAR stay.
  auto legacy=sources[10].second;
  const std::string lookup="int offset=m[35]==0u?0:int(m[m[35]+(row*(2u*m[93]+1u)+path/delayWidth)*m[74]+age]);";
  const auto at=legacy.find(lookup);require(at!=std::string::npos,"Offset lookup seam missing");
  legacy.replace(at,lookup.size(),"int offset=fractionalDelayOffset(readD(94u),row,uint(rr),age);");
  legacyTbd=std::make_unique<Kernel>(c,legacy.c_str(),buffers);
  cfar=std::make_unique<Kernel>(c,sources[3].second.c_str(),buffers);
 }
 void run(const History& input,double scale,uint32_t maximum=8,uint32_t available=8,
     bool angleGate=false,bool mismatch=false,uint32_t dopplerBins=4,bool legacy=false){
  for(Buffer* b:{&metadata,&fused,&scratch,&history,&power,&banks,&maps,&weights,&angles})
   std::memset(b->mapped,0,size_t(b->bytes));
  auto* m=static_cast<uint32_t*>(metadata.mapped);
  m[32]=rows;m[33]=columns;m[34]=cells;m[36]=layout.levels;m[37]=layout.powers;
  const auto offsetWords=track_before_detect::fractionalDelayTableWords(rows,maximum,dopplerBins,scale);
  m[35]=offsetWords?offsetBase:0;
  m[38]=layout.snr;m[39]=layout.flags;m[40]=layout.keep;m[42]=layout.candidates;
  m[44]=96;m[45]=96+2*rows;m[46]=windows;m[49]=1;m[60]=2400000;
  m[74]=maximum;m[75]=8;m[76]=2;m[77]=angleGate?0u:UINT32_MAX;
  m[78]=1;m[79]=available-1;m[80]=track_before_detect::bankCount(maximum);
  m[84]=14;m[93]=dopplerBins;put(m,94,scale);
  for(uint32_t row=0;row<rows;++row)put(m,96+2*row,axes[row]);
  for(uint32_t col=0;col<columns;++col){
   m[96+2*rows+col]=col;const uint32_t at=windows+14*col;
   const uint32_t a=uint32_t(std::max(0,int(col)-8)),b=uint32_t(std::max(0,int(col)-2));
   const uint32_t c=std::min(columns,col+3),d=std::min(columns,col+9),n=b-a+d-c;
   m[at]=a;m[at+1]=b;m[at+2]=c;m[at+3]=d;m[at+4]=n;
   for(uint32_t pass=0;pass<=m[80];++pass){const uint32_t depth=pass?2u<<(pass-1):2;
    put(m,at+6+2*pass,track_before_detect::cfarAlpha(
      track_before_detect::passPfa(1e-6,maximum,2,dopplerBins,pass),n,depth));}
  }
  auto* hist=static_cast<double*>(history.mapped);
  auto* beam=static_cast<uint32_t*>(angles.mapped);
  std::fill(hist,hist+cells*8,std::numeric_limits<double>::quiet_NaN());
  for(uint32_t age=0;age<std::min(available,maximum);++age){const uint32_t slot=(available-1+maximum-age)%maximum;
   std::copy(input[age].begin(),input[age].end(),hist+slot*cells);
   std::fill(beam+slot*cells,beam+(slot+1)*cells,mismatch&&age?5u:1u);}
  auto* f=static_cast<double*>(fused.mapped);
  for(uint32_t i=0;i<cells;++i){f[2*i]=std::sqrt(input[0][i]);f[2*i+1]=0;}
  for(Buffer* b:{&metadata,&fused,&scratch,&history,&power,&banks,&maps,&weights,&angles})b->flush(0,b->bytes);
  gpu_reference::CommandOwner command(context);command.allocate();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"fractional test begin");
  if(offsetWords){tbd->append(command.command,{offsetWords*2u,3,0,0,0});barrier(command.command);}
  auto& paths=legacy?legacyTbd:tbd;
  levels->append(command.command,{cells,0,0,0,0});barrier(command.command);
  paths->append(command.command,{2*cells,0,0,0,0});barrier(command.command);
  for(uint32_t first=0;first<(2*dopplerBins+1)*5;first+=64){
   paths->append(command.command,{128*cells,1,first,0,0});barrier(command.command);}
  paths->append(command.command,{2*cells,2,0,0,0});barrier(command.command);
  // Full production CFAR over the longest mature bank, unchanged trial/PFA.
  if(available>=maximum)cfar->append(command.command,{cells,m[80]+1,1,0,0});
  barrier(command.command);VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  read.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,
   0,1,&read,0,nullptr,0,nullptr);
  check(vkEndCommandBuffer(command.command),"fractional test end");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
  const auto start=std::chrono::steady_clock::now();
  check(vkQueueSubmit(context.queue,1,&submit,context.fence),"fractional test submit");
  check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"fractional test fence");
  elapsedMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  check(vkResetFences(context.device,1,&context.fence),"fractional test fence reset");
  metadata.invalidate(0,metadata.bytes);banks.invalidate(0,banks.bytes);scratch.invalidate(0,scratch.bytes);
  require(m[0]==0,"Production fractional shader reported numeric error");
  require(m[29]==track_before_detect::motionEcho({2,dopplerBins,scale!=0}),"Fractional mode echo differs");
  if(offsetWords)for(uint32_t row=0;row<rows;++row)for(int dr=-int(dopplerBins);dr<=int(dopplerBins);++dr)
   for(uint32_t age=0;age<maximum;++age){const int rr=int(row)-dr*int(age);
    const int expected=rr<0||rr>=int(rows)?0:-track_before_detect::delayPathColumn(0,0,age,scale,axes[row],axes[rr]);
    const auto at=offsetBase+(row*(2*dopplerBins+1)+uint32_t(dr+int(dopplerBins)))*maximum+age;
    require(int32_t(m[at])==expected,"Precomputed signed coordinate differs from Q16 oracle");}
  observed.assign(static_cast<double*>(banks.mapped),static_cast<double*>(banks.mapped)+4*cells);
  auto* s=static_cast<uint32_t*>(scratch.mapped);flags.assign(s+layout.flags,s+layout.flags+cells);
 }
 void parity(const History& h,double scale,uint32_t maximum=8,uint32_t available=8,uint32_t dopplerBins=4){
  run(h,scale,maximum,available,false,false,dopplerBins);
  const auto optimized=observed;const auto optimizedFlags=flags;
  run(h,scale,maximum,available,false,false,dopplerBins,true);
  require(std::memcmp(optimized.data(),observed.data(),observed.size()*sizeof(double))==0&&optimizedFlags==flags,
   "Precomputed offsets changed exact GPU bank bits or CFAR decisions");
  for(uint32_t bank=0;bank<=track_before_detect::bankCount(maximum);++bank){
   const uint32_t depth=bank?2u<<(bank-1):2;
   for(uint32_t row=0;row<rows;++row)for(uint32_t col=0;col<columns;++col){
    const double actual=observed[bank*cells+row*columns+col];
    if(depth>available){require(std::isnan(actual),"Warmup bank exposed a score");continue;}
    double expected=0;
    if(bank==0)expected=.5*(h[0][row*columns+col]+h[1][row*columns+col]);
    else for(int dr=-int(dopplerBins);dr<=int(dopplerBins);++dr)for(int dc=-2;dc<=2;++dc)
     expected=std::max(expected,track_before_detect::pathMean(h,columns,axes,row,col,depth,dr,dc,scale));
    require(std::abs(actual-expected)<=1e-11*std::max(1.0,expected),"Production temporal bank differs from scalar oracle");
   }
  }
 }
 double longestThreshold(uint32_t row,uint32_t col){
  const auto* m=static_cast<const uint32_t*>(metadata.mapped);
  const uint32_t at=windows+14*col,bank=m[80];
  double train=0,alpha=0;
  for(uint32_t c=m[at];c<m[at+1];++c)train+=observed[bank*cells+row*columns+c];
  for(uint32_t c=m[at+2];c<m[at+3];++c)train+=observed[bank*cells+row*columns+c];
  std::memcpy(&alpha,m+at+6+2*bank,sizeof(alpha));
  return alpha*(train/m[at+4]);
 }
};

void clutterScratchParity(Context& context){
 constexpr uint32_t samples=128,capacity=2;
 GpuGeometry geometry{};geometry.channels=capacity;geometry.clutterSamples=samples;geometry.clutterBins=3;
 ClutterPipeline legacy(context,geometry,false,false),resident(context,geometry,false,false,true);
 require(resident.residentScratchReusedBytes()==uint64_t(samples)*capacity*8&&
  legacy.residentScratchReusedBytes()==0,"Resident scratch sharing leaked into legacy mode");
 Buffer staging(context,resident.residentSurveillance().bytes,Fixture::host,Fixture::preferred);
 auto transfer=[&](Buffer& target,bool upload,uint64_t bytes){
  gpu_reference::CommandOwner command(context);command.allocate();
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"scratch parity recording");
  VkMemoryBarrier sync{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  sync.srcAccessMask=upload?VK_ACCESS_HOST_WRITE_BIT:VK_ACCESS_MEMORY_WRITE_BIT;
  sync.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(command.command,upload?VK_PIPELINE_STAGE_HOST_BIT:VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
   VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&sync,0,nullptr,0,nullptr);
  VkBufferCopy copy{0,0,bytes};
  vkCmdCopyBuffer(command.command,upload?staging.handle:target.handle,upload?target.handle:staging.handle,1,&copy);
  sync.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
  sync.dstAccessMask=upload?VK_ACCESS_MEMORY_READ_BIT:VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(command.command,VK_PIPELINE_STAGE_TRANSFER_BIT,
   upload?VK_PIPELINE_STAGE_ALL_COMMANDS_BIT:VK_PIPELINE_STAGE_HOST_BIT,0,1,&sync,0,nullptr,0,nullptr);
  check(vkEndCommandBuffer(command.command),"scratch parity recording end");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
  check(vkQueueSubmit(context.queue,1,&submit,context.fence),"scratch parity submit");
  check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"scratch parity fence");
  check(vkResetFences(context.device,1,&context.fence),"scratch parity fence reset");
 };
 std::mt19937 random(77219);std::uniform_real_distribution<float> noise(-1,1);
 std::vector<std::complex<float>> reference(samples),surveillance(samples*capacity),expected(samples*capacity);
 for(unsigned frame=0;frame<3;++frame){
  for(auto& value:reference)value={noise(random),noise(random)};
  std::memcpy(staging.mapped,reference.data(),samples*8);staging.flush(0,samples*8);
  transfer(resident.residentReference(),true,samples*8);
  require(legacy.beginFrame(reference.data(),samples)&&resident.beginFrameResident(),"Scratch parity reference rejected");
  uint32_t first=0;
  for(uint32_t active:{2u,1u}){
   for(uint32_t i=0;i<samples*capacity;++i)surveillance[i]=i<samples*active?
    reference[i%samples]*.7f+std::complex<float>(noise(random),noise(random))*.1f:std::complex<float>{};
   // Poison the padded tail too: neither retained FFT data nor the previous
   // frame/batch is allowed to become an active channel's sample.
   std::memset(staging.mapped,0xa5,size_t(staging.bytes));
   std::memcpy(staging.mapped,surveillance.data(),surveillance.size()*8);staging.flush(0,staging.bytes);
   transfer(resident.residentSurveillance(),true,staging.bytes);
   require(legacy.process(reference.data(),samples,surveillance.data(),samples*active,expected.data(),samples*active,first)&&
    resident.processResident(first,active),"Scratch parity batch rejected");
   transfer(resident.residentEstimate(),false,samples*active*8);staging.invalidate(0,samples*active*8);
   require(std::memcmp(staging.mapped,expected.data(),samples*active*8)==0,"Reused scratch changed exact clutter estimates");
   first+=active;
  }
  legacy.endFrame();resident.endFrame();
 }
 std::cout<<"PASS resident scratch exact parity frames=3 batches=full,partial legacy_unchanged=1\n";
}
}

int main(int argc,char** argv)try{
 require(argc==2&&std::string(argv[1])=="--software","Usage: testGpuTbdFractional --software");
 auto instance=std::make_shared<::blah2::Instance>();auto candidate=softwareCandidate(*instance);
 std::cout<<"device="<<candidate.info.name<<" software_only=1\n";
 ::blah2::Context context(instance,candidate);Fixture fixture(context);
 clutterScratchParity(context);
 History h(8,std::vector<double>(cells,1));
 for(uint32_t maximum:{4u,8u})for(uint32_t available=1;available<=maximum;++available)
  fixture.parity(h,-.566,maximum,available);
 // Distinct CPI powers make accidental repeated-ring-slot votes observable.
 for(uint32_t age=0;age<8;++age)for(uint32_t i=0;i<cells;++i)h[age][i]=1+(i*7+age*13)%23;
 fixture.parity(h,0);fixture.parity(h,-.566);fixture.parity(h,-1.0);
 fixture.parity(h,-.566,8,8,16); // Three trajectory chunks; row/age edges.
 for(double scale:{-.5,std::nextafter(-.5,0.0),std::nextafter(-.5,-1.0),-32.0})
  fixture.parity(h,scale);
 fixture.parity(h,track_before_detect::delayDriftPerHz(1001,2000));
 fixture.parity(h,track_before_detect::delayDriftPerHz(999,2000));
 fixture.parity(h,-.566,8,11);fixture.parity(h,-.566,4,7);
 for(double peak:{10.0,40.0})for(int sign:{-1,1}){
  h.assign(8,std::vector<double>(cells,1));const uint32_t row=uint32_t(8-sign),col=32;
  for(uint32_t age=0;age<8;++age)for(uint32_t c=0;c<columns;++c){
   const double distance=double(c)-(double(col)-sign*.566*age);
   h[age][row*columns+c]+=peak*std::exp(-.5*distance*distance/(.4*.4));}
  fixture.parity(h,0);const double old=fixture.observed[3*cells+row*columns+col];
  const double oldThreshold=fixture.longestThreshold(row,col);
  const bool oldAccepted=fixture.flags[row*columns+col]&1;
  fixture.parity(h,track_before_detect::delayDriftPerHz(1132,2000));
  const double aligned=fixture.observed[3*cells+row*columns+col];
  const double alignedThreshold=fixture.longestThreshold(row,col);
  const bool alignedAccepted=fixture.flags[row*columns+col]&1;
  require(aligned>2*old,"Fractional motion did not recover aligned power");
  require(oldAccepted==(old>oldThreshold)&&alignedAccepted==(aligned>alignedThreshold),
   "Production CFAR acceptance differs from unchanged threshold");
  // At peak10 the nearby max-motion training cells self-mask the target:
  // alignment recovers its power, but neither arm should claim detection.
  // Peak40 crosses only the aligned arm at the exact same PFA/windows/trials.
  require(!oldAccepted&&alignedAccepted==(peak==40),
   "Fractional paired acceptance/rejection differs at unchanged CFAR threshold");
  std::cout<<"PASS slope="<<sign*.566<<" peak="<<peak<<" old_power="<<old
   <<" old_threshold="<<oldThreshold<<" aligned_power="<<aligned
   <<" aligned_threshold="<<alignedThreshold<<" accepted="<<alignedAccepted<<" unchanged_cfar=1\n";
 }
 // Stationary/integer trajectories, edge clipping and angle-gate preservation.
 for(int slope:{-1,0,1}){
  h.assign(8,std::vector<double>(cells,1));
  for(uint32_t age=0;age<8;++age)h[age][8*columns+32-slope*int(age)]=11;
  fixture.parity(h,0);const auto legacy=fixture.observed;
  fixture.parity(h,-.566);
  require(fixture.observed[3*cells+8*columns+32]==legacy[3*cells+8*columns+32],"Zero-Doppler integer parity changed");
 }
 h.assign(8,std::vector<double>(cells,1));
 fixture.run(h,-.566,8,8,true,true);
 for(uint32_t i=0;i<cells;++i)require(fixture.observed[3*cells+i]==0,"Incompatible beam history admitted");
 std::mt19937 generator(73481);std::exponential_distribution<double> noise(1);
 unsigned falseAccepts=0;double legacyMs=0,fractionalMs=0;
 for(unsigned scene=0;scene<12;++scene){
  for(auto& frame:h)for(auto& value:frame)value=noise(generator);
  fixture.parity(h,0);legacyMs+=fixture.elapsedMs;
  fixture.parity(h,-.566);fractionalMs+=fixture.elapsedMs;
  for(auto flag:fixture.flags)falseAccepts+=bool(flag&1);
 }
 require(falseAccepts==0,"Seeded target-free fixture falsely accepted");
 h.assign(8,std::vector<double>(cells,1));h[7][8*columns+32]=11;
 fixture.parity(h,-.566);require(!(fixture.flags[8*columns+32]&1),"One old impulse falsely accepted as persistent evidence");
 std::cout<<"COMPLETE fractional_tbd checks="<<checks<<" noise_scenes=12 false_accepts="<<falseAccepts
  <<" legacy_ms="<<legacyMs/12<<" fractional_ms="<<fractionalMs/12
  <<" banks=2,4,8 exact_gpu_offset_parity=1 extra_trials=0 extra_buffers=0 coherent_cpi_integration=0\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL fractional TBD: "<<e.what()<<'\n';return 1;}
