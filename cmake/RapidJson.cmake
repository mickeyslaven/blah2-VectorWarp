if(NOT TARGET blah2RapidJson)
  find_path(RAPIDJSON_INCLUDE_DIRS "rapidjson/allocators.h")
  if(NOT RAPIDJSON_INCLUDE_DIRS)
    message(FATAL_ERROR "RapidJSON headers were not found")
  endif()

  add_library(blah2RapidJson INTERFACE)
  target_include_directories(blah2RapidJson SYSTEM INTERFACE
    "${RAPIDJSON_INCLUDE_DIRS}")

  # Intel AppleClang searches /usr/local/include implicitly, so CMake can
  # omit its SYSTEM flag even though Homebrew headers there produce warnings.
  # Preserve vendor-header isolation without relaxing project diagnostics.
  if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"
      AND RAPIDJSON_INCLUDE_DIRS IN_LIST CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    target_compile_options(blah2RapidJson INTERFACE
      "-isystem${RAPIDJSON_INCLUDE_DIRS}")
  endif()
endif()
