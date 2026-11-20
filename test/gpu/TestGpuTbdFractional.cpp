// Explicit CPU Vulkan only. Executes the production levels/TBD/CFAR sources;
// no worker, receiver, service, physical GPU, or live configuration.
// A small resident/legacy clutter parity case also checks scratch lifetimes.
#include "process/detection/DetectorIntegration.h"
#include "process/ambiguity/TemporalCandidateCell.h"
#include "process/fusion/BearingRefinement.h"
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
 static constexpr uint32_t rowBounds=windows+14*columns,colBounds=rowBounds+2*rows;
 static constexpr uint32_t offsetBase=colBounds+2*columns;
 static constexpr uint32_t alphaBase=offsetBase+rows*33*8;
 Context& context;
 gpu_postprocess::Layout layout=gpu_postprocess::layout(rows,columns,8);
 Buffer metadata,fused,scratch,history,power,banks,maps,weights,angles;
 std::unique_ptr<Kernel> levels,tbd,legacyTbd,cfar;
 std::vector<std::unique_ptr<Kernel>> detectorKernels;
 std::vector<double> observed;
 std::vector<uint32_t> flags;
 double elapsedMs=0;
 Fixture(Context& c):context(c),metadata(c,(alphaBase+4*14*2)*4,host,preferred),
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
     bool angleGate=false,bool mismatch=false,uint32_t dopplerBins=4,bool legacy=false,uint32_t cfarPass=0){
  for(Buffer* b:{&metadata,&fused,&scratch,&history,&power,&banks,&maps,&weights,&angles})
   std::memset(b->mapped,0,size_t(b->bytes));
  auto* m=static_cast<uint32_t*>(metadata.mapped);
  m[32]=rows;m[33]=columns;m[34]=cells;m[36]=layout.levels;m[37]=layout.powers;
  const auto offsetWords=track_before_detect::fractionalDelayTableWords(rows,maximum,dopplerBins,scale);
  m[35]=offsetWords?offsetBase:0;
  m[38]=layout.snr;m[39]=layout.flags;m[40]=layout.keep;m[42]=layout.candidates;
  m[41]=layout.indices;m[43]=layout.records;m[57]=layout.bankCandidates;m[58]=layout.bankMotion;
  m[86]=layout.direction;
  m[91]=layout.metricPartials;m[92]=layout.metricPartialCount;
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
  std::vector<uint32_t> alphaWords(m,m+alphaBase);
  gpu_postprocess::appendTemporalAlphas(alphaWords,1e-6,maximum,2,dopplerBins,6);
  std::memcpy(m,alphaWords.data(),alphaWords.size()*4);
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
  if(available>=maximum)cfar->append(command.command,{cells,cfarPass?cfarPass:m[80]+1,1,0,0});
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
    double expected=-1;
    if(bank==0)expected=.5*(h[0][row*columns+col]+h[1][row*columns+col]);
    else for(int dr=-int(dopplerBins);dr<=int(dopplerBins);++dr)for(int dc=-2;dc<=2;++dc)
     expected=std::max(expected,track_before_detect::pathMean(h,columns,axes,row,col,depth,dr,dc,scale));
    require(std::abs(actual-expected)<=1e-11*std::max(1.0,expected),"Production temporal bank differs from scalar oracle");
   }
  }
  if(available>=maximum)for(uint32_t row=0;row<rows;++row)for(uint32_t col=0;col<columns;++col){
   const double cut=observed[track_before_detect::bankCount(maximum)*cells+row*columns+col];
   require(bool(flags[row*columns+col]&1u)==(cut>=0&&cut>longestThreshold(row,col)),
    "Resident fractional validity/CFAR differs from valid-population scalar oracle");
  }
 }
 double longestThreshold(uint32_t row,uint32_t col){
  const auto* m=static_cast<const uint32_t*>(metadata.mapped);
  const uint32_t at=windows+14*col,bank=m[80];
  double train=0;uint32_t count=0;
  const auto add=[&](uint32_t c){const double value=observed[bank*cells+row*columns+c];
   if(std::isfinite(value)&&value>=0){train+=value;++count;}};
  for(uint32_t c=m[at];c<m[at+1];++c)add(c);
  for(uint32_t c=m[at+2];c<m[at+3];++c)add(c);
  if(!count)return std::numeric_limits<double>::infinity();
  return track_before_detect::cfarAlpha(track_before_detect::passPfa(1e-6,m[74],2,m[93],bank),count,m[74])*(train/count);
 }
 void detectorParity(const std::vector<double>& values,uint32_t maximum=8,uint32_t radius=1,uint32_t expectedSource=UINT32_MAX){
  require(values.size()==4*cells,"Synthetic bank shape invalid");
  run(History(8,std::vector<double>(cells,1)),0,maximum,maximum);
  auto* m=static_cast<uint32_t*>(metadata.mapped);auto* s=static_cast<uint32_t*>(scratch.mapped);
  std::fill(m,m+32,0);std::memset(s,0,size_t(scratch.bytes));
  std::copy(values.begin(),values.end(),static_cast<double*>(banks.mapped));
  m[47]=rowBounds;m[48]=colBounds;m[50]=3;put(m,68,radius);put(m,70,1);
  for(uint32_t r=0;r<rows;++r){m[rowBounds+2*r]=r;m[rowBounds+2*r+1]=r+1;}
  for(uint32_t c=0;c<columns;++c){m[colBounds+2*c]=uint32_t(std::max(0,int(c)-int(radius)+1));m[colBounds+2*c+1]=std::min(columns,c+radius);}
  for(Buffer* b:{&metadata,&scratch,&banks})b->flush(0,b->bytes);
  if(detectorKernels.empty()){const std::vector<Buffer*> buffers{&metadata,&fused,&scratch,&history,&power,&banks,&maps,&weights,&angles};
   for(const auto& source:gpu_postprocess::shaderSources())detectorKernels.push_back(std::make_unique<Kernel>(context,source.second.c_str(),buffers));}
  gpu_reference::CommandOwner command(context);command.allocate();VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"Bank parity recording");
  const auto dispatch=[&](uint32_t kernel,Push push){detectorKernels[kernel]->append(command.command,push);barrier(command.command);};
  for(uint32_t pass=1;pass<=m[80]+1;++pass){dispatch(1,{cells,pass,0,0,0});dispatch(13,{1,pass,0,0,0});
   dispatch(3,{cells,pass,1,1,0});dispatch(4,{cells,0,pass,0,0});dispatch(5,{cells,0,pass,0,0});}
  dispatch(14,{cells,0,0,0,0});for(uint32_t pass=1;pass<=m[80]+1;++pass)dispatch(14,{cells,1,pass,0,0});
  dispatch(14,{cells,2,0,0,0});dispatch(14,{cells,3,0,0,0});dispatch(14,{cells,4,0,0,0});
  gpu_postprocess::appendStableCompaction(command.command,detectorKernels,cells,0);dispatch(7,{cells,0,0,0,0});gpu_resident::hostRead(command.command);
  check(vkEndCommandBuffer(command.command),"Bank parity recording end");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
  check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Bank parity submit");
  check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"Bank parity fence");
  check(vkResetFences(context.device,1,&context.fence),"Bank parity fence reset");
  metadata.invalidate(0,metadata.bytes);scratch.invalidate(0,scratch.bytes);require(!m[0],"Independent bank detector numeric rejection");
  detection::IntegrationMetrics metrics;metrics.cpis=maximum;metrics.delayMotionBins=2;metrics.dopplerMotionBins=4;metrics.ready=true;
  metrics.evidence=std::make_shared<integration::TemporalEvidence::Result>();
  for(uint32_t pass=0;pass<=m[80];++pass){auto& bank=metrics.evidence->banks[pass];bank.depth=pass?2u<<(pass-1):2;bank.ready=true;
   bank.window.epoch=1;bank.window.newestSequence=maximum;bank.window.depth=uint8_t(bank.depth);
   for(uint32_t age=0;age<bank.depth;++age)bank.window.sequences[age]=maximum-age;
   bank.motion.resize(cells);bank.validCells.resize(cells);bank.detectorPower.resize(cells);bank.map=std::make_unique<Map<std::complex<double>>>(rows,columns);
   bank.map->doppler.assign(axes.begin(),axes.end());for(uint32_t c=0;c<columns;++c)bank.map->delay.push_back(c);
   Map<std::complex<double>>::MetricAccumulator noise;
   for(uint32_t i=0;i<cells;++i){const double value=values[pass*cells+i];const bool valid=std::isfinite(value)&&value>=0;
    bank.validCells[i]=valid;bank.detectorPower[i]=value;bank.map->data[i/columns][i%columns]={valid?std::sqrt(value):0,0};if(valid)noise.add(bank.map->data[i/columns][i%columns]);}
   noise.apply(*bank.map);
   CfarDetector1D raw(track_before_detect::passPfa(1e-6,maximum,2,4,pass),2,6,0,0,bank.depth);
   const auto rawDetections=raw.process(bank.map.get(),bank.validCells,nullptr,nullptr,
    std::numeric_limits<size_t>::max(),nullptr,nullptr,&bank.detectorPower);
   std::vector<uint8_t> accepted(cells);const auto rawDelay=rawDetections->get_delay(),rawDoppler=rawDetections->get_doppler();
   for(size_t j=0;j<rawDelay.size();++j)accepted[uint32_t(rawDoppler[j]+8)*columns+uint32_t(rawDelay[j])]=1;
   for(uint32_t i=0;i<cells;++i)require(bool(s[layout.bankCandidates+8*pass*cells+i]&1u)==bool(accepted[i]),
    "Resident pre-centroid CFAR differs from exact CPU bank power");}
  CfarDetector1D detector(1e-6,2,6,0,0);Centroid centroid(radius,1,1);Interpolate interpolation(true,true);
  auto expected=detection::detectProducts(*metrics.evidence->banks[0].map,detector,centroid,interpolation,metrics);
  require(m[11]==expected->get_nDetections(),"Resident final bank detector count differs from production CPU");
  if(expectedSource!=UINT32_MAX)require(m[11]==1,"Half-bin source fixture did not deduplicate to one candidate");
  std::vector<PostprocessDetection> actual(m[11]);
  static_assert(std::is_trivially_copyable_v<PostprocessDetection>);
  std::memcpy(static_cast<void*>(actual.data()),s+layout.records,actual.size()*sizeof(PostprocessDetection));
  const auto delays=expected->get_delay(),dopplers=expected->get_doppler(),snrs=expected->get_snr();
  for(size_t j=0;j<actual.size();++j){
   require(std::abs(actual[j].delay-delays[j])<1e-9&&std::abs(actual[j].doppler-dopplers[j])<1e-9,
    "Resident originating-bank interpolation or canonical candidate order differs from production CPU");
   require(std::abs(actual[j].snr-snrs[j])<1e-9,"Resident originating-bank SNR differs from production CPU");
   const uint32_t cell=s[layout.indices+j],qualified=(s[layout.flags+cell]>>3)&15;
   const auto source=temporal_candidate::sourceCell(cell,s[layout.flags+cell],rows,columns,maximum);
   if(expectedSource!=UINT32_MAX)require(source==expectedSource&&cell==expectedSource+1&&actual[j].delay==32.5,
    "Half-bin dedup changed originating complex-map cell or finalized key order");
   require(qualified==(1u<<(expected->get_evidence()[j].bank-1)),"Resident selected bank lineage differs from production CPU");}
 }
};

