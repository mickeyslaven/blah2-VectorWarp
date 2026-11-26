option(BLAH2_TEST_SOFTWARE_VULKAN "Run explicit CPU-only Vulkan parity tests in CTest (not physical GPU qualification)" OFF)
add_executable(testGpuWorkerDouble ${PROJECT_ROOT}/test/gpu/WorkerDouble.cpp)
target_link_libraries(testGpuWorkerDouble PRIVATE blah2GpuProcess)
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
add_executable(testGpuDeviceSelection ${PROJECT_ROOT}/test/gpu/TestGpuDeviceSelection.cpp)
target_compile_features(testGpuDeviceSelection PRIVATE cxx_std_17)
target_include_directories(testGpuDeviceSelection PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(testGpuDeviceSelection PRIVATE blah2GpuProcess blah2RapidJson)
add_test(NAME gpuResidentDeviceSelection COMMAND testGpuDeviceSelection)
add_executable(testReplayGpuQualification ${PROJECT_ROOT}/test/gpu/TestReplayGpuQualification.cpp)
target_compile_features(testReplayGpuQualification PRIVATE cxx_std_17)
target_include_directories(testReplayGpuQualification PRIVATE ${PROJECT_ROOT}/src)
add_test(NAME gpuReplayQualificationOptIn COMMAND testReplayGpuQualification)
set_tests_properties(gpuReplayQualificationOptIn PROPERTIES TIMEOUT 10)
add_executable(testClutterReplayCheckpoint ${PROJECT_ROOT}/test/gpu/TestClutterReplayCheckpoint.cpp)
target_compile_features(testClutterReplayCheckpoint PRIVATE cxx_std_17)
target_include_directories(testClutterReplayCheckpoint PRIVATE ${PROJECT_ROOT}/src)
add_test(NAME clutterReplayCheckpointCpuContract COMMAND testClutterReplayCheckpoint)
set_tests_properties(clutterReplayCheckpointCpuContract PROPERTIES TIMEOUT 10)
if(TARGET blah2GpuVulkan)
  add_executable(testClutterReplaySoftware ${PROJECT_ROOT}/test/gpu/TestClutterReplaySoftware.cpp)
  target_compile_features(testClutterReplaySoftware PRIVATE cxx_std_17)
  target_compile_definitions(testClutterReplaySoftware PRIVATE VKFFT_BACKEND=0)
  target_include_directories(testClutterReplaySoftware PRIVATE ${PROJECT_ROOT}/src)
  target_include_directories(testClutterReplaySoftware SYSTEM PRIVATE
    ${VKFFT_ROOT}/vkFFT ${Vulkan_INCLUDE_DIRS} ${ARMADILLO_INCLUDE_DIRS} ${GLSLANG_C_INCLUDE})
  target_link_libraries(testClutterReplaySoftware PRIVATE Vulkan::Vulkan glslang::glslang
    glslang::SPIRV glslang::glslang-default-resource-limits ${BLAH2_FFTW3_LIBRARY}
    ${BLAH2_FFTW3_THREADS_LIBRARY} ${ARMADILLO_LIBRARIES} Threads::Threads)
  target_compile_options(testClutterReplaySoftware PRIVATE -Wno-missing-field-initializers)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(testClutterReplaySoftware PRIVATE -Wno-return-type-c-linkage)
  endif()
  blah2_isolate_vkfft_vendor_warnings(testClutterReplaySoftware)
  if(BLAH2_TEST_SOFTWARE_VULKAN)
    add_test(NAME clutterReplaySoftwareVulkanParity COMMAND testClutterReplaySoftware --software)
    add_test(NAME clutterReplaySoftwareVulkanNativeParity COMMAND testClutterReplaySoftware --software --native)
    set_tests_properties(clutterReplaySoftwareVulkanParity clutterReplaySoftwareVulkanNativeParity
      PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS "software-vulkan;clutter-replay")
  endif()
endif()
add_executable(testGpuReceiveDeadline ${PROJECT_ROOT}/test/gpu/TestGpuReceiveDeadline.cpp)
target_compile_features(testGpuReceiveDeadline PRIVATE cxx_std_17)
target_include_directories(testGpuReceiveDeadline PRIVATE ${PROJECT_ROOT}/src)
add_test(NAME gpuReceiveDeadlineCpuContract COMMAND testGpuReceiveDeadline)
set_tests_properties(gpuReceiveDeadlineCpuContract PROPERTIES TIMEOUT 10)
add_executable(testGpuWidebandGather ${PROJECT_ROOT}/test/gpu/TestGpuWidebandGather.cpp)
target_compile_features(testGpuWidebandGather PRIVATE cxx_std_17)
target_link_libraries(testGpuWidebandGather PRIVATE blah2GpuWidebandProcess)
add_test(NAME gpuWidebandGatherCpuContract COMMAND testGpuWidebandGather)
set_tests_properties(gpuWidebandGatherCpuContract PROPERTIES TIMEOUT 30)
if(TARGET blah2GpuWidebandVulkan)
  # Explicit CPU-Vulkan execution only; never scheduled on a physical device.
  add_executable(testGpuWidebandGatherSoftware ${PROJECT_ROOT}/test/gpu/TestGpuWidebandGather.cpp)
  target_compile_features(testGpuWidebandGatherSoftware PRIVATE cxx_std_17)
  target_compile_definitions(testGpuWidebandGatherSoftware PRIVATE
    VKFFT_BACKEND=0 VECTORWARP_TEST_GATHER_VULKAN=1)
  target_compile_options(testGpuWidebandGatherSoftware PRIVATE -Wno-missing-field-initializers)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(testGpuWidebandGatherSoftware PRIVATE -Wno-return-type-c-linkage)
  endif()
  target_include_directories(testGpuWidebandGatherSoftware PRIVATE
    ${PROJECT_ROOT}/src/process/ambiguity)
  target_include_directories(testGpuWidebandGatherSoftware SYSTEM PRIVATE
    ${VKFFT_ROOT}/vkFFT ${Vulkan_INCLUDE_DIRS} ${GLSLANG_C_INCLUDE})
  target_link_libraries(testGpuWidebandGatherSoftware PRIVATE blah2GpuWidebandProcess blah2RapidJson
    Vulkan::Vulkan glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits
    ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY})
  blah2_isolate_vkfft_vendor_warnings(testGpuWidebandGatherSoftware)
