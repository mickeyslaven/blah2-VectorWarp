// Portable batched ambiguity processing. Uses Vulkan compute, not graphics or
// vendor-specific CUDA/ROCm runtimes. VkFFT supplies the FFT kernels; the two
// small shaders below implement the same correlation/gather as CPU Ambiguity.
#include "GpuBackend.h"
#include "ClutterReplayCheckpoint.private.h"
#include "GpuClutterBatch.h"
#include "GpuMemory.h"
#include "ReplayGpuQualification.h"
#include "GpuClutterRefinement.h"
#include "GpuClutterSolver.h"
#include "GpuClutterProfile.h"
#include <vulkan/vulkan.h>
#include <glslang/Include/glslang_c_interface.h>
#if __has_include(<glslang/Public/resource_limits_c.h>)
#include <glslang/Public/resource_limits_c.h>
#else
// glslang 11 (Ubuntu 22.04) exports this C API but does not install its header.
extern "C" const glslang_resource_t* glslang_default_resource(void);
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#ifdef __APPLE__
#include <fftw3.h>
#endif

// VkFFT is header-only. Route its own device allocations through the same
// budget as our buffers, including transient upload and prime-length scratch.
// The definitions below call the real Vulkan functions after these macros end.
static VkResult budgetAllocateMemory(VkDevice, const VkMemoryAllocateInfo*,
  const VkAllocationCallbacks*, VkDeviceMemory*);
static void budgetFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*);
#define vkAllocateMemory budgetAllocateMemory
#define vkFreeMemory budgetFreeMemory
#include <vkFFT.h>
#undef vkFreeMemory
#undef vkAllocateMemory

namespace {
struct DeviceAllocations {
  struct Entry { uint32_t type; uint64_t bytes; };
  blah2::gpu_memory::AllocationBudget budget;
  std::unordered_map<VkDeviceMemory, Entry> allocations;
  DeviceAllocations(uint64_t limit, blah2::gpu_memory::Properties properties,
      bool separateHostHeaps = false)
    : budget(limit, std::move(properties), separateHostHeaps) {}
};
std::mutex allocationsMutex;
std::unordered_map<VkDevice, DeviceAllocations> deviceAllocations;
}

static VkResult budgetAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
  std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto found = deviceAllocations.find(device);
  if (found == deviceAllocations.end()) return VK_ERROR_INITIALIZATION_FAILED;
  auto& state = found->second;
  *memory = VK_NULL_HANDLE;
  if (!state.budget.reserve(info->memoryTypeIndex, info->allocationSize)) {
    if(std::getenv("BLAH2_REPLAY_GPU_QUALIFICATION"))
      blah2::replay_gpu_qualification::Worker("portable").allocationRejected(info->allocationSize,state.budget.limit());
    if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
      std::cerr << "GPU allocation budget rejected bytes=" << info->allocationSize
        << " type=" << info->memoryTypeIndex << " used=" << state.budget.used()
        << " limit=" << state.budget.limit()
        << " device_used=" << state.budget.deviceUsed()
        << " device_limit=" << state.budget.deviceLimit() << '\n';
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  const VkResult result = vkAllocateMemory(device, info, callbacks, memory);
  if (result == VK_SUCCESS) {
    try {
      state.allocations.emplace(*memory,
        DeviceAllocations::Entry{info->memoryTypeIndex, info->allocationSize});
      return result;
    } catch (...) {
      vkFreeMemory(device, *memory, callbacks); *memory = VK_NULL_HANDLE;
      state.budget.release(info->memoryTypeIndex, info->allocationSize);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  state.budget.release(info->memoryTypeIndex, info->allocationSize);
  return result;
}
static void budgetFreeMemory(VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks* callbacks) {
  std::lock_guard<std::mutex> lock(allocationsMutex);
  const auto found = deviceAllocations.find(device);
  if (found != deviceAllocations.end()) {
    auto& state = found->second;
    const auto allocation = state.allocations.find(memory);
    if (allocation != state.allocations.end()) {
      state.budget.release(allocation->second.type, allocation->second.bytes);
      state.allocations.erase(allocation);
    }
  }
  vkFreeMemory(device, memory, callbacks);
}

namespace blah2 {
namespace {
gpu_memory::Properties memoryProperties(VkPhysicalDevice physical);
class ClutterRejected final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
static_assert(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT == gpu_memory::deviceLocal);
static_assert(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT == gpu_memory::hostVisible);
static_assert(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT == gpu_memory::hostCoherent);
static_assert(VK_MEMORY_PROPERTY_HOST_CACHED_BIT == gpu_memory::hostCached);
void startupTrace(const char* stage) {
  if (!std::getenv("BLAH2_GPU_DIAGNOSTICS")) return;
  std::cerr << "GPU startup " << std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count() << " ms: " << stage << std::endl;
}
void check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string("GPU ") + operation + " failed (" +
      std::to_string(result) + "); using CPU");
}
void checkFft(VkFFTResult result) {
  if (result != VKFFT_SUCCESS)
    throw std::runtime_error("GPU FFT failed (" + std::to_string(result) + "); using CPU");
}
struct Instance {
  VkInstance handle = VK_NULL_HANDLE;
  bool diagnosticProperties = false;
  explicit Instance(bool diagnostic = false) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "blah2"; app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    uint32_t count = 0;
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "extension discovery");
    std::vector<VkExtensionProperties> extensions(count);
    check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()), "extension discovery");
    const auto supports = [&](const char* name) {
      return std::any_of(extensions.begin(), extensions.end(), [&](const auto& value) {
        return std::strcmp(value.extensionName, name) == 0;
      });
    };
    std::vector<const char*> enabled;
    const char* properties = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    if (supports(properties)) {
      enabled.push_back(properties);
      diagnosticProperties = diagnostic;
    }
