find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(receiver_fixture_dir "${PROJECT_BINARY_DIR}/receiver-fixtures")
# Exercise all loader branches even in an enclosing local-kit build, without
# allowing that build's RSPduo source-kit flag into this non-local fixture ABI.
function(blah2_configure_receiver_module_fixture_cohort)
  foreach(receiver USRP HACKRF RSPDUO)
    set(BLAH2_BUILT_${receiver} 1)
  endforeach()
  set(BLAH2_LOCAL_BUILD_RSPDUO OFF)
  configure_file("${PROJECT_ROOT}/cmake/ReceiverCohort.h.in"
    "${receiver_fixture_dir}/include/ReceiverCohort.h" @ONLY)
endfunction()
blah2_configure_receiver_module_fixture_cohort()
add_executable(testReceiverModule ${PROJECT_ROOT}/test/capture/ReceiverModuleFixture.cpp
  ${PROJECT_ROOT}/src/capture/ReceiverLoader.cpp)
target_compile_features(testReceiverModule PRIVATE cxx_std_17)
target_include_directories(testReceiverModule PRIVATE "${receiver_fixture_dir}/include")
target_compile_options(testReceiverModule PRIVATE -UNDEBUG)
target_link_libraries(testReceiverModule PRIVATE blah2CaptureCore blah2RapidJson ${CMAKE_DL_LIBS})
set_target_properties(testReceiverModule PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
  INSTALL_RPATH "$ORIGIN" RUNTIME_OUTPUT_DIRECTORY "${receiver_fixture_dir}")
add_library(receiverFixtureRuntime SHARED ${PROJECT_ROOT}/test/capture/ReceiverModuleRuntimeFake.cpp)
set_target_properties(receiverFixtureRuntime PROPERTIES OUTPUT_NAME receiver-fixture-runtime
  VERSION 3.15 SOVERSION 3
  LIBRARY_OUTPUT_DIRECTORY "${receiver_fixture_dir}")
foreach(fixture valid badAbi badCohort hackrf createError missingRuntime)
  add_library(receiverFixture_${fixture} MODULE ${PROJECT_ROOT}/test/capture/ReceiverModuleFake.cpp)
  target_compile_features(receiverFixture_${fixture} PRIVATE cxx_std_17)
  target_include_directories(receiverFixture_${fixture} PRIVATE "${receiver_fixture_dir}/include")
  target_link_libraries(receiverFixture_${fixture} PRIVATE blah2CaptureCore)
  set_target_properties(receiverFixture_${fixture} PROPERTIES PREFIX "" OUTPUT_NAME "${fixture}"
    LIBRARY_OUTPUT_DIRECTORY "${receiver_fixture_dir}" BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN")
  add_dependencies(testReceiverModule receiverFixture_${fixture})
endforeach()
target_compile_definitions(receiverFixture_badAbi PRIVATE FIXTURE_BAD_ABI=1)
target_compile_definitions(receiverFixture_badCohort PRIVATE FIXTURE_BAD_COHORT=1)
target_compile_definitions(receiverFixture_hackrf PRIVATE FIXTURE_RECEIVER="HackRF")
target_compile_definitions(receiverFixture_createError PRIVATE FIXTURE_CREATE_ERROR=1)
target_compile_definitions(receiverFixture_missingRuntime PRIVATE FIXTURE_MISSING_RUNTIME=1)
target_link_libraries(receiverFixture_missingRuntime PRIVATE receiverFixtureRuntime)
add_test(NAME receiverModuleIsolation COMMAND ${Python3_EXECUTABLE}
  "${PROJECT_ROOT}/test/capture/test_receiver_modules.py" "$<TARGET_FILE:testReceiverModule>"
  "$<TARGET_FILE:blah2CaptureCore>" "${receiver_fixture_dir}")
set_tests_properties(receiverModuleIsolation PROPERTIES TIMEOUT 30)
