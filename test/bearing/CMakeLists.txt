cmake_minimum_required(VERSION 3.16)
project(BearingPhaseProjectionRegression LANGUAGES CXX)
include(CTest)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DSP_ROOT "${PROJECT_ROOT}" CACHE PATH "Tree containing production DSP sources")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Catch2 3 CONFIG REQUIRED)
find_package(Armadillo REQUIRED)
find_package(Threads REQUIRED)
find_path(BLAH2_FFTW3_INCLUDE_DIR fftw3.h REQUIRED)
find_library(BLAH2_FFTW3_LIBRARY NAMES fftw3 REQUIRED)
find_library(BLAH2_FFTW3_THREADS_LIBRARY NAMES fftw3_threads REQUIRED)
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)

add_executable(testBearingPhaseProjection TestBearingPhaseProjection.cpp)
target_link_libraries(testBearingPhaseProjection PRIVATE Catch2::Catch2WithMain)
if(DSP_ROOT STREQUAL PROJECT_ROOT)
  include(${PROJECT_ROOT}/cmake/ProcessingLibraries.cmake)
  target_link_libraries(testBearingPhaseProjection PRIVATE blah2RadarDsp)
else()
  # Explicit external-source numerical oracle, never the production graph.
  target_sources(testBearingPhaseProjection PRIVATE
    ${DSP_ROOT}/src/process/ambiguity/Ambiguity.cpp
    ${DSP_ROOT}/src/process/conditioning/ArrayReferenceSynthesizer.cpp
    ${DSP_ROOT}/src/process/clutter/WienerHopf.cpp
    ${DSP_ROOT}/src/process/meta/HammingNumber.cpp
    ${DSP_ROOT}/src/data/IqData.cpp ${DSP_ROOT}/src/data/Map.cpp)
  target_include_directories(testBearingPhaseProjection PRIVATE
    ${DSP_ROOT}/src ${ARMADILLO_INCLUDE_DIRS} ${BLAH2_FFTW3_INCLUDE_DIR})
  target_link_libraries(testBearingPhaseProjection PRIVATE blah2RapidJson
    ${ARMADILLO_LIBRARIES} ${BLAH2_FFTW3_LIBRARY} ${BLAH2_FFTW3_THREADS_LIBRARY} Threads::Threads)
endif()
add_test(NAME bearingPhaseProjection COMMAND testBearingPhaseProjection)
set_tests_properties(bearingPhaseProjection PROPERTIES
  ENVIRONMENT "VECTORWARP_FFTW_PLAN=estimate;VECTORWARP_CLUTTER_WORKERS=1;OPENBLAS_NUM_THREADS=1")
