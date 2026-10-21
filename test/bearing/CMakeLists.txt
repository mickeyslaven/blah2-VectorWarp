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
find_path(FFTW_INCLUDE_DIR fftw3.h REQUIRED)
find_library(FFTW_LIBRARY NAMES fftw3 REQUIRED)
find_library(FFTW_THREADS_LIBRARY NAMES fftw3_threads REQUIRED)
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)

add_executable(testBearingPhaseProjection TestBearingPhaseProjection.cpp
  ${DSP_ROOT}/src/process/ambiguity/Ambiguity.cpp
  ${DSP_ROOT}/src/process/conditioning/ArrayReferenceSynthesizer.cpp
  ${DSP_ROOT}/src/process/clutter/WienerHopf.cpp
  ${DSP_ROOT}/src/process/meta/HammingNumber.cpp
  ${DSP_ROOT}/src/data/IqData.cpp ${DSP_ROOT}/src/data/Map.cpp)
target_include_directories(testBearingPhaseProjection PRIVATE
  ${DSP_ROOT}/src ${ARMADILLO_INCLUDE_DIRS} ${FFTW_INCLUDE_DIR})
target_link_libraries(testBearingPhaseProjection PRIVATE Catch2::Catch2WithMain
  blah2RapidJson ${ARMADILLO_LIBRARIES} ${FFTW_LIBRARY} ${FFTW_THREADS_LIBRARY} Threads::Threads)
add_test(NAME bearingPhaseProjection COMMAND testBearingPhaseProjection)
set_tests_properties(bearingPhaseProjection PROPERTIES
  ENVIRONMENT "VECTORWARP_FFTW_PLAN=estimate;VECTORWARP_CLUTTER_WORKERS=1;OPENBLAS_NUM_THREADS=1")
