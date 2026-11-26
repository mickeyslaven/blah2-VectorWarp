#include "GpuResultJson.private.h"
#include "ManifoldEvidence.h"
#include "BearingManifold.h"
#include "data/IqData.h"
#include "data/Map.h"
#include "data/Detection.h"
#include "data/Track.h"
#include "data/meta/Constants.h"
#include "data/meta/EventTime.h"
#include "process/tracker/CausalTraceJson.h"
#include "ResidentTemporalPublication.h"
#include "GpuResidentQualification.h"
#include "GpuResidentInput.h"
#include "GpuResidentCoordinator.h"
#include "process/analysis/StartupIq.h"
#include "process/pipeline/RadarPipeline.h"
#include "process/integration/TemporalEvidence.h"
#include <complex>
#include <vector>
#include <deque>
#include <iostream>
#include <limits>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <set>
namespace {
using namespace blah2::gpu_result_json;
void check(bool yes,const char*why){if(!yes)throw std::runtime_error(why);}
template<class F>void rejects(F&&f){bool yes=false;try{f();}catch(const std::invalid_argument&){yes=true;}check(yes,"Malformed result view accepted");}
template<class T> View<T> view(const std::vector<T>&v){return {v.data(),v.size()};}
void same(const std::string&a,const std::string&b){check(a==b,"Legacy GPU-result JSON bytes differ");rapidjson::Document d;d.Parse(a.c_str());check(!d.HasParseError()&&d.IsObject(),"Malformed GPU-result JSON");}
void diagnosticCases(){
 using blah2::resident::QualificationHistory;
 using blah2::integration::TemporalEvidence;
 QualificationHistory<double> lifetime;double sample=1;
 for(unsigned i=0;i<12;++i){sample=i;lifetime.retain(true,true,8,&sample,1);
  check(lifetime.maps().size()==std::min(i+1,8u)&&lifetime.maps().front()[0]==i,
   "Qualification diagnostic history lost its eight-map bound/order");}
 check(lifetime.maps().back()[0]==4,"Qualification diagnostic history did not evict oldest");
 lifetime.retain(false,true,8,nullptr,0);check(lifetime.maps().empty(),"Public frames retained diagnostic maps");
 lifetime.retain(true,true,8,&sample,1);lifetime.retain(true,false,8,nullptr,0);
 check(lifetime.maps().empty(),"Successful qualification retained diagnostic maps");
 lifetime.retain(true,true,8,&sample,1);lifetime.clear();
 check(lifetime.maps().empty(),"Reset/stop retained diagnostic maps");
 rejects([&]{lifetime.retain(true,true,9,&sample,1);});
 rejects([&]{lifetime.retain(true,true,8,&sample,(1u<<20)+1);});
 constexpr unsigned rows=17,columns=65,cell=8*columns+32;
 for(unsigned depth:{2u,4u,8u})for(double drift:{0.0,-.75}){
  TemporalEvidence evidence({depth,1,1,drift});QualificationHistory<double> history;
  TemporalEvidence::Result result;
  for(unsigned frame=0;frame<depth;++frame){Map<std::complex<double>> map(rows,columns);
   std::vector<double> power(rows*columns);
   for(unsigned row=0;row<rows;++row)map.doppler.push_back(int(row)-8);
   for(unsigned column=0;column<columns;++column)map.delay.push_back(column);
   for(unsigned row=0;row<rows;++row)for(unsigned column=0;column<columns;++column){
    const double amplitude=1+double((frame*31+row*7+column*13)%101)/17;
    map.data[row][column]={amplitude,0};power[row*columns+column]=amplitude*amplitude;}
   history.retain(true,true,depth,power.data(),power.size());result=evidence.process(map,1,frame+1);
  }
  for(unsigned bank=0;bank<4;++bank)if(result.banks[bank].ready){const auto& selected=result.banks[bank];
   const auto motion=selected.motion[cell];
   const auto cpu=evidence.auditPath(cell,selected.depth,motion.doppler,motion.delay,bank==0);
   const auto resident=evidence.auditPathWith(cell,selected.depth,motion.doppler,motion.delay,bank==0,
    [&](unsigned age,uint64_t source){return std::make_pair(history.maps().at(age).at(source),0u);});
   check(cpu.valid&&resident.valid&&cpu.mean==selected.detectorPower[cell]&&cpu.mean==resident.mean&&
    cpu.samples==resident.samples&&cpu.cells==resident.cells,"Path diagnostic disagrees with canonical selected bank");
  }
  check(!evidence.auditPath(0,depth,1,1,false).valid,"Path diagnostic admitted incomplete edge path");
  evidence.reset();check(!evidence.auditPath(cell,depth,0,0,false).valid,"CPU path diagnostic retained reset history");
 }
 TemporalEvidence gated({4,0,0,0,4,0});
 for(unsigned frame=0;frame<4;++frame){Map<std::complex<double>> map(1,1);map.delay={0};map.doppler={0};
  map.data[0][0]={1,0};std::vector<uint32_t> beams{frame?1u:2u};gated.process(map,1,frame+1,nullptr,&beams);}
 check(!gated.auditPath(0,4,0,0,false).valid,"Path diagnostic ignored beam gating");
 std::cout<<"PASS startup-path-audit depths=2,4,8 fractional=1 beam_gate=1 history_max=8 lifecycle_clear=1 steady_retention=0\n";
}
void composedOracleCases(){
 namespace resident=blah2::resident;namespace pipeline=blah2::pipeline;
 constexpr unsigned rows=17,columns=65,cell=8*columns+32;
 resident::Config config;config.geometry.doppler=rows;config.geometry.delays=columns;
 config.sampleRate=2400000;config.fusion.mode=resident::FusionMode::coherentRegularPolygon;config.fusion.beams=4;
 for(unsigned row=0;row<rows;++row)config.dopplerAxis.push_back(int(row)-8);
 for(unsigned column=0;column<columns;++column)config.delayAxis.push_back(column);
 const auto stateKey=[](const TrackerQualificationState& s){return std::tie(s.id,s.state,s.spatial,s.source,
  s.delay,s.doppler,s.acceleration,s.missedSeconds,s.inactive,s.measurements,s.updates,s.epoch,
  s.highWaterSequence,s.accelerationEstimated,s.everConfirmed,s.lineageKnown,s.confirmationVotes);};
 for(unsigned depth:{2u,4u,8u}){
  pipeline::ProductConfig p;p.integrationCpis=depth;p.integrationMotion=depth==2?1:track_before_detect::encodeMotionBins(1,0);
  p.minimumDoppler=0;p.centroidDelay=0;p.centroidDoppler=0;p.trackerM=1;p.trackerN=8;p.trackerDelete=8;
  p.maximumAcceleration=0;p.minimumSnr=0;p.beamCount=4;
  const pipeline::RadarPlan plan(p);
  pipeline::ProductStages independent(plan),sameInput(plan),residentInput(plan),corruptInput(plan);
  for(unsigned frame=0;frame<depth;++frame){
   std::vector<std::complex<double>> cpu(rows*columns,{1,2}),gpu=cpu,bad;
   if(frame+2==depth){cpu[cell-1]={100*(1+1e-7),2};gpu[cell-1]={100*(1-1e-7),2};
    cpu[cell+1]=gpu[cell+1]={100,2};}
   // Keep deep-bank qualification observable when the current CPI is weak;
   // a strong current hit now intentionally takes precedence over its paths.
   if(frame+1==depth)cpu[cell]=gpu[cell]={depth>2?1.0:100.0,2};
   bad=gpu;if(frame+1==depth){bad[cell]={1,2};bad[cell+5*columns+20]={100,2};}
   const pipeline::Position position{1,frame*100u,(frame+1)*100u,1000+frame*500u,true};
   const auto raw=[&](const auto& input){
    Map<std::complex<double>> map(rows,columns);std::vector<uint32_t> beams;
    resident::copyProductOracleInput(config,{input.data(),input.size()},map,beams);map.set_metrics();
    return map.to_json_km(position.timestampMs,config.sampleRate);
   };
   // Independent frontend acceptance happens before either temporal owner.
   // It must still catch corruption even if downstream products agree with it.
   compareOracleJson(raw(cpu),raw(gpu),"independent raw fused map");
   if(frame+1==depth)rejects([&]{compareOracleJson(raw(cpu),raw(bad),"corrupt raw fused map");});
   const auto run=[&](pipeline::ProductStages& stages,const auto& input){
    auto map=std::make_unique<Map<std::complex<double>>>(rows,columns);std::vector<uint32_t> beams;
    resident::copyProductOracleInput(config,{input.data(),input.size()},*map,beams);map->set_metrics();
    check(beams[cell]==3&&map->data[8][32].imag()==0,"Oracle confused beam annotation with complex power");
    auto products=stages.process(map,position,true,{},&beams,[](uint32_t,uint32_t){return 3u;});
    return std::make_pair(std::move(map),std::move(products));
   };
   auto expected=run(independent,cpu),actual=run(residentInput,gpu),oracle=run(sameInput,gpu),corrupted=run(corruptInput,bad);
   const auto stamp=position.timestampMs;
   compareOracleJson(actual.first->to_json_km(stamp,config.sampleRate),oracle.first->to_json_km(stamp,config.sampleRate),"same-input map");
   compareOracleJson(actual.second.detections->to_json_km(stamp,config.sampleRate),oracle.second.detections->to_json_km(stamp,config.sampleRate),"same-input lineage");
   compareOracleJson(actual.second.tracks->to_json(stamp),oracle.second.tracks->to_json(stamp),"same-input tracks");
   const auto a=residentInput.tracker.qualificationState(),b=sameInput.tracker.qualificationState();
   check(a.size()==b.size(),"Same-input hidden tracker population differs");
   for(size_t i=0;i<a.size();++i)check(stateKey(a[i])==stateKey(b[i]),"Same-input hidden tracker state differs");
   if(frame+1==depth){
    const auto& left=expected.second.integration->evidence->banks[depth==8?3:depth==4?2:1];
    const auto& right=actual.second.integration->evidence->banks[depth==8?3:depth==4?2:1];
    check(left.motion[cell].delay==1&&right.motion[cell].delay==-1,"Perturbation fixture did not reverse exact argmax");
    // Existing independent numerical acceptance remains; it is not an exact
    // argmax equivalence relation across different, tolerated frontend inputs.
    compareOracleJson(expected.first->to_json_km(stamp,config.sampleRate),actual.first->to_json_km(stamp,config.sampleRate),"independent map");
    rejects([&]{compareOracleJson(expected.second.detections->to_json_km(stamp,config.sampleRate),
     actual.second.detections->to_json_km(stamp,config.sampleRate),"different-input exact lineage");});
    rejects([&]{compareOracleJson(expected.first->to_json_km(stamp,config.sampleRate),
     corrupted.first->to_json_km(stamp,config.sampleRate),"corrupt independent map");});
    check(!a.empty(),"Same-input hidden tracker check was vacuous");
    const auto badState=corruptInput.tracker.qualificationState();
    bool different=a.size()!=badState.size();
    for(size_t i=0;!different&&i<a.size();++i)different=stateKey(a[i])!=stateKey(badState[i]);
    check(different,"Meaningful input corruption left hidden tracker unchanged");
   }
  }
 }
 for(unsigned depth:{1u,2u,4u,8u}){
  const unsigned count=resident::Qualification::startupCount(8,depth);
  const resident::Qualification qualification(count,depth);
  check(qualification.maximumFrames()==count+depth-1&&
   qualification.maximumFrames()<=resident::Qualification::maximumStartupFrames,
   "Startup progress frame count differs from qualification bound");
 }
 for(unsigned depth:{2u,4u,8u}){
  // A raw log-display rejection is not equivalent to an integrated-display
  // rejection: weak current cells have very different relative conditioning.
  // Preserve the original strict integrated contract, without widening it.
  blah2::detection::DetectorIntegration cpu(depth,0),gpu(depth,0),corrupt(depth,0);
  for(unsigned frame=0;frame<2;++frame){
   const auto make=[&](double amplitude){
    auto map=std::make_unique<Map<std::complex<double>>>(rows,columns);
    std::vector<std::complex<double>> input(rows*columns,{1,2});
    if(frame)input[cell]={amplitude,2};
    std::vector<uint32_t> beams;
    resident::copyProductOracleInput(config,{input.data(),input.size()},*map,beams);map->set_metrics();
    return map;
   };
   auto a=make(std::pow(10.,-11.51/10.)),b=make(std::pow(10.,-11.49/10.)),bad=make(1000);
   if(frame)rejects([&]{compareOracleJson(a->to_json_km(frame,config.sampleRate),
    b->to_json_km(frame,config.sampleRate),"raw boundary is not integrated boundary");});
   cpu.process(a,1,frame*100,(frame+1)*100,true);
   gpu.process(b,1,frame*100,(frame+1)*100,true);
   corrupt.process(bad,1,frame*100,(frame+1)*100,true);
   compareOracleJson(a->to_json_km(frame,config.sampleRate),b->to_json_km(frame,config.sampleRate),
    "unchanged independent integrated boundary");
   if(frame)rejects([&]{compareOracleJson(a->to_json_km(frame,config.sampleRate),
    bad->to_json_km(frame,config.sampleRate),"corrupt independent integrated boundary");});
  }
 }
 Map<std::complex<double>> output(rows,columns);std::vector<uint32_t> beams;
 std::vector<std::complex<double>> invalid(rows*columns,{1,4});
 rejects([&]{resident::copyProductOracleInput(config,{invalid.data(),invalid.size()},output,beams);});
 invalid[0]={std::numeric_limits<double>::quiet_NaN(),0};
 rejects([&]{resident::copyProductOracleInput(config,{invalid.data(),invalid.size()},output,beams);});
 std::cout<<"PASS stage-composed-oracle depths=2,4,8 perturbation_reverses_argmax=1 same_input_lineage_and_hidden_tracker=1 independent_integrated_corruption_rejected=1 raw_boundary_not_equivalent=1 startup_progress_bounded=1\n";
}
void hostTrackerHandoffCases(){
 namespace pipeline=blah2::pipeline;
 for(uint32_t depth:{2u,4u,8u}){
  pipeline::ProductConfig p;p.integrationCpis=depth;p.minimumSnr=-100;p.maximumAcceleration=0;
  p.trackerM=2;p.trackerN=16;p.trackerDelete=16;p.rangeResolution=50;
  p.ridgeBirth={true,3,2,.25};
  pipeline::ProductStages oracle{pipeline::RadarPlan(p)},handoff{pipeline::RadarPlan(p)};
  auto map=std::make_unique<Map<std::complex<double>>>(41,512);
  for(unsigned r=0;r<41;++r)map->doppler.push_back(100+r);
  for(unsigned c=0;c<512;++c)map->delay.push_back(c);
  bool ridgeRejected=false;
  for(uint32_t frame=0;frame<10;++frame){
   const uint64_t epoch=frame<5?UINT64_C(9007199254740993):UINT64_C(9007199254740995);
   const uint64_t newest=UINT64_C(9007199254741093)+frame;
   const uint8_t bank=depth==2?1:depth==4?3:4;
   Detection::Evidence evidence{epoch,newest,uint8_t(depth),bank,0,0,{}};
   std::vector<uint64_t> sequences(depth);
   for(uint32_t age=0;age<depth;++age){evidence.sequences[age]=newest-age;sequences[depth-1-age]=newest-age;}
   std::vector<blah2::resident::DetectionRecord> records{{32+20.0*frame,120,12,0},{42+20.0*frame,121,12,0}};
   std::vector<uint32_t> flags(2,4u|(1u<<(bank+2))|(2u<<17)|(16u<<20)),cells{0,1};
   auto decoded=hostDetections(view(records),view(flags),view(cells),epoch,view(sequences),depth);
   Detection expected({records[0].delay,records[1].delay},{120,121},{12,12});
   expected.set_evidence({evidence,evidence});expected.set_evidence_identity(epoch,newest);
   std::optional<blah2::detection::IntegrationMetrics> metadata(std::in_place);
   metadata->cpis=depth;metadata->ready=true;metadata->historyReset=frame==0||frame==5;
   const uint64_t timestamp=1000+frame*500;
   oracle.tracker.configureGrid(map->delay,map->doppler);
   auto expectedTracks=blah2::detection::trackProducts(expected,oracle.tracker,timestamp,metadata);
   pipeline::Products products;products.detections=std::make_unique<Detection>(std::move(decoded));products.integration=metadata;
   auto actual=handoff.track(map,{epoch,frame*100u,(frame+1)*100u,timestamp,true},std::move(products));
   check(actual.detections->to_json(timestamp)==expected.to_json(timestamp),"Host handoff changed exact evidence/records");
   check(actual.tracks->to_json(timestamp)==expectedTracks->to_json(timestamp),"Host detector handoff changed tracker outcomes");
   check(handoff.tracker.frame_diagnostics().birthRidgeRejected==oracle.tracker.frame_diagnostics().birthRidgeRejected,
    "Host detector handoff changed Doppler-ridge birth rejection");
   ridgeRejected=ridgeRejected||handoff.tracker.frame_diagnostics().birthRidgeRejected>0;
   if(frame==5)check(!handoff.tracker.frame_diagnostics().birthRidgeRejected,"Input reset retained a learned ridge");
  }
  check(ridgeRejected,"Host tracker handoff fixture never exercised ridge rejection");
 }
 std::cout<<"PASS host-tracker-handoff depths=2,4,8 fp64_records=1 exact_support=1 ridge_birth=1 reset=1\n";
}

void privateConfirmationCases(){
 namespace resident=blah2::resident;namespace pipeline=blah2::pipeline;
 constexpr unsigned rows=33,columns=65;
 // A fixed echo is a negative control with all-Doppler quarantine enabled.
 // The positive uses the production private-IQ trajectory, not relaxed gates.
 for(const auto& [amplitude,moving]:{std::pair{8.,false},std::pair{4.,true},std::pair{100.,false}}){
  pipeline::ProductConfig p;p.integrationCpis=8;
  p.integrationMotion=track_before_detect::encodeMotionBins(2,4,true);
  p.delayDriftPerHz=-2400000./509000000.*.5;p.pfa=1e-5/12;
  p.minimumDelay=5;p.minimumDoppler=15;p.trackerM=4;p.trackerN=25;p.trackerDelete=7;
  p.minimumSnr=5;p.rangeResolution=299792458./2400000.;p.wavelength=299792458./509000000.;
  p.recovery.stationaryClutter=true;p.recovery.persistenceCpis=12;p.recovery.maximumDopplerHz=0;
  pipeline::ProductStages stages{pipeline::RadarPlan(p)};
  resident::Qualification controls(resident::Qualification::startupCount(8,8,4,25,7),8);
  check(controls.maximumFrames()==40,"Private M4/depth8 confirmation deadline changed");
  blah2::startup::Target target{32,-16,2400000,amplitude,{}};
  if(moving)target.amplitude*=std::acos(-1.0)/2; // Same private moving-stimulus margin as Coordinator.
  blah2::startup::TargetMotion motion;
  bool rejected=false;uint64_t stationarySuppressed=0,rangeCombRejected=0;
  for(unsigned frame=0;frame<40;++frame){
   auto map=std::make_unique<Map<std::complex<double>>>(rows,columns);
   for(unsigned row=0;row<rows;++row){map->doppler.push_back((int(row)-16)*2);
    for(unsigned col=0;col<columns;++col)map->data[row][col]={1,0};}
   for(unsigned col=0;col<columns;++col)map->delay.push_back(col);
   if(frame>=8){
    if(moving)motion.advance(target,frame-8,map->doppler,p.cpiSeconds,p.delayDriftPerHz,p.recovery.persistenceCpis);
    const auto row=std::llround(target.dopplerHz/2)+16;
    check(row>=0&&row<rows&&target.delay<columns,"Private moving control left map geometry");
    map->data[size_t(row)][target.delay]={target.amplitude,0};
   }
   map->set_metrics();
   const auto products=stages.process(map,{1,frame*100u,(frame+1)*100u,1000+frame*500u,true});
   uint64_t confirmed=0;const auto states=stages.tracker.qualificationState();
   for(const auto& s:states)confirmed+=s.state==2||s.state==3;
   stationarySuppressed+=stages.tracker.frame_diagnostics().stationaryQuarantined+
    stages.tracker.frame_diagnostics().stationaryBirthRejected;
   rangeCombRejected+=stages.tracker.frame_diagnostics().birthRangeCombRejected;
   try{controls.control(frame>=8,frame>=8?frame>=15:frame>=7,
    products.detections->get_nDetections(),states.size(),confirmed,true,frame>=39);}
   catch(const std::logic_error& error){
    const bool expectedReason=std::string(error.what())==
     "Private qualification positive control produced no matching detection/track";
    if(frame!=39||moving||!expectedReason){std::ostringstream message;
     message<<"Private control failed: "<<error.what()<<" frame="<<frame+1<<" amplitude="<<target.amplitude
      <<" moving="<<moving<<" detections="<<products.detections->get_nDetections()
      <<" tracks="<<states.size()<<" confirmed="<<confirmed
      <<resident::controlDiagnostic(&stages.tracker,{},target.delay,target.dopplerHz,2);
     throw std::runtime_error(message.str());
    }
    rejected=true;
   }
  }
  if(moving){check(!rejected&&stationarySuppressed==0,"Moving positive control failed M4/depth8 confirmation");controls.requireControls();}
  else if(amplitude==8)check(rejected&&stationarySuppressed>0,
   "Fixed positive echo unexpectedly escaped stationary quarantine");
  else check(rejected&&rangeCombRejected>0,
   "Overstrong positive control unexpectedly evaded the range-comb control regression");
  const std::array<resident::DetectionRecord,3> records{{{32,-16,20},{32.25,-6,10},{40,-16,8}}};
  const auto diagnostic=resident::controlDiagnostic(&stages.tracker,{records.data(),records.size()},32,-16,2);
  check(diagnostic.size()<=4096&&diagnostic.find("range_comb_peers=1")!=std::string::npos&&
   diagnostic.find("maximum_votes=")!=std::string::npos&&
   diagnostic.find("\"birth_range_comb_rejected\"")!=std::string::npos,
   "Private failure audit lost bounded lineage/rejection evidence");
 }
 rejects([]{resident::controlDiagnostic(nullptr,{},0,0,0);});
 std::cout<<"PASS private-confirmation M=4 N=25 delete=7 depth=8 frames=40 moving_confirms=1 fixed_quarantined=1 overstrong_range_comb_rejects=1 fail_audit_bounded=1\n";
}
void oracleCases(){
 const auto compare=[](const std::string& left,const std::string& right){
  compareOracleJson(left,right,"fixture oracle");
 };
 compare("{\"data\":[[1.0,2.0]],\"timestamp\":9007199254740993}",
  "{\"timestamp\":9007199254740993,\"data\":[[1.01,2.0]]}");
 const auto mismatch=[&](const std::string& left,const std::string& right,
     const std::vector<std::string>& evidence){
  std::string error;try{compare(left,right);}catch(const std::invalid_argument& e){error=e.what();}
  check(!error.empty()&&error.size()<600,"Oracle accepted mismatch or emitted unbounded output");
  for(const auto& field:evidence)check(error.find(field)!=std::string::npos,
   "Oracle first-mismatch diagnostic lost path/type/value/tolerance");
 };
 mismatch("{\"data\":[[1.0,2.0,4.0]]}","{\"data\":[[1.0,2.02,5.0]]}",
  {"path=$.data[0][1]","expected_type=double","actual_type=double","expected=2 ","actual=2.02","tolerance="});
 mismatch("{\"timestamp\":9007199254740993}","{\"timestamp\":9007199254740994}",
  {"path=$.timestamp","reason=integer","expected=9007199254740993","actual=9007199254740994","tolerance=0"});
 mismatch("{\"data\":[[1.0]]}","{\"data\":[[1.0,2.0]]}",
  {"path=$.data[0]","reason=array-size","expected=size:1","actual=size:2"});
 mismatch("{\"data\":1}","{\"data\":\"1\"}",
  {"path=$.data","reason=type","expected_type=int64","actual_type=string"});
 mismatch("{\"data\":null}","{\"other\":null}",{"path=$.data","reason=missing-member"});
 mismatch("{\"data\":null}","{}",{"path=$","reason=member-count"});
 mismatch("{\"data\":\"left\"}","{\"data\":\"right\"}",{"path=$.data","reason=string"});
 mismatch("{\"data\":true}","{\"data\":false}",{"path=$.data","expected_type=bool"});
 rejects([&]{compare("{", "{}");});
 rapidjson::Value finiteValue(1.0),nonfinite(std::numeric_limits<double>::quiet_NaN());
 rejects([&]{compareOracleJson(finiteValue,nonfinite,"nonfinite oracle");});
 rapidjson::Document left,right;
 const char* lineage="{\"delay\":[1.0,2.0],\"doppler\":[3.0,4.0],\"snr\":[5.0,6.0],\"evidence\":[{\"bank\":2,\"delay_motion_bins\":1},{\"bank\":3,\"delay_motion_bins\":1,\"doppler_motion_bins\":-2,\"support_sequences_exact\":[\"9\",\"8\",\"7\",\"6\"]}]}";
 left.Parse(lineage);right.Parse(lineage);right["evidence"][1]["delay_motion_bins"].SetInt(0);
 std::vector<uint32_t> flags(9),cells{0,4};flags[4]=4u|(1u<<5)|(2u<<17)|(14u<<20)|(4u<<26);
 size_t index=SIZE_MAX;
 const auto detail=oracleLineageDiagnostic(left,right,view(flags),view(cells),3,3,8,index);
 check(index==1&&detail.size()<1600,"Lineage receipt selected wrong candidate or was unbounded");
 for(const char* field:{"lineage_index=1","gpu_cell=4","raw_flags=0x","decoded_bank=3",
   "decoded_delay_motion=0","decoded_doppler_motion=-2","gpu_source_cell=3",
   "cpu={delay=2,doppler=4,snr=6,","delay_motion_bins=1,","delay_motion_bins=0,","support_sequences_exact=[9,8,7,6]"})
  check(detail.find(field)!=std::string::npos,"Lineage receipt omitted bounded source evidence");
 rejects([&]{compareOracleJson(left,right,"lineage still fails");});
 check(oracleLineageDiagnostic(left,left,view(flags),view(cells),3,3,8,index).empty()&&index==SIZE_MAX,
  "Equal evidence manufactured a lineage diagnostic");
 std::cout<<"PASS startup-product-oracle first_path=1 exact_uint64=1 shape_types=1 bounded_diagnostic=1 unchanged_tolerance=1\n";
}
void spectrumCases(){
 for(const std::vector<std::complex<double>>&values:std::vector<std::vector<std::complex<double>>>{{},{{0,0},{1e-15,0},{1,0},{-3,4}},{{1e200,1e200},{std::numeric_limits<double>::max()/2,std::numeric_limits<double>::max()/2}}}){
  IqData cpu(uint32_t(values.size()));std::vector<double>frequency,db;for(size_t i=0;i<values.size();++i){frequency.push_back(101234.5+i*.125);db.push_back(20*std::log10(std::max(1e-15,std::abs(values[i]))));}
  cpu.update_frequency(frequency);cpu.update_spectrum(values);SpectrumView s{UINT64_C(4294967297),view(frequency),view(db)};same(spectrum(s),cpu.to_json(s.timestamp));
 }
 const std::vector<double>one{0};rejects([&]{spectrum({0,view(one),{nullptr,0}});});const std::vector<double>bad{std::numeric_limits<double>::infinity()};rejects([&]{spectrum({0,view(bad),view(one)});});
 std::cout<<"PASS serializer-spectrum legacy_zero_metadata=1 floor=-300 extreme_finite=1\n";
}
void mapCases(){
 constexpr uint32_t fs=2000000;
 for(bool zero:{true,false}){Map<std::complex<double>>cpu(3,3);cpu.delay={-1,0,1};cpu.doppler={-10,0,10};std::vector<double>km,doppler(cpu.doppler.begin(),cpu.doppler.end()),display;
  for(size_t r=0;r<3;++r){for(size_t c=0;c<3;++c){cpu.data[r][c]=zero?std::complex<double>{0,0}:std::complex<double>{double(r+c+1),double(int(r)-int(c))};}}
  cpu.set_metrics();
  for(int d:cpu.delay){km.push_back(1.0*d*(Constants::c/double(fs))/1000);}
  for(const auto&row:cpu.data)for(const auto z:row)display.push_back(10*std::log10(std::max(1e-30,std::abs(z)))-cpu.noisePower);
  MapView v{7,3,3,cpu.noisePower,cpu.maxPower,view(km),view(doppler),view(display)};same(map(v),cpu.to_json_km(7,fs));
  v.display.count-=1;rejects([&]{map(v);});
 }
 // Preserve actual legacy scientific-notation parser/rewrite behavior.
 Map<std::complex<double>>large(1,1);large.delay={1};large.doppler={1e19};large.data[0][0]={1,0};large.noisePower=1e19;large.maxPower=-1e19;const std::vector<double>km{Constants::c/double(fs)/1000},doppler{1e19},display{-1e19};same(map({UINT64_MAX,1,1,1e19,-1e19,view(km),view(doppler),view(display)}),large.to_json_km(UINT64_MAX,fs));
 rejects([&]{map({0,0,0,0,0,{},{},{}});});
 std::cout<<"PASS serializer-map zero_map_finite=1 legacy_km=1 scientific_roundtrip=1 rectangular_order=1\n";
}
void detectionCases(){
 constexpr uint32_t fs=2000000;
 for(const std::vector<double>&d:std::vector<std::vector<double>>{{},{-1,0,.5,2}}){std::vector<double>f(d.size()),s(d.size()),km;for(size_t i=0;i<d.size();++i){f[i]=double(i)*-.125;s[i]=3.14159+i;km.push_back(1.0*d[i]*(Constants::c/double(fs))/1000);}Detection cpu(d,f,s);same(detections({0,view(km),view(f),view(s)}),cpu.to_json_km(0,fs));}
 Detection big(1.125,1e19,-1e20);const std::vector<double>km{1.125*(Constants::c/double(fs))/1000},f{1e19},s{-1e20};same(detections({4294967297,view(km),view(f),view(s)}),big.to_json_km(4294967297,fs));
 rejects([&]{detections({0,view(km),{},view(s)});});const std::vector<double>nan{std::numeric_limits<double>::quiet_NaN()};rejects([&]{detections({0,view(nan),view(f),view(s)});});
 struct Record {double delay,doppler,snr,delayKm;};const std::vector<Record>records{{1.125,1e19,-1e20,km[0]}};same(detectionRecords(UINT64_C(4294967297),View<Record>{records.data(),records.size()}),big.to_json_km(UINT64_C(4294967297),fs));
 for(uint32_t depth:{2u,4u,8u}){
  const uint32_t bank=depth==2?1:depth==4?3:4;
  const int dc=depth==2?0:1,dr=depth==2?0:-2;
  std::vector<uint32_t>flags{7u|(1u<<(bank+2))|(uint32_t(dc+2)<<17)|(uint32_t(dr+16)<<20)|(depth>2?5u<<26:0u)},cells{0};
  std::vector<uint64_t>sequences(depth);for(size_t i=0;i<depth;++i)sequences[i]=UINT64_C(9007199254740993)+i;
  Detection expected=big;Detection::Evidence support;
  support.epoch=UINT64_C(9007199254741093);support.newestSequence=sequences.back();support.depth=uint8_t(depth);
  support.bank=uint8_t(bank);support.delayMotion=int8_t(dc);support.dopplerMotion=int8_t(dr);
  for(size_t age=0;age<depth;++age){support.sequences[age]=sequences[depth-1-age];}
  expected.set_evidence({support});
  expected.set_evidence_identity(support.epoch,support.newestSequence);
  const auto base=detectionRecords(17,View<Record>{records.data(),records.size()});
  same(temporalEvidence(base,view(flags),view(cells),support.epoch,view(sequences),depth),expected.to_json_km(17,fs));
  auto handoff=hostDetections(view(records),view(flags),view(cells),support.epoch,view(sequences),depth);
  check(handoff.get_delay()==expected.get_delay()&&handoff.get_doppler()==expected.get_doppler()&&
   handoff.get_snr()==expected.get_snr(),"Host detector handoff rounded FP64 records");
  same(handoff.to_json_km(17,fs),expected.to_json_km(17,fs));
  auto gap=sequences;--gap[0];
  rejects([&]{hostDetections(view(records),view(flags),view(cells),support.epoch,view(gap),depth);});
  Detection empty(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  const auto absent=empty.to_json_km(17,fs);
  empty.set_evidence_identity(support.epoch,support.newestSequence);
  same(temporalEvidence(absent,{}, {},support.epoch,view(sequences),depth),empty.to_json_km(17,fs));
  sequences.pop_back();rejects([&]{temporalEvidence(base,view(flags),view(cells),support.epoch,view(sequences),depth);});
 }
 for(uint32_t maximum:{4u,8u}){
  std::vector<uint32_t> flags{7u|128u|(130u<<17)|(5u<<26)},cells{0};
  const std::vector<uint64_t> sequences{UINT64_C(9007199254740993)};
  auto current=hostDetections(view(records),view(flags),view(cells),17,view(sequences),maximum);
  const auto& evidence=current.get_evidence().front();
  check(evidence.bank==0&&evidence.depth==1&&evidence.newestSequence==sequences[0]&&
   evidence.sequences[0]==sequences[0]&&evidence.sequences[1]==0&&
   !evidence.delayMotion&&!evidence.dopplerMotion,"Deep current-CPI host handoff lost exact support");
  flags[0]|=8u;rejects([&]{hostDetections(view(records),view(flags),view(cells),17,view(sequences),maximum);});
  flags[0]&=~8u;flags[0]^=1u<<17;
  rejects([&]{hostDetections(view(records),view(flags),view(cells),17,view(sequences),maximum);});
 }
 for(bool hit:{false,true}){
  Detection expected=hit?Detection(1,2,3):Detection(std::vector<double>{},std::vector<double>{},std::vector<double>{});
  const auto base=expected.to_json_km(17,fs);Detection::Evidence identity;
  identity.epoch=UINT64_C(9007199254740993);identity.newestSequence=UINT64_C(9007199254740995);
  identity.depth=1;identity.sequences[0]=identity.newestSequence;
  expected.set_evidence_identity(identity.epoch,identity.newestSequence);
  if(hit)expected.set_evidence({identity});
  same(singleCpiEvidence(base,identity.epoch,identity.newestSequence),expected.to_json_km(17,fs));
 }
 for(int dr=-1;dr<=1;++dr)for(int dc=-1;dc<=1;++dc){
  const uint32_t code=1+uint32_t(dc+1)+3*uint32_t(dr+1);
  std::vector<uint32_t>flags(25);flags[12]=7u|8u|(130u<<17)|(code<<26);
  const std::vector<uint32_t>keys{12};
  const auto sources=temporalSourceCells(view(flags),view(keys),5,5,8);
  check(sources==std::vector<uint32_t>{uint32_t(12+dr*5+dc)}&&keys[0]==12,
   "Temporal source offsets changed finalized key or selected the wrong cell");
  flags[12]&=~(15u<<26);rejects([&]{temporalSourceCells(view(flags),view(keys),5,5,8);});
 }
 struct Acknowledgement {uint32_t availableCpis=1;bool detectorReady=true;
  uint64_t temporalEpoch=7,newestSequence=11;std::array<uint64_t,8>supportSequences{11};};
 blah2::resident::Config temporalConfig;
 temporalConfig.geometry.clutterSamples=250001;temporalConfig.reference.centreHz=101100000;
 temporalConfig.detector.integrationMotionBins=track_before_detect::encodeMotionBins(2,4,true);
 for(uint32_t depth:{2u,4u,8u}){
  temporalConfig.detector.integrationCpis=depth;
  Acknowledgement first;first.detectorReady=depth>2;
  const auto firstFrame=blah2::resident::temporalPublication(temporalConfig,first,17,100,200);
  check(firstFrame&&firstFrame->ready==first.detectorReady&&firstFrame->availableCpis==1&&
   firstFrame->historyReset&&firstFrame->window.depth==1&&firstFrame->window.sequences[0]==11,
   "Resident first-frame readiness disagreed with the configured temporal banks");
  auto contradictory=first;contradictory.detectorReady=!contradictory.detectorReady;
  rejects([&]{blah2::resident::temporalPublication(temporalConfig,contradictory,17,100,200);});
  auto empty=first;empty.availableCpis=0;
  rejects([&]{blah2::resident::temporalPublication(temporalConfig,empty,17,100,200);});
  auto excessive=first;excessive.availableCpis=depth+1;
  rejects([&]{blah2::resident::temporalPublication(temporalConfig,excessive,17,100,200);});
  Acknowledgement second;second.availableCpis=2;second.detectorReady=true;
  second.newestSequence=12;second.supportSequences={12,11};
  const auto baseline=blah2::resident::temporalPublication(temporalConfig,second,17,200,300);
  check(baseline&&baseline->ready&&baseline->availableCpis==2&&baseline->window.depth==2&&
   baseline->window.sequences[0]==12&&baseline->window.sequences[1]==11,
   "Resident two-CPI baseline readiness or lineage was lost");
 }
 temporalConfig.detector.integrationCpis=8;
 Acknowledgement acknowledgement;
 auto warm=blah2::resident::temporalPublication(temporalConfig,acknowledgement,17,100,200);
 check(warm&&warm->ready&&warm->availableCpis==1&&warm->historyReset&&warm->window.depth==1,
  "Resident current-CPI startup was hidden before the two-CPI baseline matured");
 check(warm->delayDriftPerHz==-250001.0/101100000,
  "Noncoherent temporal publication used fusion/capture frequency instead of lane carrier");
 acknowledgement.availableCpis=2;acknowledgement.detectorReady=true;acknowledgement.newestSequence=12;acknowledgement.supportSequences={12,11};
 auto ready=blah2::resident::temporalPublication(temporalConfig,acknowledgement,17,200,300);
 check(ready&&ready->ready&&ready->availableCpis==2&&ready->firstInputSample==100,"Mature two-CPI baseline hidden until maximum bank depth");
 check(std::string(ready->backend)=="vulkan-resident","Resident temporal processing was mislabeled CPU");
 acknowledgement.availableCpis=3;acknowledgement.newestSequence=13;acknowledgement.supportSequences={13,12,11};
 auto partial=blah2::resident::temporalPublication(temporalConfig,acknowledgement,17,300,400);
 check(partial&&partial->window.depth==2&&partial->window.sequences[2]==0&&partial->availableCpis==3,
  "Resident intermediate warmup invented a three-CPI detector bank");
 rapidjson::Document partialJson;
 partialJson.Parse(blah2::detection::annotateIntegration("{}",partial).c_str());
 const auto& readiness=partialJson["detector_integration"]["bank_ready"];
 check(readiness.Size()==4&&readiness[0].GetBool()&&readiness[1].GetBool()&&!readiness[2].GetBool()&&!readiness[3].GetBool(),
  "Resident per-bank readiness used configured depth instead of acknowledged history");
 acknowledgement={};acknowledgement.detectorReady=true;acknowledgement.temporalEpoch=8;
 acknowledgement.newestSequence=13;acknowledgement.supportSequences={13};
 auto reset=blah2::resident::temporalPublication(temporalConfig,acknowledgement,17,300,400);
 check(reset&&reset->ready&&reset->window.depth==1&&reset->window.epoch==8&&
  reset->window.sequences[1]==0,"Resident reset retained pre-gap temporal support");
 rapidjson::Document ordinaryDetections;ordinaryDetections.Parse(detectionRecords(100000,
  View<Record>{records.data(),records.size()}).c_str());
 check(!ordinaryDetections.HasParseError()&&!ordinaryDetections.HasMember("track_guided_shadow"),
  "Production detections exported retired shadow evidence");
 const std::vector<Record>evidenceRecords{{1,20,9,.15}};const std::vector<uint32_t>cells{3};
 // Channel maps use the production delay-major, FFT-shifted layout. Cell 3 is
 // row 1, column 1 in a 3x2 row-major detector grid, hence shifted row 0.
 std::vector<std::complex<float>>channelMaps(12);channelMaps[3]={.12345679f,1e-20f};channelMaps[9]={3,4};
 const std::vector<std::complex<double>>weights{{.12345678901234567,1e-20},{-.5,.75}};
 const std::vector<uint32_t>mapping{0,1};
 // Exact +half-bin delay interpolation deduplicates to column1 while the
 // winning bank/source remains column0. Complex evidence must sample source0.
 const std::array<uint32_t,6>halfFlags{{0,0,0,7u|8u|(130u<<17)|(4u<<26),0,0}};
 const auto halfSources=temporalSourceCells({halfFlags.data(),halfFlags.size()},view(cells),3,2,4);
 const uint32_t missingSource=uint32_t(halfFlags.size());
 rejects([&]{temporalSourceCells({halfFlags.data(),halfFlags.size()},{&missingSource,1},3,2,4);});
 auto halfMaps=channelMaps;halfMaps[0]={7,8};halfMaps[6]={9,10};
 const std::vector<Record>halfRecords{{.5,20,9,.075}};
 const std::vector<double>halfGains{14,14};
 rapidjson::Document halfJson;halfJson.Parse(manifoldEvidence(5,539e6,2.4e6,3,2,2,
  View<Record>{halfRecords.data(),halfRecords.size()},view(halfSources),view(halfMaps),view(weights),
  20,2,.8,6.1,1.2,1,1,60,0,1,.01,view(mapping),view(halfGains)).c_str());
 check(!halfJson.HasParseError()&&halfSources[0]==2&&cells[0]==3&&
  halfJson["data"][0]["response"][0][0].GetDouble()==7&&
  halfJson["data"][0]["response"][1][1].GetDouble()==10,
  "Half-bin manifold JSON sampled finalized key instead of originating CFAR cell");
 // Model the real metadata path: tuner gains originate as floats and are then
 // widened to doubles before the evidence serializer sees them.
 const std::vector<double>actualGains{double(float(14.2f)),double(float(14.1f))};
 rapidjson::Document evidence;evidence.Parse<rapidjson::kParseFullPrecisionFlag>(blah2::annotate_event_time(manifoldEvidence(5,539e6,2.4e6,3,2,2,
  View<Record>{evidenceRecords.data(),evidenceRecords.size()},{cells.data(),cells.size()},
  {channelMaps.data(),channelMaps.size()},{weights.data(),weights.size()},20,2,.8,6.1,1.2,1,1,60,0,1,.01,
  {mapping.data(),mapping.size()},{actualGains.data(),actualGains.size()}),
  blah2::kraken_event_time_unavailable()).c_str());
 check(!evidence.HasParseError()&&std::string(evidence["schema"].GetString())=="vectorwarp-manifold-evidence-v2"&&
  std::string(evidence["precision_contract"].GetString())=="binary64-json-roundtrip-v1"&&
  evidence["data"].Size()==1&&
  evidence["data"][0]["response"][0][0].GetDouble()==double(channelMaps[3].real())&&
  evidence["data"][0]["response"][0][1].GetDouble()==double(channelMaps[3].imag())&&
  evidence["data"][0]["response"][1][1].GetDouble()==4&&evidence["reference_weights"].Size()==2&&
  evidence["reference_weights"][0][0].GetDouble()==weights[0].real()&&
  evidence["reference_weights"][0][1].GetDouble()==weights[0].imag()&&
  evidence["reference_state"]["updates"].GetUint64()==2&&
  evidence["reference_state"]["coherent_gain_db"].GetDouble()==6.1&&
  evidence["data"][0]["covariance"].Size()==2&&
  std::string(evidence["bearing_replay"]["reference_anchor"].GetString())=="fixed_site"&&
  evidence["bearing_replay"]["channel_mapping"].Size()==2,
  "Sparse manifold evidence serialization differs");
 check(evidence["actual_gain_db"].Size()==2&&
  evidence["actual_gain_db"][0].GetDouble()==actualGains[0]&&
  evidence["actual_gain_db"][1].GetDouble()==actualGains[1],
  "Sparse manifold evidence gain serialization is not lossless");
 // Exercise the complete contract that regressed: real evidence serialization
 // -> parsed corpus identity -> runtime loader -> exact applied-gain check.
 blah2::bearing_manifold::Identity manifoldIdentity{
  {0,1},{14,14},{{{0,0,0}},{{.1,0,0}}},{40,-80,300},{40.1,-79.9,400},
  539e6,2.4e6,80,90};
 std::vector<double>serializedGains{
  evidence["actual_gain_db"][0].GetDouble(),evidence["actual_gain_db"][1].GetDouble()};
 const auto fingerprint=blah2::bearing_manifold::compatibilityFingerprint(
  manifoldIdentity,serializedGains);
 rapidjson::StringBuffer artifactBuffer;rapidjson::Writer<rapidjson::StringBuffer> artifact(artifactBuffer);
 artifact.StartObject();
 artifact.Key("schema");artifact.String("vectorwarp-detection-manifold-runtime-v1");
 artifact.Key("frequency_hz");artifact.Double(539e6);artifact.Key("sample_rate_hz");artifact.Double(2.4e6);
 artifact.Key("channels");artifact.Uint(2);artifact.Key("beams");artifact.Uint(2);
 artifact.Key("runtime_compatible");artifact.Bool(true);artifact.Key("intended_use");
 artifact.String("offline-production-equivalent-replay-only");
 artifact.Key("channel_mapping");artifact.StartArray();artifact.Uint(0);artifact.Uint(1);artifact.EndArray();
 artifact.Key("direct_path_local_azimuth_deg");artifact.Double(90);artifact.Key("direct_path_beam");artifact.Uint(1);
 artifact.Key("manifold_identity");artifact.StartObject();
 artifact.Key("frequency_hz");artifact.Double(539e6);artifact.Key("sample_rate_hz");artifact.Double(2.4e6);
 artifact.Key("compatibility_fingerprint");artifact.String(fingerprint.c_str());
 artifact.Key("channel_mapping");artifact.StartArray();artifact.Uint(0);artifact.Uint(1);artifact.EndArray();
 artifact.Key("requested_gain_db");artifact.StartArray();artifact.Double(14);artifact.Double(14);artifact.EndArray();
 artifact.Key("actual_gain_db");evidence["actual_gain_db"].Accept(artifact);
 artifact.Key("positions_m");artifact.StartArray();
 artifact.StartArray();artifact.Double(0);artifact.Double(0);artifact.Double(0);artifact.EndArray();
 artifact.StartArray();artifact.Double(.1);artifact.Double(0);artifact.Double(0);artifact.EndArray();artifact.EndArray();
 artifact.Key("receiver_location");artifact.StartArray();artifact.Double(40);artifact.Double(-80);artifact.Double(300);artifact.EndArray();
 artifact.Key("transmitter_location");artifact.StartArray();artifact.Double(40.1);artifact.Double(-79.9);artifact.Double(400);artifact.EndArray();
 artifact.Key("x_axis_bearing_deg_true");artifact.Double(80);artifact.EndObject();
 artifact.Key("direct_anchor");artifact.StartObject();artifact.Key("local_azimuth_deg");artifact.Double(90);
 artifact.Key("beam");artifact.Uint(1);artifact.Key("response");artifact.StartArray();
 artifact.StartArray();artifact.Double(1);artifact.Double(0);artifact.EndArray();
 artifact.StartArray();artifact.Double(0);artifact.Double(1);artifact.EndArray();artifact.EndArray();artifact.EndObject();
 artifact.Key("responses");artifact.StartArray();
 artifact.StartArray();artifact.StartArray();artifact.Double(1);artifact.Double(0);artifact.EndArray();artifact.StartArray();artifact.Double(1);artifact.Double(0);artifact.EndArray();artifact.EndArray();
 artifact.StartArray();artifact.StartArray();artifact.Double(1);artifact.Double(0);artifact.EndArray();artifact.StartArray();artifact.Double(0);artifact.Double(1);artifact.EndArray();artifact.EndArray();artifact.EndArray();
 artifact.EndObject();
 const auto manifoldPath=std::filesystem::temp_directory_path()/"vectorwarp-gain-roundtrip-manifold.json";
 {std::ofstream out(manifoldPath);out.write(artifactBuffer.GetString(),std::streamsize(artifactBuffer.GetSize()));}
 const auto loaded=blah2::bearing_manifold::loadDetection(manifoldPath.string(),539e6,2,2,
  manifoldIdentity,blah2::bearing_manifold::DetectionUse::offlineReplay);
 blah2::bearing_manifold::validateActualGains(loaded,actualGains);
 std::error_code removeError;std::filesystem::remove(manifoldPath,removeError);
 // The initial analysis is update one while the completed-frame counter is
 // still zero. Those counters have different meanings and no ordering rule.
 auto initial=manifoldEvidence(5,539e6,2.4e6,3,2,2,
  View<Record>{evidenceRecords.data(),evidenceRecords.size()},{cells.data(),cells.size()},
  {channelMaps.data(),channelMaps.size()},{weights.data(),weights.size()},0,1,.8,6.1,1.2,1,1,60,0,1,.01,
  {mapping.data(),mapping.size()},{actualGains.data(),actualGains.size()});
 check(!initial.empty(),"Initial reference update rejected");
 Map<std::complex<double>>map0(3,2),map1(3,2);map0.delay={0,1};map1.delay=map0.delay;
 map0.doppler={-20,0,20};map1.doppler=map0.doppler;map0.data[2][1]={5,6};map1.data[2][1]={7,8};
 Detection cpuEvidence(1.2,18,9);std::vector<Map<std::complex<double>>*>cpuMaps{&map0,&map1};
 rapidjson::Document cpuDoc;cpuDoc.Parse(blah2::manifold_evidence::cpu(6,539e6,2e6,
  cpuEvidence,cpuMaps,weights,actualGains).c_str());
 check(!cpuDoc.HasParseError()&&cpuDoc["data"][0]["cell"].GetUint()==5&&
  std::string(cpuDoc["precision_contract"].GetString())=="binary64-json-roundtrip-v1"&&
  cpuDoc["data"][0]["response"][0][0].GetDouble()==5&&
  cpuDoc["data"][0]["response"][1][1].GetDouble()==8,
  "CPU sparse manifold evidence serialization differs");
 std::cout<<"PASS serializer-detection empty=1 order=1 legacy_km=1 scientific_roundtrip=1\n";
}
void trackCases(){
 Track cpu;cpu.add(Detection(0,0,1));for(int i=1;i<=500;++i){cpu.set_state(0,"ASSOCIATED");cpu.set_current(0,Detection(i,-i,1));}cpu.promote(0,250,255);
 std::vector<double>d,f;std::vector<uint32_t>state,measured(100,1);for(int i=401;i<=500;++i){d.push_back(i);f.push_back(-i);state.push_back(i==500?2u:1u);}PublishedTrack row{0,2,500,-500,0,501,{view(d),view(f),{state.data(),state.size()},{measured.data(),measured.size()}},true,true,1};same(tracks({UINT64_MAX,1,0,0,1,0,{&row,1}}),cpu.to_json(UINT64_MAX));
 rapidjson::Document doc;doc.Parse(tracks({0,1,0,0,1,0,{&row,1}}).c_str());const auto&data=doc["data"][0];check(data["n"].GetUint64()==501&&data["associated_delay"].Size()==100&&data["associated_doppler"].Size()==100&&data["associated_state"].Size()==100&&data["associated_delay"][0].GetDouble()==401&&data["associated_delay"][99].GetDouble()==500&&std::string(data["associated_state"][99].GetString())=="ACTIVE","Original100-history assertions");

 // The compact raw-bank layout retains exactly the 100 public trail points
 // when N is smaller; the lifetime association count remains independent.
 constexpr size_t historyCapacity=100,recordWords=20+8*historyCapacity;
 std::vector<uint32_t>bank(32+recordWords,0);auto pair=[&](size_t at,uint64_t v){bank[at]=uint32_t(v);bank[at+1]=uint32_t(v>>32);};auto real=[&](size_t at,double v){uint64_t bits;std::memcpy(&bits,&v,8);pair(at,bits);};
 const CheckpointBinding binding{1,9,UINT64_C(4294967299),44};bank[0]=5;bank[1]=recordWords*4;bank[2]=1;bank[3]=historyCapacity;pair(4,1);pair(6,500);bank[8]=1;pair(10,9);pair(12,binding.generation);pair(14,44);pair(20,1);pair(24,1);
 constexpr size_t base=32,timing=base+16+8*historyCapacity;
 real(base,500);real(base+2,-500);real(base+4,1);real(base+6,0);pair(base+8,501);bank[base+12]=0xc0000000u;bank[base+13]=1u|(100u<<8)|(1u<<16)|(100u<<24);bank[base+14]=2;bank[base+15]=0;
 real(timing,0);pair(timing+2,501);
 for(uint32_t value=401;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity);real(at,value);real(at+2,-double(value));real(at+4,1);bank[at+6]=(value==500?2u:1u)|4u|(1u<<4u);}
 auto raw=[&]{return View<uint32_t>{bank.data(),bank.size()};};
 // Retired checkpoint schemas are tombstones, not read-only compatibility
 // formats. Neither a legacy stride nor a schema5-shaped prefix admits them.
 auto spatial=binding;spatial.beamCount=12;
 for(uint32_t schema:{0u,1u,2u,3u,4u,6u,UINT32_MAX}){
  bank[0]=schema;
  for(size_t words:{recordWords,recordWords-4}){bank[1]=words*4;
   const View<uint32_t> retired{bank.data(),32+words};
   rejects([&]{trackerCheckpoint(0,retired,binding);});
   rejects([&]{bearingCheckpoint(0,retired,spatial,{true,12,4,2,80,30});});}
 }
 bank[0]=5;bank[1]=recordWords*4;
 auto provenance=[&](const std::string&json,bool measured,const char*source,size_t history=100){rapidjson::Document value;value.Parse(json.c_str());
  check(!value.HasParseError()&&value["data"].Size()==1&&value["data"][0]["measured"].GetBool()==measured&&
   std::string(value["data"][0]["source"].GetString())==source&&value["data"][0]["associated_measured"].Size()==history,
   "Schema5 measurement provenance differs");};
 const auto ordinaryJson=trackerCheckpoint(UINT64_MAX,raw(),binding);
 {rapidjson::Document missing;missing.Parse(ordinaryJson.c_str());
  check(missing["data"][0]["measurement_evidence"].IsNull(),"Absent measurement support must remain unavailable");}
 provenance(ordinaryJson,true,"detector");
 same(ordinaryJson,cpu.to_json(UINT64_MAX));
 {const auto source=Track::Source::prediction;
   Track predicted=cpu;predicted.set_predicted_current(0,Detection(501,-501,1),source);
   predicted.set_state(0,"COASTING");auto predictedRow=row;predictedRow.state=3;
   predictedRow.delay=501;predictedRow.doppler=-501;predictedRow.measured=false;
   predictedRow.source=uint32_t(source);
   predictedRow.history={{d.data()+1,99},{f.data()+1,99},{state.data()+1,99},{measured.data()+1,99}};
   same(tracks({UINT64_MAX,1,0,0,0,1,{&predictedRow,1}}),predicted.to_json(UINT64_MAX));
 }
 const size_t newestState=base+16+8*(500%historyCapacity)+6;
 const uint32_t measuredState=bank[newestState];bank[newestState]=8;
 rejects([&]{trackerCheckpoint(0,raw(),binding);});bank[newestState]=7;
 rejects([&]{trackerCheckpoint(0,raw(),binding);});bank[newestState]=4u|(1u<<4u);
 trackerCheckpoint(0,raw(),binding);bank[newestState]=measuredState;
 bank[base+16+8*(500%historyCapacity)+7]=12;provenance(trackerCheckpoint(UINT64_MAX,raw(),spatial),true,"detector");bank[base+16+8*(500%historyCapacity)+7]=13;rejects([&]{trackerCheckpoint(0,raw(),spatial);});bank[base+16+8*(500%historyCapacity)+7]=0;
 const uint32_t packedSpatial=12u|(48u<<9u)|(4u<<20u);
 bank[base+16+8*(500%historyCapacity)+7]=packedSpatial;
 rapidjson::Document bearing;bearing.Parse(bearingCheckpoint(7,raw(),spatial,{true,12,4,2,80,30}).c_str());
 check(!bearing.HasParseError()&&bearing["data"].Size()==1&&
  bearing["data"][0]["current_bearing_deg_true"].GetDouble()==87.5&&
  bearing["aperture_uncertainty_floor_deg"].GetDouble()==30&&
  bearing["scan_step_deg"].GetDouble()==7.5&&
  bearing["physical_resolution_limited"].GetBool()&&
  bearing["data"][0]["uncertainty_deg"].GetDouble()==30&&
  bearing["data"][0]["peak_separation_db"].GetUint()==4&&
  bearing["data"][0]["current_peak_separation_db"].GetUint()==4,
  "Fine bearing publication differs");
 // Adjacent fine winners belong to one aperture-limited lobe. Confidence is
 // the temporal median, not whichever quality happened to arrive last.
 bank[base+16+8*(498%historyCapacity)+7]=12u|(48u<<9u)|(2u<<20u);
 bank[base+16+8*(499%historyCapacity)+7]=12u|(1u<<9u)|(4u<<20u);
 bank[base+16+8*(500%historyCapacity)+7]=12u|(2u<<9u)|(6u<<20u);
 rapidjson::Document temporal;temporal.Parse(bearingCheckpoint(8,raw(),spatial,{true,12,4,2,80,30}).c_str());
 check(!temporal.HasParseError()&&temporal["data"][0]["consistent_samples"].GetUint()==3&&
  temporal["data"][0]["peak_separation_db"].GetUint()==4&&
  temporal["data"][0]["current_peak_separation_db"].GetUint()==6&&
  temporal["data"][0]["resolved"].GetBool()&&temporal["data"][0]["candidates"].Size()==1,
  "Temporal independent-lobe bearing confidence differs");
 for(uint32_t value=498;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity);
  real(at,250);real(at+2,value==499?1.0:0.0);}
 rapidjson::Document stationary;stationary.Parse(bearingCheckpoint(8,raw(),spatial,
  {true,12,4,2,80,30,3,0,0,.5,0,3,15}).c_str());
 check(!stationary.HasParseError()&&stationary["data"][0]["stationary_clutter"].GetBool()&&
  !stationary["data"][0]["publishable"].GetBool()&&
  !stationary["data"][0]["resolved"].GetBool(),
  "Stationary bearing classification differs");
 rapidjson::Document fullSpanStationary;fullSpanStationary.Parse(bearingCheckpoint(8,raw(),spatial,
  {true,12,4,2,80,30,3,0,0,.5,0,3,0}).c_str());
 check(!fullSpanStationary.HasParseError()&&
  fullSpanStationary["data"][0]["stationary_clutter"].GetBool(),
  "Full-span stationary bearing classification differs");
 real(base+16+8*(499%historyCapacity)+2,4.0);
 rapidjson::Document crossing;crossing.Parse(bearingCheckpoint(8,raw(),spatial,
  {true,12,4,2,80,30,3,0,0,.5,0,3,0}).c_str());
 check(!crossing.HasParseError()&&!crossing["data"][0]["stationary_clutter"].GetBool(),
  "Transient Doppler crossing was classified as stationary");
 for(uint32_t value=498;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity);
  real(at,value);real(at+2,-double(value));}
 rapidjson::Document strict;strict.Parse(bearingCheckpoint(9,raw(),spatial,{true,12,4,2,80,30,5}).c_str());
 check(!strict.HasParseError()&&strict["min_peak_separation_db"].GetUint()==5&&
  !strict["data"][0]["resolved"].GetBool(),"Bearing confidence threshold differs");
 auto multi=spatial;multi.scanBeamCount=60;auto packed=[](uint32_t a,uint32_t b,uint32_t c,uint32_t rb,uint32_t rc){return 0x80000000u|a|(b<<7u)|(c<<14u)|(rb<<21u)|(rc<<26u);};
 bank[base+16+8*(500%historyCapacity)+7]=packed(10,0,0,0,0);
 provenance(trackerCheckpoint(UINT64_MAX,raw(),multi),true,"detector");
 rapidjson::Document primaryOnly;primaryOnly.Parse(bearingCheckpoint(10,raw(),multi,{true,12,5,2,80,30,1,1,60}).c_str());
 check(!primaryOnly.HasParseError()&&primaryOnly["data"].Size()==1,
  "Primary-only multi-lobe bearing evidence was rejected");
 bank[base+16+8*(500%historyCapacity)+7]=packed(10,30,0,25,0);
 provenance(trackerCheckpoint(UINT64_MAX,raw(),multi),true,"detector");
 rejects([&]{bank[base+16+8*(500%historyCapacity)+7]=packed(0,30,50,25,10);trackerCheckpoint(0,raw(),multi);});
 rejects([&]{bank[base+16+8*(500%historyCapacity)+7]=packed(10,61,0,25,0);bearingCheckpoint(0,raw(),multi,{true,12,5,2,80,30,1,1,60});});
 bank[base+16+8*(498%historyCapacity)+7]=packed(10,30,50,25,10);
 bank[base+16+8*(499%historyCapacity)+7]=packed(11,30,50,25,10);
 bank[base+16+8*(500%historyCapacity)+7]=packed(10,30,50,25,10);
 rapidjson::Document adaptive;adaptive.Parse(bearingCheckpoint(10,raw(),multi,{true,12,5,2,80,30,3,1,60}).c_str());
 check(!adaptive.HasParseError()&&std::string(adaptive["method"].GetString())=="covariance_mvdr"&&
  adaptive["fine_beams"].GetUint()==60&&adaptive["scan_step_deg"].GetDouble()==6&&
  adaptive["physical_resolution_limited"].GetBool()&&adaptive["data"][0]["consistent_samples"].GetUint()==3&&
  adaptive["data"][0]["candidates"].Size()==3&&!adaptive["data"][0]["resolved"].GetBool(),
  "Adaptive multi-lobe bearing publication differs");
 bank[base+16+8*(498%historyCapacity)+7]=packed(10,11,50,25,10);
 bank[base+16+8*(499%historyCapacity)+7]=packed(30,12,50,25,10);
 bank[base+16+8*(500%historyCapacity)+7]=packed(50,13,30,25,10);
 rapidjson::Document moving;moving.Parse(bearingCheckpoint(11,raw(),multi,{true,12,5,2,80,30,3,1,60,.5,20}).c_str());
 check(!moving.HasParseError()&&moving["data"][0]["hypothesis_tracking"].GetBool()&&
  moving["data"][0]["current_bearing_deg_true"].GetDouble()==8&&
  std::abs(moving["data"][0]["bearing_rate_deg_s"].GetDouble()+12)<.001,
  "Motion-constrained multi-hypothesis bearing path differs");
 // All three measured frames have valid lobes, but no permitted rate path.
 bank[base+16+8*(498%historyCapacity)+7]=packed(1,0,0,0,0);
 bank[base+16+8*(499%historyCapacity)+7]=packed(3,0,0,0,0);
 bank[base+16+8*(500%historyCapacity)+7]=packed(1,0,0,0,0);
 rapidjson::Document impossibleRate;impossibleRate.Parse(
  bearingCheckpoint(11,raw(),spatial,{true,12,4,2,80,30,3,1,48,.5,1}).c_str());
 check(!impossibleRate.HasParseError()&&impossibleRate["data"].Size()==1&&
  !impossibleRate["data"][0]["hypothesis_tracking"].GetBool()&&
  !impossibleRate["data"][0]["resolved"].GetBool(),
  "Impossible bearing-rate fallback claimed a resolved constrained path");
 bank[base+16+8*(498%historyCapacity)+7]=0;bank[base+16+8*(499%historyCapacity)+7]=0;
 bank[base+16+8*(500%historyCapacity)+7]=packedSpatial;
 provenance(trackerCheckpoint(UINT64_MAX,raw(),spatial),true,"detector");
 bank[base+16+8*(500%historyCapacity)+7]=0;
 rejects([&]{auto wrong=binding;++wrong.generation;trackerCheckpoint(0,raw(),wrong);});rejects([&]{auto v=raw();--v.count;trackerCheckpoint(0,v,binding);});
 const size_t newest=base+16+8*(500%historyCapacity),priorState=bank[newest+6];
 std::vector<uint32_t> priorStates;priorStates.reserve(100);
 bank[27]=1;bank[26]=500;pair(base+8,601);real(timing,50);
 for(uint32_t value=401;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity)+6;
  priorStates.push_back(bank[at]);bank[at]=(bank[at]&3u)|8u|(2u<<5u);}
 rejects([&]{trackerCheckpoint(UINT64_MAX,raw(),spatial);});
 rejects([&]{bearingCheckpoint(12,raw(),spatial,{true,12,4,2,80,30,3});});
 for(uint32_t value=401;value<=500;++value)bank[base+16+8*(value%historyCapacity)+6]=priorStates[value-401];
 bank[27]=0;bank[26]=0;pair(base+8,501);real(timing,0);
 bank[newest+6]=(priorState&3u)|12u;rejects([&]{trackerCheckpoint(0,raw(),binding);});
 bank[newest+6]=3u|8u;rejects([&]{trackerCheckpoint(0,raw(),binding);});
 bank[newest+6]=priorState;
 bank[base+15]=1;rejects([&]{trackerCheckpoint(0,raw(),binding);});bank[base+15]=0;bank[base+13]^=1u<<16;rejects([&]{trackerCheckpoint(0,raw(),binding);});bank[base+13]^=1u<<16;
 pair(20,0);rejects([&]{trackerCheckpoint(0,raw(),binding);});pair(20,1);
 bank[base+12]=65534;rapidjson::Document wrapped;wrapped.Parse(trackerCheckpoint(0,raw(),binding).c_str());check(std::string(wrapped["data"][0]["id"].GetString())=="FFFE","GPU ID copy/hex differs");
 bank[base+12]=0x8000fffeu;wrapped.Parse(trackerCheckpoint(0,raw(),binding).c_str());check(!wrapped.HasParseError()&&std::string(wrapped["data"][0]["id"].GetString())=="FFFE","GPU acceleration latch leaked into public ID");
 bank[base+12]=0x00010000u;rejects([&]{trackerCheckpoint(0,raw(),binding);});bank[base+12]=0;
 // Schema5's disjoint policy has a distinct credit bit and support encoding. Neither
 // independent credit nor a guided prediction may masquerade as measurement.
 for(uint32_t value=401;value<=500;++value){const size_t at=base+16+8*(value%historyCapacity)+6;
  bank[at]=(bank[at]&15u)|((bank[at]>>4u)<<5u);}
 bank[27]=1;bank[26]=500;bank[base+12]=0xc0000000u;
 bank[newestState]=2u|8u|16u|(2u<<5u);
 rejects([&]{trackerCheckpoint(UINT64_MAX,raw(),binding);});
 bank[newestState]=2u|4u|(8u<<5u);
 provenance(trackerCheckpoint(UINT64_MAX,raw(),binding),true,"detector");
 bank[newestState]=3u;bank[base+14]=3;pair(20,0);pair(22,1);
 provenance(trackerCheckpoint(UINT64_MAX,raw(),binding),false,"prediction",99);
 bank[newestState]=3u|8u|16u|(2u<<5u);
 rejects([&]{trackerCheckpoint(UINT64_MAX,raw(),binding);});
 bank[newestState]=3u|16u;rejects([&]{trackerCheckpoint(0,raw(),binding);});
 bank[newestState]=2u|8u|(4u<<5u);rejects([&]{trackerCheckpoint(0,raw(),binding);});
 bank[newestState]=2u|4u|16u|(8u<<5u);bank[base+14]=2;pair(20,1);pair(22,0);
 provenance(trackerCheckpoint(UINT64_MAX,raw(),binding),true,"detector");
 same(trackerCheckpoint(UINT64_MAX,raw(),binding),ordinaryJson);
 // Record layout and history encoding are separate: schema5 supports both
 // ordinary and disjoint policy bits, with exact timing/counts in either case.
 check(historyEncoding(bank.data())==HistoryEncoding::disjointCredit&&
  validHistoryMetadata(2u|4u|16u|(4u<<5u),historyEncoding(bank.data())),
  "Schema5 disjoint history encoding differs from record schema");
 bank[27]=0;
 check(historyEncoding(bank.data())==HistoryEncoding::measurement&&
  validHistoryMetadata(2u|4u|(1u<<4u),historyEncoding(bank.data())),
  "Schema5 ordinary history encoding differs from record schema");
 bank[27]=1;
 same(trackerCheckpoint(UINT64_MAX,raw(),binding),cpu.to_json(UINT64_MAX));
 for(uint32_t depth:{1u,2u,4u,8u}){
  std::set<std::string> support;const auto saved=bank[newestState];
  bank[newestState]=2u|4u|(depth<<5u);
  for(uint64_t frame=0;frame<3;++frame){
   std::vector<uint64_t> sequences(depth);
   for(uint64_t age=0;age<depth;++age)sequences[age]=UINT64_C(9007199254740993)+frame+age;
   const MeasurementWindow window{UINT64_C(9007199254741093),binding.consumedSequence,view(sequences)};
   const auto json=trackerCheckpoint(99,raw(),binding,window);rapidjson::Document value;value.Parse(json.c_str());
   const auto& evidence=value["data"][0]["measurement_evidence"];
   check(evidence["cpis"].GetUint()==depth&&evidence["support_sequences_exact"].Size()==depth,
    "GPU measured support depth differs");
   check(std::string(evidence["newest_sequence_exact"].GetString())==std::to_string(sequences.back()),
    "GPU measured support loses integer precision");
   for(const auto& sequence:evidence["support_sequences_exact"].GetArray())support.insert(sequence.GetString());
   same(json,trackerCheckpoint(99,raw(),binding,window)); // Formatting a no-op cannot add support.
   auto wrong=window;++wrong.consumedSequence;rejects([&]{trackerCheckpoint(99,raw(),binding,wrong);});
   sequences.back()=0;rejects([&]{trackerCheckpoint(99,raw(),binding,window);});
  }
  check(support.size()==depth+2u,"Overlapping GPU support cannot be deduplicated");
  bank[newestState]=saved;
 }
 real(timing,-1);rejects([&]{trackerCheckpoint(0,raw(),binding);});real(timing,0);
 pair(timing+2,0);rejects([&]{trackerCheckpoint(0,raw(),binding);});pair(timing+2,501);
 bank[27]=2;rejects([&]{trackerCheckpoint(0,raw(),binding);});
 rejects([&]{bearingCheckpoint(0,raw(),spatial,{true,12,4,2,80,30});});bank[27]=1;
 Track aged=cpu;
 for(unsigned i=0;i<100;++i){aged.set_state(0,"COASTING");aged.set_predicted_current(0,Detection(500,-500,1));}
 bank[base+14]=3;pair(20,0);pair(22,1);pair(base+8,601);real(timing,50);
 for(uint32_t j=0;j<historyCapacity;++j)bank[base+16+8*j+6]=3;
 same(trackerCheckpoint(UINT64_MAX,raw(),binding),aged.to_json(UINT64_MAX));
 Track tentative;tentative.add(Detection(1,1,1));same(tracks({0,1,1,0,0,0,{}}),tentative.to_json(0));Track empty;same(tracks({0,0,0,0,0,0,{}}),empty.to_json(0));
 rejects([&]{tracks({0,1,0,1,0,0,{}});});auto bad=row;bad.state=0;rejects([&]{tracks({0,1,0,0,1,0,{&bad,1}});});bad=row;bad.history.state.count-=1;rejects([&]{tracks({0,1,0,0,1,0,{&bad,1}});});
 std::cout<<"PASS serializer-track gpu_selected_slice=100 schema5_only=1 legacy_refused=1 binding_rejects=1 hex_ids=1 uint64_counts=1 tentative_omitted=1\n";
}
}
int main()try{diagnosticCases();composedOracleCases();hostTrackerHandoffCases();privateConfirmationCases();oracleCases();spectrumCases();mapCases();detectionCases();trackCases();
 namespace trace=blah2::causal_trace;std::array<uint32_t,trace::words> t{};t[0]=1;t[1]=1;
 const uint64_t times[]={UINT64_C(9007199254740993),17,UINT64_C(1794521720793),8};
 std::memcpy(t.data()+4,times,sizeof(times));auto*r=t.data()+trace::headerWords;
 r[1]=8;r[5]=3|(2<<8);r[26]=65534;r[27]=2;double sample[]={1,-226.05,5.22};std::memcpy(r+6,sample,sizeof(sample));
 const std::string configHash(64,'a'),moduleHash(64,'b');
 auto encoded=trace::json(t.data(),t.size(),.125,UINT64_C(9007199254740993),64,{true,3,10,20},false,3,configHash,moduleHash);
 rapidjson::Document d;d.Parse(encoded.c_str());check(!d.HasParseError(),"causal JSON invalid");
 check(std::string(d["event_time_ns"].GetString())=="1794521720793000000"&&
  std::string(d["sequence"].GetString())=="9007199254740993"&&
  d["candidates"][0]["bearing_candidates_deg"].Empty()&&
  std::string(d["producer"]["config_fingerprint"].GetString())==configHash&&
  std::string(d["producer"]["build_id"].GetString())==moduleHash&&
  std::string(d["transport_dropped"].GetString())=="3"&&
  d["candidates"][0]["confirmation_credit"].GetBool()&&
  std::string(d["candidates"][0]["spawned_first_track_id"].GetString())=="t65534"&&
  d["candidates"][0]["spawned_track_count"].GetUint()==2,"causal exact time or unknown bearing corrupted");
 rejects([&]{trace::json(t.data(),t.size(),1,1,64,{},false,0,"invalid",moduleHash);});
 if(const char* path=std::getenv("CAUSAL_FIXTURE_PATH")){std::ofstream f(path);f<<encoded<<'\n';check(bool(f),"causal fixture write");}
 t[1]=100;t[2]=100;for(uint32_t i=0;i<64;++i){auto*q=t.data()+trace::headerWords+i*trace::recordWords;
  std::memcpy(q,r,trace::recordWords*4);q[0]=i;}
 d.Parse(trace::json(t.data(),t.size(),.125,1,7).c_str());
 check(d["candidates"].Size()==7&&d["coverage"]["truncated"].GetBool(),"causal prefix bound");
 t[8]=UINT32_MAX;t[9]=UINT32_MAX;rejects([&]{trace::json(t.data(),t.size(),1,1,64);});
 std::cout<<"COMPLETE GPU-result JSON CPU compatibility groups=8 exact_causal_time=1 bounded_prefix=1 candidate_dsp_math=0 gpu_execution=0\n";return 0;}catch(const std::exception&e){std::cerr<<"FAIL GPU-result JSON: "<<e.what()<<'\n';return 1;}