void exactPowerBoundaryParity(Fixture& fixture){
 fixture.run(History(8,std::vector<double>(cells,1)),0);
 auto* m=static_cast<uint32_t*>(fixture.metadata.mapped);auto* s=static_cast<uint32_t*>(fixture.scratch.mapped);
 auto* banks=static_cast<double*>(fixture.banks.mapped);
 constexpr uint32_t column=32,cell=8*columns+column;
 Map<std::complex<double>> map(rows,columns);map.doppler.assign(axes.begin(),axes.end());
 for(uint32_t c=0;c<columns;++c)map.delay.push_back(c);
 map.noisePower=0;
 // Include the dedicated two-CPI p.a=0 path as well as every mature bank.
 for(uint32_t pass:{0u,2u,3u,4u}){
  const uint32_t depth=pass<3?2:pass==3?4:8;
  const double pfa=pass?track_before_detect::passPfa(1e-6,8,2,4,pass-1):1e-6;
  const double threshold=track_before_detect::cfarAlpha(pfa,12,depth);
  CfarDetector1D detector(pfa,2,6,0,0,depth);
  m[74]=pass?8:2;m[28]=1;
  for(uint32_t c=0;c<columns;++c){const uint32_t at=Fixture::windows+14*c;
   put(m,at+6+2*(pass?pass-1:0),track_before_detect::cfarAlpha(pfa,m[at+4],depth));}
  for(double cut:{std::nextafter(threshold,0.0),threshold,
      std::nextafter(threshold,std::numeric_limits<double>::infinity())}){
   std::fill(m,m+28,0);std::memset(s,0,size_t(fixture.scratch.bytes));
   std::vector<double> power(cells,1);power[cell]=cut;
   for(uint32_t i=0;i<cells;++i){map.data[i/columns][i%columns]={std::sqrt(power[i]),0};
    if(pass)banks[(pass-1)*cells+i]=power[i];else std::memcpy(s+fixture.layout.powers+2*i,&power[i],8);}
   const auto expected=detector.process(&map,nullptr,nullptr,std::numeric_limits<size_t>::max(),nullptr,nullptr,&power);
   require(expected->get_nDetections()==(cut>threshold?1u:0u),"Exact CPU CFAR strict boundary changed");
   for(Buffer* b:{&fixture.metadata,&fixture.scratch,&fixture.banks})b->flush(0,b->bytes);
   gpu_reference::CommandOwner command(fixture.context);command.allocate();VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   check(vkBeginCommandBuffer(command.command,&begin),"CFAR boundary recording");fixture.cfar->append(command.command,{cells,pass,1,1,0});
   barrier(command.command);gpu_resident::hostRead(command.command);check(vkEndCommandBuffer(command.command),"CFAR boundary recording end");
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
   check(vkQueueSubmit(fixture.context.queue,1,&submit,fixture.context.fence),"CFAR boundary submit");
   check(vkWaitForFences(fixture.context.device,1,&fixture.context.fence,VK_TRUE,UINT64_C(30000000000)),"CFAR boundary fence");
   check(vkResetFences(fixture.context.device,1,&fixture.context.fence),"CFAR boundary fence reset");
   fixture.metadata.invalidate(0,fixture.metadata.bytes);fixture.scratch.invalidate(0,fixture.scratch.bytes);
   const uint32_t flag=pass?fixture.layout.bankCandidates+8*(pass-1)*cells+cell:fixture.layout.flags+cell;
   require(!m[0]&&bool(s[flag]&1u)==(cut>threshold)&&m[6]==expected->get_nDetections(),
    "Resident/CPU CFAR adjacent power boundary differs");
  }
 }
}

