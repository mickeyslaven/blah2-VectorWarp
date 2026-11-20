#pragma once
#include "GpuResidentBackend.h"
#include "process/detection/DetectorIntegration.h"

namespace blah2::resident {
// Formatting/control metadata only. Numeric history and readiness are supplied
// by the acknowledged resident frame, never inferred from the configured depth.
template<class Publication>
std::optional<detection::IntegrationMetrics> temporalPublication(
 const Config& config,const Publication& publication,uint64_t captureEpoch,
 uint64_t first,uint64_t end){
 if(config.detector.integrationCpis==1)return std::nullopt;
 if(!publication.availableCpis||publication.availableCpis>config.detector.integrationCpis||
    publication.detectorReady!=(publication.availableCpis>=2)||end<=first)
  throw std::invalid_argument("Resident temporal publication acknowledgement invalid");
 detection::IntegrationMetrics result;
 result.backend="vulkan-resident";
 result.cpis=config.detector.integrationCpis;result.motionBins=config.detector.integrationMotionBins;
 const auto motion=track_before_detect::decodeMotionBins(result.motionBins);
 result.delayMotionBins=motion.delay;result.dopplerMotionBins=motion.doppler;
 result.delayDriftPerHz=motion.fractionalDelay?track_before_detect::delayDriftPerHz(
  config.geometry.clutterSamples,config.fusion.frequencyHz):0;
 result.availableCpis=publication.availableCpis;result.ready=publication.detectorReady;
 result.historyReset=publication.availableCpis==1;result.captureEpoch=captureEpoch;
 const uint64_t span=end-first,older=publication.availableCpis-1;
 if(older&&span>first/older)throw std::invalid_argument("Resident temporal sample history precedes capture origin");
 result.firstInputSample=first-older*span;result.endInputSample=end;
 result.window.epoch=publication.temporalEpoch;result.window.newestSequence=publication.newestSequence;
 // Canonical publication describes the ready baseline's support; the full
 // acknowledged ring separately supplies each candidate's originating bank.
 result.window.depth=publication.detectorReady?2:1;
 for(uint32_t age=0;age<result.window.depth;++age)
  result.window.sequences[age]=publication.supportSequences[age];
 return result;
}
}