endif()
set_tests_properties(gpuResidentDeviceSelection PROPERTIES TIMEOUT 10)
add_executable(testStageExecution ${PROJECT_ROOT}/test/gpu/TestStageExecution.cpp)
target_compile_features(testStageExecution PRIVATE cxx_std_17)
target_include_directories(testStageExecution PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(testStageExecution PRIVATE blah2RapidJson)
add_test(NAME gpuStageExecutionLineage COMMAND testStageExecution)
set_tests_properties(gpuStageExecutionLineage PROPERTIES TIMEOUT 10)
if(APPLE)
  add_executable(testMacGpuCompatibility ${PROJECT_ROOT}/test/gpu/TestMacGpuCompatibility.cpp)
  target_compile_features(testMacGpuCompatibility PRIVATE cxx_std_17)
  target_include_directories(testMacGpuCompatibility PRIVATE ${PROJECT_ROOT}/src)
  add_test(NAME macGpuCompatibilityPolicy COMMAND testMacGpuCompatibility)
endif()
add_executable(testGpuProcess ${PROJECT_ROOT}/test/gpu/TestGpuProcess.cpp)
target_link_libraries(testGpuProcess PRIVATE blah2Acceleration)
add_dependencies(testGpuProcess testGpuWorkerDouble)
add_test(NAME gpuProcessIsolation COMMAND testGpuProcess)
set_tests_properties(gpuProcessIsolation PROPERTIES TIMEOUT 30)
add_executable(testGpuRetiredWork ${PROJECT_ROOT}/test/gpu/TestGpuRetiredWork.cpp)
target_link_libraries(testGpuRetiredWork PRIVATE blah2GpuProcess)
add_test(NAME gpuRetiredWorkRejected COMMAND testGpuRetiredWork)
set_tests_properties(gpuRetiredWorkRejected PROPERTIES TIMEOUT 15)
add_executable(testClutterAcceleration ${PROJECT_ROOT}/test/gpu/TestClutterAcceleration.cpp)
target_link_libraries(testClutterAcceleration PRIVATE blah2Acceleration)
if(TARGET blah2GpuWorker)
  add_dependencies(testClutterAcceleration blah2GpuWorker)
endif()
add_test(NAME gpuClutterCpuOracle COMMAND testClutterAcceleration)
set_tests_properties(gpuClutterCpuOracle PROPERTIES TIMEOUT 30)

add_executable(testClutterBatch ${PROJECT_ROOT}/test/gpu/TestClutterBatch.cpp)
target_compile_features(testClutterBatch PRIVATE cxx_std_17)
target_include_directories(testClutterBatch PRIVATE ${PROJECT_ROOT}/src)
add_test(NAME gpuClutterBatch COMMAND testClutterBatch)
set_tests_properties(gpuClutterBatch PROPERTIES TIMEOUT 10)

# Driver-independent coverage for asynchronous reference preparation and its
# shared-fence recovery, including GPU=OFF/Linux/macOS builds.
add_executable(testClutterSubmission ${PROJECT_ROOT}/test/gpu/TestClutterSubmission.cpp)
target_compile_features(testClutterSubmission PRIVATE cxx_std_17)
target_include_directories(testClutterSubmission PRIVATE ${PROJECT_ROOT}/src)
add_test(NAME gpuClutterSubmissionLifetime COMMAND testClutterSubmission)
set_tests_properties(gpuClutterSubmissionLifetime PROPERTIES TIMEOUT 10)

# The FP64 baseline has an independent direct-convolution CPU oracle. This
# default regression needs no Vulkan driver or worker, including GPU=OFF builds.
find_path(BLAH2_FFTW3_INCLUDE_DIR NAMES fftw3.h)
if(NOT BLAH2_FFTW3_INCLUDE_DIR)
  message(FATAL_ERROR "GPU clutter CPU regression needs FFTW development headers")
endif()
add_executable(testClutterRefinement ${PROJECT_ROOT}/test/gpu/TestClutterRefinement.cpp)
target_compile_features(testClutterRefinement PRIVATE cxx_std_17)
target_include_directories(testClutterRefinement PRIVATE ${PROJECT_ROOT}/src)
target_include_directories(testClutterRefinement SYSTEM PRIVATE ${BLAH2_FFTW3_INCLUDE_DIR})
target_link_libraries(testClutterRefinement PRIVATE
  ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY} Threads::Threads)