void spatialLineageFlags(Context& context){
 const auto layout=gpu_postprocess::layout(3,16);
 Buffer metadata(context,512,Fixture::host,Fixture::preferred),a(context,512,Fixture::host,Fixture::preferred),
  b(context,512,Fixture::host,Fixture::preferred),scratch(context,layout.bytes,Fixture::host,Fixture::preferred),
  post(context,512,Fixture::host,Fixture::preferred),clutter(context,512,Fixture::host,Fixture::preferred);
 for(Buffer* buffer:{&metadata,&a,&b,&scratch,&post,&clutter})std::memset(buffer->mapped,0,size_t(buffer->bytes));
 const auto sources=gpu_tracker::shaderSources();const auto end=sources[0].second.rfind(gpu_tracker::initializeBody);
 require(end!=std::string::npos,"Tracker spatial probe seam absent");
 const auto source=sources[0].second.substr(0,end)+"\nvoid main(){if(indexOf()==0u)m[100]=detectionSpatial(0u);}\n";
 Kernel probe(context,source.c_str(),{&metadata,&a,&b,&scratch,&post,&clutter});
 auto* p=static_cast<uint32_t*>(post.mapped);auto* s=static_cast<uint32_t*>(scratch.mapped);
 p[39]=layout.flags;p[41]=layout.indices;p[86]=layout.direction;
 s[layout.flags]=(3u<<8)|(5u<<17)|(2u<<28);
 for(uint32_t depth:{1u,2u,4u,8u}){p[74]=depth;
  for(Buffer* buffer:{&metadata,&a,&b,&scratch,&post,&clutter})buffer->flush(0,buffer->bytes);
  gpu_reference::CommandOwner command(context);command.allocate();VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"Spatial lineage recording");probe.append(command.command,{1,0,0,0,0});barrier(command.command);gpu_resident::hostRead(command.command);
  check(vkEndCommandBuffer(command.command),"Spatial lineage recording end");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
  check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Spatial lineage submit");
  check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"Spatial lineage fence");
  check(vkResetFences(context.device,1,&context.fence),"Spatial lineage fence reset");metadata.invalidate(0,metadata.bytes);
  require(static_cast<uint32_t*>(metadata.mapped)[100]==(depth==1?3u|(5u<<9)|(2u<<20):3u),
   "Temporal path lineage was interpreted as legacy fine-beam evidence");}
}

