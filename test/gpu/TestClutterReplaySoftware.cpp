// Opt-in CPU Vulkan regression of the unchanged production two-stage FIR.
// Never selects physical hardware or produces physical/release receipts.
#include "../../src/process/ambiguity/VulkanBackend.cpp"
#include <cstdlib>
#include <unistd.h>
namespace c=blah2::clutter_replay;
blah2::Candidate software(blah2::Instance& instance){
 uint32_t count=0;blah2::check(vkEnumeratePhysicalDevices(instance.handle,&count,nullptr),"software count");
 std::vector<VkPhysicalDevice> devices(count);
 blah2::check(vkEnumeratePhysicalDevices(instance.handle,&count,devices.data()),"software discovery");
 for(auto device:devices){blah2::Candidate candidate{};candidate.physical=device;
  vkGetPhysicalDeviceProperties(device,&candidate.properties);
  if(candidate.properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU)continue;
  candidate.info={"test-only-software",candidate.properties.deviceName,UINT64_C(1)<<30};
  uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(device,&count,nullptr);
  std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(device,&count,queues.data());
  for(uint32_t i=0;i<count;++i)if(queues[i].queueCount&&(queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT)){
   candidate.queue=i;return candidate;}
 }throw std::runtime_error("CPU Vulkan is required; physical devices are forbidden");
}
int main(int argc,char** argv)try{
 const bool native=argc==3&&std::string(argv[2])=="--native";
 c::require((argc==2||native)&&std::string(argv[1])=="--software","Explicit --software [--native] required");
 char directory[]="/tmp/vectorwarp-clutter-software-XXXXXX";
 c::require(mkdtemp(directory),"Temporary directory failed");
 const std::string path=std::string(directory)+"/checkpoint";
 struct Cleanup{std::string path,directory;~Cleanup(){unlink(path.c_str());rmdir(directory.c_str());}} cleanup{path,directory};
 auto instance=std::make_shared<blah2::Instance>();blah2::Context context(instance,software(*instance));
 const blah2::GpuGeometry geometry{8,4,2,2,0,32,3,-1}; // 2-channel batches, 3 physical channels.
 blah2::ClutterPipeline pipeline(context,geometry,false,false,!native);
 c::Plan p;p.mode="export";p.path=path;p.identity=std::string(64,'a');p.sourceSha=std::string(40,'b');
 p.configSha=std::string(64,'c');p.warmFrame=2;p.epoch=1;p.samples=32;p.channels=3;p.taps=3;p.delayMin=-1;
 p.warmFirst=32;p.warmEnd=64;p.injectionFrame=3;p.injectionFirst=64;
 c::Frame frame;std::vector<std::complex<float>> reference(32),surveillance(96),output(96);
 for(size_t i=0;i<32;++i)reference[i]={float(int(i*i%31)-15)/16,float(int(i*7%17)-8)/16};
 for(size_t channel=0;channel<3;++channel)for(size_t i=0;i<32;++i)
  surveillance[channel*32+i]=float(channel+2)*reference[i]+std::complex<float>(float(i%3)/128,float(i%5)/256);
 const auto execute=[&](c::State& state,uint64_t number){
  frame={number,1,(number-1)*32,number*32};pipeline.replayCheckpoint(&state);
  state.begin(reference.data(),32,surveillance.data(),96);
  c::require(pipeline.beginFrame(reference.data(),32),"Production reference rejected");
  const bool accepted=blah2::processClutterBatches(32,3,2,surveillance.data(),96,output.data(),96,
   [&](uint32_t first,uint32_t,const auto* input,size_t count,auto* result){
    return pipeline.process(reference.data(),32,input,count,result,count,first);});
  state.complete(accepted);pipeline.endFrame();return output;
 };
 c::State exported(p,[&]{return frame;});execute(exported,1);const auto warm=execute(exported,2);
 auto bytes=c::read(path);p.mode="fixed";p.expectedSha=c::hash(bytes.data(),bytes.size());
 const auto checkpoint=c::Checkpoint::decode(bytes,p);
 // Independent linear convolution catches correlation/FIR workspace overlap,
 // including the padded FFT tail and the partial final channel batch.
 const auto checkFir=[&](const auto& actual){
  for(uint32_t channel=0;channel<p.channels;++channel)for(uint32_t i=0;i<p.samples;++i){
   std::complex<double> expected{};
   for(uint32_t tap=0;tap<p.taps&&tap<=i;++tap){
    const size_t at=size_t(channel)*p.taps+tap;
    expected+=std::complex<double>(reference[i-tap])*(checkpoint.initial[at]+
      std::complex<double>(checkpoint.correction[at]));
   }
   const double error=std::abs(std::complex<double>(actual[size_t(channel)*p.samples+i])-expected);
   c::require(std::isfinite(error)&&error<=1e-5*std::max(1.,std::abs(expected)),
     "Shared correlation/FIR storage differs from direct convolution");
  }
 };
 checkFir(warm);
 c::State fixed(p,[&]{return frame;});execute(fixed,1);c::require(execute(fixed,2)==warm,"Checkpoint warm replay differs");
 const auto first=execute(fixed,3);
 checkFir(first);
 c::require(first==warm,"Frozen checkpoint does not reproduce same-input two-stage FIR");
 for(auto& value:surveillance)value+=std::complex<float>(13,-7);
 c::require(execute(fixed,4)==first,"Frozen estimate depends on surveillance");
 for(auto& value:reference)value*=2.f;
 const auto changed=execute(fixed,5);
 checkFir(changed);
 for(size_t i=0;i<changed.size();++i)c::require(std::abs(changed[i]-2.f*first[i])<1e-5,
  "Frozen FIR retained stale reference spectra");
 std::fill(reference.begin(),reference.end(),std::complex<float>{});const auto zero=execute(fixed,6);
 checkFir(zero);
 for(auto value:zero)c::require(value==std::complex<float>{},"Frozen zero reference ran an irrelevant solve");
 const auto proof=fixed.proof();c::require(proof.fixed&&!proof.covariance&&!proof.factor&&!proof.solve,
  "Frozen pipeline performed fitting work");pipeline.replayCheckpoint(nullptr);
 std::cout<<"PASS portable q0/q1 checkpoint, partial batch, current-reference and solve bypass; physical_gpu_execution=0\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
