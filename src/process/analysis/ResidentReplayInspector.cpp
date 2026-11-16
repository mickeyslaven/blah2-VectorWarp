#include "ResidentReplayInspector.h"
#include "process/ambiguity/GpuResidentWire.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace blah2::resident_replay {
namespace {
using resident::wire::require;
bool nonzero(const Hash& h){return std::any_of(h.begin(),h.end(),[](uint8_t b){return b!=0;});}
uint64_t number(const std::string& s){
  require(!s.empty(),"Empty manifest identity");
  uint64_t n=0;
  for(char c:s){require(c>='0'&&c<='9',"Manifest identity must be decimal unsigned integer");
    require(n<=(std::numeric_limits<uint64_t>::max()-uint64_t(c-'0'))/10,"Manifest identity overflow");
    n=n*10+uint64_t(c-'0');}
  return n;
}
bool next(uint64_t a,uint64_t b){return a!=std::numeric_limits<uint64_t>::max()&&b==a+1;}
resident::ConstBytes descriptor(const resident::wire::Reader& r,uint32_t kind){
  const size_t at=512+24*(kind-1);
  require(r.u32(at)==kind&&!r.u32(at+4),"Snapshot block descriptor invalid");
  const auto offset=r.u64(at+8),length=r.u64(at+16);
  require(offset>=headerBytes&&offset<=r.b.size&&length<=r.b.size-offset,"Snapshot block out of bounds");
  return {r.b.data+size_t(offset),size_t(length)};
}
}
Hash parseInspectorHash(const std::string& text){
  require(text.size()==64,"Expected SHA-256 must contain exactly 64 hex digits");
  const auto nibble=[](char c)->uint8_t{
    if(c>='0'&&c<='9')return uint8_t(c-'0');
    if(c>='a'&&c<='f')return uint8_t(c-'a'+10);
    if(c>='A'&&c<='F')return uint8_t(c-'A'+10);
    throw std::invalid_argument("Invalid SHA-256 hex digit");
  };
  Hash result{};
  for(size_t i=0;i<result.size();++i)result[i]=uint8_t(nibble(text[2*i])*16+nibble(text[2*i+1]));
  require(nonzero(result),"Expected SHA-256 must be nonzero");
  return result;
}
std::vector<Identity> readInspectorManifest(const std::string& path){
  std::ifstream input(path,std::ios::binary|std::ios::ate);
  require(bool(input),"Cannot open expected identity manifest");
  require(input.tellg()>=0&&input.tellg()<=std::streamoff(frameLimit*4096),"Manifest exceeds byte bound");
  input.seekg(0);
  std::vector<Identity> result;
  std::string line;
  while(std::getline(input,line)){
    require(line.size()<=4096,"Manifest line exceeds bound");
    std::istringstream row(line);
    std::string field[10],extra;
    if(!(row>>field[0]))continue;
    if(field[0][0]=='#')continue;
    for(size_t i=1;i<10;++i)require(bool(row>>field[i]),"Incomplete manifest identity");
    require(!(row>>extra),"Unexpected manifest field");
    require(result.size()<frameLimit,"Manifest exceeds frame limit");
    Identity id{number(field[0]),number(field[1]),number(field[2]),number(field[3]),number(field[4]),number(field[5]),
      parseInspectorHash(field[6]),parseInspectorHash(field[7]),parseInspectorHash(field[8]),parseInspectorHash(field[9])};
    result.push_back(id);
  }
  require(input.eof(),"Could not read expected identity manifest");
  return result;
}
std::vector<uint8_t> readInspectorSnapshot(const std::string& path){
  require(path.size()>=6&&path.substr(path.size()-6)==".vwrps","Snapshot filename must end in .vwrps");
  std::ifstream input(path,std::ios::binary|std::ios::ate);
  require(bool(input),"Cannot open snapshot");
  const auto end=input.tellg();
  require(end>=std::streamoff(headerBytes)&&end<=std::streamoff(byteLimit),"Snapshot file outside size bound");
  std::vector<uint8_t> data(static_cast<size_t>(end));
  input.seekg(0);
  require(bool(input.read(reinterpret_cast<char*>(data.data()),std::streamsize(data.size()))),"Cannot read complete snapshot");
  require(input.peek()==std::char_traits<char>::eof(),"Snapshot changed size during read");
  return data;
}
SequenceInspector::SequenceInspector(Hash source,Hash rf,std::vector<Identity> expected,size_t warmup)
  :source_(source),rf_(rf),expected_(std::move(expected)),warmup_(warmup){
  require(nonzero(source_)&&nonzero(rf_),"Independent source and RF hashes are required");
  require(!expected_.empty()&&expected_.size()<=frameLimit&&warmup_<expected_.size(),"Expected sequence/warmup bound invalid");
  for(size_t i=0;i<expected_.size();++i){
    const auto& b=expected_[i];
    require(b.epoch&&b.logicalFrame&&b.generation&&b.consumedSequence&&nonzero(b.configHash)&&nonzero(b.moduleHash)&&nonzero(b.rawIqHash)&&nonzero(b.deviceHash),"Expected identity incomplete");
    if(!i)continue;
    const auto& a=expected_[i-1];
    require(a.epoch==b.epoch&&a.configHash==b.configHash&&a.moduleHash==b.moduleHash&&a.deviceHash==b.deviceHash,
      "Sequence changes epoch/config/module/device");
    require(next(a.logicalFrame,b.logicalFrame)&&next(a.generation,b.generation)&&b.consumedSequence>a.consumedSequence&&b.signalTimestampMs>=a.signalTimestampMs,
      "Expected sequence duplicates, skips, or reorders frames");
    require((!a.trackerTransaction&&!b.trackerTransaction)||next(a.trackerTransaction,b.trackerTransaction),"Expected tracker transaction discontinuity");
  }
}
InspectedFrame SequenceInspector::append(const uint8_t* data,size_t bytes){
  require(seen_<expected_.size(),"Unexpected extra snapshot");
  require(data&&bytes>=headerBytes&&bytes<=byteLimit,"Snapshot input outside size bound");
  const resident::wire::Reader r{{data,bytes}};
  require(r.u32(0)==0x52505331&&r.u32(4)==version&&r.u32(8)==headerBytes&&r.u32(12)==20&&r.u64(16)==bytes,"Snapshot envelope invalid");
  const auto configBytes=descriptor(r,uint32_t(BlockKind::configuration));
  const auto metadata=descriptor(r,uint32_t(BlockKind::postprocessMetadata));
  const resident::wire::Reader m{metadata};
  const auto rows=r.u32(76),cols=r.u32(80);
  require(rows<=65535&&cols<=65535,"Snapshot axes outside bound");
  const size_t dopplerOffset=96*4,delayOffset=dopplerOffset+size_t(rows)*8;
  m.range(dopplerOffset,size_t(rows)*8);m.range(delayOffset,size_t(cols)*4);
  const auto config=resident::decodeConfig(configBytes,{metadata.data+delayOffset,size_t(cols)*4},
    {metadata.data+dopplerOffset,size_t(rows)*8});
  require(config.replay.sourceHash==source_&&config.replay.rfHash==rf_,"Snapshot independent source/RF hash mismatch");
  require(expected_.size()<=config.replay.maxFrames,"Sequence exceeds configured frame bound");
  const auto snapshot=decode(data,bytes,config,expected_[seen_]);
  const auto& meta=snapshot.blocks.at(uint32_t(BlockKind::postprocessMetadata)-1).bytes;
  const resident::wire::Reader validated{{meta.data(),meta.size()}};
  InspectedFrame report;
  report.identity=snapshot.identity;report.warmup=seen_<warmup_;
  report.covarianceValid=snapshot.covarianceValid;
  report.historyAvailable=validated.u32(79*4);
  const uint32_t twoCpiReady=validated.u32(28*4);
  require(twoCpiReady<=1,"Snapshot two-CPI readiness echo invalid");
  report.historyReady=config.detector.integrationCpis==1?true:
    config.detector.integrationCpis==2?twoCpiReady!=0:
    report.historyAvailable>=config.detector.integrationCpis;
  report.detections=validated.u32(11*4);
  if(config.tracker.enabled){const auto& tracker=snapshot.blocks.at(uint32_t(BlockKind::trackerCheckpoint)-1).bytes;
    report.tracks=resident::wire::Reader{{tracker.data(),tracker.size()}}.u64(16);}
  ++seen_;
  return report;
}
void SequenceInspector::finish() const{require(seen_==expected_.size(),"Missing snapshots from expected sequence");}
}
