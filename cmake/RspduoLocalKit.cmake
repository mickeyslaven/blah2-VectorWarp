# Only our small receiver adapter and interface headers enter this source kit.
# No vendor SDK, build directory, recording, model or general source-tree copy.
set(BLAH2_RSPDUO_KIT_ID "")
if(BLAH2_LOCAL_BUILD_RSPDUO)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "The local RSPduo build contract currently requires GNU C++")
  endif()
  set(BLAH2_RSPDUO_KIT_SOURCES
    src/capture/ReceiverFactory.cpp src/capture/ReceiverModule.h
    src/capture/Source.h src/capture/Recording.h src/capture/PairedCpiQueue.h src/capture/PairedCpiSource.h
    src/capture/kraken/HeimdallFrame.h src/data/IqData.h
    src/capture/rspduo/RspDuo.cpp src/capture/rspduo/RspDuo.h
    src/capture/rspduo/SampleSequence.h src/capture/rspduo/SdkSampleClock.h
    src/capture/rspduo/UsbMode.h LICENSE)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type)
  set(BLAH2_RSPDUO_KIT_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type}}")
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
    OUTPUT_VARIABLE BLAH2_RSPDUO_COMPILER_TARGET OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE target_result)
  if(NOT target_result EQUAL 0 OR NOT BLAH2_RSPDUO_COMPILER_TARGET MATCHES "^[A-Za-z0-9_.+-]+$")
    message(FATAL_ERROR "Cannot identify the local-adapter compiler target")
  endif()
  set(kit_identity "local-rspduo-v1|${BLAH2_RECEIVER_COHORT}|${BLAH2_RSPDUO_KIT_FLAGS}|${BLAH2_RSPDUO_COMPILER_TARGET}")
  # Core implementation changes also invalidate a previously built adapter.
  foreach(source ${BLAH2_RSPDUO_KIT_SOURCES}
      src/capture/Source.cpp src/capture/Recording.cpp
      src/capture/kraken/HeimdallFrame.cpp src/data/IqData.cpp)
    file(SHA256 "${PROJECT_ROOT}/${source}" source_hash)
    string(APPEND kit_identity "|${source}:${source_hash}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_ROOT}/${source}")
  endforeach()
  string(SHA256 BLAH2_RSPDUO_KIT_ID "${kit_identity}")
endif()

function(blah2_json_string input output)
  string(REPLACE "\\" "\\\\" value "${input}")
  string(REPLACE "\"" "\\\"" value "${value}")
  string(REPLACE "\n" "\\n" value "${value}")
  string(REPLACE "\r" "\\r" value "${value}")
  string(REPLACE "\t" "\\t" value "${value}")
  set(${output} "\"${value}\"" PARENT_SCOPE)
endfunction()

function(blah2_write_rspduo_plan)
  set(sources "")
  foreach(source ${BLAH2_RSPDUO_KIT_SOURCES})
    file(SHA256 "${PROJECT_ROOT}/${source}" source_hash)
    string(APPEND sources "\"${source}\":\"${source_hash}\",")
  endforeach()
  file(SHA256 "${PROJECT_BINARY_DIR}/receiver-generated/ReceiverCohort.h" cohort_hash)
  string(APPEND sources "\"generated/ReceiverCohort.h\":\"${cohort_hash}\"")
  separate_arguments(flags UNIX_COMMAND "${BLAH2_RSPDUO_KIT_FLAGS}")
  set(flag_json "")
  set(separator "")
  foreach(flag ${flags})
    blah2_json_string("${flag}" quoted)
    string(APPEND flag_json "${separator}${quoted}")
    set(separator ",")
  endforeach()
  file(WRITE "${PROJECT_BINARY_DIR}/receiver-generated/rspduo-plan.json"
    "{\"schema\":1,\"receiver\":\"RspDuo\",\"kit_id\":\"${BLAH2_RSPDUO_KIT_ID}\","
    "\"cohort\":\"${BLAH2_RECEIVER_COHORT}\",\"compiler\":{\"id\":\"GNU\","
    "\"version\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"target\":\"${BLAH2_RSPDUO_COMPILER_TARGET}\","
    "\"cxx_flags\":[${flag_json}]},\"sources\":{${sources}}}\n")
endfunction()
