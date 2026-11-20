cmake_minimum_required(VERSION 3.16)
project(blah2RecordedBenchmark LANGUAGES CXX)
find_package(Threads REQUIRED)
find_package(Armadillo REQUIRED)
find_library(BLAH2_FFTW3_LIBRARY NAMES fftw3 REQUIRED)
find_library(BLAH2_FFTW3_THREADS_LIBRARY NAMES fftw3_threads REQUIRED)
find_path(BLAH2_FFTW3_INCLUDE_DIR NAMES fftw3.h REQUIRED)
include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(UPSTREAM_ROOT "" CACHE PATH "Unmodified pinned upstream source")
if(NOT EXISTS "${UPSTREAM_ROOT}/src/process/ambiguity/Ambiguity.cpp")
  message(FATAL_ERROR "Set UPSTREAM_ROOT to an unmodified upstream checkout")
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(PROJECT_BINARY_TEST_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
include(${PROJECT_ROOT}/cmake/ProcessingLibraries.cmake)
include(${PROJECT_ROOT}/cmake/GpuAcceleration.cmake)
# The pinned external oracle intentionally cannot use the current implementation.
add_executable(bench-upstream Pipeline.cpp ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
foreach(file IN ITEMS process/ambiguity/Ambiguity.cpp process/clutter/WienerHopf.cpp
    process/detection/CfarDetector1D.cpp process/detection/Centroid.cpp
    process/detection/Interpolate.cpp process/tracker/Tracker.cpp
    process/spectrum/SpectrumAnalyser.cpp process/meta/HammingNumber.cpp
    data/IqData.cpp data/Map.cpp data/Detection.cpp data/Track.cpp)
  target_sources(bench-upstream PRIVATE "${UPSTREAM_ROOT}/src/${file}")
endforeach()
target_include_directories(bench-upstream PRIVATE
  "${UPSTREAM_ROOT}/src" "${PROJECT_ROOT}/src" ${ARMADILLO_INCLUDE_DIRS} ${BLAH2_FFTW3_INCLUDE_DIR})
target_compile_features(bench-upstream PRIVATE cxx_std_17)
target_link_libraries(bench-upstream PRIVATE blah2RapidJson ${BLAH2_FFTW3_LIBRARY}
  ${BLAH2_FFTW3_THREADS_LIBRARY} ${ARMADILLO_LIBRARIES} Threads::Threads)
add_executable(bench-fast Pipeline.cpp ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
target_compile_definitions(bench-fast PRIVATE BLAH2_BENCH_FAST=1)
target_link_libraries(bench-fast PRIVATE blah2MultiIlluminatorRadar)
if(TARGET blah2GpuWorker)
  add_dependencies(bench-fast blah2GpuWorker)
endif()
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
add_test(NAME recordedCampaignEvidenceOffline COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_campaign.py)
add_test(NAME recordedBenchmarkContract COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_contract.py
  $<TARGET_FILE:bench-upstream> $<TARGET_FILE:bench-fast>
  ${CMAKE_CURRENT_SOURCE_DIR}/profile-example.json)

# This launches only a local fake processor and loopback peers; never an SDR.
add_test(NAME liveRspduoHarnessOffline COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/test_live_rspduo_check.py)
set_tests_properties(liveRspduoHarnessOffline PROPERTIES TIMEOUT 60)