void adaptiveBearingParity(Context& context){
 using Oracle=::blah2::fusion::BearingRefinement;
 constexpr uint32_t size=5,count=size*size,source=12,channels=2,beams=4;
 const auto layout=gpu_postprocess::layout(size,size,4);
 Buffer metadata(context,512,Fixture::host,Fixture::preferred),unused(context,8,Fixture::host,Fixture::preferred),
  scratch(context,layout.bytes,Fixture::host,Fixture::preferred),
  maps(context,channels*count*8,Fixture::host,Fixture::preferred),weights(context,beams*channels*8,Fixture::host,Fixture::preferred);
 const auto shaders=gpu_postprocess::shaderSources();
 const auto shader=std::find_if(shaders.begin(),shaders.end(),[](const auto& value){return value.first=="refine";});
 require(shader!=shaders.end(),"Bearing production shader missing");
 Kernel kernel(context,shader->second.c_str(),{&metadata,&unused,&scratch,&unused,&unused,&unused,&maps,&weights,&unused});
 Oracle::Config config;config.channels=channels;config.beams=beams;config.method=1;
 config.frequencyHz=539e6;config.radiusM=.2;config.referenceAnchor=1;
 config.guardRadius=0;config.trainingRadius=1;config.diagonalLoading=.1f;
 config.measuredResponses={{1,0},{1,0},{1,0},{-1,0},{1,0},{0,1},{1,0},{0,-1}};
 std::array<double,8> scales;scales.fill(1);
 Oracle oracle(config,{{1,0},{1,0}},scales,false);
 for(double scale:{1.0,1e10,1e19,1e30})for(unsigned fault=0;fault<6;++fault){
  for(Buffer* buffer:{&metadata,&unused,&scratch,&maps,&weights})std::memset(buffer->mapped,0,size_t(buffer->bytes));
  std::vector<Map<std::complex<double>>> cpu;cpu.reserve(channels);
  for(uint32_t channel=0;channel<channels;++channel)cpu.emplace_back(size,size);
  unsigned sample=0;for(uint32_t r=1;r<=3;++r)for(uint32_t c=1;c<=3;++c)if(r!=2||c!=2){
   const bool first=++sample%2;cpu[0].data[r][c]={first?scale:0,0};cpu[1].data[r][c]={first?0:2*scale,0};}
  cpu[0].data[2][2]=cpu[1].data[2][2]={scale,0};
  if(fault==1)cpu[0].data[1][1]={std::numeric_limits<double>::quiet_NaN(),0};
  if(fault==2)cpu[0].data[1][1]={std::numeric_limits<double>::infinity(),0};
  if(fault==3)cpu[0].data[2][2]={std::numeric_limits<double>::quiet_NaN(),0};
  if(fault==4)cpu[0].data[2][2]={std::numeric_limits<double>::infinity(),0};
  if(fault==5)for(auto& map:cpu)for(uint32_t r=0;r<size;++r)for(uint32_t c=0;c<size;++c)if(r!=2||c!=2)map.data[r][c]={};
  const auto expected=oracle.refineAt({&cpu[0],&cpu[1]},2,2);
  auto* values=static_cast<std::complex<float>*>(maps.mapped);
  for(uint32_t channel=0;channel<channels;++channel)for(uint32_t r=0;r<size;++r)for(uint32_t c=0;c<size;++c)
   values[channel*count+c*size+(r+size/2+1)%size]=std::complex<float>(cpu[channel].data[r][c]);
  std::copy(oracle.calibratedWeights().begin(),oracle.calibratedWeights().end(),static_cast<std::complex<float>*>(weights.mapped));
  auto* m=static_cast<uint32_t*>(metadata.mapped);auto* s=static_cast<uint32_t*>(scratch.mapped);
  m[32]=size;m[33]=size;m[34]=count;m[40]=layout.keep;m[41]=layout.indices;m[78]=1;
  m[82]=channels;m[83]=beams;m[86]=layout.direction;m[87]=1;m[89]=1;
  std::memcpy(m+90,&config.diagonalLoading,4);s[layout.keep+source]=1;s[layout.indices+source]=source+1;
  for(Buffer* buffer:{&metadata,&unused,&scratch,&maps,&weights})buffer->flush(0,buffer->bytes);
  gpu_reference::CommandOwner command(context);command.allocate();VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"Bearing parity recording");kernel.append(command.command,{count,0,0,0,0});barrier(command.command);gpu_resident::hostRead(command.command);
  check(vkEndCommandBuffer(command.command),"Bearing parity recording end");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
  check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Bearing parity submit");
  check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"Bearing parity fence");
  check(vkResetFences(context.device,1,&context.fence),"Bearing parity fence reset");scratch.invalidate(0,scratch.bytes);
  require(s[layout.direction+source]==expected.spatial,"Adaptive bearing scale/nonfinite CPU-GPU parity differs");
  if(fault<1)require(expected.adaptiveUsed&&expected.spatial!=0,"Finite large-scale bearing lost adaptive evidence");
  if(fault>0&&fault<5)require(expected.spatial==0,"Nonfinite bearing input published spatial evidence");
 }
}

