#pragma once
#include "GpuResidentProof.h"
#include "../../core/TrackBeforeDetect.private.h"
// Deliberately synthetic evidence for CPU transport tests only. These fields
// exercise the real schema validator; they are never hardware qualification.
struct CpuEvidenceDouble {
 using Config=blah2::resident::Config;using Packet=blah2::resident::Packet;
 using Mapping=blah2::resident::Mapping;using Section=blah2::resident::Section;
 using Phase=blah2::resident::Phase;using Op=blah2::resident::Op;
 using Writer=blah2::resident::wire::Writer;
 const Config& c;Mapping m;uint64_t event=0,readyCalls=0;
 void device(){auto b=m.bytes(Section::device);Writer w{b};w.zero();
  w.u32(0,1);w.u32(4,512);w.u32(32,64);w.u32(36,0x3f800000);w.u32(40,1);w.u32(44,1);w.u32(48,1);w.u32(52,1);
  w.u64(56,1<<20);w.u64(120,4<<20);std::fill_n(b.data+128,32,0x22);
  std::memcpy(b.data+160,c.device.c_str(),c.device.size()+1);const char name[]="CPU test double (no GPU)";std::memcpy(b.data+256,name,sizeof(name));
 }
 void common(Section section,const Packet&p,Phase phase){using namespace blah2::resident;
  auto b=m.bytes(section);Writer w{b};w.zero();w.u32(0,1);w.u32(4,b.size);w.u32(8,uint32_t(phase));
  w.u64(16,p.session[0]);w.u64(24,p.session[1]);w.u64(32,p.requestId);w.u64(40,p.frame.logicalId);w.u64(48,c.epoch);
  w.u64(56,p.frame.consumedSequence);w.u64(64,p.frame.signalTimestampMs);w.u64(72,p.frame.logicalId);w.u64(80,p.frame.logicalId);
  w.u32(88,c.stages());w.u32(92,phase==Phase::referenceReady?48:c.stages());w.u64(96,phase==Phase::referenceReady?127:4095);w.u64(104,outputPresence(c.presence,p.diagnosticPresence));
  w.u32(112,phase==Phase::prepared?1:0);w.u32(116,c.geometry.channels);const auto hash=wire::configHash(c);std::memcpy(b.data+128,hash.data(),32);
  std::fill_n(b.data+160,32,0x11);std::memcpy(b.data+192,evidence::schemaHash,32);std::fill_n(b.data+224,32,0x22);
  const size_t a=256;w.u32(a,1);w.u32(a+4,832);w.u32(a+8,1);w.u32(a+12,1);w.u32(a+16,1);
  w.u64(a+24,1<<20);w.u64(a+32,4096);w.u64(a+40,4096);w.u64(a+48,1);
  w.u64(a+64,4<<20);w.u64(a+72,1<<20);w.u64(a+80,4096);w.u32(a+88,1);w.u32(a+576,6);
  const uint64_t required=(uint64_t(1)<<31)-1;w.u64(1088,required);w.u64(1104,required);
 }
 void ready(const Packet&p){using namespace blah2::resident;const uint64_t call=readyCalls++;common(Section::readyEvidence,p,Phase::referenceReady);Writer w{m.bytes(Section::readyEvidence)};
  const bool array=c.reference.mode==ReferenceMode::capturedArray;
  const uint32_t refs=uint32_t(c.reference.channels.size());
  const uint32_t status[]={0,array?1u:0u,array?64u:0u,array?refs*refs:0u,array?c.reference.powerIterations*refs:0u,64,array?64u:0u,array?2u:0u,64,64,1,array?refs*refs:0u,array?refs*refs:0u,0,0,0};
  for(size_t i=0;i<16;++i){w.u32(1408+4*i,status[i]);}
  w.u32(1476,64);w.u32(1480,63);w.u64(1536,15);for(size_t i=0;i<4;++i)w.f64(1552+8*i,1);
  const uint32_t shape[]={64,c.inputChannels,refs,uint32_t(c.reference.mode),64,10,12,64,1,0,64,1,64,2,9,7};for(size_t i=0;i<16;++i)w.u32(1600+4*i,shape[i]);
  Writer r{m.bytes(Section::referenceCheckpoint)};r.zero();r.u32(0,1);r.u32(4,256);r.u64(8,c.epoch);r.u64(16,p.frame.logicalId);
  r.u64(24,64);r.u64(32,c.inputChannels);
  if(array){r.u64(40,call);r.u64(48,call+1);r.u64(56,refs);r.f64(64,1);r.f64(192,1);r.f64(208,1);}
 }
 void prepared(const Packet&p){using namespace blah2::resident;common(Section::preparedEvidence,p,Phase::prepared);Writer w{m.bytes(Section::preparedEvidence)};
  const uint32_t channels=c.geometry.channels;
  w.u64(1408,p.frame.logicalId);const uint32_t shape[]={64,4,1024,1021,1,channels};for(size_t i=0;i<6;++i)w.u32(1416+4*i,shape[i]);
  w.u32(1440+16,1);w.u32(1440+48,1);for(size_t i=0;i<3;++i)w.f64(1504+8*i,1);
  w.u32(1536+24,21*channels);w.u32(1536+28,21);for(size_t i=1;i<=3;++i)w.u32(1600+4*i,21);
  w.u32(1600+72,uint32_t(c.epoch));w.u32(1600+76,uint32_t(c.epoch>>32));w.u32(1600+80,1);w.u32(1600+84,7);w.u32(1600+88,3);
  w.u32(1600+108,c.detector.integrationCpis);
  const auto motion=track_before_detect::decodeMotionBins(c.detector.integrationMotionBins);
  w.u32(1600+116,track_before_detect::motionEcho(motion));w.u32(1600+120,c.detector.integrationBeamTolerance);
  w.u32(1600+124,c.fusion.mode==FusionMode::noncoherent?0:c.fusion.beams);
  w.u32(1600+16,21*(c.detector.integrationCpis>2?track_before_detect::detectorPassCount(c.detector.integrationCpis):1));w.u32(1600+48,21);w.u32(1600+92,1);w.u32(1600+96,1);w.u32(1600+100,21);w.u32(1600+104,1);
  w.u32(1728+8,2);w.u64(1728+32,17);w.u64(1728+40,p.frame.logicalId);w.u64(1728+48,p.frame.consumedSequence);w.u64(1728+56,c.epoch);w.u64(1728+64,p.frame.signalTimestampMs);w.u32(1728+96,32);
  w.u32(1728+12,4);w.u32(1728+16,21);w.u64(1728+208,p.frame.logicalId);
  w.u32(1728+38*4,1);
  wire::writeTrackerDiagnostics(w,evidence::prepared::trackerDiagnostics,{});
  w.u64(evidence::prepared::times,16383);for(size_t i=0;i<14;++i)w.f64(evidence::prepared::times+16+8*i,1);
  const size_t a=evidence::prepared::nativeBatches;w.u64(a,p.frame.logicalId);w.u32(a+12,channels);w.u32(a+16,channels);
  const uint32_t native[]={0,0,0,0,0,channels,64*channels,64*channels,2*channels,2*channels,64*channels,channels,0,0,0,0};for(size_t i=0;i<16;++i)w.u32(a+24+4*i,native[i]);
  w.u32(a+88+12,64*channels);w.u32(a+88+16,
   (c.replay.maxBytes||(p.diagnosticPresence&(bit(Section::filtered)|bit(Section::tail)))?64:63)*channels);
  w.u32(a+88+20,63*channels);for(size_t i=0;i<9;++i)w.f64(a+152+8*i,1);
  w.f64(a+224,1);w.f64(a+232,1);w.f64(a+240,1);
  const size_t h=evidence::prepared::hostSummary;w.u32(h,1);w.u32(h+4,128);w.u64(h+8,p.frame.logicalId);w.u64(h+24,10000);w.u64(h+40,10000);
  w.u32(h+52,2048);w.u32(h+60,128);w.u32(h+68,1);w.u32(h+72,5);w.f64(h+80,.01);
 }
 void control(const Packet&p,Phase phase){Writer w{m.bytes(Section::controlEvidence)};w.zero();w.u32(0,1);w.u32(4,512);w.u32(8,uint32_t(phase));
  std::fill_n(m.bytes(Section::controlEvidence).data+112,16,0x11);
  std::fill_n(m.bytes(Section::controlEvidence).data+488,16,0x11);
  w.u64(16,p.session[0]);w.u64(24,p.session[1]);const uint64_t fields[]={p.requestId,p.frame.logicalId,p.epoch,p.frame.consumedSequence,p.tracker.transaction,p.tracker.generation,p.tracker.consumedSequence,p.tracker.epoch};
  for(size_t i=0;i<8;++i){w.u64(32+8*i,fields[i]);}
  uint64_t mask=32;uint32_t capture=0;const bool duplicate=p.op==Op::commit&&p.phase==Phase::committed;
  if(c.tracker.enabled){if(p.op==Op::initialize||p.op==Op::resetInput){mask|=2;capture=1;}else if(p.op==Op::restore){mask|=13;capture=1;}else if(p.op==Op::commit&&!duplicate){mask|=25;capture=1;}else if(duplicate)capture=2;}
  if(p.op==Op::resetInput){mask|=64;w.u32(480,3u|(c.tracker.enabled?4u:0u));}
  w.u64(96,mask);w.u64(104,++event);if(mask&1){w.u32(128+12,c.tracker.capacity);w.u32(128+232,1);if(p.op==Op::commit){w.u64(128+32,p.tracker.transaction);w.u64(128+40,p.tracker.generation);w.u64(128+48,p.tracker.consumedSequence);w.u64(128+56,p.tracker.epoch);}}
  w.u64(416,p.op==Op::quit?0:4096);w.u64(424,4096);w.u64(432,1<<20);w.u64(440,p.op==Op::quit?0:1);w.u32(456,2);w.u32(460,duplicate);w.u32(468,p.op==Op::commit?1:0);w.u32(472,p.op==Op::commit?c.stages():0);w.u32(476,capture);
 }
};
