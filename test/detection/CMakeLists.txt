cmake_minimum_required(VERSION 3.16)
project(InterpolationSnrRegression LANGUAGES CXX)
include(CTest)
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DSP_ROOT "${PROJECT_ROOT}" CACHE PATH "Source tree containing production interpolation/data classes")
add_executable(testInterpolationSnr
  ${PROJECT_ROOT}/test/unit/process/detection/TestInterpolationSnr.cpp)
if(DSP_ROOT STREQUAL PROJECT_ROOT)
  include(${PROJECT_ROOT}/cmake/RadarProducts.cmake)
  target_link_libraries(testInterpolationSnr PRIVATE blah2Detection)
else()
  # Explicit external-source numerical oracle, never the production graph.
  include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
  target_sources(testInterpolationSnr PRIVATE
    ${DSP_ROOT}/src/process/detection/Interpolate.cpp
    ${DSP_ROOT}/src/data/Map.cpp ${DSP_ROOT}/src/data/Detection.cpp)
  target_compile_features(testInterpolationSnr PRIVATE cxx_std_17)
  target_include_directories(testInterpolationSnr PRIVATE ${DSP_ROOT}/src)
  target_link_libraries(testInterpolationSnr PRIVATE blah2RapidJson)
endif()
add_test(NAME interpolationSnr COMMAND testInterpolationSnr)
