#pragma once
#include "process/ambiguity/ClutterReplayCheckpoint.private.h"
// Qualification-only serialization of the samples/statistics actually consumed
// by production. No DSP, oracle, truncation, or live-mode fallback lives here.
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <array>
#include <cerrno>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace blah2::qualification_observer {
struct Config {
  std::string directory;
  uint32_t maxFrames=64,maxSamplesPerChannel=4096,maxMapCells=8192;
  uint64_t maxBytes=256ULL<<20;
  bool clutter=false;
  bool summaryOnly=false;
  std::string clutterFilterPolicy="refit-weights";
};
template<class T> struct View { const T* data=nullptr; size_t size=0; };
struct Frame {
  std::string lane="primary",backend,device,fusionMode,productBackend="cpu";
  uint64_t sequence=0,captureEpoch=0,captureFirst=0,captureEnd=0,frequencyHz=0;
  uint32_t sampleRateHz=0,inputSamplesPerOutput=1,beamCount=0,integrationCpis=1;
  uint32_t referenceAnalysisSamples=0,referenceAnalysisInterval=0,referencePowerIterations=0;
  double referenceSmoothing=0,referenceLoading=0,directPathAzimuthRad=0,directPathNullDb=0;
  bool samplePositionAvailable=false;
  std::vector<uint32_t> channelIds,referenceChannelIds;
  std::vector<double> actualGainDb;
  std::vector<std::array<double,3>> positionsM;
  std::string detectionManifoldFile,bearingManifoldFile;
  std::string resolvedProductPlan;
};
struct ResidentAllocation {
  bool available=false,laneBudgetAvailable=false;
  std::array<uint64_t,2> session{};
  uint64_t limitBytes=0,usedBytes=0,peakBytes=0,liveAllocations=0,sharedReservationBytes=0;
  uint64_t aggregateCapBytes=0,sessionReservationBytes=0;
  uint32_t aggregateSessionCap=0;
  void validate()const {
    const bool counters=available&&(session[0]||session[1])&&limitBytes&&sharedReservationBytes&&
      usedBytes<=peakBytes&&peakBytes<=limitBytes;
    if(!counters)throw std::runtime_error("Qualification observer resident allocation counters invalid");
    if(laneBudgetAvailable){
      if(!aggregateSessionCap||aggregateSessionCap>8||!aggregateCapBytes||
        sessionReservationBytes!=aggregateCapBytes/aggregateSessionCap||
        sharedReservationBytes>=sessionReservationBytes||
        limitBytes>sessionReservationBytes-sharedReservationBytes)
        throw std::runtime_error("Qualification observer resident allocation exceeds lane reservation");
    }else if(aggregateCapBytes||sessionReservationBytes||aggregateSessionCap)
      throw std::runtime_error("Qualification observer invented an aggregate lane budget");
  }
  std::string sessionId()const {
    // Stable identity from the two authenticated wire nonce words, never from
    // lane names, process IDs, device names, or a synthesized observer counter.
    constexpr char hex[]="0123456789abcdef";std::string id(32,'0');
    for(size_t word=0;word<2;++word)for(size_t digit=0;digit<16;++digit)
      id[word*16+digit]=hex[(session[word]>>(4*(15-digit)))&15];
    return id;
  }
};
struct Products {
  uint32_t rows=0,columns=0;
  std::vector<double> delayBins,dopplerHz;
  size_t channels=0,referenceSamples=0;
  std::function<std::complex<double>(size_t,size_t)> caf;
  std::function<std::complex<double>(size_t)> reference,fused;
  // A host magnitude map contains no beam IDs. Never call its imaginary zero
  // a measured beam, and never label a fused amplitude as complex phase.
  bool fusedBeamIdsAvailable=false;
  std::vector<std::complex<double>> referenceWeights;
  View<uint32_t> flags,detectionCells;
  View<double> unflooredSnr;
  bool residentProofAvailable=false;
  uint64_t logicalFrame=0,consumedSequence=0,nativeGeneration=0,residentGeneration=0;
  std::array<uint32_t,32> postprocessStatus{};
  std::array<uint32_t,64> trackerStatus{};
  ResidentAllocation residentAllocation;
};
// Owned copies made only at the opt-in production clutter boundary, before
// ambiguity can consume its borrowed output. No filter is executed here.
struct Clutter {
  bool enabled=false,gpuExecuted=false,cpuExecuted=false;
  int32_t delayMin=0,delayMax=0;
  std::string backend,checkpointProof;
  std::vector<std::complex<double>> reference;
  std::vector<std::vector<std::complex<double>>> filtered;
};
template<class Reference,class Filtered>
Clutter snapshotClutter(size_t samples,size_t channels,Reference reference,Filtered filtered,
    bool enabled,int32_t first,int32_t end,const std::string& backend,bool gpu,bool cpu) {
  if(!samples||samples>4096||!channels||channels>8)
    throw std::runtime_error("Qualification clutter snapshot exceeds bounds");
  Clutter result;result.enabled=enabled;result.delayMin=first;result.delayMax=end;
  result.backend=backend;result.gpuExecuted=gpu;result.cpuExecuted=cpu;
  result.reference.resize(samples);result.filtered.resize(channels);
  for(size_t i=0;i<samples;++i)result.reference[i]=reference(i);
  for(size_t c=0;c<channels;++c){result.filtered[c].resize(samples);
    for(size_t i=0;i<samples;++i)result.filtered[c][i]=filtered(c,i);}
  return result;
}
class Observer {
  using Writer=rapidjson::Writer<rapidjson::StringBuffer>;
  Config config_;
  int fd_=-1;
  uint64_t bytes_=0;
  std::mutex mutex_;
  struct Pending { uint64_t sequence,epoch,first,end; bool finished=false,detectorFinished=false; };
  std::map<std::string,Pending> pending_;
  struct ClutterInput { Frame frame;std::vector<std::vector<std::complex<double>>> samples;bool finished=false; };
  std::map<std::string,ClutterInput> clutterInputs_;
  std::map<std::string,uint32_t> counts_;
  static void require(bool ok,const char* message) {
    if(!ok)throw std::runtime_error(message);
  }
  static void number(Writer& w,double value) {
    require(std::isfinite(value),"Qualification observer received nonfinite numeric data");w.Double(value);
  }
  static void complex(Writer& w,std::complex<double> value) {
    w.StartArray();number(w,value.real());number(w,value.imag());w.EndArray();
  }
  static void identity(Writer& w,const Frame& f,const char* stage) {
    w.StartObject();w.Key("schema");w.String("vectorwarp-coherent-observer-v1");
    w.Key("stage");w.String(stage);w.Key("lane");w.String(f.lane.c_str());
    w.Key("sequence");w.Uint64(f.sequence);w.Key("captureEpoch");w.Uint64(f.captureEpoch);
    w.Key("captureFirst");w.Uint64(f.captureFirst);w.Key("captureEnd");w.Uint64(f.captureEnd);
    w.Key("sampleRateHz");w.Uint(f.sampleRateHz);w.Key("frequencyHz");w.Uint64(f.frequencyHz);
    w.Key("inputSamplesPerOutput");w.Uint(f.inputSamplesPerOutput);
    w.Key("backend");w.String(f.backend.c_str());w.Key("device");w.String(f.device.c_str());
    w.Key("product_backend");w.String(f.productBackend.c_str());
    w.Key("resolved_product_plan");w.String(f.resolvedProductPlan.c_str());
    w.Key("channel_ids");w.StartArray();for(auto c:f.channelIds)w.Uint(c);w.EndArray();
    w.Key("reference_channel_ids");w.StartArray();for(auto c:f.referenceChannelIds)w.Uint(c);w.EndArray();
    w.Key("actual_gain_db");w.StartArray();for(auto gain:f.actualGainDb)number(w,gain);w.EndArray();
    w.Key("geometry");w.StartObject();w.Key("fusion_mode");w.String(f.fusionMode.c_str());
    w.Key("beam_count");w.Uint(f.beamCount);w.Key("integration_cpis");w.Uint(f.integrationCpis);
    w.Key("direct_path_azimuth_rad");number(w,f.directPathAzimuthRad);
    w.Key("direct_path_null_db");number(w,f.directPathNullDb);
    w.Key("positions_m");w.StartArray();for(const auto& p:f.positionsM){w.StartArray();for(auto x:p)number(w,x);w.EndArray();}w.EndArray();
    w.Key("detection_manifold_file");w.String(f.detectionManifoldFile.c_str());
    w.Key("bearing_manifold_file");w.String(f.bearingManifoldFile.c_str());
    w.Key("whitening_scales_available");w.Bool(false);w.EndObject();
    w.Key("reference_config");w.StartObject();
    w.Key("analysis_samples");w.Uint(f.referenceAnalysisSamples);
    w.Key("analysis_interval");w.Uint(f.referenceAnalysisInterval);
    w.Key("power_iterations");w.Uint(f.referencePowerIterations);
    w.Key("covariance_smoothing");number(w,f.referenceSmoothing);
    w.Key("diagonal_loading");number(w,f.referenceLoading);w.EndObject();
  }
  void publish(rapidjson::StringBuffer& buffer) {
    require(buffer.GetSize()+1<=config_.maxBytes-bytes_,"Qualification observer byte budget exhausted");
    std::string line(buffer.GetString(),buffer.GetSize());line+='\n';
    size_t offset=0;
    while(offset<line.size()) {
      const auto n=::write(fd_,line.data()+offset,line.size()-offset);
      if(n<0&&errno==EINTR)continue;
      require(n>0,"Cannot write qualification observer artifact");offset+=size_t(n);
    }
    bytes_+=line.size();
  }
public:
  Observer(Config config,bool replay,bool qualificationConfirmed):config_(std::move(config)) {
    require(replay&&qualificationConfirmed,"Coherent observer requires confirmed offline replay qualification");
    require(config_.clutterFilterPolicy=="refit-weights",
      "Production clutter supports refit-weights only; fixed-weights replay is unavailable");
    require(!config_.directory.empty()&&std::filesystem::path(config_.directory).is_absolute()&&
      std::filesystem::is_directory(config_.directory),"Qualification observer requires an existing absolute directory");
    require(!config_.summaryOnly||!config_.clutter,"Summary observer cannot claim complex clutter evidence");
    require(config_.maxFrames&&config_.maxFrames<=(config_.summaryOnly?256u:64u)&&config_.maxSamplesPerChannel&&
      config_.maxSamplesPerChannel<=(config_.summaryOnly?16777216u:4096u)&&config_.maxMapCells&&
      config_.maxMapCells<=(config_.summaryOnly?4194304u:8192u)&&
      config_.maxBytes&&config_.maxBytes<=(256ULL<<20),"Qualification observer bounds are invalid");
    const auto path=std::filesystem::path(config_.directory)/"coherent-observer.jsonl";
    fd_=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    require(fd_>=0,"Cannot exclusively create qualification observer artifact");
  }
  Observer(const Observer&)=delete;
  Observer& operator=(const Observer&)=delete;
  ~Observer(){if(fd_>=0)::close(fd_);}
  // Post-detection observation is separate from the pre-temporal CAF snapshot.
  // It never re-runs CFAR or uses final/interpolated detections as trial counts.
  void detector(const Frame& f,bool enabled,bool ready,const char* backend,
      uint64_t evaluated,uint64_t accepted,uint64_t nonfinite=0) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found=pending_.find(f.lane);
    require(found!=pending_.end()&&found->second.finished&&!found->second.detectorFinished&&
      found->second.sequence==f.sequence&&found->second.epoch==f.captureEpoch&&
      found->second.first==f.captureFirst&&found->second.end==f.captureEnd,
      "Qualification detector observation has no exact product lineage");
    require(accepted<=evaluated&&nonfinite<=evaluated&&evaluated<=uint64_t(config_.maxMapCells)*4&&
      ((enabled&&ready)||(!evaluated&&!accepted&&!nonfinite)),
      "Qualification detector observation has invalid evaluated population");
    rapidjson::StringBuffer b;Writer w(b);identity(w,f,"detector");
    w.Key("measurement");w.String("production-cfar-pre-centroid-v1");
    w.Key("detector_backend");w.String(backend);
    w.Key("enabled");w.Bool(enabled);w.Key("ready");w.Bool(ready);
    w.Key("evaluated_cells");w.Uint64(evaluated);
    w.Key("accepted_cells");w.Uint64(accepted);
    w.Key("nonfinite_cells");w.Uint64(nonfinite);w.EndObject();publish(b);
    found->second.detectorFinished=true;
  }
  bool observesClutter()const{return config_.clutter;}
  bool observesSamples()const{return !config_.summaryOnly;}
  template<class Sample> void input(const Frame& f,size_t channels,size_t samples,Sample sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    require(!f.lane.empty()&&f.sequence&&f.samplePositionAvailable&&f.captureEnd>f.captureFirst&&
      f.sampleRateHz&&f.frequencyHz&&f.inputSamplesPerOutput&&channels&&channels<=8&&samples&&
      samples<=config_.maxSamplesPerChannel&&
      f.captureEnd-f.captureFirst==samples*uint64_t(f.inputSamplesPerOutput),
      "Qualification observer input identity/shape invalid or exceeds budget");
    require(f.actualGainDb.empty()||f.actualGainDb.size()==channels,"Qualification observer gain topology differs");
    require(counts_.size()<8||counts_.count(f.lane),"Qualification observer lane budget exhausted");
    auto previous=pending_.find(f.lane);
    require(previous==pending_.end()||f.sequence>previous->second.sequence,
      "Qualification observer sequence was reused");
    require(counts_[f.lane]<config_.maxFrames,"Qualification observer frame budget exhausted");
    rapidjson::StringBuffer b;Writer w(b);identity(w,f,"input");
    w.Key("summary_only");w.Bool(config_.summaryOnly);w.Key("sample_count");w.Uint64(samples);
    w.Key("input_channel_ids");w.StartArray();for(size_t c=0;c<channels;++c)w.Uint(uint32_t(c));w.EndArray();
    if(!config_.summaryOnly){w.Key("channelized_iq");w.StartArray();for(size_t c=0;c<channels;++c){w.StartArray();
      for(size_t i=0;i<samples;++i){complex(w,sample(c,i));}w.EndArray();}w.EndArray();}w.EndObject();
    ClutterInput clutterInput;
    if(config_.clutter){
      require(!f.channelIds.empty()&&f.channelIds.size()<=channels,
        "Qualification clutter surveillance topology missing");
      clutterInput.frame=f;clutterInput.samples.resize(f.channelIds.size());
      for(size_t c=0;c<f.channelIds.size();++c){
        require(f.channelIds[c]<channels,"Qualification clutter channel outside input");
        for(size_t previous=0;previous<c;++previous)require(f.channelIds[previous]!=f.channelIds[c],
          "Qualification clutter channel reused");
        auto& values=clutterInput.samples[c];values.resize(samples);
        for(size_t i=0;i<samples;++i)values[i]=sample(f.channelIds[c],i);
      }
    }
    publish(b);++counts_[f.lane];pending_[f.lane]={f.sequence,f.captureEpoch,f.captureFirst,f.captureEnd,false};
    if(config_.clutter)clutterInputs_[f.lane]=std::move(clutterInput);
  }
  void clutter(const Frame& f,const Clutter& c) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found=clutterInputs_.find(f.lane);
    require(config_.clutter&&found!=clutterInputs_.end()&&!found->second.finished,
      "Qualification clutter observation disabled, missing or duplicated");
    const auto& original=found->second.frame;
    require(f.sequence==original.sequence&&f.captureEpoch==original.captureEpoch&&
      f.captureFirst==original.captureFirst&&f.captureEnd==original.captureEnd&&
      f.channelIds==original.channelIds&&f.referenceChannelIds==original.referenceChannelIds&&
      f.frequencyHz==original.frequencyHz&&f.sampleRateHz==original.sampleRateHz&&
      f.inputSamplesPerOutput==original.inputSamplesPerOutput,
      "Qualification clutter has no exact input lineage");
    const auto& before=found->second.samples;const auto samples=before.front().size();
    require(c.reference.size()==samples&&c.filtered.size()==before.size()&&
      c.delayMax>c.delayMin&&uint64_t(int64_t(c.delayMax)-c.delayMin)<=samples&&
      !c.backend.empty()&&(c.enabled?(c.gpuExecuted||c.cpuExecuted):(!c.gpuExecuted&&!c.cpuExecuted)),
      "Qualification clutter shape or execution identity differs");
    require(c.gpuExecuted?(c.cpuExecuted?c.backend=="cpu+vulkan-qualification":
      (c.backend=="vulkan"||c.backend=="vulkan-resident")):c.backend=="cpu",
      "Qualification clutter backend contradicts execution");
    for(const auto& channel:c.filtered)require(channel.size()==samples,"Qualification clutter output truncated");
    if(!c.enabled)require(c.filtered==before,"Disabled clutter changed surveillance samples");
    rapidjson::StringBuffer b;Writer w(b);identity(w,f,"clutter");
    bool fixed=false;
    if(!c.checkpointProof.empty()){
      const auto proof=clutter_replay::Proof::decode(c.checkpointProof);
      require(c.enabled&&c.gpuExecuted&&!c.cpuExecuted&&c.backend=="vulkan"&&
        proof.frame.epoch==f.captureEpoch&&proof.frame.first==f.captureFirst&&proof.frame.end==f.captureEnd&&
        (!proof.fixed||(clutter_replay::hex(proof.checkpoint)&&clutter_replay::hex(proof.prefix))),
        "Clutter checkpoint proof contradicts actual execution/lineage");
      fixed=proof.fixed;w.Key("checkpoint_proof");w.String(c.checkpointProof.c_str());
    }
    w.Key("filter_policy");w.String(c.enabled?(fixed?"fixed-weights":"refit-weights"):"disabled");
    w.Key("clutter_backend");w.String(c.backend.c_str());
    w.Key("gpu_executed");w.Bool(c.gpuExecuted);w.Key("cpu_executed");w.Bool(c.cpuExecuted);
    w.Key("delay_min");w.Int(c.delayMin);w.Key("delay_max_exclusive");w.Int(c.delayMax);
    w.Key("relative_diagonal_loading");w.Double(0);
    w.Key("reference_iq");w.StartArray();for(auto v:c.reference)complex(w,v);w.EndArray();
    const auto channels=[&](const char* name,const auto& values){w.Key(name);w.StartArray();
      for(const auto& channel:values){w.StartArray();for(auto v:channel)complex(w,v);w.EndArray();}w.EndArray();};
    channels("unfiltered_surveillance",before);channels("filtered_surveillance",c.filtered);
    w.EndObject();publish(b);found->second.finished=true;
  }
  void products(const Frame& f,const Products& p) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found=pending_.find(f.lane);
    require(found!=pending_.end()&&!found->second.finished&&found->second.sequence==f.sequence&&
      found->second.epoch==f.captureEpoch&&found->second.first==f.captureFirst&&found->second.end==f.captureEnd,
      "Qualification observer product has no exact input lineage");
    require(!config_.clutter||(clutterInputs_.count(f.lane)&&clutterInputs_.at(f.lane).finished),
      "Qualification products lack the requested clutter observation");
    const uint64_t cells=uint64_t(p.rows)*p.columns;
    require(cells&&cells<=config_.maxMapCells&&p.channels&&p.channels<=8&&
      p.channels==f.channelIds.size()&&p.delayBins.size()==p.columns&&p.dopplerHz.size()==p.rows&&
      (config_.summaryOnly||(p.referenceSamples&&p.referenceSamples<=config_.maxSamplesPerChannel&&p.caf&&p.reference&&p.fused)),
      "Qualification observer product shape invalid or exceeds budget");
    require(p.residentProofAvailable==p.residentAllocation.available,
      "Qualification observer resident allocation proof missing or fabricated");
    if(p.residentAllocation.available)p.residentAllocation.validate();
    rapidjson::StringBuffer b;Writer w(b);identity(w,f,"products");
    w.Key("rows");w.Uint(p.rows);w.Key("columns");w.Uint(p.columns);
    w.Key("delay_bins");w.StartArray();for(auto v:p.delayBins)number(w,v);w.EndArray();
    w.Key("doppler_hz");w.StartArray();for(auto v:p.dopplerHz)number(w,v);w.EndArray();
    w.Key("summary_only");w.Bool(config_.summaryOnly);
    if(config_.summaryOnly){w.EndObject();publish(b);found->second.finished=true;return;}
    w.Key("reference_iq");w.StartArray();for(size_t i=0;i<p.referenceSamples;++i)complex(w,p.reference(i));w.EndArray();
    w.Key("reference_weights");w.StartArray();for(auto v:p.referenceWeights)complex(w,v);w.EndArray();
    w.Key("caf");w.StartArray();for(size_t c=0;c<p.channels;++c){w.StartArray();
      for(size_t i=0;i<cells;++i){complex(w,p.caf(c,i));}w.EndArray();}w.EndArray();
    w.Key("fused_amplitude");w.StartArray();for(size_t i=0;i<cells;++i)number(w,p.fused(i).real());w.EndArray();
    w.Key("fused_beam_id");if(p.fusedBeamIdsAvailable){w.StartArray();
      for(size_t i=0;i<cells;++i){number(w,p.fused(i).imag());}w.EndArray();}else w.Null();
    w.Key("fused_stage");w.String("pre_temporal");
    const auto words=[&](const char* name,View<uint32_t> values){
      require(values.size<=16*cells+4096&&(!values.size||values.data),"Qualification observer support budget exceeded");
      w.Key(name);w.StartArray();for(size_t i=0;i<values.size;++i)w.Uint(values.data[i]);w.EndArray();};
    words("flags",p.flags);words("detection_cells",p.detectionCells);
    w.Key("resident_proof");if(p.residentProofAvailable){w.StartObject();
      w.Key("logical_frame");w.Uint64(p.logicalFrame);w.Key("consumed_sequence");w.Uint64(p.consumedSequence);
      w.Key("native_generation");w.Uint64(p.nativeGeneration);w.Key("resident_generation");w.Uint64(p.residentGeneration);
      words("postprocess_status",{p.postprocessStatus.data(),p.postprocessStatus.size()});
      words("tracker_status",{p.trackerStatus.data(),p.trackerStatus.size()});w.EndObject();}else w.Null();
    w.Key("resident_allocation");if(p.residentAllocation.available){
      const auto& a=p.residentAllocation;w.StartObject();
      w.Key("session_id");w.String(a.sessionId().c_str());
      w.Key("limit_bytes");w.Uint64(a.limitBytes);w.Key("used_bytes");w.Uint64(a.usedBytes);
      w.Key("peak_bytes");w.Uint64(a.peakBytes);w.Key("live_allocations");w.Uint64(a.liveAllocations);
      w.Key("shared_reservation_bytes");w.Uint64(a.sharedReservationBytes);
      w.Key("aggregate_cap_bytes");if(a.laneBudgetAvailable)w.Uint64(a.aggregateCapBytes);else w.Null();
      w.Key("session_reservation_bytes");if(a.laneBudgetAvailable)w.Uint64(a.sessionReservationBytes);else w.Null();
      w.Key("aggregate_session_cap");if(a.laneBudgetAvailable)w.Uint(a.aggregateSessionCap);else w.Null();
      w.EndObject();
    }else w.Null();
    require(p.unflooredSnr.size<=cells&&(!p.unflooredSnr.size||p.unflooredSnr.data),"Qualification observer SNR shape invalid");
    w.Key("unfloored_snr");w.StartArray();for(size_t i=0;i<p.unflooredSnr.size;++i){
      // No-signal cells legitimately have -inf SNR; represent absence, not a
      // finite surrogate that could accidentally pass a numeric comparator.
      if(std::isfinite(p.unflooredSnr.data[i]))number(w,p.unflooredSnr.data[i]);else w.Null();}w.EndArray();
    w.EndObject();publish(b);found->second.finished=true;
  }
};
}
