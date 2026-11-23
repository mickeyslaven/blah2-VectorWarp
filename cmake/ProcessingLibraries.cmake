include_guard(GLOBAL)
include(${PROJECT_ROOT}/cmake/RadarProducts.cmake)

# One receiver-independent processing graph for the application, GPU checks,
# and recorded benchmarks. GPU transports are resolved by GpuAcceleration.cmake.
# Production IQ allocation/recycling has one shared-DSO owner. SDK-free
# standalone checks compile the same implementation without a capture module.
if(TARGET blah2CaptureCore)
  add_library(blah2IqData ALIAS blah2CaptureCore)
else()
  add_library(blah2IqData STATIC EXCLUDE_FROM_ALL ${PROJECT_ROOT}/src/data/IqData.cpp)
  target_compile_features(blah2IqData PUBLIC cxx_std_17)
  target_include_directories(blah2IqData PUBLIC ${PROJECT_ROOT}/src)
  target_link_libraries(blah2IqData PUBLIC blah2RapidJson Threads::Threads)
endif()

# All CPU and accelerator callers use these exact numerical objects and their
# usage requirements. Accelerator capability flags stay on the backend/pipeline.
add_library(blah2RadarDsp STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/process/ambiguity/Ambiguity.cpp
  ${PROJECT_ROOT}/src/process/clutter/WienerHopf.cpp
  ${PROJECT_ROOT}/src/process/conditioning/ArrayReferenceSynthesizer.cpp
  ${PROJECT_ROOT}/src/process/conditioning/AtscPilotProjector.cpp
  ${PROJECT_ROOT}/src/process/meta/HammingNumber.cpp
  ${PROJECT_ROOT}/src/process/spectrum/SpectrumAnalyser.cpp)
target_include_directories(blah2RadarDsp SYSTEM PUBLIC
  ${ARMADILLO_INCLUDE_DIRS} ${BLAH2_FFTW3_INCLUDE_DIR})
target_link_libraries(blah2RadarDsp PUBLIC blah2RadarData blah2IqData
  ${ARMADILLO_LIBRARIES} ${BLAH2_FFTW3_THREADS_LIBRARY}
  ${BLAH2_FFTW3_LIBRARY} Threads::Threads)
add_library(blah2Acceleration STATIC EXCLUDE_FROM_ALL ${PROJECT_ROOT}/src/process/ambiguity/Acceleration.cpp)
target_link_libraries(blah2Acceleration PUBLIC
  blah2RadarDsp blah2GpuProcess blah2FixedExecutor)

# Production orchestration shared by the live executable and raw-IQ replay.
add_library(blah2ProductionRawIq STATIC EXCLUDE_FROM_ALL ${PROJECT_ROOT}/src/process/pipeline/RawIqStages.cpp)
target_link_libraries(blah2ProductionRawIq PUBLIC
  blah2Acceleration blah2Fusion blah2FixedExecutor)

add_library(blah2Channelizer STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/process/conditioning/IlluminatorChannelizerBank.cpp
  ${PROJECT_ROOT}/src/process/conditioning/SubbandChannelizer.cpp)
target_include_directories(blah2Channelizer SYSTEM PRIVATE
  ${BLAH2_FFTW3_INCLUDE_DIR})
target_link_libraries(blah2Channelizer PUBLIC
  blah2FixedExecutor ${BLAH2_FFTW3_LIBRARY} Threads::Threads)
add_library(blah2MultiIlluminatorRadar STATIC EXCLUDE_FROM_ALL
  ${PROJECT_ROOT}/src/process/pipeline/CanonicalRadar.cpp)
target_link_libraries(blah2MultiIlluminatorRadar PUBLIC
  blah2ProductionRawIq blah2Detection blah2Tracking blah2GpuResidentProcess)

foreach(target IN ITEMS blah2FixedExecutor blah2RadarDsp blah2Acceleration blah2ProductionRawIq
    blah2Channelizer blah2MultiIlluminatorRadar)
  target_compile_features(${target} PUBLIC cxx_std_17)
  target_include_directories(${target} PUBLIC ${PROJECT_ROOT}/src)
endforeach()
