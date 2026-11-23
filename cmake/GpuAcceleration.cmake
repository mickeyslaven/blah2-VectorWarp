set(BLAH2_GPU_DEFAULT "AUTO")
set(BLAH2_GPU "${BLAH2_GPU_DEFAULT}" CACHE STRING "Build GPU module: AUTO, ON or OFF")
set_property(CACHE BLAH2_GPU PROPERTY STRINGS AUTO ON OFF)
set(BLAH2_GPU_ADVANCED "AUTO" CACHE STRING
  "Build native resident/wideband GPU modules: AUTO, ON (required) or OFF")
set_property(CACHE BLAH2_GPU_ADVANCED PROPERTY STRINGS AUTO ON OFF)
include(${CMAKE_CURRENT_LIST_DIR}/GpuPlatformPolicy.cmake)
blah2_gpu_platform_policy("${CMAKE_SYSTEM_NAME}" "${BLAH2_GPU}" "${BLAH2_GPU_ADVANCED}"
  BLAH2_GPU_RESIDENT_ELIGIBLE BLAH2_GPU_RESIDENT_REASON
  BLAH2_GPU_WIDEBAND_ELIGIBLE BLAH2_GPU_WIDEBAND_REASON)
set(BLAH2_GPU_PORTABLE_BUILT FALSE)
set(BLAH2_GPU_RESIDENT_BUILT FALSE)
set(BLAH2_GPU_WIDEBAND_BUILT FALSE)
set(VKFFT_ROOT "" CACHE PATH "VkFFT 1.3.4 source directory")
function(blah2_isolate_vkfft_vendor_warnings target)
  # GCC 15 diagnoses VkFFT 1.3.4's code-generator pointer selection as
  # maybe-uninitialized even though the active backend fixes the selector.
  # Keep -Werror for VectorWarp and every other warning; isolate only this
  # vendor-header diagnostic on translation units that include VkFFT.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${target} PRIVATE -Wno-error=maybe-uninitialized)
  endif()
endfunction()
add_library(blah2GpuProcess STATIC ${PROJECT_ROOT}/src/process/ambiguity/GpuProcess.cpp)
target_compile_features(blah2GpuProcess PUBLIC cxx_std_17)
target_include_directories(blah2GpuProcess PUBLIC ${PROJECT_ROOT}/src)
add_library(blah2GpuWidebandProcess STATIC
  ${PROJECT_ROOT}/src/process/ambiguity/GpuWidebandChannelizer.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/GpuWidebandProcess.cpp)
