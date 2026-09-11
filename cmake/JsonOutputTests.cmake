include(${PROJECT_ROOT}/cmake/RapidJson.cmake)
add_executable(testJsonOutput ${PROJECT_ROOT}/test/json/TestJsonOutput.cpp
  ${PROJECT_ROOT}/src/data/Map.cpp ${PROJECT_ROOT}/src/data/Detection.cpp)
target_compile_features(testJsonOutput PRIVATE cxx_std_17)
target_include_directories(testJsonOutput PRIVATE ${PROJECT_ROOT}/src)
target_link_libraries(testJsonOutput PRIVATE blah2RapidJson)
add_test(NAME jsonOutputCompatibility COMMAND testJsonOutput)
set_tests_properties(jsonOutputCompatibility PROPERTIES TIMEOUT 30)
