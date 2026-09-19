add_executable(testGpuWorkerDouble ${PROJECT_ROOT}/test/gpu/WorkerDouble.cpp)
target_link_libraries(testGpuWorkerDouble PRIVATE blah2GpuProcess)
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
if(APPLE)
  add_executable(testMacGpuCompatibility ${PROJECT_ROOT}/test/gpu/TestMacGpuCompatibility.cpp)
  target_compile_features(testMacGpuCompatibility PRIVATE cxx_std_17)
  target_include_directories(testMacGpuCompatibility PRIVATE ${PROJECT_ROOT}/src)
  add_test(NAME macGpuCompatibilityPolicy COMMAND testMacGpuCompatibility)
endif()
add_executable(testGpuProcess ${PROJECT_ROOT}/test/gpu/TestGpuProcess.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/Acceleration.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/Ambiguity.cpp
  ${PROJECT_ROOT}/src/process/meta/HammingNumber.cpp
  ${PROJECT_ROOT}/src/data/IqData.cpp ${PROJECT_ROOT}/src/data/Map.cpp)
target_link_libraries(testGpuProcess PRIVATE blah2RapidJson blah2GpuProcess ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY})
add_dependencies(testGpuProcess testGpuWorkerDouble)
add_test(NAME gpuProcessIsolation COMMAND testGpuProcess)
set_tests_properties(gpuProcessIsolation PROPERTIES TIMEOUT 30)
add_executable(testClutterAcceleration ${PROJECT_ROOT}/test/gpu/TestClutterAcceleration.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/Acceleration.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/Ambiguity.cpp
  ${PROJECT_ROOT}/src/process/clutter/WienerHopf.cpp
  ${PROJECT_ROOT}/src/process/meta/HammingNumber.cpp
  ${PROJECT_ROOT}/src/data/IqData.cpp ${PROJECT_ROOT}/src/data/Map.cpp)
target_link_libraries(testClutterAcceleration PRIVATE blah2RapidJson blah2GpuProcess
  ${ARMADILLO_LIBRARIES} ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY} Threads::Threads)
if(TARGET blah2GpuWorker)
  add_dependencies(testClutterAcceleration blah2GpuWorker)
endif()
add_test(NAME gpuClutterCpuOracle COMMAND testClutterAcceleration)
set_tests_properties(gpuClutterCpuOracle PROPERTIES TIMEOUT 30)

# The diagnostic command is tested against a sibling stub, never a host driver.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(testGpuOrphanParent ${PROJECT_ROOT}/test/gpu/OrphanParent.cpp)
target_link_libraries(testGpuOrphanParent PRIVATE blah2GpuProcess)
add_dependencies(testGpuOrphanParent testGpuWorkerDouble)
add_test(NAME gpuWorkerParentDeath COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/gpu/test_parent_death.py $<TARGET_FILE:testGpuOrphanParent>)
set_tests_properties(gpuWorkerParentDeath PROPERTIES TIMEOUT 20)
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
target_link_libraries(testGpuDriverWorker PRIVATE blah2GpuProcess ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(testGpuDriverWorker PRIVATE ${BLAH2_METAL_FRAMEWORK} ${BLAH2_FOUNDATION_FRAMEWORK})
endif()
set_target_properties(testGpuDriverWorker PROPERTIES OUTPUT_NAME "blah2-gpu-worker"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/driver-status-fixture")
add_dependencies(testGpuDriverWorker testGpuDriverModule)
add_test(NAME gpuDriverDiagnostics COMMAND ${Python3_EXECUTABLE}
  ${PROJECT_ROOT}/test/gpu/test_driver_status.py $<TARGET_FILE:testGpuDriverWorker>)
set_tests_properties(gpuDriverDiagnostics PROPERTIES TIMEOUT 30)

option(BLAH2_TEST_PHYSICAL_GPU "Run synthetic qualification on a real GPU (no CPU substitution)" OFF)
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
  add_test(NAME gpuPhysicalDirectDft COMMAND testVulkanSmall)
  add_test(NAME gpuPhysicalNumerics COMMAND testAcceleration auto --matrix)
  add_test(NAME gpuPhysicalClutter COMMAND testClutterAcceleration auto)
  add_test(NAME gpuPhysicalCapacity COMMAND testAcceleration --limits)
  add_test(NAME gpuPhysicalEndurance COMMAND testAcceleration auto --endurance)
  set_tests_properties(gpuPhysicalDirectDft gpuPhysicalNumerics gpuPhysicalClutter
    gpuPhysicalCapacity gpuPhysicalEndurance PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE LABELS physical-gpu)
endif()