#ifdef VK_KHR_portability_enumeration
    // Portability drivers (including MoltenVK) are hidden by the loader unless
    // both the extension and enumeration flag are requested. Detect at runtime
    // so this also remains correct on native Vulkan implementations.
    if (supports(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
      enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
      info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    startupTrace("vkCreateInstance begin");
    check(vkCreateInstance(&info, nullptr, &handle), "driver initialization");
    startupTrace("vkCreateInstance complete");
  }
  ~Instance() { if (handle) vkDestroyInstance(handle, nullptr); }
};
struct Candidate {
  VkPhysicalDevice physical;
  VkPhysicalDeviceProperties properties;
  GpuDevice info;
  uint32_t queue;
};
std::vector<Candidate> enumerate(Instance& instance) {
  startupTrace("device enumeration begin");
  uint32_t count = 0;
  check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "device discovery");
  std::vector<VkPhysicalDevice> devices(count);
  check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "device discovery");
  std::vector<Candidate> result;
  for (size_t index = 0; index < count; ++index) {
    Candidate item{}; item.physical = devices[index];
    vkGetPhysicalDeviceProperties(item.physical, &item.properties);
    // llvmpipe/lavapipe and other software drivers must never be called GPU acceleration.
    const auto type = item.properties.deviceType;
    if (type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
        type != VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU &&
        type != VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) continue;
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(item.physical, &memory);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
      if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        bytes = std::max(bytes, uint64_t(memory.memoryHeaps[i].size));
    item.info = {std::to_string(item.properties.vendorID) + ":" +
      std::to_string(item.properties.deviceID) + ":" + std::to_string(index),
      item.properties.deviceName, bytes};
    uint32_t queues = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(item.physical, &queues, nullptr);
    std::vector<VkQueueFamilyProperties> families(queues);
    vkGetPhysicalDeviceQueueFamilyProperties(item.physical, &queues, families.data());
    item.queue = UINT32_MAX;
    for (uint32_t q = 0; q < queues; ++q)
      if (families[q].queueCount && (families[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        item.queue = q;
        if (!(families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break;
      }
    if (item.queue != UINT32_MAX) result.push_back(item);
  }
  std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    const auto rank = [](VkPhysicalDeviceType type) {
      return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 :
        type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 2;
    };
    if (a.properties.deviceType != b.properties.deviceType)
      return rank(a.properties.deviceType) < rank(b.properties.deviceType);
    return a.info.memoryBytes > b.info.memoryBytes;
  });
  startupTrace("device enumeration complete");
  return result;
}
struct Context {
  std::shared_ptr<Instance> instance;
  Candidate candidate;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool compiler = false;
  bool portability = false;
  Context(std::shared_ptr<Instance> owner, Candidate selected)
    : instance(std::move(owner)), candidate(std::move(selected)) {
    try {
      const float priority = 1;
      VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
      queueInfo.queueFamilyIndex = candidate.queue;
      queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
      VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
      info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queueInfo;
      uint32_t extensionCount = 0;
      check(vkEnumerateDeviceExtensionProperties(candidate.physical, nullptr,
        &extensionCount, nullptr), "device extension discovery");
      std::vector<VkExtensionProperties> extensions(extensionCount);
      check(vkEnumerateDeviceExtensionProperties(candidate.physical, nullptr,
        &extensionCount, extensions.data()), "device extension discovery");
      // The extension name is stable even in SDKs that gate the beta feature
      // structs. We use no optional subset features and need no beta headers.
      const char* portabilityExtension = "VK_KHR_portability_subset";
      if (std::any_of(extensions.begin(), extensions.end(), [&](const auto& value) {
        return std::strcmp(value.extensionName, portabilityExtension) == 0;
      })) {
        portability = true;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = &portabilityExtension;
      }
      startupTrace(candidate.info.name.c_str());
      startupTrace("vkCreateDevice begin");
      check(vkCreateDevice(candidate.physical, &info, nullptr, &device), "device initialization");
      {
        const auto policy = gpu_memory::allocationPolicy(candidate.info.memoryBytes,
          std::getenv("BLAH2_GPU_ALLOCATION_LIMIT_BYTES"));
        std::lock_guard<std::mutex> lock(allocationsMutex);
        deviceAllocations.try_emplace(device, policy.bytes,
          memoryProperties(candidate.physical), policy.separateHostHeaps);
      }
      startupTrace("vkCreateDevice complete");
      vkGetDeviceQueue(device, candidate.queue, 0, &queue);
      VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      poolInfo.queueFamilyIndex = candidate.queue;
      check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "command allocation");
      VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      check(vkCreateFence(device, &fenceInfo, nullptr, &fence), "fence allocation");
      startupTrace("glslang initialization begin");
      compiler = glslang_initialize_process();
      startupTrace("glslang initialization complete");
      if (!compiler) throw std::runtime_error("GPU shader compiler is unavailable");
    } catch (...) { release(); throw; }
  }
  void release() {
    if (compiler) glslang_finalize_process();
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool) vkDestroyCommandPool(device, pool, nullptr);
    if (device) {
      vkDestroyDevice(device, nullptr);
      std::lock_guard<std::mutex> lock(allocationsMutex);
      const auto found = deviceAllocations.find(device);
      if (found != deviceAllocations.end()) {
        if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
          std::cerr << "GPU allocation budget peak=" << found->second.budget.peak()
            << " limit=" << found->second.budget.limit()
            << " remaining=" << found->second.budget.used() << '\n';
        deviceAllocations.erase(found);
      }
    }
  }
  ~Context() { release(); }
};
constexpr const char* memoryFailureAction = "using CPU";
#include "VulkanResources.private.h"
class ClutterPipeline {
  Context& context_;
  uint32_t samples_, bins_, channels_, length_;
  std::unique_ptr<Buffer> reference_, surveillance_, estimate_, crosscorrelation_;
  std::unique_ptr<Buffer> filterReference_, weights_, filtered_;
  std::unique_ptr<Buffer> input_, correlations_, weightUpload_, output_;
  std::unique_ptr<Plan> referencePlan_, surveillancePlan_, crossPlan_;
  std::unique_ptr<Plan> filterReferencePlan_, weightPlan_, filteredPlan_;
  std::unique_ptr<Kernel> products_, multiply_, normalize_;
  std::unique_ptr<clutter_refinement::BlockedBaseline> baseline_;
  std::unique_ptr<clutter_refinement::DenseSolver> solver_;
  std::vector<std::complex<double>> initial_, baselineOutput_;
  bool frameReady_ = false;
  clutter_replay::State* replay_ = nullptr;
  GpuClutterProfile profile_;
  ClutterSubmission submission_;
  struct CommandBuffers {
    Context& context;
    VkCommandBuffer handles[3]{};
    explicit CommandBuffers(Context& value) : context(value) {}
    CommandBuffers(const CommandBuffers&) = delete;
    CommandBuffers& operator=(const CommandBuffers&) = delete;
    ~CommandBuffers() {
      for (const auto command : handles)
        if (command) vkFreeCommandBuffers(context.device, context.pool, 1, &command);
    }
  } commands_;
  // The command owner is destroyed before its referenced kernels/plans/buffers,
  // including when recording throws during a smaller-batch startup retry.
  VkCommandBuffer prepare_ = VK_NULL_HANDLE, correlate_ = VK_NULL_HANDLE, finish_ = VK_NULL_HANDLE;
  bool direct_ = false;

