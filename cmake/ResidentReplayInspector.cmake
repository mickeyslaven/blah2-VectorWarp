# Offline validation only: no worker, GPU module, or live processor linkage.
if(NOT TARGET blah2ResidentReplayInspector)
  add_library(blah2ResidentReplayInspector STATIC
    ${PROJECT_ROOT}/src/process/analysis/ResidentReplayInspector.cpp
    ${PROJECT_ROOT}/src/process/analysis/ResidentReplaySnapshot.cpp
    ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentWire.cpp)
  target_compile_features(blah2ResidentReplayInspector PUBLIC cxx_std_17)
  target_include_directories(blah2ResidentReplayInspector PUBLIC ${PROJECT_ROOT}/src)
  target_link_libraries(blah2ResidentReplayInspector PUBLIC blah2RapidJson)
  add_executable(residentReplayInspect ${PROJECT_ROOT}/tools/ResidentReplayInspect.cpp)
  target_link_libraries(residentReplayInspect PRIVATE blah2ResidentReplayInspector)
  if(BUILD_TESTING)
    add_executable(testResidentReplayInspector ${PROJECT_ROOT}/test/gpu/TestResidentReplayInspector.cpp)
    target_include_directories(testResidentReplayInspector PRIVATE ${PROJECT_ROOT}/src/process/ambiguity)
    target_link_libraries(testResidentReplayInspector PRIVATE blah2ResidentReplayInspector)
    add_test(NAME residentReplayInspectorCpuContract COMMAND testResidentReplayInspector)
    set_tests_properties(residentReplayInspectorCpuContract PROPERTIES TIMEOUT 10)
  endif()
endif()
