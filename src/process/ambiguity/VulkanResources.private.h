#pragma once
// Shared module-local Vulkan resources, FFT plans and common shaders, included
// after each backend's Context and check/allocation hooks. Callers select FFT
// precision; admission, allocation evidence and factory policy stay local.
gpu_memory::Properties memoryProperties(VkPhysicalDevice physical, VkInstance instance,
    const std::vector<VkExtensionProperties>& extensions) {
  VkPhysicalDeviceMemoryProperties source{};
  vkGetPhysicalDeviceMemoryProperties(physical, &source);
  gpu_memory::Properties result;
  for (uint32_t i = 0; i < source.memoryHeapCount; ++i)
    result.heaps.push_back({source.memoryHeaps[i].size,
      bool(source.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)});
  for (uint32_t i = 0; i < source.memoryTypeCount; ++i)
    result.types.push_back({source.memoryTypes[i].propertyFlags,
      source.memoryTypes[i].heapIndex});
#ifdef VK_EXT_memory_budget
  if (!instance || !std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
      return std::strcmp(extension.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
    })) return result;
  auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
    vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2"));
  if (!query) query = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
    vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
  if (!query) return result;
  VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
  properties.pNext = &budget;
  query(physical, &properties);
  for (uint32_t i = 0; i < result.heaps.size(); ++i) {
    const uint64_t limit = std::min(result.heaps[i].bytes, uint64_t(budget.heapBudget[i]));
    result.heaps[i].available = budget.heapUsage[i] < limit ? limit - budget.heapUsage[i] : 0;
  }
