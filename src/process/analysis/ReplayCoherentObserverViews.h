#pragma once
#include "ReplayCoherentObserver.h"
#include "data/Map.h"
#include "process/ambiguity/GpuResidentBackend.h"
#include "process/ambiguity/GpuResidentProof.h"
#include "process/ambiguity/ResidentLaneBudget.h"
namespace blah2::qualification_observer {
inline Clutter residentClutter(const resident::Config& config,const resident::OutputViews& out,
    resident::Span<const std::complex<double>> reference) {
  const auto samples=config.geometry.clutterSamples,channels=config.geometry.channels;
  const int64_t end=int64_t(config.geometry.clutterDelayMin)+config.geometry.clutterBins;
  if(reference.size!=samples||out.filtered.size!=uint64_t(samples)*channels||
      !reference.data||!out.filtered.data||end>INT32_MAX)
    throw std::runtime_error("Observer resident clutter samples missing or truncated");
  return snapshotClutter(samples,channels,[&](size_t i){return reference.data[i];},
    [&](size_t c,size_t i){return out.filtered.data[c*samples+i];},true,
    config.geometry.clutterDelayMin,int32_t(end),
    "vulkan-resident",true,false);
}
inline Products hostProducts(const std::vector<Map<std::complex<double>>*>& maps,
 const Map<std::complex<double>>& fused) {
  if(maps.empty()||!maps.front())throw std::runtime_error("Observer has no host CAFs");
  Products p;p.rows=maps.front()->get_nRows();p.columns=maps.front()->get_nCols();p.channels=maps.size();
  p.delayBins.assign(maps.front()->delay.begin(),maps.front()->delay.end());
  p.dopplerHz.assign(maps.front()->doppler.begin(),maps.front()->doppler.end());
  p.caf=[maps,columns=p.columns](size_t c,size_t i){return maps.at(c)->data.at(i/columns).at(i%columns);};
  p.fused=[&fused,columns=p.columns](size_t i){return fused.data.at(i/columns).at(i%columns);};return p;
}
inline Products residentProducts(const resident::Config& config,const resident::OutputViews& out,
 uint64_t sharedReservationBytes,const resident::LaneBudget* laneBudget=nullptr) {
  Products p;p.rows=config.geometry.doppler;p.columns=config.geometry.delays;p.channels=config.geometry.channels;
  const uint64_t cells=uint64_t(p.rows)*p.columns;
  if(out.maps.size!=cells*p.channels||out.fused.size!=cells||out.doppler.size!=p.rows||out.delayKm.size!=p.columns)
    throw std::runtime_error("Observer resident map geometry differs");
  for(size_t i=0;i<out.delayKm.size;++i)p.delayBins.push_back(out.delayKm.data[i]*1000*config.sampleRate/299792458.0);
  p.dopplerHz.assign(out.doppler.data,out.doppler.data+out.doppler.size);
  p.caf=[&out,cells,rows=p.rows,columns=p.columns](size_t c,size_t i){
    return std::complex<double>(out.maps.data[c*cells+(i%columns)*rows+(i/columns+rows/2+1)%rows]);};
  p.fused=[&out](size_t i){return out.fused.data[i];};
  p.fusedBeamIdsAvailable=config.fusion.mode!=resident::FusionMode::noncoherent;
  p.flags={out.flags.data,out.flags.size};p.detectionCells={out.detectionCells.data,out.detectionCells.size};
  p.unflooredSnr={out.unflooredSnr.data,out.unflooredSnr.size};
  const auto proof=resident::preparedProof(out.preparedEvidence);p.residentProofAvailable=true;
  p.logicalFrame=proof.frame;p.consumedSequence=proof.consumed;p.nativeGeneration=proof.nativeGeneration;
  p.residentGeneration=proof.residentGeneration;p.postprocessStatus=proof.postprocessStatus;p.trackerStatus=proof.trackerStatus;
  // Process::outputs has already authenticated this exact prepared envelope
  // against its session/frame. Decode allocator bytes, never estimate from IQ
  // shape or plans. The mapping reservation comes from that Session's layout.
  const resident::wire::Reader identity{out.preparedEvidence};
  const auto allocation=resident::allocationProof(resident::proof_detail::sub(
    out.preparedEvidence,resident::evidence::prepared::allocation,832));
  auto& a=p.residentAllocation;a.available=true;
  a.session={identity.u64(16),identity.u64(24)};
  a.limitBytes=allocation.limit;a.usedBytes=allocation.used;a.peakBytes=allocation.peak;
  a.liveAllocations=allocation.live;a.sharedReservationBytes=sharedReservationBytes;
  if(laneBudget){a.laneBudgetAvailable=true;a.aggregateCapBytes=laneBudget->totalBytes;
    a.sessionReservationBytes=laneBudget->bytesPerSession;a.aggregateSessionCap=laneBudget->sessions;}
  a.validate();
  return p;
}
}
