include_guard(GLOBAL)
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)

# Shared bounded scheduling also serves temporal product observers.
find_package(Threads REQUIRED)
add_library(blah2FixedExecutor STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/process/utility/FixedExecutor.cpp
  ${PROJECT_ROOT}/src/process/utility/ProcessingThreads.cpp)
target_compile_features(blah2FixedExecutor PUBLIC cxx_std_17)
target_include_directories(blah2FixedExecutor PUBLIC ${PROJECT_ROOT}/src)
target_link_libraries(blah2FixedExecutor PUBLIC Threads::Threads)

# These stage libraries also serve the dependency-light JSON/detection checks.
# No capture SDK, FFT, accelerator, or runtime pipeline is needed for products.
add_library(blah2RadarData STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/data/Map.cpp
  ${PROJECT_ROOT}/src/data/Detection.cpp
  ${PROJECT_ROOT}/src/data/Track.cpp)
target_link_libraries(blah2RadarData PUBLIC blah2RapidJson)
add_library(blah2Detection STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/process/detection/CfarDetector1D.cpp
  ${PROJECT_ROOT}/src/process/detection/Centroid.cpp
  ${PROJECT_ROOT}/src/process/detection/Interpolate.cpp
  )
add_library(blah2Tracking STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/process/tracker/Tracker.cpp)
add_library(blah2Fusion STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/process/fusion/Noncoherent.cpp
  ${PROJECT_ROOT}/src/process/fusion/CoherentBeamBank.cpp
  ${PROJECT_ROOT}/src/process/fusion/BearingRefinement.cpp)
foreach(target IN ITEMS blah2Detection blah2Tracking blah2Fusion)
  target_link_libraries(${target} PUBLIC blah2RadarData)
endforeach()
target_link_libraries(blah2Detection PUBLIC blah2FixedExecutor)
foreach(target IN ITEMS blah2RadarData blah2Detection blah2Tracking blah2Fusion)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_include_directories(${target} PUBLIC ${PROJECT_ROOT}/src)
endforeach()
