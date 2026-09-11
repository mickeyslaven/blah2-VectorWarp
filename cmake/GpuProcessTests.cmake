add_executable(testGpuWorkerDouble ${PROJECT_ROOT}/test/gpu/WorkerDouble.cpp)
target_link_libraries(testGpuWorkerDouble PRIVATE blah2GpuProcess)
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
add_executable(testGpuProcess ${PROJECT_ROOT}/test/gpu/TestGpuProcess.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/Acceleration.cpp
  ${PROJECT_ROOT}/src/process/ambiguity/Ambiguity.cpp
  ${PROJECT_ROOT}/src/process/meta/HammingNumber.cpp
  ${PROJECT_ROOT}/src/data/IqData.cpp ${PROJECT_ROOT}/src/data/Map.cpp)
target_link_libraries(testGpuProcess PRIVATE blah2RapidJson blah2GpuProcess fftw3 fftw3_threads)
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
  armadillo fftw3 fftw3_threads Threads::Threads)
if(TARGET blah2GpuWorker)
  add_dependencies(testClutterAcceleration blah2GpuWorker)
endif()
add_test(NAME gpuClutterCpuOracle COMMAND testClutterAcceleration)
set_tests_properties(gpuClutterCpuOracle PROPERTIES TIMEOUT 30)
