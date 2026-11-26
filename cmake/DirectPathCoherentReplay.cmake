# Offline replay; no live workers.
if(NOT TARGET blah2DirectPathCoherentReplay)
  add_library(blah2DirectPathCoherentReplay STATIC
    ${PROJECT_ROOT}/src/process/integration/DirectPathCoherentReplay.cpp
    ${PROJECT_ROOT}/src/process/integration/DirectPathCoherentIq.cpp
    ${PROJECT_ROOT}/src/process/analysis/ResidentReplaySnapshot.cpp
    ${PROJECT_ROOT}/src/process/ambiguity/GpuResidentWire.cpp)
  target_compile_features(blah2DirectPathCoherentReplay PUBLIC cxx_std_17)
  target_include_directories(blah2DirectPathCoherentReplay PUBLIC ${PROJECT_ROOT}/src)
  target_link_libraries(blah2DirectPathCoherentReplay PUBLIC blah2RadarData blah2FixedExecutor blah2RadarDsp)
  target_link_libraries(blah2DirectPathCoherentReplay PRIVATE blah2RapidJson)
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
  target_link_libraries(wienerHopfReplayInspect PRIVATE blah2DirectPathCoherentReplay)
  if(BUILD_TESTING)
    foreach(test IN ITEMS CoherentFamilyAcceptance PassiveIsarAperture ResidentWeakEvidenceScore DirectPathCoherentReplay)
      add_executable(test${test} ${PROJECT_ROOT}/test/unit/process/analysis/Test${test}.cpp)
      target_include_directories(test${test} PRIVATE ${PROJECT_ROOT}/src/process/ambiguity ${PROJECT_ROOT}/test/gpu)
      target_link_libraries(test${test} PRIVATE blah2DirectPathCoherentReplay)
    endforeach()
    target_link_libraries(testCoherentFamilyAcceptance PRIVATE blah2RapidJson)
    target_include_directories(testResidentWeakEvidenceScore PRIVATE ${PROJECT_ROOT}/tools)
    add_test(NAME coherentFamilyCanonicalFixture COMMAND testCoherentFamilyAcceptance --fixture)
    add_test(NAME coherentFamilySyntheticAcceptance COMMAND testCoherentFamilyAcceptance --run)
    set_tests_properties(coherentFamilyCanonicalFixture coherentFamilySyntheticAcceptance PROPERTIES
      ENVIRONMENT "VECTORWARP_BENCH_RANGE_WORKERS=0;VECTORWARP_FFTW_PLAN=estimate;OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1")
    set_tests_properties(coherentFamilyCanonicalFixture PROPERTIES TIMEOUT 20)
    set_tests_properties(coherentFamilySyntheticAcceptance PROPERTIES TIMEOUT 900 LABELS "offline;extended")
    add_test(NAME passiveIsarApertureCpuContract COMMAND testPassiveIsarAperture)
    set_tests_properties(passiveIsarApertureCpuContract PROPERTIES TIMEOUT 20
      ENVIRONMENT "VECTORWARP_BENCH_RANGE_WORKERS=0;OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1")
    add_executable(testRangeMigrationReplay
      ${PROJECT_ROOT}/test/unit/process/analysis/TestRangeMigrationReplay.cpp)
    target_link_libraries(testRangeMigrationReplay PRIVATE blah2RadarDsp)
    add_test(NAME rangeMigrationReplayCpuContract COMMAND testRangeMigrationReplay)
    set_tests_properties(rangeMigrationReplayCpuContract PROPERTIES TIMEOUT 20)
    add_test(NAME residentWeakEvidenceScoreCpuContract COMMAND testResidentWeakEvidenceScore)
    set_tests_properties(residentWeakEvidenceScoreCpuContract PROPERTIES TIMEOUT 10)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME coherentPassiveIsarRenderContract COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_ROOT}/test/unit/process/analysis/TestCoherentPassiveIsarRender.py
        $<TARGET_FILE:testCoherentFamilyAcceptance>)
      set_tests_properties(coherentPassiveIsarRenderContract PROPERTIES TIMEOUT 60)
      add_test(NAME passiveIsarApertureCliContract COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_ROOT}/test/unit/process/analysis/TestPassiveIsarApertureCli.py
        $<TARGET_FILE:passiveIsarApertureInspect> $<TARGET_FILE:testPassiveIsarAperture>)
      set_tests_properties(passiveIsarApertureCliContract PROPERTIES TIMEOUT 60)
      add_test(NAME residentWeakEvidenceScoreCliContract COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_ROOT}/test/unit/process/analysis/TestResidentWeakEvidenceScoreCli.py
        $<TARGET_FILE:residentWeakEvidenceScore> $<TARGET_FILE:testResidentWeakEvidenceScore>)
      set_tests_properties(residentWeakEvidenceScoreCliContract PROPERTIES TIMEOUT 30)
      add_test(NAME residentTruthLabelsContract COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_ROOT}/test/script/test_map_resident_truth_labels.py
        $<TARGET_FILE:residentSnapshotGeometry> $<TARGET_FILE:testResidentWeakEvidenceScore>)
      set_tests_properties(residentTruthLabelsContract PROPERTIES TIMEOUT 30)
    endif()
    add_test(NAME directPathCoherentReplayCpuContract COMMAND testDirectPathCoherentReplay)
    set_tests_properties(directPathCoherentReplayCpuContract PROPERTIES TIMEOUT 10)
  endif()
endif()
