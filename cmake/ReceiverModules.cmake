# Receiver SDKs never become dependencies of the processor executable. These
# private modules ship together, and are loaded only for the selected receiver.
set(BLAH2_RECEIVER_ABI_INPUT "${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_SIZEOF_VOID_P}|${CMAKE_CXX_FLAGS}")
foreach(interface src/capture/ReceiverModule.h src/capture/Source.h
    src/capture/Recording.h src/capture/kraken/HeimdallFrame.h src/data/IqData.h)
  file(SHA256 "${PROJECT_ROOT}/${interface}" interface_hash)
  string(APPEND BLAH2_RECEIVER_ABI_INPUT "|${interface_hash}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_ROOT}/${interface}")
endforeach()
string(SHA256 BLAH2_RECEIVER_COHORT "${BLAH2_RECEIVER_ABI_INPUT}")
set(BLAH2_BUILT_USRP ${BLAH2_ENABLE_USRP})
set(BLAH2_BUILT_HACKRF ${BLAH2_ENABLE_HACKRF})
set(BLAH2_BUILT_RSPDUO ${BLAH2_ENABLE_RSPDUO})
configure_file("${PROJECT_ROOT}/cmake/ReceiverCohort.h.in"
  "${PROJECT_BINARY_DIR}/receiver-generated/ReceiverCohort.h" @ONLY)

add_library(blah2CaptureCore SHARED
  ${PROJECT_ROOT}/src/capture/Source.cpp
  ${PROJECT_ROOT}/src/capture/Recording.cpp
  ${PROJECT_ROOT}/src/capture/kraken/HeimdallFrame.cpp
  ${PROJECT_ROOT}/src/data/IqData.cpp)
target_compile_features(blah2CaptureCore PUBLIC cxx_std_17)
target_link_libraries(blah2CaptureCore PRIVATE blah2RapidJson Threads::Threads)
set_target_properties(blah2CaptureCore PROPERTIES OUTPUT_NAME blah2-capture-core
  VERSION 1.0.0 SOVERSION 1 LIBRARY_OUTPUT_DIRECTORY "${BLAH2_OUTPUT_DIR}"
  BUILD_WITH_INSTALL_RPATH TRUE
  BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")

add_library(blah2ReceiverLoader STATIC ${PROJECT_ROOT}/src/capture/ReceiverLoader.cpp)
target_compile_features(blah2ReceiverLoader PUBLIC cxx_std_17)
target_include_directories(blah2ReceiverLoader PRIVATE "${PROJECT_BINARY_DIR}/receiver-generated")
target_link_libraries(blah2ReceiverLoader PUBLIC blah2CaptureCore PRIVATE blah2RapidJson ${CMAKE_DL_LIBS})

function(blah2_add_receiver target name definition source)
  add_library(${target} MODULE ${PROJECT_ROOT}/src/capture/ReceiverFactory.cpp ${source})
  target_compile_features(${target} PRIVATE cxx_std_17)
  target_compile_definitions(${target} PRIVATE ${definition}=1)
  target_include_directories(${target} PRIVATE "${PROJECT_BINARY_DIR}/receiver-generated")
  target_link_libraries(${target} PRIVATE blah2CaptureCore Threads::Threads ${ARGN})
  target_link_options(${target} PRIVATE "-Wl,-z,defs")
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "blah2-receiver-${name}"
    LIBRARY_OUTPUT_DIRECTORY "${BLAH2_OUTPUT_DIR}"
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    BUILD_WITH_INSTALL_RPATH TRUE
    BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endfunction()
if(BLAH2_ENABLE_USRP)
  blah2_add_receiver(blah2ReceiverUsrp usrp BLAH2_MODULE_USRP
    ${PROJECT_ROOT}/src/capture/usrp/Usrp.cpp ${UHD_LIBRARIES})
  target_include_directories(blah2ReceiverUsrp PRIVATE ${UHD_INCLUDE_DIRS})
endif()
if(BLAH2_ENABLE_HACKRF)
  blah2_add_receiver(blah2ReceiverHackrf hackrf BLAH2_MODULE_HACKRF
    ${PROJECT_ROOT}/src/capture/hackrf/HackRf.cpp PkgConfig::HACKRF)
endif()
if(BLAH2_ENABLE_RSPDUO)
  blah2_add_receiver(blah2ReceiverRspduo rspduo BLAH2_MODULE_RSPDUO
    ${PROJECT_ROOT}/src/capture/rspduo/RspDuo.cpp blah2Sdrplay)
  # Vendor-local lookup is scoped to the RSPduo loader, not an absolute RUNPATH.
  # Never retain a temporary SDK path or redistribute the licensed runtime.
endif()

if(BUILD_TESTING)
  include(${PROJECT_ROOT}/cmake/ReceiverModuleTests.cmake)
endif()
