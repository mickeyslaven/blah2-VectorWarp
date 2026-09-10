cmake_minimum_required(VERSION 3.8)
if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

project(blah2)
include(CMakePrintHelpers)
include(CTest)

option(BLAH2_KRAKEN_ONLY "Build only the Kraken capture backend" OFF)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Werror")

find_package(Threads REQUIRED)
find_package(asio QUIET)
if(NOT TARGET asio::asio)
  find_path(ASIO_INCLUDE_DIR asio.hpp REQUIRED)
  add_library(asio_system INTERFACE)
  target_include_directories(asio_system INTERFACE ${ASIO_INCLUDE_DIR})
  target_compile_definitions(asio_system INTERFACE ASIO_STANDALONE)
  add_library(asio::asio ALIAS asio_system)
endif()
find_path(RAPIDJSON_INCLUDE_DIRS "rapidjson/allocators.h")
find_package(ryml CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(Armadillo REQUIRED)
if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
endif()

if(NOT BLAH2_KRAKEN_ONLY)
  set(CMAKE_PREFIX_PATH "/opt/uhd" ${CMAKE_PREFIX_PATH})
  find_package(UHD "4.8.0.0" CONFIG REQUIRED)
endif()

set(PROJECT_ROOT "${PROJECT_SOURCE_DIR}")
set(BLAH2_OUTPUT_DIR "${PROJECT_ROOT}/bin" CACHE PATH "Executable and GPU module directory")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BLAH2_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(PROJECT_BINARY_TEST_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/test")
set(PROJECT_BINARY_TEST_UNIT_DIR "${PROJECT_BINARY_TEST_DIR}/unit")
set(PROJECT_BINARY_TEST_FUNCTIONAL_DIR "${PROJECT_BINARY_TEST_DIR}/functional")
set(PROJECT_BINARY_TEST_COMPARISON_DIR "${PROJECT_BINARY_TEST_DIR}/comparison")

message("Binary path: ${PROJECT_BINARY_DIR}")
message("Binary test path: ${PROJECT_BINARY_TEST_DIR}")

# include from top-level src dir
include_directories(src)
if(NOT BLAH2_KRAKEN_ONLY)
  include_directories(${UHD_INCLUDE_DIRS})
endif()

# TODO: create FindSdrplay.cmake for this
if(NOT BLAH2_KRAKEN_ONLY)
  add_library(sdrplay /usr/local/include/sdrplay_api.h)
  set_target_properties(sdrplay PROPERTIES LINKER_LANGUAGE C)
  target_link_libraries(sdrplay PUBLIC /usr/local/lib/libsdrplay_api.so.3.15)
endif()

set(BLAH2_CAPTURE_SOURCES
  src/capture/kraken/Kraken.cpp
  src/capture/kraken/HeimdallFrame.cpp
)
set(BLAH2_CAPTURE_LIBRARIES)
if(NOT BLAH2_KRAKEN_ONLY)
  list(APPEND BLAH2_CAPTURE_SOURCES
    src/capture/rspduo/RspDuo.cpp
    src/capture/usrp/Usrp.cpp
    src/capture/hackrf/HackRf.cpp
  )
  list(APPEND BLAH2_CAPTURE_LIBRARIES
    ${UHD_LIBRARIES}
    sdrplay
    hackrf
  )
endif()

# TODO: Move to separate src/CMakeLists.txt
add_executable(blah2
  src/blah2.cpp
  src/capture/Capture.cpp
  src/capture/Source.cpp
  src/capture/Recording.cpp
  src/capture/Replay.cpp
  ${BLAH2_CAPTURE_SOURCES}
  src/process/ambiguity/Ambiguity.cpp
  src/process/ambiguity/Acceleration.cpp
  src/process/clutter/WienerHopf.cpp
  src/process/conditioning/ArrayReferenceSynthesizer.cpp
  src/process/fusion/Noncoherent.cpp
  src/process/detection/CfarDetector1D.cpp
  src/process/detection/Centroid.cpp
  src/process/detection/Interpolate.cpp
  src/process/tracker/Tracker.cpp
  src/process/spectrum/SpectrumAnalyser.cpp
  src/process/meta/HammingNumber.cpp
  src/process/utility/Socket.cpp
  src/process/utility/ProcessingThreads.cpp
  src/data/IqData.cpp
  src/data/Map.cpp
  src/data/Detection.cpp
  src/data/Track.cpp
  src/data/meta/Timing.cpp
)

target_link_libraries(blah2 PRIVATE 
  Threads::Threads
  asio::asio
  ryml::ryml
  httplib::httplib
  armadillo
  fftw3
  fftw3_threads
  ${CMAKE_DL_LIBS}
  ${BLAH2_CAPTURE_LIBRARIES}
)
# Treat the external RapidJSON headers as system headers, not project source.
# In particular, GCC 16 diagnoses unused template bodies in upstream 1.1.0.
target_include_directories(blah2 SYSTEM PRIVATE "${RAPIDJSON_INCLUDE_DIRS}")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
  # GCC 16 misdiagnoses cpp-httplib's inlined polymorphic compressor allocation.
  target_compile_options(blah2 PRIVATE -Wno-error=array-bounds)
  # Pinned RapidJSON 1.1.0 has an unused string-reference assignment template
  # diagnosed by GCC 16 even in SYSTEM headers. Keep the warning visible while
  # allowing this external-header compatibility case; other warnings stay fatal.
  target_compile_options(blah2 PRIVATE -Wno-error=template-body)
endif()

if(BUILD_TESTING)
  include(cmake/RecordingTests.cmake)
endif()
if(BLAH2_KRAKEN_ONLY)
  target_compile_definitions(blah2 PRIVATE BLAH2_KRAKEN_ONLY=1)
endif()

include(cmake/GpuAcceleration.cmake)

if(BUILD_TESTING)
add_executable(testAcceleration
  test/unit/process/ambiguity/TestAcceleration.cpp
  src/process/ambiguity/Acceleration.cpp
  src/process/ambiguity/Ambiguity.cpp
  src/process/meta/HammingNumber.cpp
  src/data/IqData.cpp src/data/Map.cpp)
target_link_libraries(testAcceleration PRIVATE blah2GpuProcess fftw3 fftw3_threads Threads::Threads ${CMAKE_DL_LIBS})
if(TARGET blah2GpuVulkan)
  add_dependencies(testAcceleration blah2GpuWorker)
endif()
add_executable(testAccelerationFallback
  test/unit/process/ambiguity/TestAccelerationFallback.cpp
  src/process/ambiguity/Acceleration.cpp src/process/ambiguity/Ambiguity.cpp
  src/process/meta/HammingNumber.cpp src/data/IqData.cpp src/data/Map.cpp)
target_link_libraries(testAccelerationFallback PRIVATE blah2GpuProcess fftw3 fftw3_threads Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME testAccelerationFallback COMMAND testAccelerationFallback)

# unit tests
include(cmake/GpuProcessTests.cmake)
add_executable(testProcessingThreads
  test/unit/process/utility/TestProcessingThreads.cpp
  src/process/utility/ProcessingThreads.cpp
)
target_link_libraries(testProcessingThreads PRIVATE Threads::Threads)
set_target_properties(testProcessingThreads PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testProcessingThreads COMMAND testProcessingThreads)

add_executable(testAmbiguity
  test/unit/process/ambiguity/TestAmbiguity.cpp
  src/data/IqData.cpp
  src/data/Map.cpp
  src/process/ambiguity/Ambiguity.cpp
  src/process/meta/HammingNumber.cpp
)
target_link_libraries(testAmbiguity PRIVATE 
  Catch2::Catch2WithMain 
  fftw3 
  fftw3_threads
)
set_target_properties(testAmbiguity PROPERTIES 
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testTracker
  test/unit/process/tracker/TestTracker.cpp
  src/data/Detection.cpp
  src/data/Track.cpp
  src/process/tracker/Tracker.cpp
)
target_link_libraries(testTracker PRIVATE 
  Catch2::Catch2WithMain
)
set_target_properties(testTracker PROPERTIES 
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testHammingNumber
  test/unit/process/meta/TestHammingNumber.cpp
  src/process/meta/HammingNumber.cpp
)
target_link_libraries(testHammingNumber PRIVATE 
  Catch2::Catch2WithMain
)
set_target_properties(testHammingNumber PROPERTIES 
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

add_executable(testKrakenMultiChannel
  test/unit/capture/kraken/TestHeimdallFrame.cpp
  test/unit/process/conditioning/TestArrayReferenceSynthesizer.cpp
  test/unit/process/fusion/TestNoncoherent.cpp
  src/capture/kraken/HeimdallFrame.cpp
  src/process/conditioning/ArrayReferenceSynthesizer.cpp
  src/process/fusion/Noncoherent.cpp
  src/data/IqData.cpp
  src/data/Map.cpp
)
target_link_libraries(testKrakenMultiChannel PRIVATE
  Catch2::Catch2WithMain
)
set_target_properties(testKrakenMultiChannel PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

# TODO: Unsure if will be using CTest.
add_test(NAME testAmbiguity COMMAND testAmbiguity)
add_test(NAME testTracker COMMAND testTracker)
add_executable(testSpectrum
  test/unit/process/spectrum/TestSpectrum.cpp
  src/process/spectrum/SpectrumAnalyser.cpp src/data/IqData.cpp)
target_link_libraries(testSpectrum PRIVATE Catch2::Catch2WithMain fftw3 Threads::Threads)
set_target_properties(testSpectrum PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testSpectrum COMMAND testSpectrum)
add_executable(testSocket test/unit/process/utility/TestSocket.cpp)
target_link_libraries(testSocket PRIVATE Catch2::Catch2WithMain asio::asio Threads::Threads)
set_target_properties(testSocket PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testSocket COMMAND testSocket)
add_executable(testMapData test/unit/process/spectrum/TestMapData.cpp src/data/Map.cpp)
target_link_libraries(testMapData PRIVATE Catch2::Catch2WithMain)
set_target_properties(testMapData PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testMapData COMMAND testMapData)
add_executable(testDetectionMath test/unit/process/detection/TestDetectionMath.cpp
  src/process/detection/CfarDetector1D.cpp
  src/process/detection/Centroid.cpp src/process/detection/Interpolate.cpp
  src/data/Detection.cpp src/data/Map.cpp)
target_link_libraries(testDetectionMath PRIVATE Catch2::Catch2WithMain)
set_target_properties(testDetectionMath PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testDetectionMath COMMAND testDetectionMath)
add_test(NAME testHammingNumber COMMAND testHammingNumber)
add_test(NAME testKrakenMultiChannel COMMAND testKrakenMultiChannel)
endif()
