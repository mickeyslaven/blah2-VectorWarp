if(NOT TARGET blah2RapidJson)
  find_path(RAPIDJSON_INCLUDE_DIRS "rapidjson/allocators.h")
  if(NOT RAPIDJSON_INCLUDE_DIRS)
    message(FATAL_ERROR "RapidJSON headers were not found")
  endif()

  add_library(blah2RapidJson INTERFACE)
  target_include_directories(blah2RapidJson SYSTEM INTERFACE
    "${RAPIDJSON_INCLUDE_DIRS}")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
      CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    # Pinned RapidJSON 1.1.0 has an unused string-reference assignment
    # template diagnosed by GCC 15+ even in SYSTEM headers. Keep the warning
    # visible while allowing this external-header compatibility case.
    target_compile_options(blah2RapidJson INTERFACE
      -Wno-error=template-body)
  endif()
endif()
