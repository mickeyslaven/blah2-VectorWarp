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
inline constexpr uint8_t schemaHash[]={0x86,0xbe,0x3f,0x7d,0x5e,0xdf,0xac,0x1a,0x94,0x54,0x2e,0xf3,0x5a,0xd0,0x24,0x41,0xaf,0xb1,0x5d,0xfe,0x1d,0x06,0xcf,0xbc,0x8c,0x2b,0xbf,0x77,0xa9,0xc3,0xdb,0x25};
}
