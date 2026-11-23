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
constexpr size_t bytes=62224;
constexpr size_t header=0;
constexpr size_t allocation=256;
constexpr size_t cpuProof=1088;
constexpr size_t nativeReference=1408;
constexpr size_t residentFinalStatus=1536;
constexpr size_t postprocessStatus=1600;
constexpr size_t trackerPreparedStatus=1728;
constexpr size_t trackerDiagnostics=2000,trackerDiagnosticsCapacity=320;
constexpr size_t times=2320;
constexpr size_t nativeBatches=2448;
constexpr size_t hostSummary=4496;
constexpr size_t hostIntervals=4624;
constexpr size_t submitThreads=53776;
constexpr size_t submitContext=61968;
}
namespace control {
constexpr size_t bytes=512;
}
namespace device {
constexpr size_t bytes=512;
}
inline constexpr const char* labels[]={nullptr,"captured.upload","captured.submit","captured.status_outputs","captured.commit","captured.query_log","supplied.prepare","radar.upload","radar.native_reference","radar.pack","radar.native_batch","radar.subtract_pack","radar.filtered_tail_copy","radar.batch_query_log","radar.ambiguity_postprocess_submit","radar.map_status_copy","radar.tracker_prepare","radar.tracker_copy","radar.postprocess_copy","radar.postprocess_query_log","radar.end_frame","radar.result_query_log","vkQueueSubmit","vkWaitForFences","vkResetFences","query.read"};
// SHA-256 of the ABI18 evidence layout identity (including canonical tracker v2).
inline constexpr uint8_t schemaHash[]={0xf4,0x0f,0x55,0x37,0xc9,0x9d,0x4d,0x78,0x16,0x19,0x5a,0x87,0x84,0x31,0x17,0x2e,0xbc,0x46,0x7f,0x97,0x72,0x54,0xbe,0x9d,0x8e,0xd9,0x90,0x91,0x48,0xa2,0xb6,0xf7};
}
