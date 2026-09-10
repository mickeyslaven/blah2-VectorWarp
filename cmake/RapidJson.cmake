if(NOT TARGET blah2RapidJson)
  find_path(RAPIDJSON_INCLUDE_DIRS "rapidjson/allocators.h")
  if(NOT RAPIDJSON_INCLUDE_DIRS)
    message(FATAL_ERROR "RapidJSON headers were not found")
  endif()

  add_library(blah2RapidJson INTERFACE)
  target_include_directories(blah2RapidJson SYSTEM INTERFACE
    "${RAPIDJSON_INCLUDE_DIRS}")
endif()
