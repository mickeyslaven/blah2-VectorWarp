cmake_minimum_required(VERSION 3.16)
project(InterpolationSnrRegression LANGUAGES CXX)
include(CTest)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DSP_ROOT "${PROJECT_ROOT}" CACHE PATH "Source tree containing production interpolation/data classes")
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
add_executable(testInterpolationSnr
  ${PROJECT_ROOT}/test/unit/process/detection/TestInterpolationSnr.cpp
  ${DSP_ROOT}/src/process/detection/Interpolate.cpp
  ${DSP_ROOT}/src/data/Map.cpp ${DSP_ROOT}/src/data/Detection.cpp)
target_compile_features(testInterpolationSnr PRIVATE cxx_std_17)
target_include_directories(testInterpolationSnr PRIVATE ${DSP_ROOT}/src)
target_link_libraries(testInterpolationSnr PRIVATE blah2RapidJson)
add_test(NAME interpolationSnr COMMAND testInterpolationSnr)
