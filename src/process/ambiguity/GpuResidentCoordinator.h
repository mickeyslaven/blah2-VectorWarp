#pragma once
#include "GpuResidentBackend.h"
#include "GpuResidentProof.h"
#include "GpuResultJson.private.h"
#include "Acceleration.h"
#include "process/conditioning/ArrayReferenceSynthesizer.h"
#include <algorithm>
#include <iostream>
namespace blah2::resident {
// Parent control/byte-copy/serializer route. CPU numeric loops below are guarded
// startup oracles only; no accepted steady frame can invoke them.
class Coordinator {
 std::unique_ptr<Session> session_;
 uint32_t checks_=0;
 bool refChecked_=false,filteredChecked_=false,mapChecked_=false;
 const std::complex<double>* canonical_=nullptr;
 OutputViews ready_{},prepared_{};
 AccelerationStatus status_;
 std::array<uint8_t,256> referenceCheckpoint_{};
 bool haveReferenceCheckpoint_=false;
 void clearBorrowed()noexcept{canonical_=nullptr;
 ready_={};
 prepared_={};}
 template<class F>decltype(auto) sessionCall(F&& f){try{return f();}catch(...){clearBorrowed();
 throw;}}
 void oracle()const{wire::require(qualifying(),"CPU resident oracle called after qualification");}
 static bool finite(std::complex<double> v){return std::isfinite(v.real())&&std::isfinite(v.imag());}
public:
 explicit Coordinator(Config c,GpuProcessOptions options={}):session_(createProcess(c,options)){status_.requested="gpu-resident";
 status_.active="vulkan-resident";
 status_.device=c.device;
 status_.state="checking";
 status_.reason="Strict startup CPU oracle; failures stop this route";}
 const Config& config()const{return session_->config();}
 bool qualifying()const{return checks_<Acceleration::TotalQualificationFrames;}
 const AccelerationStatus& status()const{return status_;}
 void begin(const std::vector<std::unique_ptr<IqData>>& capture,Frame frame){sessionCall([&]{clearBorrowed();wire::require(capture.size()==config().geometry.channels,"Resident capture channel count differs");auto input=session_->input();canonical_=input.data;size_t offset=0;for(const auto& channel:capture){const auto& samples=channel->view_data();wire::require(samples.size()==config().geometry.clutterSamples,"Resident captured CPI size differs");std::copy(samples.begin(),samples.end(),input.data+offset);offset+=samples.size();}wire::require(offset==input.size,"Resident canonical input shape differs");refChecked_=filteredChecked_=mapChecked_=false;session_->begin(frame,qualifying()?(bit(Section::fullReference)|bit(Section::filtered)):0);ready_=session_->receiveReferenceReady();wire::require(ready_.referenceCheckpoint.size==referenceCheckpoint_.size(),"Resident acknowledged reference checkpoint shape");std::memcpy(referenceCheckpoint_.data(),ready_.referenceCheckpoint.data,referenceCheckpoint_.size());haveReferenceCheckpoint_=true;ready_.referenceCheckpoint={referenceCheckpoint_.data(),referenceCheckpoint_.size()};});}
 const OutputViews& receivePrepared(){sessionCall([&]{if(!prepared_.preparedEvidence.data)prepared_=session_->receivePrepared();});
 return prepared_;}
 void checkReference(const IqData& reference,const ArrayReferenceSynthesizer::Metrics& metrics){oracle();
 wire::require(ready_.reference.size==reference.view_data().size(),"Resident reference oracle shape differs");
 size_t i=0;
 for(auto expected:reference.view_data()){auto value=ready_.reference.data[i++];
 wire::require(finite(value)&&finite(expected)&&std::abs(value-expected)<1e-12,"Resident reference oracle rejected frame");}const auto cp=referenceState(ready_.referenceCheckpoint);
 wire::require(cp.updates==metrics.updates&&cp.weightCount==metrics.weights.size()&&std::abs(cp.coherentFraction-metrics.coherentFraction)<=1e-12&&std::abs(cp.coherentGainDb-metrics.coherentGainDb)<=1e-10,"Resident reference state oracle rejected frame");
 for(size_t c=0;c<metrics.weights.size();++c)wire::require(std::abs(cp.weights[c]-metrics.weights[c])<=1e-12,"Resident reference weight oracle rejected frame");
 refChecked_=true;}
 void checkFiltered(const std::vector<IqData*>& cpu){oracle();
 const auto& out=receivePrepared();
 const size_t n=config().geometry.clutterSamples;
 wire::require(cpu.size()==config().geometry.channels&&out.filtered.size==n*cpu.size(),"Resident sample oracle shape differs");
 for(size_t c=0;c<cpu.size();++c){const auto& expected=cpu[c]->view_data();
 wire::require(expected.size()==n,"Resident sample oracle was consumed");
 double error=0,filtered=0,input=0,maxError=0,maxFiltered=0,maxInput=0;
 for(size_t i=0;i<n;++i){const auto actual=out.filtered.data[c*n+i];
 wire::require(finite(actual)&&finite(expected[i]),"Resident sample oracle nonfinite");
 const double delta=std::norm(actual-expected[i]);
 const double original=std::norm(std::complex<float>(canonical_[c*n+i]));
 error+=delta;
 filtered+=std::norm(expected[i]);
 input+=original;
 maxError=std::max(maxError,delta);
 maxFiltered=std::max(maxFiltered,std::norm(expected[i]));
 maxInput=std::max(maxInput,original);}const double filteredFloor=std::max(filtered,input*1e-8),peakFloor=std::max(maxFiltered,maxInput*1e-8);
 wire::require(std::isfinite(error)&&std::isfinite(filtered)&&std::isfinite(input)&&error<=std::max(input*1e-8,1e-20)&&error<=std::max(filteredFloor*1e-8,1e-20)&&maxError<=std::max(maxInput*1e-8,1e-20)&&maxError<=std::max(peakFloor*1e-8,1e-20),"Resident clutter sample oracle rejected frame");}filteredChecked_=true;}
 void checkMaps(const std::vector<Ambiguity*>& cpu){oracle();
 const auto& out=receivePrepared();
 const auto& g=config().geometry;
 const size_t cells=size_t(g.doppler)*g.delays;
 wire::require(cpu.size()==g.channels&&out.maps.size==cells*g.channels,"Resident map oracle shape differs");
 for(size_t c=0;c<cpu.size();++c){const auto& data=cpu[c]->result()->data;
 double error=0,signal=0,peak=0,maxError=0;
 for(uint32_t d=0;d<g.doppler;++d)for(uint32_t r=0;r<g.delays;++r){const auto expected=data[d][r];
 const std::complex<double> actual=out.maps.data[c*cells+size_t(r)*g.doppler+(d+g.doppler/2+1)%g.doppler];
 wire::require(finite(expected)&&finite(actual),"Resident composed map nonfinite");
 const double delta=std::norm(actual-expected);
 error+=delta;
 signal+=std::norm(expected);
 peak=std::max(peak,std::norm(expected));
 maxError=std::max(maxError,delta);}wire::require(std::isfinite(error)&&std::isfinite(signal)&&error<=std::max(signal*1e-8,1e-20)&&maxError<=std::max(peak*1e-8,1e-20),"Resident composed map oracle rejected frame");}mapChecked_=true;}
 struct Publication {std::string spectrum,map,detections,tracks,bearing;};
 Publication format(uint64_t timestamp,const gpu_result_json::BearingConfig& bearing={}){const auto& v=receivePrepared();
 if(qualifying())wire::require(refChecked_&&filteredChecked_&&mapChecked_,"Resident startup oracle not completed");
 namespace json=gpu_result_json;
 Publication result;
 result.spectrum=json::spectrum({timestamp,{v.frequencies.data,v.frequencies.size},{v.spectrumDb.data,v.spectrumDb.size}});
 result.map=json::map({timestamp,config().geometry.doppler,config().geometry.delays,v.noise,v.maxPower,{v.delayKm.data,v.delayKm.size},{v.doppler.data,v.doppler.size},{v.display.data,v.display.size}});
 if(config().detector.enabled)result.detections=json::detectionRecords(timestamp,json::View<DetectionRecord>{v.detections.data,v.detections.size});
 if(config().tracker.enabled){const auto* w=v.trackerCheckpoint.data;
 wire::require(v.trackerCheckpoint.size>=32,"Resident tracker checkpoint missing");
 const json::CheckpointBinding binding{config().tracker.capacity,config().epoch,json::wordPair(w,12),json::wordPair(w,14),config().fusion.mode==FusionMode::noncoherent?0u:config().fusion.beams,config().bearing.scanBeams};
 result.tracks=json::trackerCheckpoint(timestamp,{w,v.trackerCheckpoint.size},binding);
 if(bearing.enabled)result.bearing=json::bearingCheckpoint(timestamp,{w,v.trackerCheckpoint.size},binding,bearing);}return result;}
 void commit(){wire::require(prepared_.preparedEvidence.data,"Resident commit missing prepared frame");
 if(qualifying())wire::require(refChecked_&&filteredChecked_&&mapChecked_,"Resident startup oracle missing before commit");
 sessionCall([&]{session_->commit(prepared_.trackerToken);});}
 void published(){sessionCall([&]{session_->published();});
 if(qualifying()){++checks_;
 if(!qualifying()){status_.state="ready";
 status_.reason="GPU-resident stages qualified; no steady CPU DSP oracle";}}}
 const OutputViews& prepared()const{return prepared_;}
 // The acknowledged reference state survives any later radar/control failure.
 // Tracker views are borrowed only: after failure none is exposed or retried.
 ConstBytes acknowledgedReferenceCheckpoint()const noexcept{return haveReferenceCheckpoint_?ConstBytes{referenceCheckpoint_.data(),referenceCheckpoint_.size()}:ConstBytes{};}
 void stop()noexcept{clearBorrowed();
 session_->stop();}
};
}