void temporalResetParity(Fixture& fixture){
 auto& context=fixture.context;PostprocessConfig config;
 config.enabled=true;config.detection=true;config.pfa=1e-6;config.guard=2;config.train=6;
 config.centroidDopplerResolution=1;config.integrationCpis=4;config.beamCount=1;
 config.dopplerAxis=axes;for(uint32_t c=0;c<columns;++c)config.delayAxis.push_back(c);
 auto* fused=static_cast<double*>(fixture.fused.mapped);for(uint32_t i=0;i<cells;++i){fused[2*i]=1;fused[2*i+1]=0;}
 fixture.fused.flush(0,fixture.fused.bytes);
 Buffer maps(context,cells*8,Fixture::host,Fixture::preferred),weights(context,4*8,Fixture::host,Fixture::preferred);
 std::memset(maps.mapped,0,size_t(maps.bytes));std::memset(weights.mapped,0,size_t(weights.bytes));
 maps.flush(0,maps.bytes);weights.flush(0,weights.bytes);
 gpu_postprocess::Stage stage(context,config,rows,columns,2400000,fixture.fused,fixture.scratch,fixture.history,&maps,&weights,1,4);
 const auto execute=[&](bool reset){gpu_reference::CommandOwner command(context);command.allocate();VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  check(vkBeginCommandBuffer(command.command,&begin),"Temporal reset recording");
  if(reset)stage.appendInputReset(command.command);else stage.append(command.command);
  check(vkEndCommandBuffer(command.command),"Temporal reset recording end");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
  check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Temporal reset submit");
  check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"Temporal reset fence");
  check(vkResetFences(context.device,1,&context.fence),"Temporal reset fence reset");if(reset)stage.completeInputReset();};
 PostprocessMetrics metrics;std::vector<double> display(cells);std::vector<PostprocessDetection> detections(cells);
 PostprocessOutputs out;out.metrics=&metrics;out.display=display.data();out.displayCount=cells;out.detections=detections.data();out.detectionCapacity=cells;
 for(unsigned epoch=0;epoch<2;++epoch){if(epoch)execute(true);
  for(uint32_t i=0;i<cells;++i)fused[2*i]=1;
  fixture.fused.flush(0,fixture.fused.bytes);
  for(uint32_t frame=1;frame<=4;++frame){execute(false);stage.copyOutputs(out);
   require(metrics.availableCpis==frame&&metrics.detectionCount==0,"Resident history reset/readiness differs from admitted CPI count");}
  fused[2*(8*columns+32)]=100;fixture.fused.flush(0,fixture.fused.bytes);execute(false);stage.copyOutputs(out);
  for(uint32_t i=0;i<cells;++i)fused[2*i]=0;
  fixture.fused.flush(0,fixture.fused.bytes);execute(false);stage.copyOutputs(out);
  require(metrics.detectionCount>0&&stage.evidenceStatus()[gpu_postprocess::refinedCandidates]==metrics.detectionCount,
   "Older-CPI detection failed when the current spatial map had no positive evidence");}
}

