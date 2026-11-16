cmake_minimum_required(VERSION 3.16)
project(blah2RecordedBenchmark LANGUAGES CXX)
find_package(Threads REQUIRED)
find_package(Armadillo REQUIRED)
include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(UPSTREAM_ROOT "" CACHE PATH "Unmodified pinned upstream source")
if(NOT EXISTS "${UPSTREAM_ROOT}/src/process/ambiguity/Ambiguity.cpp")
  message(FATAL_ERROR "Set UPSTREAM_ROOT to an unmodified upstream checkout")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
include(${PROJECT_ROOT}/cmake/GpuAcceleration.cmake)
set(DSP_FILES process/ambiguity/Ambiguity.cpp process/clutter/WienerHopf.cpp
  process/detection/CfarDetector1D.cpp process/detection/Centroid.cpp
  process/detection/Interpolate.cpp process/tracker/Tracker.cpp
  process/spectrum/SpectrumAnalyser.cpp process/meta/HammingNumber.cpp
  data/IqData.cpp data/Map.cpp data/Detection.cpp data/Track.cpp)
foreach(engine IN ITEMS upstream fast)
  if(engine STREQUAL "upstream")
    set(SOURCE_ROOT "${UPSTREAM_ROOT}")
  else()
    set(SOURCE_ROOT "${PROJECT_ROOT}")
  endif()
  add_executable(bench-${engine} Pipeline.cpp ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
  foreach(file IN LISTS DSP_FILES)
    target_sources(bench-${engine} PRIVATE "${SOURCE_ROOT}/src/${file}")
  endforeach()
  # Baseline headers must precede fork headers. Only the shared MCHQ adapter
  # comes from the fork; all baseline processing/data classes remain upstream.
  target_include_directories(bench-${engine} PRIVATE "${SOURCE_ROOT}/src" "${PROJECT_ROOT}/src" ${ARMADILLO_INCLUDE_DIRS})
  target_compile_features(bench-${engine} PRIVATE cxx_std_17)
  target_link_libraries(bench-${engine} PRIVATE blah2RapidJson fftw3 fftw3_threads ${ARMADILLO_LIBRARIES} Threads::Threads)
  if(engine STREQUAL "fast")
    target_compile_definitions(bench-${engine} PRIVATE BLAH2_BENCH_FAST=1)
    target_sources(bench-${engine} PRIVATE
      ${PROJECT_ROOT}/src/process/ambiguity/Acceleration.cpp
      ${PROJECT_ROOT}/src/process/utility/ProcessingThreads.cpp
      ${PROJECT_ROOT}/src/process/conditioning/ArrayReferenceSynthesizer.cpp
      ${PROJECT_ROOT}/src/process/fusion/Noncoherent.cpp)
    target_link_libraries(bench-${engine} PRIVATE blah2GpuProcess)
    if(TARGET blah2GpuWorker)
      add_dependencies(bench-${engine} blah2GpuWorker)
    endif()
  endif()
endforeach()
add_executable(testMchqReader TestReader.cpp ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
target_include_directories(testMchqReader PRIVATE ${PROJECT_ROOT}/src)
target_compile_features(testMchqReader PRIVATE cxx_std_17)
add_test(NAME recordedIqReader COMMAND testMchqReader)
add_executable(testRawReader TestRawReader.cpp ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
target_include_directories(testRawReader PRIVATE ${PROJECT_ROOT}/src)
target_compile_features(testRawReader PRIVATE cxx_std_17)
add_test(NAME recordedRawIqReader COMMAND testRawReader)
add_test(NAME recordedOutputComparison COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_compare_outputs.py)
add_test(NAME recordedBenchmarkContract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_contract.py
  $<TARGET_FILE:bench-upstream> $<TARGET_FILE:bench-fast>
  ${CMAKE_CURRENT_SOURCE_DIR}/profile-example.json)

# This launches only a local fake processor and loopback peers; never an SDR.
add_test(NAME liveRspduoHarnessOffline COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_live_rspduo_check.py)
set_tests_properties(liveRspduoHarnessOffline PROPERTIES TIMEOUT 60)