add_test(NAME gpuClutterRefinementCpuOracle COMMAND testClutterRefinement)
set_tests_properties(gpuClutterRefinementCpuOracle PROPERTIES TIMEOUT 10)

# Host solver regression also runs on GPU=OFF platforms. The largest dense
# matrix uses the HomePortal 1261-tap span, without allocating a full live CPI.
add_executable(testClutterSolver ${PROJECT_ROOT}/test/gpu/TestClutterSolver.cpp)
target_link_libraries(testClutterSolver PRIVATE blah2RadarDsp)
add_test(NAME gpuClutterDenseSolverCpuOracle COMMAND testClutterSolver)
set_tests_properties(gpuClutterDenseSolverCpuOracle PROPERTIES TIMEOUT 240
  ENVIRONMENT "OPENBLAS_NUM_THREADS=1;OMP_NUM_THREADS=1")

# Explicit physical-device regression: production geometry and strict retention
# of both qualified GPU stages. Never run this expensive case in default CTest.
add_executable(testClutterProduction ${PROJECT_ROOT}/test/gpu/TestClutterProduction.cpp)
target_link_libraries(testClutterProduction PRIVATE blah2Acceleration)
if(TARGET blah2GpuWorker)
  add_dependencies(testClutterProduction blah2GpuWorker)
endif()

