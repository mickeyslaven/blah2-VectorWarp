# Offline sidecar/reader only.  It deliberately has no dependency on a GPU
# worker or the live detector pipeline.
if(NOT TARGET blah2DirectPathCoherentReplay)
  add_library(blah2DirectPathCoherentReplay STATIC
    ${PROJECT_ROOT}/src/process/integration/DirectPathCoherentReplay.cpp
    ${PROJECT_ROOT}/src/process/analysis/ResidentReplaySnapshot.cpp
    ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentWire.cpp)
  target_compile_features(blah2DirectPathCoherentReplay PUBLIC cxx_std_17)
  target_include_directories(blah2DirectPathCoherentReplay PUBLIC ${PROJECT_ROOT}/src)
  target_link_libraries(blah2DirectPathCoherentReplay PUBLIC blah2RadarData blah2FixedExecutor blah2RadarDsp)
  add_executable(directPathCoherentReplayInspect
    ${PROJECT_ROOT}/tools/DirectPathCoherentReplayInspect.cpp)
  target_link_libraries(directPathCoherentReplayInspect PRIVATE blah2DirectPathCoherentReplay)
  add_executable(rangeMigrationReplayInspect
    ${PROJECT_ROOT}/tools/RangeMigrationReplayInspect.cpp)
  target_link_libraries(rangeMigrationReplayInspect PRIVATE blah2DirectPathCoherentReplay blah2RapidJson)
  add_executable(passiveIsarApertureInspect ${PROJECT_ROOT}/tools/PassiveIsarApertureInspect.cpp)
  target_link_libraries(passiveIsarApertureInspect PRIVATE blah2DirectPathCoherentReplay blah2RapidJson)
  add_executable(residentWeakEvidenceScore ${PROJECT_ROOT}/tools/ResidentWeakEvidenceScore.cpp)
  target_link_libraries(residentWeakEvidenceScore PRIVATE blah2DirectPathCoherentReplay blah2RapidJson)
  add_executable(residentSnapshotGeometry ${PROJECT_ROOT}/tools/ResidentSnapshotGeometry.cpp)
  target_link_libraries(residentSnapshotGeometry PRIVATE blah2DirectPathCoherentReplay blah2RapidJson)
  add_executable(wienerHopfReplayInspect
    ${PROJECT_ROOT}/tools/WienerHopfReplayInspect.cpp)
  target_link_libraries(wienerHopfReplayInspect PRIVATE blah2DirectPathCoherentReplay
    blah2RadarDsp)
  if(BUILD_TESTING)
    add_executable(testPassiveIsarAperture ${PROJECT_ROOT}/test/unit/process/analysis/TestPassiveIsarAperture.cpp)
    target_include_directories(testPassiveIsarAperture PRIVATE ${PROJECT_ROOT}/test/gpu ${PROJECT_ROOT}/src/process/ambiguity)
    target_link_libraries(testPassiveIsarAperture PRIVATE blah2DirectPathCoherentReplay blah2RadarDsp)
    add_test(NAME passiveIsarApertureCpuContract COMMAND testPassiveIsarAperture)
    set_tests_properties(passiveIsarApertureCpuContract PROPERTIES TIMEOUT 20
      ENVIRONMENT "VECTORWARP_BENCH_RANGE_WORKERS=0;OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1")
    add_executable(testRangeMigrationReplay
      ${PROJECT_ROOT}/test/unit/process/analysis/TestRangeMigrationReplay.cpp)
    target_link_libraries(testRangeMigrationReplay PRIVATE blah2RadarDsp)
    add_test(NAME rangeMigrationReplayCpuContract COMMAND testRangeMigrationReplay)
    set_tests_properties(rangeMigrationReplayCpuContract PROPERTIES TIMEOUT 20)
    add_executable(testResidentWeakEvidenceScore
      ${PROJECT_ROOT}/test/unit/process/analysis/TestResidentWeakEvidenceScore.cpp)
    target_include_directories(testResidentWeakEvidenceScore PRIVATE
      ${PROJECT_ROOT}/src/process/ambiguity ${PROJECT_ROOT}/test/gpu ${PROJECT_ROOT}/tools)
    target_link_libraries(testResidentWeakEvidenceScore PRIVATE blah2DirectPathCoherentReplay)
    add_test(NAME residentWeakEvidenceScoreCpuContract COMMAND testResidentWeakEvidenceScore)
    set_tests_properties(residentWeakEvidenceScoreCpuContract PROPERTIES TIMEOUT 10)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME residentWeakEvidenceScoreCliContract COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_ROOT}/test/unit/process/analysis/TestResidentWeakEvidenceScoreCli.py
        $<TARGET_FILE:residentWeakEvidenceScore> $<TARGET_FILE:testResidentWeakEvidenceScore>)
      set_tests_properties(residentWeakEvidenceScoreCliContract PROPERTIES TIMEOUT 30)
      add_test(NAME residentTruthLabelsContract COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_ROOT}/test/script/test_map_resident_truth_labels.py
        $<TARGET_FILE:residentSnapshotGeometry> $<TARGET_FILE:testResidentWeakEvidenceScore>)
      set_tests_properties(residentTruthLabelsContract PROPERTIES TIMEOUT 30)
    endif()
    add_executable(testDirectPathCoherentReplay
      ${PROJECT_ROOT}/test/unit/process/analysis/TestDirectPathCoherentReplay.cpp)
    target_include_directories(testDirectPathCoherentReplay PRIVATE
      ${PROJECT_ROOT}/src/process/ambiguity ${PROJECT_ROOT}/test/gpu)
    target_link_libraries(testDirectPathCoherentReplay PRIVATE blah2DirectPathCoherentReplay blah2RadarDsp)
    add_test(NAME directPathCoherentReplayCpuContract COMMAND testDirectPathCoherentReplay)
    set_tests_properties(directPathCoherentReplayCpuContract PROPERTIES TIMEOUT 10)
  endif()
endif()
