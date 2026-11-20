#include "GpuProcess.h"
#include "GpuResidentBackend.h"
#include "GpuWidebandProcess.h"
#include "GpuBackend.h"
#include "MacGpuCompatibility.h"
#include "GpuArtifactBinding.h"
#include "GpuWorkerLifetime.h"
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>

int workerMain(int argc, char** argv) {
  if(argc==2&&std::string(argv[1])=="--wideband-v1"){
#ifdef VECTORWARP_WIDEBAND_GPU
    blah2::applyMacGpuCompatibilityPolicy();
    std::unique_ptr<blah2::gpu_artifact_binding::Module> module;
    return blah2::wideband_gpu::runWorker([&](const blah2::wideband_gpu::Config& config,const std::string& device){
      module=std::make_unique<blah2::gpu_artifact_binding::Module>("blah2-gpu-wideband-vulkan.so","wideband");
      const auto create=module->factory<blah2::wideband_gpu::Create>("blah2_gpu_create_wideband",blah2::wideband_gpu::moduleAbi,device);
      return std::unique_ptr<blah2::wideband_gpu::Backend>(create(blah2::wideband_gpu::moduleAbi,&config,device.c_str()));
    });
#else
    if(!blah2::gpu_worker_lifetime::protectWorker())return 2;
    blah2::gpu_artifact_binding::workerStart();
    std::cerr<<"Wideband GPU module was not built for this platform cohort; use CPU channelization\n";return 2;
#endif
  }
  if(argc==2&&std::string(argv[1])=="--resident-v5"){
#ifdef VECTORWARP_RESIDENT_GPU
    std::unique_ptr<blah2::gpu_artifact_binding::Module> module;
    const int result=blah2::resident::runWorker([&](const blah2::resident::Config& config,const blah2::resident::Mapping& mapping){
      if(module)throw std::runtime_error("Resident module already loaded");
      module=std::make_unique<blah2::gpu_artifact_binding::Module>("blah2-gpu-resident-vulkan.so","resident");
      const auto create=module->factory<blah2::resident::Create>("blah2_gpu_create_resident",blah2::resident::moduleAbi,config.device);
      return std::unique_ptr<blah2::resident::Backend>(create(blah2::resident::moduleAbi,&config,&mapping));
    });
    return result;
#else
    if(!blah2::gpu_worker_lifetime::protectWorker())return 2;
    blah2::gpu_artifact_binding::workerStart();
    std::cerr<<"Resident GPU module was not built for this platform cohort; select cpu, auto or stage gpu acceleration explicitly\n";
    return 2;
#endif
  }
  if (argc == 2 && std::string(argv[1]) == "--driver-status") {
    // Keep the public standalone diagnostic usable without a parent recipe.
    // Only explicitly spawned diagnostics participate in replay receipts.
    if(blah2::gpu_worker_lifetime::expectedParent()>0){
      if(!blah2::gpu_worker_lifetime::protectWorker())return 2;
      blah2::gpu_artifact_binding::workerStart();
    }
    // Enumeration only: no logical device, FFT plan, IQ, or persistent cache.
    // The caller must bound this separate process, just like processing workers.
    blah2::applyMacGpuCompatibilityPolicy();
    std::string response;
    int code = 0;
    try {
      blah2::gpu_artifact_binding::Module diagnostic("blah2-gpu-vulkan.so","portable");
      const auto status=diagnostic.factory<blah2::GpuDriverStatus>("blah2_gpu_driver_status",blah2::GPU_ABI,"auto");
      response = status(1);
      if (response.size() > 32768) throw std::runtime_error("GPU driver diagnostic exceeded its output limit");
    } catch (const std::exception& error) {
      response = "{\"version\":1,\"available\":false,\"devices\":[],\"qualification\":\"not-run\",\"error\":" +
        blah2::gpuDiagnosticString(error.what()) + "}";
      code = 1;
    }
    std::cout << response << '\n';
    return code;
  }
  if (argc != 1) { std::cerr << "Usage: blah2-gpu-worker [--driver-status | --resident-v5 | --wideband-v1]\n";
  return 2; }
  std::unique_ptr<blah2::gpu_artifact_binding::Module> module;
  const int result = blah2::runGpuWorker([&](const blah2::GpuGeometry& geometry,
      const std::string& device) -> std::unique_ptr<blah2::GpuBackend> {
    blah2::applyMacGpuCompatibilityPolicy();
    module=std::make_unique<blah2::gpu_artifact_binding::Module>("blah2-gpu-vulkan.so","portable");
    const auto create=module->factory<blah2::GpuCreate>("blah2_gpu_create",blah2::GPU_ABI,device);
    return std::unique_ptr<blah2::GpuBackend>(create(blah2::GPU_ABI, &geometry, device.c_str()));
  });
  return result;
}

int main(int argc,char** argv){
  try{return workerMain(argc,argv);}
  catch(const std::exception& error){std::cerr<<"GPU worker: "<<error.what()<<'\n';return 2;}
}
