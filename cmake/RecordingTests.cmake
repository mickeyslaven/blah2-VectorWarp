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

# Getter coercion checks use a fake device and require no UHD SDK or radio.
add_executable(testUsrpReadback ${PROJECT_ROOT}/test/capture/UsrpReadbackFixture.cpp)
target_compile_features(testUsrpReadback PRIVATE cxx_std_17)
target_include_directories(testUsrpReadback PRIVATE ${PROJECT_ROOT}/src)
target_compile_options(testUsrpReadback PRIVATE -UNDEBUG)
add_test(NAME usrpAppliedReadback COMMAND testUsrpReadback)

# Licensed headers are supplied only by an opted-in RSPduo source build. The
# fixture provides every SDK function itself; never link the vendor runtime.
if(BLAH2_ENABLE_RSPDUO)
  add_executable(testRspDuoFailures ${PROJECT_ROOT}/test/capture/RspDuoFailureFixture.cpp
    ${PROJECT_ROOT}/src/capture/rspduo/RspDuo.cpp
    ${PROJECT_ROOT}/src/capture/Source.cpp
    ${PROJECT_ROOT}/src/capture/Recording.cpp
    ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp)
  target_compile_features(testRspDuoFailures PRIVATE cxx_std_17)
  target_include_directories(testRspDuoFailures PRIVATE ${PROJECT_ROOT}/src
    ${PROJECT_ROOT}/src/capture ${BLAH2_SDRPLAY_INCLUDE_DIR})
  target_compile_options(testRspDuoFailures PRIVATE -UNDEBUG)
  target_link_libraries(testRspDuoFailures PRIVATE Threads::Threads)
  add_test(NAME rspduoStructuredFailures COMMAND testRspDuoFailures)
endif()