#endif
  return result;
}
struct Buffer {
  Context& context;
  VkBuffer handle = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  uint64_t bytes, allocationBytes = 0;
  VkMemoryPropertyFlags properties = 0;
  uint32_t heap = UINT32_MAX;
  Buffer(Context& ctx, uint64_t size, VkMemoryPropertyFlags required,
      VkMemoryPropertyFlags preferred = 0) : context(ctx), bytes(size) {
    try {
      VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      info.size = size;
      info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      check(vkCreateBuffer(ctx.device, &info, nullptr, &handle), "buffer allocation");
      VkMemoryRequirements requirements{};
      vkGetBufferMemoryRequirements(ctx.device, handle, &requirements);
      const auto view = memoryProperties(ctx.candidate.physical);
      const uint32_t type = gpu_memory::chooseType(requirements.memoryTypeBits,
        required, preferred, view);
      if (type == gpu_memory::noType)
        throw std::runtime_error(std::string("GPU has no suitable memory; ") + memoryFailureAction);
      this->properties = view.types[type].flags;
      heap = view.types[type].heap;
      allocationBytes = requirements.size;
      VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize = requirements.size; allocation.memoryTypeIndex = type;
      check(budgetAllocateMemory(ctx.device, &allocation, nullptr, &memory), "memory allocation");
      check(vkBindBufferMemory(ctx.device, handle, memory, 0), "memory binding");
      if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        check(vkMapMemory(ctx.device, memory, 0, allocationBytes, 0, &mapped), "host mapping");
    } catch (...) { release(); throw; }
  }
  void synchronize(uint64_t offset, uint64_t size, bool flush) {
    if (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) return;
    const auto range = gpu_memory::alignedRange(offset, size, allocationBytes,
      context.candidate.properties.limits.nonCoherentAtomSize);
    if ((!range.bytes && !range.whole) || !mapped)
      throw std::runtime_error(std::string("GPU mapped-memory range is invalid; ") + memoryFailureAction);
    VkMappedMemoryRange memoryRange{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    memoryRange.memory = memory; memoryRange.offset = range.offset;
    memoryRange.size = range.whole ? VK_WHOLE_SIZE : range.bytes;
    check(flush ? vkFlushMappedMemoryRanges(context.device, 1, &memoryRange) :
      vkInvalidateMappedMemoryRanges(context.device, 1, &memoryRange),
      flush ? "host-memory flush" : "host-memory invalidate");
  }
  void flush(uint64_t offset, uint64_t size) { synchronize(offset, size, true); }
  void invalidate(uint64_t offset, uint64_t size) { synchronize(offset, size, false); }
  void release() {
    if (mapped) vkUnmapMemory(context.device, memory);
    if (handle) vkDestroyBuffer(context.device, handle, nullptr);
    if (memory) budgetFreeMemory(context.device, memory, nullptr);
  }
  ~Buffer() { release(); }
};

struct Push { uint32_t count, range, doppler, delays;
int32_t delayMin; };
struct Kernel {
  Context& context;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  uint32_t pushBytes=sizeof(Push);
  Kernel(Context& ctx, const char* source, const std::vector<Buffer*>& buffers,
      uint32_t requestedPushBytes=sizeof(Push)) : context(ctx),pushBytes(requestedPushBytes) {
    if(!pushBytes||pushBytes>128||pushBytes%4)throw std::invalid_argument("GPU push constant shape invalid");
    try {
      glslang_input_t input{};
      input.language = GLSLANG_SOURCE_GLSL;
      input.stage = GLSLANG_STAGE_COMPUTE;
      input.client = GLSLANG_CLIENT_VULKAN;
      input.client_version = GLSLANG_TARGET_VULKAN_1_0;
      input.target_language = GLSLANG_TARGET_SPV;
      input.target_language_version = GLSLANG_TARGET_SPV_1_0;
      input.code = source;
      input.default_version = 450;
      input.default_profile = GLSLANG_NO_PROFILE;
      input.messages = static_cast<glslang_messages_t>(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
      input.resource = glslang_default_resource();
      auto shaderSource = std::unique_ptr<glslang_shader_t, decltype(&glslang_shader_delete)>(
        glslang_shader_create(&input), glslang_shader_delete);
      if (!shaderSource || !glslang_shader_preprocess(shaderSource.get(), &input) ||
          !glslang_shader_parse(shaderSource.get(), &input)) {
        const char* info = shaderSource ? glslang_shader_get_info_log(shaderSource.get()) : nullptr;
        const char* debug = shaderSource ? glslang_shader_get_info_debug_log(shaderSource.get()) : nullptr;
        std::string reason = "GPU shader compilation failed";
        if (info && *info) reason += std::string(": ") + info;
        if (debug && *debug) reason += std::string("; ") + debug;
        throw std::runtime_error(reason);
      }
      auto program = std::unique_ptr<glslang_program_t, decltype(&glslang_program_delete)>(
        glslang_program_create(), glslang_program_delete);
      if (!program) throw std::runtime_error("GPU shader compiler allocation failed");
      glslang_program_add_shader(program.get(), shaderSource.get());
      if (!glslang_program_link(program.get(), input.messages)) {
        const char* info = glslang_program_get_info_log(program.get());
        const char* debug = glslang_program_get_info_debug_log(program.get());
        std::string reason = "GPU shader link failed";
        if (info && *info) reason += std::string(": ") + info;
        if (debug && *debug) reason += std::string("; ") + debug;
        throw std::runtime_error(reason);
      }
      glslang_program_SPIRV_generate(program.get(), input.stage);
      const auto words = glslang_program_SPIRV_get_size(program.get());
      if (!words) throw std::runtime_error("GPU shader generation failed");
      VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shaderInfo.codeSize = words * sizeof(uint32_t);
      shaderInfo.pCode = glslang_program_SPIRV_get_ptr(program.get());
      check(vkCreateShaderModule(ctx.device, &shaderInfo, nullptr, &shader), "shader creation");
      std::vector<VkDescriptorSetLayoutBinding> bindings(buffers.size());
      for (uint32_t i = 0; i < buffers.size(); ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      setInfo.bindingCount = bindings.size();
      setInfo.pBindings = bindings.data();
      check(vkCreateDescriptorSetLayout(ctx.device, &setInfo, nullptr, &setLayout), "descriptor layout");
      VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
      VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
      layoutInfo.setLayoutCount = 1;
      layoutInfo.pSetLayouts = &setLayout;
      layoutInfo.pushConstantRangeCount = 1;
      layoutInfo.pPushConstantRanges = &range;
      check(vkCreatePipelineLayout(ctx.device, &layoutInfo, nullptr, &layout), "pipeline layout");
      VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipelineInfo.layout = layout;
      pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      pipelineInfo.stage.module = shader;
      pipelineInfo.stage.pName = "main";
      check(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "compute pipeline");
      VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(buffers.size())};
      VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      poolInfo.maxSets = 1;
      poolInfo.poolSizeCount = 1;
      poolInfo.pPoolSizes = &poolSize;
      check(vkCreateDescriptorPool(ctx.device, &poolInfo, nullptr, &pool), "descriptor pool");
      VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      allocation.descriptorPool = pool;
      allocation.descriptorSetCount = 1;
      allocation.pSetLayouts = &setLayout;
      check(vkAllocateDescriptorSets(ctx.device, &allocation, &set), "descriptor allocation");
      for (uint32_t i = 0; i < buffers.size(); ++i) {
        VkDescriptorBufferInfo bufferInfo{buffers[i]->handle, 0, buffers[i]->bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.dstBinding = i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(ctx.device, 1, &write, 0, nullptr);
      }
    } catch (...) { release();
    throw; }
  }
  void release() {
    if (pipeline) vkDestroyPipeline(context.device, pipeline, nullptr);
    if (shader) vkDestroyShaderModule(context.device, shader, nullptr);
    if (layout) vkDestroyPipelineLayout(context.device, layout, nullptr);
    if (pool) vkDestroyDescriptorPool(context.device, pool, nullptr);
    if (setLayout) vkDestroyDescriptorSetLayout(context.device, setLayout, nullptr);
  }
  ~Kernel() { release(); }
  // Most resident shaders use 128 lanes.  A narrowly specialized shader may
  // use a smaller, explicitly supplied workgroup without making the generic
  // dispatch geometry lie about its invocation count.
  void append(VkCommandBuffer command, Push push, uint32_t workgroupSize=128u) {
    if(!workgroupSize)throw std::invalid_argument("GPU workgroup size is zero");
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const uint32_t groups = (push.count + workgroupSize - 1u) / workgroupSize;
    const uint32_t x = std::min(groups, 65535u);
    vkCmdDispatch(command, x, (groups + x - 1) / x, 1);
  }
  template<class T>void appendWide(VkCommandBuffer command,const T& push,uint32_t count){
    static_assert(std::is_trivially_copyable<T>::value,"GPU push constants must be trivial");
    if(sizeof(T)!=pushBytes)throw std::logic_error("GPU push constant ABI differs");
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(T),&push);
    const uint32_t groups=(count+127)/128,x=std::min(groups,65535u);
    if(groups)vkCmdDispatch(command,x,(groups+x-1)/x,1);
  }
};
constexpr const char* multiplySource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference { vec2 ref[]; };
layout(binding=1) buffer Surveillance { vec2 surv[]; };
layout(push_constant) uniform Parameters { uint count; uint range; uint doppler; uint delays; int delayMin; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  vec2 a = surv[i], b = ref[i % (p.range * p.doppler)];
  surv[i] = vec2(a.x*b.x+a.y*b.y, a.y*b.x-a.x*b.y) / float(p.range);
})glsl";
constexpr const char* gatherSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Surveillance { vec2 surv[]; };
layout(binding=1) writeonly buffer Doppler { vec2 result[]; };
layout(push_constant) uniform Parameters { uint count; uint range; uint doppler; uint delays; int delayMin; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  uint channel = i / (p.delays * p.doppler);
  uint delay = (i / p.doppler) % p.delays, d = i % p.doppler;
  int lag = int(delay) + p.delayMin;
  uint bin = uint(lag < 0 ? int(p.range) + lag : lag);
  result[i] = surv[(channel * p.doppler + d) * p.range + bin];
})glsl";
void barrier(VkCommandBuffer command) {
  VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
}
#ifdef __APPLE__
constexpr const char* chirpPrepareSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Input { vec2 inputData[]; };
layout(binding=1) writeonly buffer Work { vec2 workData[]; };
layout(binding=2) readonly buffer Chirp { vec2 chirp[]; };
layout(push_constant) uniform Parameters { uint count; uint n; uint padded; uint batches; int direction; } p;
void main() {
  uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
  if(i>=p.count) return;
  uint j=i%p.padded;
  if(j>=p.n) { workData[i]=vec2(0); return; }
  vec2 a=inputData[(i/p.padded)*p.n+j], b=chirp[j];
  if(p.direction>0) b.y=-b.y;
  workData[i]=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x);
})glsl";
constexpr const char* chirpProductSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) buffer Work { vec2 workData[]; };
layout(binding=1) readonly buffer Spectrum { vec2 spectrum[]; };
layout(push_constant) uniform Parameters { uint count; uint n; uint padded; uint batches; int direction; } p;
void main() {
  uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
  if(i>=p.count) return;
  vec2 a=workData[i], b=spectrum[(p.direction>0?p.padded:0)+i%p.padded];
  workData[i]=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x);
})glsl";
constexpr const char* chirpFinishSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Work { vec2 workData[]; };
layout(binding=1) writeonly buffer Output { vec2 outputData[]; };
layout(binding=2) readonly buffer Chirp { vec2 chirp[]; };
layout(push_constant) uniform Parameters { uint count; uint n; uint padded; uint batches; int direction; } p;
void main() {
  uint i=gl_GlobalInvocationID.x+gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128;
  if(i>=p.count) return;
  uint j=i%p.n;
  vec2 a=workData[(i/p.n)*p.padded+j], b=chirp[j];
  if(p.direction>0) b.y=-b.y;
  outputData[i]=vec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x)/float(p.padded);
})glsl";

