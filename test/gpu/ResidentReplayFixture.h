#pragma once
// Synthetic contract fixture, never a CPU substitute for native GPU parity.
#include "GpuResidentWire.h"
#include "GpuResidentHash.h"
#include "../../core/TrackBeforeDetect.private.h"
#include <cmath>
#include <limits>
namespace blah2::resident_replay::test {
inline void enable(resident::Config& c){c.replay.maxBytes=byteLimit;c.replay.maxFrames=3;
 c.replay.frequencyHz=539000000;c.replay.configuredPfa=c.detector.pfa*std::max(1u,c.fusion.beams);
 c.replay.sourceHash.fill(0x33);c.replay.rfHash.fill(0x44);c.replay.actualGainDb.assign(c.geometry.channels,29.7);
 c.presence=resident::wire::mandatoryPresence(c);}
inline ResidentReplaySnapshot fixture(const resident::Config& c,uint32_t available=1,
 resident::Frame frame={1,101,1001},uint64_t transaction=0){
 using namespace resident;const uint32_t rows=c.geometry.doppler,cols=c.geometry.delays,cells=rows*cols,ch=c.geometry.channels;
 const bool tbd=c.detector.integrationCpis>2;const uint32_t depth=tbd?c.detector.integrationCpis:1;
 const uint32_t passes=tbd?track_before_detect::detectorPassCount(depth):1;
 const uint32_t refinement=tbd?(c.bearing.scanBeams?c.bearing.scanBeams:c.fusion.beams*4):0;
 const uint32_t windows=(96+2*rows+cols+1)&~1u,windowWords=tbd?14:8;
 const uint64_t lengths[]={256,1024,uint64_t(c.fusion.beams)*ch*8,uint64_t(refinement)*ch*8,
  uint64_t(windows+windowWords*cols+2*rows+2*cols)*4,uint64_t(cells)*depth*8,
  tbd&&c.detector.integrationBeamTolerance!=UINT32_MAX?uint64_t(cells)*depth*4:0,
  uint64_t(cells)*8,tbd?uint64_t(cells)*passes*8:0,uint64_t(cells)*std::max(1u,c.fusion.beams)*8,
  uint64_t(cells)*passes*40,uint64_t(cells)*16,uint64_t(cells)*ch*8,
  uint64_t(c.geometry.clutterSamples)*ch*16,uint64_t(c.geometry.clutterSamples)*16,
  uint64_t(c.geometry.clutterSamples)*ch*16,uint64_t(cells)*4,uint64_t(cells)*32,
  c.tracker.enabled?uint64_t(trackerHeaderWords+c.tracker.capacity*trackerRecordWords(c.tracker))*4:0,configBytes};
 ResidentReplaySnapshot s;for(uint32_t i=0;i<20;++i)s.blocks.push_back({BlockKind(i+1),std::vector<uint8_t>(size_t(lengths[i]),0)});
 const auto writer=[&](BlockKind kind){auto& b=s.blocks.at(uint32_t(kind)-1).bytes;return wire::Writer{{b.data(),b.size()}};};
 auto cp=writer(BlockKind::referenceCheckpoint);cp.u32(0,1);cp.u32(4,256);cp.u64(8,c.epoch);cp.u64(16,frame.logicalId);
 cp.u64(24,c.geometry.clutterSamples);cp.u64(32,ch);cp.u64(40,available);cp.u64(48,1);cp.u64(56,c.reference.channels.size());cp.f64(208,1);
 for(size_t i=0;i<c.reference.channels.size();++i)cp.f64(64+i*16,1/std::sqrt(double(c.reference.channels.size())));
 auto metadata=writer(BlockKind::postprocessMetadata);
 metadata.u32(32*4,rows);metadata.u32(33*4,cols);metadata.u32(34*4,cells);metadata.u32(46*4,windows);
 metadata.u32(72*4,uint32_t(c.epoch));metadata.u32(73*4,uint32_t(c.epoch>>32));metadata.u32(74*4,c.detector.integrationCpis);
 metadata.u32(75*4,c.fusion.beams);const auto motion=track_before_detect::decodeMotionBins(c.detector.integrationMotionBins);
 metadata.u32(76*4,motion.delay);metadata.u32(77*4,c.detector.integrationBeamTolerance);
 metadata.u32(79*4,tbd?available:0);metadata.u32(84*4,windowWords);metadata.u32(93*4,motion.doppler);
 for(uint32_t row=0;row<rows;++row)metadata.f64((96+2*row)*4,c.dopplerAxis[row]);
 for(uint32_t j=0;j<cols;++j){metadata.u32((96+2*rows+j)*4,uint32_t(c.delayAxis[j]));
  const int n=cols,g=c.detector.guard,t=c.detector.train,k=j;const uint32_t bounds[]={uint32_t(std::max(0,k-g-t)),uint32_t(std::max(0,k-g)),uint32_t(std::min(n,k+g+1)),uint32_t(std::min(n,k+g+t+1))};
  const auto at=windows+windowWords*j,count=bounds[1]-bounds[0]+bounds[3]-bounds[2];
  for(size_t b=0;b<4;++b)metadata.u32((at+b)*4,bounds[b]);
  metadata.u32((at+4)*4,count);
  for(uint32_t pass=0;pass<passes;++pass){const uint32_t cpis=tbd?(pass?track_before_detect::bankLength(pass-1):2):c.detector.integrationCpis;
   const double pfa=tbd?track_before_detect::passPfa(c.detector.pfa,depth,motion.delay,motion.doppler,pass):c.detector.pfa;
   metadata.f64((at+6+2*pass)*4,count?(cpis==1?count*(std::pow(pfa,-1.0/count)-1):track_before_detect::cfarAlpha(pfa,count,cpis)):0);}}
 auto history=writer(BlockKind::powerHistory);const double nan=std::numeric_limits<double>::quiet_NaN();
 for(uint32_t slot=0;slot<depth;++slot)for(uint32_t i=0;i<cells;++i)history.f64((slot*cells+i)*8,tbd&&slot>=available?nan:double(i+1));
 if(tbd){auto banks=writer(BlockKind::temporalBanks);for(uint32_t pass=0;pass<passes;++pass){const auto length=pass?track_before_detect::bankLength(pass-1):2;
  for(uint32_t i=0;i<cells;++i)banks.f64((pass*cells+i)*8,available<length?nan:1);}}
 if(c.tracker.enabled){auto tracker=writer(BlockKind::trackerCheckpoint);tracker.u32(0,1);tracker.u32(4,trackerRecordWords(c.tracker)*4);
  tracker.u32(8,c.tracker.capacity);tracker.u32(12,trackerHistoryCapacity(c.tracker));tracker.u64(40,c.epoch);tracker.u64(48,frame.logicalId);tracker.u64(56,frame.consumedSequence);}
 const auto config=encodeConfig(c);std::copy(config.begin(),config.end(),s.blocks[19].bytes.begin());
 s.identity={c.epoch,frame.logicalId,frame.logicalId,frame.consumedSequence,frame.signalTimestampMs,transaction,wire::configHash(c),{}, {}};
 s.identity.moduleHash.fill(0x11);s.identity.deviceHash.fill(0x55);const auto& raw=s.blocks[13].bytes;Sha256 h;h.add(raw.data(),raw.size());s.identity.rawIqHash=h.finish();return s;
}
}