target_compile_features(blah2GpuWidebandProcess PUBLIC cxx_std_17)
target_include_directories(blah2GpuWidebandProcess PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(blah2GpuWidebandProcess PUBLIC blah2GpuProcess
  blah2FixedExecutor Threads::Threads)
if(TARGET blah2)
  target_link_libraries(blah2 PRIVATE blah2GpuWidebandProcess)
endif()
# The resident protocol is adjacent to ABI4 and uses canonical FP64 input.
add_library(blah2GpuResidentProcess STATIC
  ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentWire.cpp
  ${PROJECT_ROOT}/src/process/analysis/ResidentReplaySnapshot.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentProof.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentProcess.cpp)
target_compile_features(blah2GpuResidentProcess PUBLIC cxx_std_17)
target_include_directories(blah2GpuResidentProcess PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(blah2GpuResidentProcess PUBLIC blah2GpuProcess blah2RapidJson Threads::Threads)
if(TARGET blah2)
  target_link_libraries(blah2 PRIVATE blah2GpuResidentProcess)
endif()

target_link_libraries(blah2GpuProcess PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(TARGET blah2)
  target_link_libraries(blah2 PRIVATE blah2GpuProcess)
endif()
if(NOT BLAH2_GPU STREQUAL "OFF")
  find_package(Vulkan QUIET)
  find_package(glslang CONFIG QUIET)
  # Ubuntu 22.04 ships exported targets without a glslangConfig.cmake.
  if(NOT TARGET glslang::glslang)
    find_path(GLSLANG_LEGACY_CMAKE glslangTargets.cmake
      PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/cmake /usr/lib64/cmake /usr/lib/cmake)
    if(GLSLANG_LEGACY_CMAKE)
      foreach(lib IN ITEMS OSDependent OGLCompiler HLSL glslang SPIRV glslang-default-resource-limits)
        include(${GLSLANG_LEGACY_CMAKE}/${lib}Targets.cmake)
        if(TARGET ${lib} AND NOT TARGET glslang::${lib})
          add_library(glslang::${lib} ALIAS ${lib})
        endif()
      endforeach()
    endif()
  endif()
  if(Vulkan_FOUND AND TARGET glslang::glslang AND EXISTS "${VKFFT_ROOT}/vkFFT/vkFFT.h")
    set(BLAH2_GPU_PORTABLE_BUILT TRUE)
    if(TARGET blah2)
      target_compile_definitions(blah2 PRIVATE VECTORWARP_GPU=1)
    endif()
    add_library(blah2GpuVulkan MODULE ${PROJECT_ROOT}/src/process/ambiguity/VulkanBackend.cpp)
    target_compile_features(blah2GpuVulkan PRIVATE cxx_std_17)
    target_compile_definitions(blah2GpuVulkan PRIVATE VKFFT_BACKEND=0)
    target_include_directories(blah2GpuVulkan SYSTEM PRIVATE
      ${VKFFT_ROOT}/vkFFT ${Vulkan_INCLUDE_DIRS} ${ARMADILLO_INCLUDE_DIRS})
    # VkFFT includes glslang_c_interface.h without its parent path.
    find_path(GLSLANG_C_INCLUDE glslang_c_interface.h PATH_SUFFIXES glslang/Include REQUIRED)
    target_include_directories(blah2GpuVulkan SYSTEM PRIVATE ${GLSLANG_C_INCLUDE})
    target_link_libraries(blah2GpuVulkan PRIVATE Vulkan::Vulkan glslang::glslang
      glslang::SPIRV glslang::glslang-default-resource-limits
      ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY} ${ARMADILLO_LIBRARIES}
      blah2RapidJson Threads::Threads)
    # The isolated module owns the short FP64 clutter baseline and macOS
    # prime-plan constants. Its loader-only worker does not use FFTW directly.
    # Vulkan's zero-initialized structs intentionally initialize sType first.
    target_compile_options(blah2GpuVulkan PRIVATE -Wno-missing-field-initializers)
    blah2_isolate_vkfft_vendor_warnings(blah2GpuVulkan)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      # This versioned C++ plugin ABI uses unmangled names for dlsym; these
      # exports are never called from C. Keep the stable names under -Werror.
      target_compile_options(blah2GpuVulkan PRIVATE -Wno-return-type-c-linkage)
    endif()
    set_target_properties(blah2GpuVulkan PROPERTIES PREFIX "" OUTPUT_NAME "blah2-gpu-vulkan"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    if(TARGET blah2)
      add_dependencies(blah2 blah2GpuVulkan)
    endif()
    set(BLAH2_GPU_WORKER_SOURCES ${PROJECT_ROOT}/src/process/ambiguity/GpuWorker.cpp)
    if(APPLE)
      # Keep the project CXX-only and compile this one worker-only source with
      # AppleClang's explicit Objective-C++ language flag.
      set(BLAH2_MAC_GPU_COMPATIBILITY ${PROJECT_ROOT}/src/process/ambiguity/MacGpuCompatibility.mm)
      set_source_files_properties(${BLAH2_MAC_GPU_COMPATIBILITY} PROPERTIES
        LANGUAGE CXX COMPILE_FLAGS "-x objective-c++")
      list(APPEND BLAH2_GPU_WORKER_SOURCES ${BLAH2_MAC_GPU_COMPATIBILITY})
      find_library(BLAH2_METAL_FRAMEWORK Metal)
      find_library(BLAH2_FOUNDATION_FRAMEWORK Foundation)
      if(NOT BLAH2_METAL_FRAMEWORK OR NOT BLAH2_FOUNDATION_FRAMEWORK)
        message(FATAL_ERROR "macOS GPU worker compatibility needs Metal and Foundation frameworks")
      endif()
    endif()
    add_executable(blah2GpuWorker ${BLAH2_GPU_WORKER_SOURCES})
    target_link_libraries(blah2GpuWorker PRIVATE blah2GpuProcess
      blah2GpuWidebandProcess blah2GpuResidentProcess ${CMAKE_DL_LIBS})
    if(APPLE)
      # Metal enumeration is isolated to the worker; never link it to blah2.
      target_link_libraries(blah2GpuWorker PRIVATE ${BLAH2_METAL_FRAMEWORK} ${BLAH2_FOUNDATION_FRAMEWORK})
    endif()
    set_target_properties(blah2GpuWorker PROPERTIES OUTPUT_NAME "blah2-gpu-worker")
    add_dependencies(blah2GpuWorker blah2GpuVulkan)
    if(TARGET blah2)
      add_dependencies(blah2 blah2GpuWorker)
    endif()
    if(BLAH2_GPU_WIDEBAND_ELIGIBLE)
      set(BLAH2_GPU_WIDEBAND_BUILT TRUE)
      add_library(blah2GpuWidebandVulkan MODULE
        ${PROJECT_ROOT}/src/process/ambiguity/GpuWidebandModule.cpp
        ${PROJECT_ROOT}/src/process/ambiguity/GpuWidebandChannelizer.cpp)
      target_compile_features(blah2GpuWidebandVulkan PRIVATE cxx_std_17)
      target_compile_definitions(blah2GpuWidebandVulkan PRIVATE VKFFT_BACKEND=0)
      target_include_directories(blah2GpuWidebandVulkan PRIVATE
        ${PROJECT_ROOT}/src/process/ambiguity ${PROJECT_ROOT}/src)
      target_include_directories(blah2GpuWidebandVulkan SYSTEM PRIVATE
        ${VKFFT_ROOT}/vkFFT ${Vulkan_INCLUDE_DIRS} ${GLSLANG_C_INCLUDE})
      target_link_libraries(blah2GpuWidebandVulkan PRIVATE Vulkan::Vulkan
        glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits
        ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY}
        blah2RapidJson Threads::Threads)
      target_compile_options(blah2GpuWidebandVulkan PRIVATE
        -Wno-missing-field-initializers)
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(blah2GpuWidebandVulkan PRIVATE
          -Wno-return-type-c-linkage)
      endif()
      blah2_isolate_vkfft_vendor_warnings(blah2GpuWidebandVulkan)
      if(APPLE)
        target_link_options(blah2GpuWidebandVulkan PRIVATE "LINKER:-undefined,error")
      else()
        target_link_options(blah2GpuWidebandVulkan PRIVATE "LINKER:-z,defs")
      endif()
      set_target_properties(blah2GpuWidebandVulkan PROPERTIES PREFIX ""
        OUTPUT_NAME "blah2-gpu-wideband-vulkan"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
      add_dependencies(blah2GpuWorker blah2GpuWidebandVulkan)
      target_compile_definitions(blah2GpuWorker PRIVATE VECTORWARP_WIDEBAND_GPU=1)
      if(TARGET blah2)
        target_compile_definitions(blah2 PRIVATE VECTORWARP_WIDEBAND_GPU=1)
      endif()
      if(BUILD_TESTING OR BLAH2_BUILD_OFFLINE_TOOLS)
        add_executable(testGpuWidebandPhysical
          ${PROJECT_ROOT}/test/gpu/TestGpuWidebandPhysical.cpp)
        target_link_libraries(testGpuWidebandPhysical PRIVATE
          blah2GpuWidebandProcess blah2Channelizer
          ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY})
        target_compile_definitions(testGpuWidebandPhysical PRIVATE
          GPU_WIDEBAND_WORKER="$<TARGET_FILE:blah2GpuWorker>")
        add_dependencies(testGpuWidebandPhysical blah2GpuWorker
          blah2GpuWidebandVulkan)
        set_target_properties(testGpuWidebandPhysical PROPERTIES
          RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_DIR}")
        add_executable(testGpuWidebandEndToEnd
          ${PROJECT_ROOT}/test/gpu/TestGpuWidebandEndToEnd.cpp)
        target_link_libraries(testGpuWidebandEndToEnd PRIVATE
          blah2GpuWidebandProcess blah2MultiIlluminatorRadar blah2Channelizer)
        target_compile_definitions(testGpuWidebandEndToEnd PRIVATE
          GPU_WIDEBAND_WORKER="$<TARGET_FILE:blah2GpuWorker>")
        add_dependencies(testGpuWidebandEndToEnd blah2GpuWorker
          blah2GpuWidebandVulkan)
        set_target_properties(testGpuWidebandEndToEnd PROPERTIES
          RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_DIR}")
      endif()
    endif()
    if(BLAH2_GPU_RESIDENT_ELIGIBLE)
      set(BLAH2_GPU_RESIDENT_BUILT TRUE)
      if(TARGET blah2MultiIlluminatorRadar)
        target_compile_definitions(blah2MultiIlluminatorRadar PRIVATE VECTORWARP_RESIDENT_GPU=1)
      endif()
      add_library(blah2GpuResidentVulkan MODULE
        ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentModule.cpp
        ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentWire.cpp
        ${PROJECT_ROOT}/src/process/analysis/ResidentReplaySnapshot.cpp)
      target_compile_features(blah2GpuResidentVulkan PRIVATE cxx_std_17)
      target_compile_definitions(blah2GpuResidentVulkan PRIVATE VKFFT_BACKEND=0)
      target_include_directories(blah2GpuResidentVulkan PRIVATE
        ${PROJECT_ROOT}/src/process/ambiguity ${PROJECT_ROOT}/src)
      target_include_directories(blah2GpuResidentVulkan SYSTEM PRIVATE
        ${VKFFT_ROOT}/vkFFT ${Vulkan_INCLUDE_DIRS} ${GLSLANG_C_INCLUDE})
      target_link_libraries(blah2GpuResidentVulkan PRIVATE Vulkan::Vulkan
        glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits
        blah2RapidJson Threads::Threads ${CMAKE_DL_LIBS})
      target_compile_options(blah2GpuResidentVulkan PRIVATE -Wno-missing-field-initializers)
      blah2_isolate_vkfft_vendor_warnings(blah2GpuResidentVulkan)
      set_property(TARGET blah2GpuResidentVulkan APPEND_STRING PROPERTY LINK_FLAGS " -Wl,-z,defs")
      if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(blah2GpuResidentVulkan PRIVATE -Wno-return-type-c-linkage)
      endif()
      set_target_properties(blah2GpuResidentVulkan PROPERTIES PREFIX ""
        OUTPUT_NAME "blah2-gpu-resident-vulkan" LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
      add_dependencies(blah2GpuWorker blah2GpuResidentVulkan)
      target_compile_definitions(blah2GpuWorker PRIVATE VECTORWARP_RESIDENT_GPU=1)
      if(TARGET blah2)
        target_compile_definitions(blah2 PRIVATE VECTORWARP_RESIDENT_GPU=1)
        add_dependencies(blah2 blah2GpuResidentVulkan)
      endif()
    endif()
    message(STATUS "GPU acceleration: portable Vulkan module enabled")
  elseif(BLAH2_GPU STREQUAL "ON" OR BLAH2_GPU_ADVANCED STREQUAL "ON")
    message(FATAL_ERROR "Required GPU build needs Vulkan/glslang development packages and VKFFT_ROOT (VkFFT 1.3.4)")
  else()
    if(BLAH2_GPU_RESIDENT_ELIGIBLE)
      set(BLAH2_GPU_RESIDENT_REASON "GPU dependencies absent; CPU processing remains available")
    endif()
    if(BLAH2_GPU_WIDEBAND_ELIGIBLE)
      set(BLAH2_GPU_WIDEBAND_REASON "GPU dependencies absent; CPU processing remains available")
    endif()
    message(STATUS "GPU dependencies absent: building CPU fallback (BLAH2_GPU=ON requires GPU support)")
  endif()
endif()
message(STATUS "GPU build capabilities: portable=${BLAH2_GPU_PORTABLE_BUILT}; resident=${BLAH2_GPU_RESIDENT_BUILT}; wideband=${BLAH2_GPU_WIDEBAND_BUILT}")
message(STATUS "Resident GPU capability: ${BLAH2_GPU_RESIDENT_REASON}")
message(STATUS "Wideband GPU capability: ${BLAH2_GPU_WIDEBAND_REASON}")