#endif // __APPLE__

struct NativePlan {
  VkFFTApplication app{};
  Buffer& buffer_;
  NativePlan(Context& context, Buffer& buffer, uint32_t length, uint32_t batches,
      bool twiddleLut = false, bool fp64 = false) : buffer_(buffer) {
    VkFFTConfiguration config{};
    config.FFTdim = 1; config.size[0] = length; config.numberBatches = batches;
    config.doublePrecision = fp64 ? 1 : 0;
    // VkFFT's FP32 defaults calculate twiddles on NVIDIA/AMD but use a LUT on
    // Intel. Clutter prefers the bounded LUT candidate; a capacity-selected
    // native candidate must pass the same cancellation/FP64 qualification.
    // Ambiguity retains the library's native tuning.
    if (twiddleLut) config.useLUT = 1;
    config.device = &context.device; config.queue = &context.queue;
    config.fence = &context.fence; config.commandPool = &context.pool;
    config.physicalDevice = &context.candidate.physical;
    // Bind at append time, after VkFFT has allocated prime-length FFT tables.
    // Binding during initialization can leave descriptors pointing at null LUTs.
    config.bufferSize = &buffer.bytes;
    config.isCompilerInitialized = 1;
    startupTrace(("VkFFT plan begin length=" + std::to_string(length) + " batches=" + std::to_string(batches)).c_str());
    const auto result = initializeVkFFT(&app, config);
    startupTrace(("VkFFT plan complete result=" + std::to_string(result)).c_str());
    if (result != VKFFT_SUCCESS) { deleteVkFFT(&app); checkFft(result); }
  }
  ~NativePlan() { deleteVkFFT(&app); }
  void append(VkCommandBuffer command, int direction) {
    VkFFTLaunchParams params{}; params.commandBuffer = &command;
    params.buffer = &buffer_.handle;
    checkFft(VkFFTAppend(&app, direction, &params));
  }
};
#ifdef __APPLE__
// Native VkFFT prime plans were intermittently incorrect through MoltenVK.
// Express Bluestein explicitly using smooth-length GPU FFTs. Only constant
// chirp spectra are generated in FP64 on the CPU at setup. The separate
// clutter baseline also uses CPU FFTW; ambiguity frame transforms stay on GPU.
struct PortablePrimePlan {
  uint32_t length_, batches_, padded_;
  std::unique_ptr<Buffer> work_, chirp_, spectrum_;
  std::unique_ptr<NativePlan> fft_;
  std::unique_ptr<Kernel> prepare_, product_, finish_;
  PortablePrimePlan(Context& context, Buffer& buffer, uint32_t length, uint32_t batches)
    : length_(length), batches_(batches), padded_(gpu_memory::nextSmooth(2ULL*length-1)) {
    const uint64_t elements=uint64_t(padded_)*batches;
    if (!padded_ || elements>UINT32_MAX/2 ||
        elements*sizeof(std::complex<float>)>context.candidate.properties.limits.maxStorageBufferRange)
      throw std::runtime_error(std::string("GPU prime FFT capacity is too small; ") + memoryFailureAction);
    work_=std::make_unique<Buffer>(context,elements*8,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const auto host=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const auto preferred=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    chirp_=std::make_unique<Buffer>(context,uint64_t(length)*8,host,preferred);
    spectrum_=std::make_unique<Buffer>(context,uint64_t(padded_)*16,host,preferred);
    std::vector<std::complex<double>> chirps(length), input(padded_), output(padded_);
    auto* chirp=static_cast<std::complex<float>*>(chirp_->mapped);
    auto* spectrum=static_cast<std::complex<float>*>(spectrum_->mapped);
    const double pi=std::acos(-1.);
    for (uint32_t i=0;i<length;++i) {
      // Reduce the integer phase before conversion to avoid large-angle error.
      const uint64_t phase=uint64_t(i)*i%(2ULL*length);
      chirps[i]=std::polar(1.,-pi*double(phase)/length);
      chirp[i]=static_cast<std::complex<float>>(chirps[i]);
    }
    auto plan=clutter_refinement::makePlan(padded_,input.data(),output.data(),FFTW_FORWARD);
    for (unsigned inverse=0;inverse<2;++inverse) {
      std::fill(input.begin(),input.end(),std::complex<double>{}); input[0]=1.;
      for (uint32_t i=1;i<length;++i)
        input[i]=input[padded_-i]=inverse?chirps[i]:std::conj(chirps[i]);
      fftw_execute(plan.get());
      for (uint32_t i=0;i<padded_;++i)
        spectrum[inverse*padded_+i]=static_cast<std::complex<float>>(output[i]);
    }
    plan.reset();
    chirp_->flush(0,chirp_->bytes); spectrum_->flush(0,spectrum_->bytes);
    fft_=std::make_unique<NativePlan>(context,*work_,padded_,batches,true);
    prepare_=std::make_unique<Kernel>(context,chirpPrepareSource,
      std::vector<Buffer*>{&buffer,work_.get(),chirp_.get()});
    product_=std::make_unique<Kernel>(context,chirpProductSource,
      std::vector<Buffer*>{work_.get(),spectrum_.get()});
    finish_=std::make_unique<Kernel>(context,chirpFinishSource,
      std::vector<Buffer*>{work_.get(),&buffer,chirp_.get()});
    startupTrace(("portable prime FFT length="+std::to_string(length)+
      " convolution="+std::to_string(padded_)).c_str());
  }
  void append(VkCommandBuffer command,int direction) {
    VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostWrite.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
    hostWrite.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&hostWrite,0,nullptr,0,nullptr);
    Push push{padded_*batches_,length_,padded_,batches_,direction};
    prepare_->append(command,push); barrier(command);
    fft_->append(command,-1); barrier(command);
    product_->append(command,push); barrier(command);
    fft_->append(command,1); barrier(command);
    push.count=length_*batches_;
    finish_->append(command,push); barrier(command);
  }
};
#endif
struct Plan {
  std::unique_ptr<NativePlan> native_;
#ifdef __APPLE__
  std::unique_ptr<PortablePrimePlan> portable_;
#endif
  Plan(Context& context,Buffer& buffer,uint32_t length,uint32_t batches,
      bool twiddleLut=false) {
#ifdef __APPLE__
    uint32_t rest=length;
    for (uint32_t radix:{2u,3u,5u,7u,11u,13u}) while(rest%radix==0) rest/=radix;
    if (context.portability && rest!=1) {
      portable_=std::make_unique<PortablePrimePlan>(context,buffer,length,batches);
      return;
    }
#endif
    native_=std::make_unique<NativePlan>(context,buffer,length,batches,twiddleLut);
  }
  void append(VkCommandBuffer command,int direction) {
#ifdef __APPLE__
    if (portable_) { portable_->append(command,direction); return; }
#endif
    native_->append(command,direction);
  }
};
constexpr const char* clutterProductsSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference { vec2 ref[]; };
layout(binding=1) readonly buffer Surveillance { vec2 surv[]; };
layout(binding=2) writeonly buffer Cross { vec2 crosscorr[]; };
layout(push_constant) uniform Parameters { uint count; uint samples; uint channels; uint unused; int unused2; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  vec2 x = ref[i % p.samples], y = surv[i];
  crosscorr[i] = vec2(y.x*x.x+y.y*x.y, y.y*x.x-y.x*x.y);
})glsl";
constexpr const char* clutterMultiplySource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Reference { vec2 ref[]; };
layout(binding=1) readonly buffer Weights { vec2 weights[]; };
layout(binding=2) writeonly buffer Filtered { vec2 filtered[]; };
layout(push_constant) uniform Parameters { uint count; uint fftLength; uint channels; uint unused; int unused2; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  vec2 a = ref[i % p.fftLength], b = weights[i];
  filtered[i] = vec2(a.x*b.x-a.y*b.y, a.x*b.y+a.y*b.x);
})glsl";
constexpr const char* clutterNormalizeSource = R"glsl(#version 450
layout(local_size_x=128) in;
layout(binding=0) readonly buffer Filtered { vec2 filtered[]; };
layout(binding=1) writeonly buffer Estimate { vec2 estimate[]; };
layout(push_constant) uniform Parameters { uint count; uint samples; uint fftLength; uint unused; int unused2; } p;
void main() {
  uint i = gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * 128;
  if (i >= p.count) return;
  uint channel = i / p.samples, sampleIndex = i % p.samples;
  estimate[i] = filtered[channel * p.fftLength + sampleIndex] / float(p.fftLength);
})glsl";
