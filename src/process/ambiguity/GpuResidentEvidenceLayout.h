#pragma once
#include <cstddef>
#include <cstdint>
namespace blah2::resident::evidence {
namespace ready {
constexpr size_t bytes=4096;
constexpr size_t header=0;
constexpr size_t allocation=256;
constexpr size_t cpuProof=1088;
constexpr size_t referenceStatus=1408;
constexpr size_t residentStatus=1472;
constexpr size_t times=1536;
constexpr size_t shape=1600;
}
namespace prepared {
constexpr size_t bytes=61888;
constexpr size_t header=0;
constexpr size_t allocation=256;
constexpr size_t cpuProof=1088;
constexpr size_t nativeReference=1408;
constexpr size_t residentFinalStatus=1536;
constexpr size_t postprocessStatus=1600;
constexpr size_t trackerPreparedStatus=1728;
constexpr size_t times=1984;
constexpr size_t nativeBatches=2112;
constexpr size_t hostSummary=4160;
constexpr size_t hostIntervals=4288;
constexpr size_t submitThreads=53440;
constexpr size_t submitContext=61632;
}
namespace control {
constexpr size_t bytes=512;
}
namespace device {
constexpr size_t bytes=512;
}
inline constexpr const char* labels[]={nullptr,"captured.upload","captured.submit","captured.status_outputs","captured.commit","captured.query_log","supplied.prepare","radar.upload","radar.native_reference","radar.pack","radar.native_batch","radar.subtract_pack","radar.filtered_tail_copy","radar.batch_query_log","radar.ambiguity_postprocess_submit","radar.map_status_copy","radar.tracker_prepare","radar.tracker_copy","radar.postprocess_copy","radar.postprocess_query_log","radar.end_frame","radar.result_query_log","vkQueueSubmit","vkWaitForFences","vkResetFences","query.read"};
inline constexpr uint8_t schemaHash[]={0x6e,0x05,0x76,0x15,0x87,0x4c,0x66,0x78,0x4c,0xc8,0xdc,0x24,0xcb,0x37,0x43,0xc1,0x24,0xeb,0x45,0x6a,0x17,0x0e,0x87,0xd3,0xf7,0x30,0x09,0xfa,0xbe,0x3c,0x07,0xd6};
}
