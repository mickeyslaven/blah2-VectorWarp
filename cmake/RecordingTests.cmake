add_executable(testRecording ${PROJECT_ROOT}/test/recording/TestRecording.cpp
  ${PROJECT_ROOT}/src/capture/Recording.cpp
  ${PROJECT_ROOT}/src/capture/Replay.cpp
  ${PROJECT_ROOT}/src/capture/Source.cpp
  ${PROJECT_ROOT}/src/data/IqData.cpp
  ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
target_compile_features(testRecording PRIVATE cxx_std_17)
target_include_directories(testRecording PRIVATE ${PROJECT_ROOT}/src)
target_compile_options(testRecording PRIVATE -Wall -Wextra -Werror)
find_package(Threads REQUIRED)
include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
target_link_libraries(testRecording PRIVATE Threads::Threads blah2RapidJson)
add_test(NAME recordingFormats COMMAND testRecording)

add_executable(testKrakenStream ${PROJECT_ROOT}/test/capture/KrakenStreamFixture.cpp
  ${PROJECT_ROOT}/src/capture/kraken/Kraken.cpp)
target_compile_features(testKrakenStream PRIVATE cxx_std_17)
target_compile_options(testKrakenStream PRIVATE -UNDEBUG)
target_include_directories(testKrakenStream PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(testKrakenStream PRIVATE Threads::Threads blah2RapidJson)
if(TARGET blah2CaptureCore)
  target_link_libraries(testKrakenStream PRIVATE blah2CaptureCore)
else()
  # The standalone recording checks intentionally have no receiver modules.
  target_sources(testKrakenStream PRIVATE ${PROJECT_ROOT}/src/capture/Source.cpp
    ${PROJECT_ROOT}/src/capture/Recording.cpp ${PROJECT_ROOT}/src/data/IqData.cpp
    ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
endif()
add_test(NAME krakenSocketCapture COMMAND testKrakenStream)
set_tests_properties(krakenSocketCapture PROPERTIES TIMEOUT 30)

# Getter coercion checks use a fake device and require no UHD SDK or radio.
add_executable(testUsrpReadback ${PROJECT_ROOT}/test/capture/UsrpReadbackFixture.cpp)
target_compile_features(testUsrpReadback PRIVATE cxx_std_17)
target_include_directories(testUsrpReadback PRIVATE ${PROJECT_ROOT}/src)
target_compile_options(testUsrpReadback PRIVATE -UNDEBUG)
add_test(NAME usrpAppliedReadback COMMAND testUsrpReadback)

# Keep transport parsing and scaled-counter rollover coverage SDK-free.
add_executable(testRspDuoHelpers ${PROJECT_ROOT}/test/capture/RspDuoHelpersFixture.cpp)
target_compile_features(testRspDuoHelpers PRIVATE cxx_std_17)
target_include_directories(testRspDuoHelpers PRIVATE ${PROJECT_ROOT}/src)
target_compile_options(testRspDuoHelpers PRIVATE -UNDEBUG -Wall -Wextra -Werror)
add_test(NAME rspduoCallbackHelpers COMMAND testRspDuoHelpers)

# Compile the actual receive loop against a local SDK double, never a radio.
add_executable(testUsrpIngress ${PROJECT_ROOT}/test/capture/UsrpIngressFixture.cpp
  ${PROJECT_ROOT}/src/capture/usrp/Usrp.cpp
  ${PROJECT_ROOT}/src/capture/Source.cpp
  ${PROJECT_ROOT}/src/capture/Recording.cpp
  ${PROJECT_ROOT}/src/data/IqData.cpp
  ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
target_compile_features(testUsrpIngress PRIVATE cxx_std_17)
target_compile_options(testUsrpIngress PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(testUsrpIngress BEFORE PRIVATE
  ${PROJECT_ROOT}/test/capture/fake-uhd ${PROJECT_ROOT}/src)
target_link_libraries(testUsrpIngress PRIVATE Threads::Threads blah2RapidJson)
add_test(NAME usrpReceiveIngress COMMAND testUsrpIngress)
set_tests_properties(usrpReceiveIngress PROPERTIES TIMEOUT 20)

if(BLAH2_ENABLE_HACKRF)
  add_executable(testHackRfSettings ${PROJECT_ROOT}/test/capture/HackRfSettingsFixture.cpp
    ${PROJECT_ROOT}/src/capture/hackrf/HackRf.cpp)
  target_compile_features(testHackRfSettings PRIVATE cxx_std_17)
  target_include_directories(testHackRfSettings PRIVATE ${HACKRF_INCLUDE_DIRS})
  target_compile_options(testHackRfSettings PRIVATE -UNDEBUG)
  # The fixture supplies SDK functions. Never link/open the actual USB runtime.
  target_link_libraries(testHackRfSettings PRIVATE blah2CaptureCore Threads::Threads)
  add_test(NAME hackrfAppliedSettings COMMAND testHackRfSettings)
endif()

# Licensed headers are supplied only by an opted-in RSPduo source build. The
# fixture provides every SDK function itself; never link the vendor runtime.
if(BLAH2_ENABLE_RSPDUO)
  add_executable(testRspDuoFailures ${PROJECT_ROOT}/test/capture/RspDuoFailureFixture.cpp
    ${PROJECT_ROOT}/src/capture/ReceiverFactory.cpp
    ${PROJECT_ROOT}/src/capture/rspduo/RspDuo.cpp
    ${PROJECT_ROOT}/src/capture/Source.cpp
    ${PROJECT_ROOT}/src/capture/Recording.cpp
    ${PROJECT_ROOT}/src/data/IqData.cpp
    ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
  target_compile_features(testRspDuoFailures PRIVATE cxx_std_17)
  target_compile_definitions(testRspDuoFailures PRIVATE BLAH2_MODULE_RSPDUO=1)
  target_include_directories(testRspDuoFailures PRIVATE ${PROJECT_ROOT}/src
    ${PROJECT_ROOT}/src/capture ${BLAH2_SDRPLAY_INCLUDE_DIR}
    "${PROJECT_BINARY_DIR}/receiver-generated")
  target_compile_options(testRspDuoFailures PRIVATE -UNDEBUG)
  target_link_libraries(testRspDuoFailures PRIVATE Threads::Threads blah2RapidJson)
  add_test(NAME rspduoStructuredFailures COMMAND testRspDuoFailures)
  set_tests_properties(rspduoStructuredFailures PROPERTIES TIMEOUT 30)
endif()