void beamHistoryParity(Fixture& fixture){
 for(const uint32_t depth:{4u,8u})for(const bool mismatch:{false,true}){
  History history(depth,std::vector<double>(cells));
  for(uint32_t age=0;age<depth;++age)for(uint32_t i=0;i<cells;++i)
   history[age][i]=1+(i*7+age*13)%23;
  for(uint32_t available=1;available<=depth;++available){
   fixture.run(history,-.566,depth,available,true,mismatch);
   integration::TemporalEvidence cpu({depth,2,4,-.566,8,0});
   integration::TemporalEvidence::Result expected;
   for(uint32_t sequence=1;sequence<=available;++sequence){
    const uint32_t age=available-sequence;
    Map<std::complex<double>> map(rows,columns);map.doppler.assign(axes.begin(),axes.end());
    for(uint32_t c=0;c<columns;++c)map.delay.push_back(c);
    for(uint32_t i=0;i<cells;++i)map.data[i/columns][i%columns]={std::sqrt(history[age][i]),0};
    std::vector<uint32_t> beams(cells,mismatch&&age?5u:1u);
    expected=cpu.process(map,17,sequence,nullptr,&beams);
   }
   require(expected.availableCpis==available,"CPU beam history silently reduced configured depth");
   for(uint32_t bank=0;bank<=track_before_detect::bankCount(depth);++bank){
    const auto& result=expected.banks[bank];
    require(result.ready==(result.depth<=available),"Beam-aware warmup readiness differs");
    if(!result.ready)continue;
    for(uint32_t i=0;i<cells;++i){
     const double actual=fixture.observed[bank*cells+i];
     const bool valid=result.validCells.empty()||result.validCells[i];
     require(valid==(actual>=0),"CPU/resident beam-path validity differs");
     if(valid){const double power=std::norm(result.map->data[i/columns][i%columns]);
      require(std::abs(power-actual)<1e-10*std::max(1.0,actual),"CPU/resident beam-history statistic differs");
      const auto lineage=result.lineageAt(i);
      require(lineage.epoch==17&&lineage.newestSequence==available&&lineage.depth==result.depth,
       "Beam-aware numerical bank lost source lineage");
     }
    }
   }
   cpu.reset();
   Map<std::complex<double>> reset(rows,columns);reset.doppler.assign(axes.begin(),axes.end());
   for(uint32_t c=0;c<columns;++c)reset.delay.push_back(c);
   std::vector<uint32_t> beams(cells,1);
   const auto fresh=cpu.process(reset,18,1,nullptr,&beams);
   require(fresh.availableCpis==1&&!fresh.banks[0].ready,"Beam history leaked across reset");
  }
 }
}

