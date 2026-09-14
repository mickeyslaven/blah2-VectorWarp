cmake_minimum_required(VERSION 3.8)
if(COMMAND cmake_policy)
  cmake_policy(SET CMP0003 NEW)
endif(COMMAND cmake_policy)

project(blah2)
include(CMakePrintHelpers)
include(CTest)

option(BLAH2_KRAKEN_ONLY "Build only the Kraken capture backend" OFF)
# Keep the historical CMake behavior unless a caller explicitly selects the
# individual live backends. Kraken remains the always-available baseline.
set(BLAH2_OPTIONAL_RECEIVER_DEFAULT ON)
if(BLAH2_KRAKEN_ONLY)
  set(BLAH2_OPTIONAL_RECEIVER_DEFAULT OFF)
endif()
option(BLAH2_ENABLE_RSPDUO "Build the SDRplay RSPduo live capture backend"
  ${BLAH2_OPTIONAL_RECEIVER_DEFAULT})
option(BLAH2_LOCAL_BUILD_RSPDUO "Ship our RSPduo source kit for local compilation against a user-installed SDK" OFF)
if(BLAH2_LOCAL_BUILD_RSPDUO AND BLAH2_ENABLE_RSPDUO)
  message(FATAL_ERROR "Select either a precompiled RSPduo adapter or its local build kit")
endif()
option(BLAH2_ENABLE_USRP "Build the Ettus USRP live capture backend"
  ${BLAH2_OPTIONAL_RECEIVER_DEFAULT})
option(BLAH2_ENABLE_HACKRF "Build the HackRF live capture backend"
  ${BLAH2_OPTIONAL_RECEIVER_DEFAULT})

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
include(cmake/RapidJson.cmake)
find_package(ryml CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(Armadillo REQUIRED)
if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
endif()

if(BLAH2_ENABLE_USRP)
  set(CMAKE_PREFIX_PATH "/opt/uhd" ${CMAKE_PREFIX_PATH})
  # Current capture/setter/getter API is compile-qualified against Ubuntu 22.04
  # UHD 4.1.0.5 as well as Fedora's UHD 4.9; no private newer UHD is required.
  find_package(UHD "4.1.0.0" CONFIG REQUIRED)
endif()

if(BLAH2_ENABLE_HACKRF)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(HACKRF REQUIRED IMPORTED_TARGET libhackrf)
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

# TODO: create FindSdrplay.cmake for this
if(BLAH2_ENABLE_RSPDUO)
  set(BLAH2_SDRPLAY_INCLUDE_DIR "/usr/local/include" CACHE PATH
    "Directory containing sdrplay_api.h")
  set(BLAH2_SDRPLAY_LIBRARY "/usr/local/lib/libsdrplay_api.so.3.15" CACHE FILEPATH
    "SDRplay API 3.15 shared library")
  if(NOT EXISTS "${BLAH2_SDRPLAY_INCLUDE_DIR}/sdrplay_api.h")
    message(FATAL_ERROR "SDRplay header not found: ${BLAH2_SDRPLAY_INCLUDE_DIR}/sdrplay_api.h")
  endif()
  if(NOT EXISTS "${BLAH2_SDRPLAY_LIBRARY}")
    message(FATAL_ERROR "SDRplay library not found: ${BLAH2_SDRPLAY_LIBRARY}")
  endif()
  add_library(blah2Sdrplay UNKNOWN IMPORTED)
  set_target_properties(blah2Sdrplay PROPERTIES
    IMPORTED_LOCATION "${BLAH2_SDRPLAY_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${BLAH2_SDRPLAY_INCLUDE_DIR}")
endif()

include(cmake/ReceiverModules.cmake)

# TODO: Move to separate src/CMakeLists.txt
add_executable(blah2
  src/blah2.cpp
  src/capture/Capture.cpp
  src/capture/Replay.cpp
  src/capture/kraken/Kraken.cpp
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
  src/data/Map.cpp
  src/data/Detection.cpp
  src/data/Track.cpp
  src/data/meta/Timing.cpp
)

target_link_libraries(blah2 PRIVATE 
  Threads::Threads
  blah2RapidJson
  asio::asio
  ryml::ryml
  httplib::httplib
  armadillo
  fftw3
  fftw3_threads
  ${CMAKE_DL_LIBS}
  blah2ReceiverLoader
)
set_target_properties(blah2 PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN"
  BUILD_WITH_INSTALL_RPATH TRUE)
foreach(receiver blah2ReceiverUsrp blah2ReceiverHackrf blah2ReceiverRspduo)
  if(TARGET ${receiver})
    add_dependencies(blah2 ${receiver})
  endif()
endforeach()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 13)
  # GCC 13+ can misdiagnose cpp-httplib's inlined std::regex bookkeeping.
  # Keep this external-header warning visible without making it fatal.
  target_compile_options(blah2 PRIVATE -Wno-error=array-bounds)