# The diagnostic command is tested against a sibling stub, never a host driver.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BLAH2_STANDALONE_GPU_TEST_VKFFT "")
if(BLAH2_GPU_PORTABLE_BUILT)
  set(BLAH2_STANDALONE_GPU_TEST_VKFFT "${VKFFT_ROOT}")
endif()
set(BLAH2_CMAKE_APPLICATION_CHECK)
if(TARGET blah2)
  set(BLAH2_CMAKE_APPLICATION_CHECK --application)
endif()
add_test(NAME gpuStandaloneCmakeGraphs COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/gpu/test_standalone_cmake.py
  ${BLAH2_CMAKE_APPLICATION_CHECK}
  --vkfft-root "${BLAH2_STANDALONE_GPU_TEST_VKFFT}"
  --toolchain "${CMAKE_TOOLCHAIN_FILE}"
  --vcpkg-installed-dir "${VCPKG_INSTALLED_DIR}"
  --prefix-path "${CMAKE_PREFIX_PATH}")
set_tests_properties(gpuStandaloneCmakeGraphs PROPERTIES TIMEOUT 180)
add_executable(testGpuOrphanParent ${PROJECT_ROOT}/test/gpu/OrphanParent.cpp)
target_link_libraries(testGpuOrphanParent PRIVATE blah2GpuProcess)
add_dependencies(testGpuOrphanParent testGpuWorkerDouble)
add_test(NAME gpuWorkerParentDeath COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/gpu/test_parent_death.py $<TARGET_FILE:testGpuOrphanParent>)
set_tests_properties(gpuWorkerParentDeath PROPERTIES TIMEOUT 20)
add_executable(testGpuWorkerLifetime ${PROJECT_ROOT}/test/gpu/TestGpuWorkerLifetime.cpp)
target_compile_features(testGpuWorkerLifetime PRIVATE cxx_std_17)
target_include_directories(testGpuWorkerLifetime PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(testGpuWorkerLifetime PRIVATE Threads::Threads)
add_test(NAME gpuWorkerLifetime COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/gpu/test_worker_lifetime.py $<TARGET_FILE:testGpuWorkerLifetime>)
set_tests_properties(gpuWorkerLifetime PROPERTIES TIMEOUT 30)
add_test(NAME gpuWorkerPidNamespace COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/gpu/test_worker_lifetime.py $<TARGET_FILE:testGpuWorkerLifetime> --pid-namespace)
set_tests_properties(gpuWorkerPidNamespace PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
add_library(testGpuDriverModule MODULE ${PROJECT_ROOT}/test/gpu/DriverStatusModule.cpp)
target_compile_features(testGpuDriverModule PRIVATE cxx_std_17)
target_include_directories(testGpuDriverModule PRIVATE ${PROJECT_ROOT}/src)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(testGpuDriverModule PRIVATE -Wno-return-type-c-linkage)
endif()
set_target_properties(testGpuDriverModule PROPERTIES PREFIX "" OUTPUT_NAME "blah2-gpu-vulkan"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/driver-status-fixture")
set(BLAH2_GPU_DRIVER_WORKER_SOURCES ${PROJECT_ROOT}/src/process/ambiguity/GpuWorker.cpp)
if(APPLE)
  set(BLAH2_MAC_GPU_COMPATIBILITY ${PROJECT_ROOT}/src/process/ambiguity/MacGpuCompatibility.mm)
  set_source_files_properties(${BLAH2_MAC_GPU_COMPATIBILITY} PROPERTIES
    LANGUAGE CXX COMPILE_FLAGS "-x objective-c++")
  list(APPEND BLAH2_GPU_DRIVER_WORKER_SOURCES ${BLAH2_MAC_GPU_COMPATIBILITY})
  find_library(BLAH2_METAL_FRAMEWORK Metal)
  find_library(BLAH2_FOUNDATION_FRAMEWORK Foundation)
  if(NOT BLAH2_METAL_FRAMEWORK OR NOT BLAH2_FOUNDATION_FRAMEWORK)
    message(FATAL_ERROR "macOS GPU worker compatibility needs Metal and Foundation frameworks")
  endif()
endif()
add_executable(testGpuDriverWorker ${BLAH2_GPU_DRIVER_WORKER_SOURCES})
target_link_libraries(testGpuDriverWorker PRIVATE blah2GpuProcess
  blah2GpuWidebandProcess blah2GpuResidentProcess ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(testGpuDriverWorker PRIVATE ${BLAH2_METAL_FRAMEWORK} ${BLAH2_FOUNDATION_FRAMEWORK})
endif()
set_target_properties(testGpuDriverWorker PROPERTIES OUTPUT_NAME "blah2-gpu-worker"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/driver-status-fixture")
add_dependencies(testGpuDriverWorker testGpuDriverModule)
add_test(NAME gpuDriverDiagnostics COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/gpu/test_driver_status.py $<TARGET_FILE:testGpuDriverWorker>)
set_tests_properties(gpuDriverDiagnostics PROPERTIES TIMEOUT 30)


# CPU-only resident wire and result-publication regressions; no device/module is loaded.
add_executable(testGpuResidentWire ${PROJECT_ROOT}/test/gpu/TestGpuResidentWire.cpp)
target_compile_features(testGpuResidentWire PRIVATE cxx_std_17)
target_include_directories(testGpuResidentWire PRIVATE
  ${PROJECT_ROOT}/src ${PROJECT_ROOT}/src/process/ambiguity)
target_link_libraries(testGpuResidentWire PRIVATE blah2GpuResidentProcess)
add_test(NAME residentWireCpuBoundary COMMAND testGpuResidentWire)
set_tests_properties(residentWireCpuBoundary PROPERTIES TIMEOUT 10)

# Compile exact sources directly: the CPU double owns
# gpuSiblingPath, so linking blah2GpuProcess would duplicate that definition.
add_executable(testGpuResidentProcess ${PROJECT_ROOT}/test/gpu/TestGpuResidentProcess.cpp
  ${PROJECT_ROOT}/src/process/analysis/ResidentReplaySnapshot.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentProcess.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentWire.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentProof.cpp)
target_compile_features(testGpuResidentProcess PRIVATE cxx_std_17)
target_include_directories(testGpuResidentProcess PRIVATE
  ${PROJECT_ROOT}/src ${PROJECT_ROOT}/src/process/ambiguity)
target_link_libraries(testGpuResidentProcess PRIVATE Threads::Threads blah2RapidJson)
add_test(NAME residentProcessCpuBoundary COMMAND testGpuResidentProcess)
set_tests_properties(residentProcessCpuBoundary PROPERTIES TIMEOUT 10 RUN_SERIAL TRUE)

add_executable(testResidentReplaySnapshot ${PROJECT_ROOT}/test/gpu/TestResidentReplaySnapshot.cpp)
target_compile_features(testResidentReplaySnapshot PRIVATE cxx_std_17)
target_include_directories(testResidentReplaySnapshot PRIVATE ${PROJECT_ROOT}/src ${PROJECT_ROOT}/src/process/ambiguity)
target_link_libraries(testResidentReplaySnapshot PRIVATE blah2GpuResidentProcess)
add_test(NAME residentReplaySnapshotCpuContract COMMAND testResidentReplaySnapshot)
set_tests_properties(residentReplaySnapshotCpuContract PROPERTIES TIMEOUT 60)
if(TARGET blah2GpuResidentVulkan)
  function(blah2_add_resident_vulkan_test name)
    add_executable(test${name} ${PROJECT_ROOT}/test/gpu/Test${name}.cpp)
    target_compile_features(test${name} PRIVATE cxx_std_17)
    target_compile_definitions(test${name} PRIVATE VKFFT_BACKEND=0)
    target_compile_options(test${name} PRIVATE -Wno-missing-field-initializers)
    target_include_directories(test${name} PRIVATE
      ${PROJECT_ROOT}/src ${PROJECT_ROOT}/src/process/ambiguity)
    target_include_directories(test${name} SYSTEM PRIVATE
      ${VKFFT_ROOT}/vkFFT ${Vulkan_INCLUDE_DIRS} ${GLSLANG_C_INCLUDE})
    target_link_libraries(test${name} PRIVATE ${ARGN} blah2RapidJson Vulkan::Vulkan
      glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits Threads::Threads)
  endfunction()
  # Explicitly invoked only: this small numerical test requires a physical GPU.
  blah2_add_resident_vulkan_test(ArrayReferencePhysical blah2RadarDsp)
  blah2_add_resident_vulkan_test(ResidentReplayShaders)
  add_test(NAME residentReplayShaderCpuCompile COMMAND testResidentReplayShaders)
  set_tests_properties(residentReplayShaderCpuCompile PROPERTIES TIMEOUT 30)
  # CPU-Vulkan execution is opt-in and refuses physical devices.
  blah2_add_resident_vulkan_test(GpuTrackerCorrectness blah2Tracking)
  blah2_add_resident_vulkan_test(GpuTbdFractional blah2Detection blah2Fusion)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    foreach(target IN ITEMS testGpuTrackerCorrectness testGpuTbdFractional)
      target_compile_options(${target} PRIVATE -Wno-return-type-c-linkage -Wno-unused-private-field)
    endforeach()
  endif()
endif()
if(BLAH2_TEST_SOFTWARE_VULKAN)
  if(NOT TARGET testGpuTbdFractional OR NOT TARGET testGpuTrackerCorrectness)
    message(FATAL_ERROR "BLAH2_TEST_SOFTWARE_VULKAN requires the resident Vulkan test targets")
  endif()
  add_test(NAME residentTemporalSoftwareVulkanParity COMMAND testGpuTbdFractional --software)
  add_test(NAME residentTrackerSoftwareVulkanParity COMMAND testGpuTrackerCorrectness --software)
  set_tests_properties(residentTemporalSoftwareVulkanParity residentTrackerSoftwareVulkanParity
    PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE LABELS "software-vulkan;resident-parity")
endif()

add_executable(testGpuResultJson ${PROJECT_ROOT}/test/gpu/TestGpuResultJson.cpp)
target_compile_features(testGpuResultJson PRIVATE cxx_std_17)
target_include_directories(testGpuResultJson PRIVATE
  ${PROJECT_ROOT}/src ${PROJECT_ROOT}/src/process/ambiguity)
target_include_directories(testGpuResultJson SYSTEM PRIVATE ${ARMADILLO_INCLUDE_DIRS})
target_link_libraries(testGpuResultJson PRIVATE blah2RadarData blah2IqData blah2FixedExecutor blah2Detection blah2Tracking
  ${ARMADILLO_LIBRARIES} Threads::Threads)
add_test(NAME gpuResultJsonCpuCompatibility COMMAND testGpuResultJson)

add_executable(testStartupIqCalibration ${PROJECT_ROOT}/test/gpu/TestStartupIqCalibration.cpp)
target_compile_features(testStartupIqCalibration PRIVATE cxx_std_17)
target_include_directories(testStartupIqCalibration PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(testStartupIqCalibration PRIVATE blah2RadarDsp blah2Fusion blah2Detection blah2Tracking)
add_test(NAME startupIqCalibrationCpuContract COMMAND testStartupIqCalibration)
set_tests_properties(startupIqCalibrationCpuContract PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
  ENVIRONMENT "OPENBLAS_NUM_THREADS=1;OMP_NUM_THREADS=1;VECTORWARP_CLUTTER_WORKERS=1")

add_executable(testBestEffortSocket ${PROJECT_ROOT}/test/gpu/TestBestEffortSocket.cpp)
target_compile_features(testBestEffortSocket PRIVATE cxx_std_17)
target_include_directories(testBestEffortSocket PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(testBestEffortSocket PRIVATE asio::asio Threads::Threads)
add_test(NAME causalBestEffortSocket COMMAND testBestEffortSocket)
set_tests_properties(causalBestEffortSocket PROPERTIES TIMEOUT 10)

# Explicit offline adapter: emits unthresholded statistics only, never runs as
# a production stage and never declares PFA/false-track qualification.
add_executable(weakEvidenceBootstrap ${PROJECT_ROOT}/test/recording/WeakEvidenceBootstrap.cpp)
target_link_libraries(weakEvidenceBootstrap PRIVATE blah2RadarData blah2FixedExecutor)
add_executable(testTrackerZeroAcceleration ${PROJECT_ROOT}/test/gpu/TestTrackerZeroAcceleration.cpp)
target_link_libraries(testTrackerZeroAcceleration PRIVATE blah2Tracking)
add_test(NAME trackerZeroAcceleration COMMAND testTrackerZeroAcceleration)
set_tests_properties(gpuResultJsonCpuCompatibility PROPERTIES TIMEOUT 10)

add_executable(testFrameDiagnostics ${PROJECT_ROOT}/test/gpu/TestFrameDiagnostics.cpp)
target_compile_features(testFrameDiagnostics PRIVATE cxx_std_17)
target_include_directories(testFrameDiagnostics PRIVATE ${PROJECT_ROOT})
target_link_libraries(testFrameDiagnostics PRIVATE Threads::Threads)
add_test(NAME residentFrameDiagnosticsCpuContract COMMAND testFrameDiagnostics)
set_tests_properties(residentFrameDiagnosticsCpuContract PROPERTIES TIMEOUT 10)

add_executable(testGpuTrackerAdmission ${PROJECT_ROOT}/test/gpu/TestGpuTrackerAdmission.cpp)
target_compile_features(testGpuTrackerAdmission PRIVATE cxx_std_17)
target_include_directories(testGpuTrackerAdmission PRIVATE ${PROJECT_ROOT} ${PROJECT_ROOT}/src)
add_test(NAME gpuTrackerCapacityAdmission COMMAND testGpuTrackerAdmission)
add_test(NAME gpuTrackerLineageIntegerContract COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/script/test_gpu_tracker_lineage_contract.py)
set_tests_properties(gpuTrackerLineageIntegerContract PROPERTIES TIMEOUT 45
  ENVIRONMENT "CXX=${CMAKE_CXX_COMPILER}")
set_tests_properties(gpuTrackerCapacityAdmission PROPERTIES TIMEOUT 10)

add_executable(testTrackBeforeDetect ${PROJECT_ROOT}/test/gpu/TestTrackBeforeDetect.cpp)
target_compile_features(testTrackBeforeDetect PRIVATE cxx_std_17)
target_include_directories(testTrackBeforeDetect PRIVATE ${PROJECT_ROOT})
add_test(NAME trackBeforeDetectMath COMMAND testTrackBeforeDetect)
set_tests_properties(trackBeforeDetectMath PROPERTIES TIMEOUT 10)


option(BLAH2_TEST_PHYSICAL_GPU "Run synthetic qualification on a real GPU (no CPU substitution)" OFF)
set(BLAH2_TEST_GPU_DEVICE "" CACHE STRING
  "Exact Vulkan device id for resident and wideband physical tests")
if(BLAH2_TEST_PHYSICAL_GPU)
  if(NOT TARGET blah2GpuWorker)
    message(FATAL_ERROR "Physical GPU tests require BLAH2_GPU=ON and its dependencies")
  endif()
  if(NOT TARGET testVulkanSmall)
    add_executable(testVulkanSmall ${PROJECT_ROOT}/test/gpu/TestVulkanSmall.cpp)
    target_link_libraries(testVulkanSmall PRIVATE blah2GpuProcess)
  endif()
  add_dependencies(testVulkanSmall blah2GpuWorker)
  add_dependencies(testAcceleration blah2GpuWorker)
  if(TARGET blah2GpuResidentVulkan)
    add_executable(testGpuResidentTbdPhysical
      ${PROJECT_ROOT}/test/gpu/TestGpuResidentTbdPhysical.cpp)
    target_compile_features(testGpuResidentTbdPhysical PRIVATE cxx_std_17)
    target_include_directories(testGpuResidentTbdPhysical PRIVATE
      ${PROJECT_ROOT}/src ${PROJECT_ROOT}/src/process/ambiguity)
    target_link_libraries(testGpuResidentTbdPhysical PRIVATE
      blah2GpuProcess blah2GpuResidentProcess Threads::Threads)
    add_dependencies(testGpuResidentTbdPhysical blah2GpuWorker blah2GpuResidentVulkan)
  endif()
  add_test(NAME gpuPhysicalDirectDft COMMAND testVulkanSmall)
  add_test(NAME gpuPhysicalNumerics COMMAND testAcceleration auto --matrix)
  add_test(NAME gpuPhysicalClutter COMMAND testClutterAcceleration auto)
  add_test(NAME gpuPhysicalCapacity COMMAND testAcceleration --limits)
  if(BLAH2_TEST_GPU_DEVICE AND TARGET testGpuResidentTbdPhysical)
    add_test(NAME gpuPhysicalResidentTbd COMMAND testGpuResidentTbdPhysical
      $<TARGET_FILE:blah2GpuWorker> ${BLAH2_TEST_GPU_DEVICE})
    add_test(NAME gpuPhysicalResidentReplay COMMAND testGpuResidentTbdPhysical
      $<TARGET_FILE:blah2GpuWorker> ${BLAH2_TEST_GPU_DEVICE} --snapshot-parity)
  endif()
  add_test(NAME gpuPhysicalEndurance COMMAND testAcceleration auto --endurance)
  set(physicalGpuTests gpuPhysicalDirectDft gpuPhysicalNumerics gpuPhysicalClutter
    gpuPhysicalCapacity gpuPhysicalEndurance)
  if(TARGET testGpuWidebandPhysical AND TARGET testGpuWidebandEndToEnd)
    add_test(NAME gpuPhysicalWideband COMMAND testGpuWidebandPhysical)
    # Keep the registered smoke geometry bounded; the executables retain their
    # larger configurable benchmark envelopes for separate qualification.
    add_test(NAME gpuPhysicalWidebandEndToEnd COMMAND testGpuWidebandEndToEnd
      --input-rate 1200000 --output-rate 300000 --lanes 2 --iterations 6
      --lane-workers 1 --lane-acceleration gpu)
    add_test(NAME gpuPhysicalWidebandAuto COMMAND testGpuWidebandEndToEnd
      --input-rate 1200000 --output-rate 300000 --lanes 2 --iterations 6
      --lane-workers 1 --lane-acceleration auto)
    set_tests_properties(gpuPhysicalWideband PROPERTIES
      ENVIRONMENT "VW_FRAMES=6;VW_GAP_EVERY=3")
    set(widebandGpuTests gpuPhysicalWideband gpuPhysicalWidebandEndToEnd
      gpuPhysicalWidebandAuto)
    # A prefix-only first CPI must not hide phase drift in later full CPIs.
    foreach(offset IN ITEMS -437501 437501 -437500.25 437500.25)
      set(phaseTest "gpuPhysicalWidebandPhase${offset}")
      add_test(NAME ${phaseTest} COMMAND testGpuWidebandPhysical)
      set_tests_properties(${phaseTest} PROPERTIES ENVIRONMENT
        "VW_INPUT_RATE=2400000;VW_OUTPUT_RATE=300000;VW_CHANNELS=2;VW_LANES=1;VW_SAMPLES=1000000;VW_FRAMES=3;VW_FIRST=17;VW_GAP_EVERY=2;VW_OFFSET_HZ=${offset};VW_PREFIX_BURST=4096")
      list(APPEND widebandGpuTests ${phaseTest})
    endforeach()
    if(BLAH2_TEST_GPU_DEVICE)
      set_property(TEST ${widebandGpuTests} APPEND PROPERTY ENVIRONMENT
        "BLAH2_GPU_DEVICE=${BLAH2_TEST_GPU_DEVICE}")
    endif()
    list(APPEND physicalGpuTests ${widebandGpuTests})
  endif()
  if(BLAH2_TEST_GPU_DEVICE AND TARGET testGpuResidentTbdPhysical)
    list(APPEND physicalGpuTests gpuPhysicalResidentTbd gpuPhysicalResidentReplay)
  endif()
  set_tests_properties(${physicalGpuTests} PROPERTIES
    TIMEOUT 240 RUN_SERIAL TRUE LABELS physical-gpu)
endif()