void twoCpiLineageParity(Fixture& fixture){
 auto& context=fixture.context;
 for(uint32_t motion:{0u,1u})for(int sign:{-1,1}){
  PostprocessConfig config;config.enabled=true;config.detection=true;config.pfa=1e-6;
  config.guard=2;config.train=6;config.centroidDelay=1;config.centroidDoppler=1;
  config.centroidDopplerResolution=1;config.integrationCpis=2;
  config.integrationDelayMotionBins=motion;config.integrationDopplerMotionBins=motion;
  config.dopplerAxis=axes;for(uint32_t c=0;c<columns;++c)config.delayAxis.push_back(c);
  gpu_postprocess::Stage stage(context,config,rows,columns,2400000,
   fixture.fused,fixture.scratch,fixture.history);
  detection::DetectorIntegration integration(2,motion);
  CfarDetector1D detector(1e-6,2,6,0,0,2);Centroid centroid(1,1,1);Interpolate interpolate(true,true);
  PostprocessMetrics metrics;std::vector<double> display(cells);
  std::vector<PostprocessDetection> actual(cells);std::vector<uint32_t> flags(cells),keys(cells);
  PostprocessOutputs out;out.metrics=&metrics;out.display=display.data();out.displayCount=cells;
  out.detections=actual.data();out.detectionCapacity=cells;out.decisionFlags=flags.data();out.decisionFlagCount=cells;
  out.detectionCells=keys.data();out.detectionCellCapacity=cells;
  for(uint32_t frame=0;frame<2;++frame){
   auto map=std::make_unique<Map<std::complex<double>>>(rows,columns);
   map->doppler.assign(axes.begin(),axes.end());for(uint32_t c=0;c<columns;++c)map->delay.push_back(c);
   auto* fused=static_cast<double*>(fixture.fused.mapped);
   const uint32_t peakRow=uint32_t(8-(frame?0:int(motion)*sign)),peakColumn=uint32_t(32-(frame?0:int(motion)*sign));
   for(uint32_t r=0;r<rows;++r)for(uint32_t c=0;c<columns;++c){
    const double value=(r==peakRow&&c==peakColumn)?1000:1;
    map->data[r][c]={value,0};fused[2*(r*columns+c)]=value;fused[2*(r*columns+c)+1]=0;}
   // A current zero with positive history is a usable integrated neighbor.
   if(frame){map->data[8][31]={0,0};fused[2*(8*columns+31)]=0;}
   fixture.fused.flush(0,fixture.fused.bytes);
   auto integrated=integration.process(map,1,frame*cells,(frame+1)*cells,true);
   auto expected=detection::detectProducts(*map,detector,centroid,interpolate,integrated);
   gpu_reference::CommandOwner command(context);command.allocate();VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   check(vkBeginCommandBuffer(command.command,&begin),"Two-CPI parity recording");stage.append(command.command);
   check(vkEndCommandBuffer(command.command),"Two-CPI parity recording end");
   VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
   check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Two-CPI parity submit");
   check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"Two-CPI parity fence");
   check(vkResetFences(context.device,1,&context.fence),"Two-CPI parity fence reset");stage.copyOutputs(out);
   require(metrics.availableCpis==frame+1&&metrics.detectionCount==expected->get_nDetections(),
    "Two-CPI CPU/resident warmup or detection count differs");
   if(frame)require(metrics.detectionCount>0,"Two-CPI lineage fixture produced no candidates");
   const auto delays=expected->get_delay(),dopplers=expected->get_doppler(),snrs=expected->get_snr();
   bool chosenMotion=false;
   for(uint32_t j=0;j<metrics.detectionCount;++j){const auto& lineage=expected->get_evidence()[j];const uint32_t flag=flags[keys[j]];
    require(std::abs(actual[j].delay-delays[j])<1e-9&&std::abs(actual[j].doppler-dopplers[j])<1e-9&&std::abs(actual[j].snr-snrs[j])<1e-9,
     "Two-CPI originating-bank numeric parity differs");
    require(((flag>>3)&15u)==(1u<<(lineage.bank-1))&&int((flag>>17)&7u)-2==lineage.delayMotion&&
     int((flag>>20)&63u)-16==lineage.dopplerMotion&&lineage.depth==2&&lineage.epoch==1&&
     lineage.newestSequence==2&&lineage.sequences[0]==2&&lineage.sequences[1]==1,
     "Two-CPI selected path/source lineage differs");
    chosenMotion=chosenMotion||(lineage.delayMotion==sign&&lineage.dopplerMotion==sign);}
   if(frame&&motion)require(chosenMotion,"Two-CPI fixture never credited its selected nonzero path");
  }
 }
}