endif()

if(BUILD_TESTING)
  include(cmake/RecordingTests.cmake)
endif()
if(BLAH2_KRAKEN_ONLY AND NOT BLAH2_ENABLE_RSPDUO AND
    NOT BLAH2_ENABLE_USRP AND NOT BLAH2_ENABLE_HACKRF)
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
target_link_libraries(testAcceleration PRIVATE blah2RapidJson blah2GpuProcess fftw3 fftw3_threads Threads::Threads ${CMAKE_DL_LIBS})
if(TARGET blah2GpuVulkan)
  add_dependencies(testAcceleration blah2GpuWorker)
endif()
add_executable(testAccelerationFallback
  test/unit/process/ambiguity/TestAccelerationFallback.cpp
  src/process/ambiguity/Acceleration.cpp src/process/ambiguity/Ambiguity.cpp
  src/process/meta/HammingNumber.cpp src/data/IqData.cpp src/data/Map.cpp)
target_link_libraries(testAccelerationFallback PRIVATE blah2RapidJson blah2GpuProcess fftw3 fftw3_threads Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME testAccelerationFallback COMMAND testAccelerationFallback)

add_executable(testClutterFft
  test/unit/process/clutter/TestClutterFft.cpp
  src/process/clutter/WienerHopf.cpp src/data/IqData.cpp)
target_compile_features(testClutterFft PRIVATE cxx_std_17)
target_link_libraries(testClutterFft PRIVATE blah2RapidJson armadillo fftw3 Threads::Threads)
add_test(NAME clutterFftPadding COMMAND testClutterFft)
set_tests_properties(clutterFftPadding PROPERTIES TIMEOUT 30)

add_executable(testCfarEfficiency
  test/unit/process/detection/TestCfarEfficiency.cpp
  src/process/detection/CfarDetector1D.cpp src/data/Detection.cpp src/data/Map.cpp)
target_link_libraries(testCfarEfficiency PRIVATE blah2RapidJson)
add_test(NAME cfarExactOutput COMMAND testCfarEfficiency)
add_executable(testIqDataFifo
  test/unit/data/TestIqDataFifo.cpp src/data/IqData.cpp)
target_link_libraries(testIqDataFifo PRIVATE blah2RapidJson Threads::Threads)
add_test(NAME iqDataFifo COMMAND testIqDataFifo)
include(cmake/JsonOutputTests.cmake)

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
  blah2RapidJson
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
  blah2RapidJson
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
  blah2RapidJson
)
set_target_properties(testKrakenMultiChannel PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")

# TODO: Unsure if will be using CTest.
add_test(NAME testAmbiguity COMMAND testAmbiguity)
add_test(NAME testTracker COMMAND testTracker)
add_executable(testSpectrum
  test/unit/process/spectrum/TestSpectrum.cpp
  src/process/spectrum/SpectrumAnalyser.cpp src/data/IqData.cpp)
target_link_libraries(testSpectrum PRIVATE Catch2::Catch2WithMain blah2RapidJson fftw3 Threads::Threads)
set_target_properties(testSpectrum PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testSpectrum COMMAND testSpectrum)
add_executable(testSocket test/unit/process/utility/TestSocket.cpp)
target_link_libraries(testSocket PRIVATE Catch2::Catch2WithMain asio::asio Threads::Threads)
set_target_properties(testSocket PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testSocket COMMAND testSocket)
add_executable(testMapData test/unit/process/spectrum/TestMapData.cpp src/data/Map.cpp)
target_link_libraries(testMapData PRIVATE Catch2::Catch2WithMain blah2RapidJson)
set_target_properties(testMapData PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testMapData COMMAND testMapData)
add_executable(testDetectionMath test/unit/process/detection/TestDetectionMath.cpp
  src/process/detection/CfarDetector1D.cpp
  src/process/detection/Centroid.cpp src/process/detection/Interpolate.cpp
  src/data/Detection.cpp src/data/Map.cpp)
target_link_libraries(testDetectionMath PRIVATE Catch2::Catch2WithMain blah2RapidJson)
set_target_properties(testDetectionMath PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testDetectionMath COMMAND testDetectionMath)
add_executable(testInterpolationSnr test/unit/process/detection/TestInterpolationSnr.cpp
  src/process/detection/Interpolate.cpp src/data/Detection.cpp src/data/Map.cpp)
target_link_libraries(testInterpolationSnr PRIVATE blah2RapidJson)
set_target_properties(testInterpolationSnr PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_TEST_UNIT_DIR}")
add_test(NAME testInterpolationSnr COMMAND testInterpolationSnr)
add_test(NAME testHammingNumber COMMAND testHammingNumber)
add_test(NAME testKrakenMultiChannel COMMAND testKrakenMultiChannel)
endif()
