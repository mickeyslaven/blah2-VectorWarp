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
target_link_libraries(testRecording PRIVATE Threads::Threads)
add_test(NAME recordingFormats COMMAND testRecording)