  void enqueue(VkCommandBuffer command, const char* phase) {
    submission_.enqueue([&] {
      VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
      check(vkQueueSubmit(context_.queue, 1, &submit, context_.fence), phase);
    });
  }
  void complete(const char* phase) {
    submission_.complete(
      [&] { auto timing=profile_.scope(GpuClutterProfile::FenceWait);
        check(vkWaitForFences(context_.device, 1, &context_.fence, VK_TRUE, UINT64_MAX), phase); },
      [&] { check(vkResetFences(context_.device, 1, &context_.fence), "clutter fence reset"); });
  }
  void submit(VkCommandBuffer command, const char* phase) {
    enqueue(command, phase);
    complete(phase);
  }
  void reportCompatibility(uint64_t previousAttempts, uint64_t previousFactors) noexcept {
    const auto attempts = solver_->compatibilityAttempts();
    if (attempts == previousAttempts || attempts > 8) return;
    try {
      std::ostringstream line;
      line << "GPU_CLUTTER_CHOLESKY_COMPATIBILITY samples=" << samples_
        << " bins=" << bins_ << " channels=" << channels_
        << " attempts=" << attempts << " factors=" << solver_->compatibilityFactors()
        << " factor=" << (solver_->compatibilityFactors() > previousFactors ? "accepted" : "rejected")
        << " trigger=lapack_failure retry=legacy_fp64 log_limit=8"
        << " frame_acceptance=requires_solve_residual_guards\n";
      profile_.diagnostics() << line.str();
    } catch (...) { /* Diagnostics must not change frame acceptance. */ }
  }
  void factorReference(const std::vector<std::complex<double>>& covariance) {
    const auto attempts = solver_->compatibilityAttempts();
    const auto factors = solver_->compatibilityFactors();
    try { solver_->factorCovariance(covariance.data(), covariance.size()); }
    catch (...) { reportCompatibility(attempts, factors); throw; }
    reportCompatibility(attempts, factors);
  }
  const arma::cx_mat& solveWeights() {
    const auto* values = static_cast<const std::complex<float>*>(correlations_->mapped);
    return solver_->solve(values);
  }
public:
  void replayCheckpoint(clutter_replay::State* state){replay_=state;}
  static bool twiddleLut() {
    const char* setting = std::getenv("BLAH2_GPU_CLUTTER_TWIDDLES");
    const std::string mode = setting ? setting : "lut";
    if (mode != "lut" && mode != "native")
      throw std::invalid_argument("BLAH2_GPU_CLUTTER_TWIDDLES must be lut or native");
    return mode == "lut";
  }
  static uint32_t filterLength(const GpuGeometry& g) {
    const char* setting = std::getenv("BLAH2_GPU_CLUTTER_PADDING");
    const std::string mode = setting ? setting : "smooth";
    if (mode == "exact") {
      const uint64_t value = uint64_t(g.clutterSamples) + g.clutterBins + 1;
      return value <= UINT32_MAX ? uint32_t(value) : 0;
    }
    if (mode != "smooth")
      throw std::invalid_argument("BLAH2_GPU_CLUTTER_PADDING must be smooth or exact");
    return gpu_memory::nextSmooth(uint64_t(g.clutterSamples) + g.clutterBins - 1);
  }
  static uint64_t requiredBytes(const GpuGeometry& g) {
    if (!g.clutterSamples) return 0;
    const uint64_t n = g.clutterSamples, c = g.channels;
    const uint64_t b = g.clutterBins, l = filterLength(g);
    if (!l) return std::numeric_limits<uint64_t>::max();
    uint64_t elements = 3*n + 5*n*c + l + 2*l*c + b*c + b*(1+c);
    // Preflight allowance for the six plan tables. Batched transforms share
    // their LUTs. Actual tables, scratch and upload allocations are additionally
    // enforced by budgetAllocateMemory; this estimate is not the final guard.
    if (twiddleLut()) elements += 3*n + 3*l;
    return elements * sizeof(std::complex<float>);
  }
  ClutterPipeline(Context& context, const GpuGeometry& g,
      bool allowDirect, bool forceDirect)
    : context_(context), samples_(g.clutterSamples), bins_(g.clutterBins),
      channels_(g.channels), length_(filterLength(g)),
      profile_(context.candidate.physical, context.device, context.candidate.queue,
        context.candidate.properties.limits.timestampPeriod, samples_, bins_, channels_),
      commands_(context) {
    const uint64_t n = samples_, nc = uint64_t(samples_) * channels_;
    const uint64_t l = length_, lc = uint64_t(length_) * channels_;
    const auto& limit = context.candidate.properties.limits;
    if (!samples_ || !bins_ || bins_ > samples_ || !channels_ || channels_ > 8 ||
        !length_ || length_ < uint64_t(samples_) + bins_ - 1 ||
        nc > UINT32_MAX || lc > UINT32_MAX ||
        nc > SIZE_MAX / sizeof(std::complex<double>) ||
        std::max({n, nc, l, lc}) * sizeof(std::complex<float>) > limit.maxStorageBufferRange)
      throw std::runtime_error("GPU clutter capacity is too small; using CPU clutter");
    constexpr auto device = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    constexpr auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (allowDirect) {
      try {
        const auto preferred = coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        reference_ = std::make_unique<Buffer>(context, n*8, device | host, preferred);
        surveillance_ = std::make_unique<Buffer>(context, nc*8, device | host, preferred);
        estimate_ = std::make_unique<Buffer>(context, nc*8, device | host, preferred);
        const auto memory = memoryProperties(context.candidate.physical);
        const bool oneHeap = reference_->heap == surveillance_->heap &&
          reference_->heap == estimate_->heap;
        const uint64_t bytes = reference_->allocationBytes +
          surveillance_->allocationBytes + estimate_->allocationBytes;
        // Every allocation is also charged to the existing aggregate/per-heap
        // budget. AUTO requires the same unified-memory topology as ambiguity.
        direct_ = forceDirect || (oneHeap && gpu_memory::autoDirect(
          context.candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
          reference_->heap, bytes, memory));
      } catch (const std::exception&) { direct_ = false; }
      if (!direct_) { reference_.reset(); surveillance_.reset(); estimate_.reset(); }
    }
    if (!direct_) {
      reference_ = std::make_unique<Buffer>(context, n*8, device);
      surveillance_ = std::make_unique<Buffer>(context, nc*8, device);
      estimate_ = std::make_unique<Buffer>(context, nc*8, device);
      input_ = std::make_unique<Buffer>(context, (n+nc)*8, host, coherent);
      output_ = std::make_unique<Buffer>(context, nc*8, host,
        coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    startupTrace(direct_ ? "clutter direct mapped device memory selected" :
      "clutter persistent staging selected");
    crosscorrelation_ = std::make_unique<Buffer>(context, nc*8, device);
    filterReference_ = std::make_unique<Buffer>(context, l*8, device);
    weights_ = std::make_unique<Buffer>(context, lc*8, device);
    filtered_ = std::make_unique<Buffer>(context, lc*8, device);
    correlations_ = std::make_unique<Buffer>(context, bins_*(1+channels_)*8, host,
      coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    weightUpload_ = std::make_unique<Buffer>(context, uint64_t(bins_)*channels_*8,
      host, coherent);
    solver_ = std::make_unique<clutter_refinement::DenseSolver>(samples_, bins_, channels_);
    const bool lut = twiddleLut();
    referencePlan_ = std::make_unique<Plan>(context, *reference_, samples_, 1, lut);
    surveillancePlan_ = std::make_unique<Plan>(context, *surveillance_, samples_, channels_, lut);
    crossPlan_ = std::make_unique<Plan>(context, *crosscorrelation_, samples_, channels_, lut);
    filterReferencePlan_ = std::make_unique<Plan>(context, *filterReference_, length_, 1, lut);
    weightPlan_ = std::make_unique<Plan>(context, *weights_, length_, channels_, lut);
    filteredPlan_ = std::make_unique<Plan>(context, *filtered_, length_, channels_, lut);
    products_ = std::make_unique<Kernel>(context, clutterProductsSource,
      std::vector<Buffer*>{reference_.get(), surveillance_.get(), crosscorrelation_.get()});
    multiply_ = std::make_unique<Kernel>(context, clutterMultiplySource,
      std::vector<Buffer*>{filterReference_.get(), weights_.get(), filtered_.get()});
    normalize_ = std::make_unique<Kernel>(context, clutterNormalizeSource,
      std::vector<Buffer*>{filtered_.get(), estimate_.get()});
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 3;
    check(vkAllocateCommandBuffers(context.device, &allocation, commands_.handles), "clutter command buffers");
    prepare_ = commands_.handles[0]; correlate_ = commands_.handles[1]; finish_ = commands_.handles[2];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(prepare_, &begin), "clutter reference recording");
    profile_.recordBegin(prepare_, 0);
    VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    hostWrite.dstAccessMask = direct_ ?
      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(prepare_, VK_PIPELINE_STAGE_HOST_BIT,
      direct_ ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    if (!direct_) {
      VkBufferCopy ref{0,0,n*8};
      vkCmdCopyBuffer(prepare_, input_->handle, reference_->handle, 1, &ref);
    }
    vkCmdFillBuffer(prepare_, filterReference_->handle, 0, l*8, 0);
    barrier(prepare_);
    VkBufferCopy filterRef{0,0,n*8};
    vkCmdCopyBuffer(prepare_, direct_ ? reference_->handle : input_->handle,
      filterReference_->handle, 1, &filterRef);
    barrier(prepare_);
    referencePlan_->append(prepare_, -1);
    filterReferencePlan_->append(prepare_, -1); barrier(prepare_);
    VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    profile_.recordEnd(prepare_, 0);
    check(vkEndCommandBuffer(prepare_), "clutter reference recording");
    check(vkBeginCommandBuffer(correlate_, &begin), "clutter RHS recording");
    profile_.recordBegin(correlate_, 1);
    vkCmdPipelineBarrier(correlate_, VK_PIPELINE_STAGE_HOST_BIT,
      direct_ ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    if (!direct_) {
      VkBufferCopy surv{n*8,0,nc*8};
      vkCmdCopyBuffer(correlate_, input_->handle, surveillance_->handle, 1, &surv);
    }
    barrier(correlate_);
    surveillancePlan_->append(correlate_, -1); barrier(correlate_);
    // Only the RHS uses FP32 GPU correlation; the covariance is computed from
    // the same rounded reference in host FP64. Keep the reference FFT intact.
    products_->append(correlate_, {uint32_t(nc), samples_, channels_, 0, 0}); barrier(correlate_);
    crossPlan_->append(correlate_, 1); barrier(correlate_);
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      VkBufferCopy copy{uint64_t(channel)*samples_*8,
        uint64_t(bins_)*(1+channel)*8, uint64_t(bins_)*8};
      vkCmdCopyBuffer(correlate_, crosscorrelation_->handle, correlations_->handle, 1, &copy);
    }
    vkCmdPipelineBarrier(correlate_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &hostRead, 0, nullptr, 0, nullptr);
    profile_.recordEnd(correlate_, 1);
    check(vkEndCommandBuffer(correlate_), "clutter RHS recording");
    check(vkBeginCommandBuffer(finish_, &begin), "clutter finish recording");
    profile_.recordBegin(finish_, 2);
    hostWrite.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(finish_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(finish_, weights_->handle, 0, lc*8, 0);
    barrier(finish_); // Preserve padded zeros before copying each compact weight vector.
    for (uint32_t channel = 0; channel < channels_; ++channel) {
      VkBufferCopy copy{uint64_t(channel)*bins_*8,
        uint64_t(channel)*length_*8, uint64_t(bins_)*8};
      vkCmdCopyBuffer(finish_, weightUpload_->handle, weights_->handle, 1, &copy);
    }
    barrier(finish_);
    weightPlan_->append(finish_, -1); barrier(finish_);
    multiply_->append(finish_, {uint32_t(lc), length_, channels_, 0, 0}); barrier(finish_);
    filteredPlan_->append(finish_, 1); barrier(finish_);
    normalize_->append(finish_, {uint32_t(nc), samples_, length_, 0, 0}); barrier(finish_);
    if (!direct_) {
      VkBufferCopy outputCopy{0,0,nc*8};
      vkCmdCopyBuffer(finish_, estimate_->handle, output_->handle, 1, &outputCopy);
    }
    hostRead.srcAccessMask = direct_ ? VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(finish_,
      direct_ ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &hostRead, 0, nullptr, 0, nullptr);
    profile_.recordEnd(finish_, 2);
    check(vkEndCommandBuffer(finish_), "clutter finish recording");
    baseline_ = std::make_unique<clutter_refinement::BlockedBaseline>(samples_, bins_);
    const uint64_t hostBytes = uint64_t(baseline_->storageBytes()) + solver_->storageBytes() +
      uint64_t(channels_) * bins_ * sizeof(std::complex<double>) +
      nc * sizeof(std::complex<double>);
    if (hostBytes > SIZE_MAX)
      throw std::runtime_error("GPU clutter CPU workspace exceeds addressable storage");
    initial_.resize(size_t(channels_) * bins_);
    baselineOutput_.resize(size_t(channels_) * samples_);
    if (std::getenv("BLAH2_GPU_DIAGNOSTICS"))
      std::cerr << "GPU clutter CPU workspace bytes=" << hostBytes
        << " baseline_fft=" << baseline_->fft() << " covariance=host_fp64_blocked\n";
  }
  ~ClutterPipeline() {
    // Normal and numerical-rejection paths are already complete. Device/API
    // faults may leave work pending; drain before freeing commands or buffers.
    if (submission_.needsDrain()) vkDeviceWaitIdle(context_.device);
  }
  void ensureIdle() const { submission_.ensureIdle(); }
  void endFrame() {
    profile_.finish(); frameReady_ = false;
    baseline_->invalidate(); solver_->invalidate();
  }
  bool rejectFrame(const std::exception& error, const char* phase,
      uint32_t firstChannel, uint32_t activeChannels) {
    profile_.diagnostics() << "GPU clutter precision rejection samples=" << samples_
      << " bins=" << bins_ << " channels=" << channels_
      << " first_channel=" << firstChannel << " active_channels=" << activeChannels
      << " phase=" << phase << " reason=" << error.what() << '\n';
    endFrame();
    return false;
  }
  bool beginFrame(const std::complex<float>* reference, size_t count) {
    ensureIdle();
    endFrame();
    if (!reference || count != samples_)
      throw std::invalid_argument("GPU clutter reference dimensions changed");
    profile_.beginFrame();
    try {
      {
        auto timing = profile_.scope(GpuClutterProfile::ReferenceCopy);
        for (size_t i = 0; i < count; ++i)
          if (!clutter_refinement::finite(reference[i]))
            throw ClutterRejected("Nonfinite rounded clutter reference");
        if (direct_) {
          std::memcpy(reference_->mapped, reference, count*8);
          reference_->flush(0, reference_->bytes);
        } else {
          std::memcpy(input_->mapped, reference, count*8);
          input_->flush(0, count*8);
        }
      }
      {
        // The host only reads the original reference and its own FP64 scratch.
        // GPU prepare owns the uploaded copy and device FFT buffers. Neither
        // host factorization nor covariance needs the GPU result anymore.
        auto overlapTiming = profile_.scope(GpuClutterProfile::ReferenceOverlapWall);
        {
          auto timing = profile_.scope(GpuClutterProfile::ReferenceEnqueue);
          enqueue(prepare_, "clutter reference submission");
        }
        submission_.overlap([&] {
          {
            auto timing = profile_.scope(GpuClutterProfile::BaselinePrepare);
            baseline_->prepare(reference, count);
          }
          if(!replay_||!replay_->fixed()){
            const std::vector<std::complex<double>>* covariance;
            {
              auto timing = profile_.scope(GpuClutterProfile::Covariance);
              if(replay_)replay_->work(1,0,0);
              covariance = &baseline_->covariance();
            }
            { auto timing = profile_.scope(GpuClutterProfile::Factor);
              if(replay_){replay_->work(0,1,0);}
              factorReference(*covariance); }
          }
        }, [&] {
          {
            auto timing = profile_.scope(GpuClutterProfile::ReferenceWaitTail);
            complete("clutter reference execution");
          }
          profile_.collect(0, 0);
        });
      }
      frameReady_ = true;
      return true;
    } catch (const ClutterRejected& error) {
      return rejectFrame(error, "reference", 0, 0);
    } catch (const std::domain_error& error) {
      return rejectFrame(error, "reference", 0, 0);
    }
  }
  std::complex<float>* surveillanceUpload() {
    return direct_ ? static_cast<std::complex<float>*>(surveillance_->mapped) :
      static_cast<std::complex<float>*>(input_->mapped) + samples_;
  }
  void flushSurveillanceUpload() {
    if (direct_) surveillance_->flush(0, surveillance_->bytes);
    else input_->flush(size_t(samples_)*8, size_t(samples_)*channels_*8);
  }
  void uploadSurveillance(const std::complex<float>* surveillance, size_t count) {
    copyClutterBatch(surveillanceUpload(), size_t(samples_)*channels_, surveillance, count);
    flushSurveillanceUpload();
  }
  bool process(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>* surveillance, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount, uint32_t firstChannel) {
    ensureIdle();
    if (!reference || !surveillance || !output || referenceCount != samples_ ||
        !surveillanceCount || surveillanceCount > uint64_t(samples_)*channels_ ||
        surveillanceCount % samples_ || outputCount != surveillanceCount)
      throw std::invalid_argument("GPU clutter buffer dimensions changed");
    using Double = std::complex<double>;
    using Float = std::complex<float>;
    if (!frameReady_) throw std::logic_error("GPU clutter frame is not prepared");
    const uint32_t active = uint32_t(surveillanceCount / samples_);
    const bool frozen=replay_&&replay_->fixed();
    try {
      {
        auto timing = profile_.scope(GpuClutterProfile::InputCopy);
        for (size_t i = 0; i < surveillanceCount; ++i)
          if (!clutter_refinement::finite(surveillance[i]))
            throw ClutterRejected("Nonfinite rounded clutter surveillance");
        uploadSurveillance(surveillance, surveillanceCount);
      }
      if(!frozen){
        auto timing = profile_.scope(GpuClutterProfile::InitialWait);
        submit(correlate_, "clutter initial correlations");
      }
      if(!frozen){
        profile_.collect(1, 1);
        correlations_->invalidate(0, correlations_->bytes);
        auto timing = profile_.scope(GpuClutterProfile::InitialSolve);
        if(replay_)replay_->work(0,0,1);
        const auto& solved = solveWeights();
        std::copy_n(solved.memptr(), solved.n_elem, initial_.begin());
      }else std::copy_n(replay_->checkpoint().initial.data()+size_t(firstChannel)*bins_,
        size_t(active)*bins_,initial_.begin());
      // Preserve the dominant coefficients and their linear FIR estimate in
      // FP64. Only the residual correction crosses the FP32 GPU FIR boundary.
      // Initial correlation is complete, so its mapped upload can now hold
      // the residual. The original surveillance remains in separate IPC
      // storage. No flush or submission occurs until every channel succeeds.
      auto* residualUpload = surveillanceUpload();
      {
        auto timing = profile_.scope(GpuClutterProfile::Baseline);
        for (uint32_t channel = 0; channel < active; ++channel)
          baseline_->estimateResidual(
            surveillance + size_t(channel)*samples_,
            initial_.data() + size_t(channel)*bins_,
            baselineOutput_.data() + size_t(channel)*samples_,
            residualUpload + size_t(channel)*samples_);
      }
      {
        // Residual formation is fused into Baseline above. Retain the bounded
        // stage so existing diagnostic schemas report its separate cost as
        // approximately zero instead of silently dropping a field.
        auto timing = profile_.scope(GpuClutterProfile::Residual);
      }
      if(!frozen){
        // Keep the diagnostic field: only unused-channel padding and mapped
        // visibility remain here; active residuals no longer need a host copy.
        auto timing = profile_.scope(GpuClutterProfile::ResidualCopy);
        padClutterBatch(residualUpload, size_t(samples_)*channels_, surveillanceCount);
        flushSurveillanceUpload();
      }
      if(!frozen){
        auto timing = profile_.scope(GpuClutterProfile::ResidualWait);
        submit(correlate_, "clutter residual correlations");
      }
      const arma::cx_mat* correctionWeights = nullptr;
      if(!frozen){
        profile_.collect(1, 2);
        correlations_->invalidate(0, correlations_->bytes);
        auto timing = profile_.scope(GpuClutterProfile::CorrectionSolve);
        if(replay_)replay_->work(0,0,1);
        correctionWeights = &solveWeights();
      }
      {
        auto timing = profile_.scope(GpuClutterProfile::WeightCopy);
        auto* upload = static_cast<Float*>(weightUpload_->mapped);
        if(frozen){
          std::fill_n(upload,size_t(channels_)*bins_,Float{});
          std::copy_n(replay_->checkpoint().correction.data()+size_t(firstChannel)*bins_,size_t(active)*bins_,upload);
        }else for (size_t i = 0; i < correctionWeights->n_elem; ++i) {
          upload[i] = (*correctionWeights)[i];
          if (!clutter_refinement::finite(upload[i]))
            throw ClutterRejected("Nonfinite rounded correction clutter weights");
        }
        weightUpload_->flush(0, weightUpload_->bytes);
      }
      {
        auto timing = profile_.scope(GpuClutterProfile::FinishWait);
        submit(finish_, "clutter correction FIR");
      }
      profile_.collect(2, 3);
      auto timing = profile_.scope(GpuClutterProfile::Merge);
      Buffer& result = direct_ ? *estimate_ : *output_;
      result.invalidate(0, result.bytes);
      const auto* correction = static_cast<const Float*>(result.mapped);
      for (size_t i = 0; i < outputCount; ++i) {
        const Float merged(baselineOutput_[i] + Double(correction[i]));
        if (!clutter_refinement::finite(correction[i]) || !clutter_refinement::finite(merged))
          throw ClutterRejected("Nonfinite merged clutter estimate");
        output[i] = merged;
      }
      if(replay_)replay_->batch(firstChannel,active,initial_.data(),static_cast<const Float*>(weightUpload_->mapped));
      return true;
    } catch (const ClutterRejected& error) {
      return rejectFrame(error, "channel_batch", firstChannel, active);
    } catch (const std::domain_error& error) {
      return rejectFrame(error, "channel_batch", firstChannel, active);
    }
  }
};

class VulkanBackend final : public GpuBackend, public GpuBufferBackend,
    public GpuClutterBufferBackend, public clutter_replay::Backend {
  Context context_;
  GpuGeometry geometry_;
  // Destruction order is intentional: plans/kernels, buffers, then context.
  std::unique_ptr<Buffer> reference_, surveillance_, doppler_, input_, output_;
  std::unique_ptr<Plan> referencePlan_, rangePlan_, dopplerPlan_;
  std::unique_ptr<Kernel> multiply_, gather_;
  VkCommandBuffer command_ = VK_NULL_HANDLE;
  bool direct_ = false;
  std::unique_ptr<ClutterPipeline> clutter_;
  std::unique_ptr<clutter_replay::State> replayClutter_;
  uint32_t clutterBatchChannels_ = 0;
public:
  VulkanBackend(std::shared_ptr<Instance> instance, Candidate candidate, GpuGeometry g)
    : context_(std::move(instance), std::move(candidate)), geometry_(g) {
    if (g.kind != GpuWorkKind::radar || g.firSamples || g.firTaps ||
        g.firFft || g.firPercent)
      throw std::invalid_argument("Invalid radar GPU work geometry");
    if (!g.range || g.range > 65535 || !g.doppler || g.doppler > 65535 ||
        !g.delays || g.delays > 65535 || !g.channels || g.channels > 8)
      throw std::runtime_error("GPU radar dimensions are unsupported; using CPU");
    const uint64_t range = uint64_t(g.range) * g.doppler;
    const uint64_t doppler = uint64_t(g.doppler) * g.delays * g.channels;
    const uint64_t ambiguityBytes = 2 * (range * (1 + g.channels) + doppler) *
      sizeof(std::complex<float>);
    const auto& limits = context_.candidate.properties.limits;
    const auto budget = gpu_memory::heapBudget(context_.candidate.info.memoryBytes);
    if (!g.range || !g.doppler || !g.delays || !g.channels || g.channels > 8 ||
        range * g.channels > UINT32_MAX / 2 || doppler > UINT32_MAX / 2 ||
        g.delayMin <= -int64_t(g.range) || int64_t(g.delayMin) + g.delays > g.range ||
        ambiguityBytes > budget || limits.maxComputeWorkGroupInvocations < 128 ||
        limits.maxComputeWorkGroupSize[0] < 128 || limits.maxComputeWorkGroupCount[0] < 65535 ||
        std::max(range * g.channels, doppler) * sizeof(std::complex<float>) > limits.maxStorageBufferRange)
      throw std::runtime_error("GPU capacity is too small for these radar settings; using CPU");
    startupTrace("buffer allocations begin");
    const auto* setting = std::getenv("BLAH2_GPU_MEMORY_PATH");
    const std::string memoryPath = setting ? setting : "auto";
    if (memoryPath != "auto" && memoryPath != "direct" && memoryPath != "staged")
      throw std::runtime_error("BLAH2_GPU_MEMORY_PATH must be auto, direct, or staged; using CPU");
    if (memoryPath != "staged") {
      try {
        constexpr auto directFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        constexpr auto hostPreferred = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
          VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        reference_ = std::make_unique<Buffer>(context_, range * 8, directFlags, hostPreferred);
        surveillance_ = std::make_unique<Buffer>(context_, range * g.channels * 8,
          directFlags, hostPreferred);
        doppler_ = std::make_unique<Buffer>(context_, doppler * 8, directFlags, hostPreferred);
        const uint64_t directBytes = reference_->allocationBytes +
          surveillance_->allocationBytes + doppler_->allocationBytes;
        const bool oneHeap = reference_->heap == surveillance_->heap &&
          reference_->heap == doppler_->heap;
        const auto memory = memoryProperties(context_.candidate.physical);
        std::vector<uint64_t> heapUse(memory.heaps.size());
        for (const auto* buffer : {reference_.get(), surveillance_.get(), doppler_.get()})
          if (buffer->heap < heapUse.size()) heapUse[buffer->heap] += buffer->allocationBytes;
        bool fits = true;
        for (size_t heap = 0; heap < heapUse.size(); ++heap)
          if (heapUse[heap] > gpu_memory::heapBudget(memory.heaps[heap].bytes))
            fits = false;
        direct_ = fits && (memoryPath == "direct" || (oneHeap && gpu_memory::autoDirect(
          context_.candidate.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
          reference_->heap, directBytes, memory)));
      } catch (const std::exception&) { direct_ = false; }
      if (!direct_) { reference_.reset(); surveillance_.reset(); doppler_.reset(); }
    }
    if (!direct_) {
      reference_ = std::make_unique<Buffer>(context_, range * 8,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      surveillance_ = std::make_unique<Buffer>(context_, range * g.channels * 8,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      doppler_ = std::make_unique<Buffer>(context_, doppler * 8,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      input_ = std::make_unique<Buffer>(context_, reference_->bytes + surveillance_->bytes,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      output_ = std::make_unique<Buffer>(context_, doppler_->bytes,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    startupTrace(direct_ ? "direct mapped device memory selected" : "persistent staging selected");
    startupTrace("buffer allocations complete");
    referencePlan_ = std::make_unique<Plan>(context_, *reference_, g.range, g.doppler);
    rangePlan_ = std::make_unique<Plan>(context_, *surveillance_, g.range, g.doppler * g.channels);
    dopplerPlan_ = std::make_unique<Plan>(context_, *doppler_, g.doppler, g.delays * g.channels);
    startupTrace("multiply kernel begin");
    multiply_ = std::make_unique<Kernel>(context_, multiplySource, std::vector<Buffer*>{reference_.get(), surveillance_.get()});
    startupTrace("gather kernel begin");
    gather_ = std::make_unique<Kernel>(context_, gatherSource, std::vector<Buffer*>{surveillance_.get(), doppler_.get()});
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = context_.pool; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(context_.device, &allocation, &command_), "command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command_, &begin), "recording");
    if (direct_) {
      VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      hostWrite.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &hostWrite, 0, nullptr, 0, nullptr);
    } else {
      VkMemoryBarrier hostWrite{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      hostWrite.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
      hostWrite.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hostWrite, 0, nullptr, 0, nullptr);
      VkBufferCopy refCopy{0, 0, reference_->bytes};
      VkBufferCopy survCopy{reference_->bytes, 0, surveillance_->bytes};
      vkCmdCopyBuffer(command_, input_->handle, reference_->handle, 1, &refCopy);
      vkCmdCopyBuffer(command_, input_->handle, surveillance_->handle, 1, &survCopy);
      barrier(command_);
    }
    referencePlan_->append(command_, -1); rangePlan_->append(command_, -1); barrier(command_);
    Push push{uint32_t(range * g.channels), g.range, g.doppler, g.delays, g.delayMin};
    multiply_->append(command_, push); barrier(command_);
    rangePlan_->append(command_, 1); barrier(command_);
    push.count = doppler;
    gather_->append(command_, push); barrier(command_);
    dopplerPlan_->append(command_, -1); barrier(command_);
    VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hostRead.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    if (direct_) {
      hostRead.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostRead, 0, nullptr, 0, nullptr);
    } else {
      VkBufferCopy outputCopy{0, 0, doppler_->bytes};
      vkCmdCopyBuffer(command_, doppler_->handle, output_->handle, 1, &outputCopy);
      // Fence completion alone does not make discrete-GPU staging writes visible.
      hostRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(command_, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostRead, 0, nullptr, 0, nullptr);
    }
    check(vkEndCommandBuffer(command_), "command recording");
    if (g.clutterSamples) {
      // Retain the complete ambiguity/IPC frame, but bound clutter's working
      // storage by its channel batch. The allocator still enforces every
      // aggregate/per-heap limit, including VkFFT scratch and lookup tables.
      for (uint32_t channels = g.channels; channels > 0; --channels) {
        auto batch = g;
        batch.channels = channels;
        if (ClutterPipeline::requiredBytes(batch) > budget - ambiguityBytes) continue;
        startupTrace(("clutter pipeline begin batch_channels=" +
          std::to_string(channels)).c_str());
        try {
          clutter_ = std::make_unique<ClutterPipeline>(context_, batch,
            direct_, memoryPath == "direct");
          clutterBatchChannels_ = channels;
          startupTrace(("clutter pipeline complete batch_channels=" +
            std::to_string(channels) + " frame_channels=" +
            std::to_string(g.channels)).c_str());
          break;
        } catch (const std::exception& error) {
          startupTrace(error.what());
          // Failed construction releases its buffers/plans before retrying a
          // smaller batch. No frame input or output has been consumed yet.
        }
      }
      if (!clutter_) startupTrace("clutter capacity or initialization unavailable; ambiguity remains available");
    }
    if (std::getenv("BLAH2_GPU_DIAGNOSTICS")) {
      std::lock_guard<std::mutex> lock(allocationsMutex);
      const auto& budget = deviceAllocations.at(context_.device).budget;
      std::cerr << "GPU allocation budget used=" << budget.used()
        << " peak=" << budget.peak() << " limit=" << budget.limit() << '\n';
    }
    startupTrace("backend ready");
  }
  GpuDevice device() const override { return context_.candidate.info; }
  void configureClutterReplay(const clutter_replay::Plan& declared,std::function<clutter_replay::Frame()> binding) override {
    if(replayClutter_||!clutter_||declared.samples!=geometry_.clutterSamples||declared.channels!=geometry_.channels||
       declared.taps!=geometry_.clutterBins||declared.delayMin!=geometry_.clutterDelayMin)
      throw std::runtime_error("Replay clutter checkpoint native geometry/backend differs");
    VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(context_.candidate.physical,&features);
    auto plan=declared;plan.identity=clutter_replay::hash(plan.identity+context_.candidate.info.id+":"+
      std::to_string(context_.candidate.properties.driverVersion)+":"+std::to_string(features.shaderFloat64)+":"+
      std::to_string(ClutterPipeline::filterLength(geometry_))+":"+std::to_string(ClutterPipeline::twiddleLut()));
    replayClutter_=std::make_unique<clutter_replay::State>(std::move(plan),std::move(binding));
    clutter_->replayCheckpoint(replayClutter_.get());
  }
  clutter_replay::Proof clutterReplayProof()const override {
    if(!replayClutter_)throw std::runtime_error("Replay clutter checkpoint was not authenticated");
    return replayClutter_->proof();
  }
  void process(const std::vector<std::complex<float>>& reference,
    const std::vector<std::complex<float>>& surveillance,
    std::vector<std::complex<float>>& output) override {
    output.resize(doppler_->bytes / sizeof(std::complex<float>));
    processBuffers(reference.data(), reference.size(), surveillance.data(),
      surveillance.size(), output.data(), output.size());
  }
  void processBuffers(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>* surveillance, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount) override {
    if (clutter_) clutter_->ensureIdle(); // Ambiguity shares the same queue/fence.
    if (!reference || !surveillance || !output ||
        referenceCount != reference_->bytes / sizeof(*reference) ||
        surveillanceCount != surveillance_->bytes / sizeof(*surveillance) ||
        outputCount != doppler_->bytes / sizeof(*output))
      throw std::invalid_argument("GPU buffer dimensions changed");
    if (direct_) {
      std::memcpy(reference_->mapped, reference, reference_->bytes);
      std::memcpy(surveillance_->mapped, surveillance, surveillance_->bytes);
      reference_->flush(0, reference_->bytes);
      surveillance_->flush(0, surveillance_->bytes);
    } else {
      std::memcpy(input_->mapped, reference, reference_->bytes);
      std::memcpy(static_cast<char*>(input_->mapped) + reference_->bytes,
        surveillance, surveillance_->bytes);
      input_->flush(0, input_->bytes);
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command_;
    check(vkQueueSubmit(context_.queue, 1, &submit, context_.fence), "submission");
    // Device loss is surfaced by the driver. A slow-but-running queue must be
    // drained before freeing its buffers; it is not safe to pretend cancellation.
    check(vkWaitForFences(context_.device, 1, &context_.fence, VK_TRUE, UINT64_MAX), "execution");
    check(vkResetFences(context_.device, 1, &context_.fence), "fence reset");
    Buffer& result = direct_ ? *doppler_ : *output_;
    result.invalidate(0, result.bytes);
    std::memcpy(output, result.mapped, result.bytes);
  }
  bool processClutterBuffers(const std::complex<float>* reference, size_t referenceCount,
      const std::complex<float>* surveillance, size_t surveillanceCount,
      std::complex<float>* output, size_t outputCount) override {
    // This interface remains available when construction rejected only clutter
    // (for example its memory budget). Reject this stage without killing the
    // worker and discarding the already-qualified ambiguity backend.
    if (!reference || !geometry_.clutterSamples ||
        referenceCount != geometry_.clutterSamples)
      throw std::invalid_argument("GPU clutter buffer dimensions changed");
    if (!clutter_) {
      // The protocol rejection already reports this stage as unavailable.
      return false;
    }
    clutter_->endFrame();
    struct ResetFrame { ClutterPipeline& pipeline; ~ResetFrame() { pipeline.endFrame(); } } reset{*clutter_};
    if(replayClutter_)replayClutter_->begin(reference,referenceCount,surveillance,surveillanceCount);
    try{const bool accepted=processClutterBatches(geometry_.clutterSamples, geometry_.channels,
      clutterBatchChannels_, surveillance, surveillanceCount, output, outputCount,
      [&](uint32_t first, uint32_t, const std::complex<float>* input,
          size_t count, std::complex<float>* candidate) {
        if (first == 0 && !clutter_->beginFrame(reference, referenceCount)) return false;
        return clutter_->process(reference, referenceCount, input, count,
          candidate, count, first);
      });
      if(replayClutter_){replayClutter_->complete(accepted);}
      return accepted;
    }catch(...){if(replayClutter_)replayClutter_->poison();throw;}
  }
};
}
}
extern "C" std::vector<blah2::GpuDevice> blah2_gpu_devices() {
  blah2::Instance instance;
  std::vector<blah2::GpuDevice> result;
  for (const auto& item : blah2::enumerate(instance)) result.push_back(item.info);
  return result;
}
extern "C" std::string blah2_gpu_driver_status(unsigned version) {
  if (version != 1) throw std::runtime_error("Unsupported GPU diagnostic version");
  blah2::Instance instance(true);
  auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2KHR>(
    vkGetInstanceProcAddr(instance.handle, "vkGetPhysicalDeviceProperties2KHR"));
  std::ostringstream json;
  json << "{\"version\":1,\"available\":true,\"qualification\":\"not-run\",\"devices\":[";
  bool first = true;
  for (const auto& item : blah2::enumerate(instance)) {
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(item.physical, &features);
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    uint32_t count = 0;
    blah2::check(vkEnumerateDeviceExtensionProperties(item.physical, nullptr, &count, nullptr), "driver extension discovery");
    std::vector<VkExtensionProperties> extensions(count);
    blah2::check(vkEnumerateDeviceExtensionProperties(item.physical, nullptr, &count, extensions.data()), "driver extension discovery");
    const bool supported = item.properties.apiVersion >= VK_API_VERSION_1_2 ||
      std::any_of(extensions.begin(), extensions.end(), [](const auto& value) {
        return std::strcmp(value.extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) == 0;
      });
    if (instance.diagnosticProperties && getProperties && supported) {
      VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      properties.pNext = &driver;
      getProperties(item.physical, &properties);
    }
    if (!first) json << ',';
    first = false;
    json << "{\"id\":" << blah2::gpuDiagnosticString(item.info.id)
      << ",\"name\":" << blah2::gpuDiagnosticString(item.info.name)
      << ",\"vendorId\":" << item.properties.vendorID
      << ",\"deviceId\":" << item.properties.deviceID
      << ",\"driverId\":" << driver.driverID
      << ",\"driverName\":" << blah2::gpuDiagnosticString(driver.driverName)
      << ",\"driverInfo\":" << blah2::gpuDiagnosticString(driver.driverInfo)
      << ",\"driverVersionRaw\":" << item.properties.driverVersion
      << ",\"shaderFloat64\":" << (features.shaderFloat64 ? "true" : "false")
      << ",\"mesaVersion\":";
    // Packed Mesa versions are meaningful here only for the identified V3DV
    // driver, not NVIDIA's different encoding or an unknown vendor's driver.
    if (driver.driverID == VK_DRIVER_ID_MESA_V3DV) {
      const uint32_t v = item.properties.driverVersion;
      json << '"' << VK_VERSION_MAJOR(v) << '.' << VK_VERSION_MINOR(v) << '.' << VK_VERSION_PATCH(v) << '"';
    } else json << "null";
    json << '}';
  }
  json << "]}";
  return json.str();
}
extern "C" blah2::GpuBackend* blah2_gpu_create(unsigned abi, const blah2::GpuGeometry* geometry, const char* requested) {
  if (abi != blah2::GPU_ABI || !geometry) throw std::runtime_error("GPU module version mismatch");
  if (geometry->kind != blah2::GpuWorkKind::radar || geometry->firSamples ||
      geometry->firTaps || geometry->firFft || geometry->firPercent)
    throw std::invalid_argument("Retired or unsupported GPU work geometry");
  auto instance = std::make_shared<blah2::Instance>();
  std::string reason = "No compatible GPU is available; using CPU";
  const std::string selection = requested ? requested : "auto";
  for (const auto& candidate : blah2::enumerate(*instance)) {
    if (selection != "auto" && selection != candidate.info.id) continue;
    try {
      return new blah2::VulkanBackend(instance, candidate, *geometry);
    }
    catch (const std::exception& error) { reason = error.what(); }
  }
  throw std::runtime_error(reason);
}