void twoCpiNextafterTieParity(Fixture& fixture){
 constexpr uint32_t side=3,count=side*side,centre=4;
 const auto layout=gpu_postprocess::layout(side,side,2);
 for(Buffer* buffer:{&fixture.metadata,&fixture.fused,&fixture.scratch,&fixture.history,&fixture.power})
  std::memset(buffer->mapped,0,size_t(buffer->bytes));
 auto* metadata=static_cast<uint32_t*>(fixture.metadata.mapped);
 metadata[32]=side;metadata[33]=side;metadata[34]=count;
 metadata[36]=layout.levels;metadata[37]=layout.powers;metadata[39]=layout.flags;
 metadata[74]=2;metadata[76]=1;metadata[93]=1;
 auto* older=static_cast<double*>(fixture.history.mapped);
 const double nextAmplitude=std::nextafter(1.0,2.0);
 older[8]=1;older[7]=nextAmplitude*nextAmplitude;
 auto* fused=static_cast<double*>(fixture.fused.mapped);fused[2*centre]=2;
 for(Buffer* buffer:{&fixture.metadata,&fixture.fused,&fixture.scratch,&fixture.history,&fixture.power})
  buffer->flush(0,buffer->bytes);
 auto& context=fixture.context;
 gpu_reference::CommandOwner command(context);command.allocate();
 VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
 check(vkBeginCommandBuffer(command.command,&begin),"Two-CPI rounded tie begin");
 fixture.levels->append(command.command,{count,0,0,0,0});barrier(command.command);
 VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
 read.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;read.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
 vkCmdPipelineBarrier(command.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,
  0,1,&read,0,nullptr,0,nullptr);
 check(vkEndCommandBuffer(command.command),"Two-CPI rounded tie end");
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command.command;
 check(vkQueueSubmit(context.queue,1,&submit,context.fence),"Two-CPI rounded tie submit");
 check(vkWaitForFences(context.device,1,&context.fence,VK_TRUE,UINT64_C(30000000000)),"Two-CPI rounded tie fence");
 check(vkResetFences(context.device,1,&context.fence),"Two-CPI rounded tie fence reset");
 fixture.metadata.invalidate(0,fixture.metadata.bytes);fixture.scratch.invalidate(0,fixture.scratch.bytes);
 require(metadata[0]==0,"Two-CPI rounded tie production shader rejected fixture");
 const auto* scratch=static_cast<const uint32_t*>(fixture.scratch.mapped);
 const auto flags=scratch[layout.flags+centre];double actualPower=0;
 std::memcpy(&actualPower,scratch+layout.powers+2*centre,sizeof(actualPower));
 integration::TemporalEvidence cpu({2,1,1});
 Map<std::complex<double>> prior(side,side),current(side,side);
 prior.delay=current.delay={0,1,2};prior.doppler=current.doppler={-1,0,1};
 prior.data[2][2]={1,0};prior.data[2][1]={nextAmplitude,0};current.data[1][1]={2,0};
 cpu.process(prior,7,1);const auto expected=cpu.process(current,7,2);
 const auto lineage=expected.banks[1].lineageAt(centre);
 require(actualPower==2.5&&expected.banks[1].map->data[1][1].real()==std::sqrt(actualPower),
  "Two-CPI rounded tie mean power differs");
 require(lineage.depth==2&&lineage.delayMotion==-1&&lineage.dopplerMotion==-1&&
  int((flags>>17)&7u)-2==lineage.delayMotion&&int((flags>>20)&63u)-16==lineage.dopplerMotion,
  "Two-CPI rounded mean tie selected different CPU/resident source paths");
 std::cout<<"PASS two-CPI nextafter tie grid=3x3 rounded_mean=2.5 first_path=1 lineage_parity=1\n";
}

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
 spatialLineageFlags(context);
 adaptiveBearingParity(context);
 exactPowerBoundaryParity(fixture);
 temporalResetParity(fixture);
 beamHistoryParity(fixture);
 twoCpiLineageParity(fixture);
 twoCpiNextafterTieParity(fixture);
 std::vector<double> synthetic(4*cells,1);
 const uint32_t left=8*columns+32,right=left+1;
 synthetic[left]=1e5;synthetic[left-1]=4;
 synthetic[cells+left]=2e4;
 synthetic[2*cells+right]=1e7;synthetic[2*cells+right+1]=4;
 fixture.detectorParity(synthetic,4,1); // Opposing peaks survive their own bank interpolation.
 fixture.detectorParity(synthetic,4,2); // Final centroid compares finalized bank SNRs.
 synthetic.assign(4*cells,1);synthetic[12*columns+40]=1e6;synthetic[2*cells+4*columns+20]=1e7;
 fixture.detectorParity(synthetic,4); // Bank insertion order must not change association input order.
 synthetic.assign(4*cells,1);synthetic[left]=synthetic[right]=1e6;
 fixture.detectorParity(synthetic,4,1,left); // Exact half-bin tie retains source separately from finalized key.
 synthetic.assign(4*cells,1);for(uint32_t bank=0;bank<4;++bank)synthetic[bank*cells+left]=1e6;
 fixture.detectorParity(synthetic); // Equal bank SNR retains the first canonical originating bank.
 synthetic.assign(4*cells,-1);synthetic[left]=1e6;
 fixture.detectorParity(synthetic); // No valid training samples cannot manufacture a detection.
 synthetic.assign(4*cells,1);
 for(uint32_t c=0;c<columns;c+=2)synthetic[3*cells+8*columns+c]=-1;
 synthetic[3*cells+8*columns+1]=1e4;
 fixture.detectorParity(synthetic); // Partial validity recomputes both population and alpha.
 synthetic.assign(4*cells,std::numeric_limits<double>::quiet_NaN());fixture.detectorParity(synthetic);
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
 for(uint32_t i=0;i<cells;++i){require(fixture.observed[3*cells+i]<0,"Incompatible beam history admitted");
  require(!(fixture.flags[i]&1u),"Invalid motion history produced a detection");}
 fixture.run(h,-.566,8,8,true,true,4,false,1);
 for(uint32_t i=0;i<cells;++i)require(!(fixture.flags[i]&1u),"Negative incompatible baseline manufactured a detection");
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
  <<" banks=2,4,8 exact_gpu_offset_parity=1 originating_bank_parity=1 canonical_order=1 extra_trials=0 coherent_cpi_integration=0\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL fractional TBD: "<<e.what()<<'\n';return 1;}
