#include "process/analysis/StartupIq.h"
#include "process/pipeline/RadarPipeline.h"
#include "process/ambiguity/ResidentFusionWeights.h"
#include "process/ambiguity/GpuResidentQualification.h"
#include "process/ambiguity/GpuResidentCoordinator.h"
#include "process/utility/FftwThreads.h"
#include <iostream>

namespace {
void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
template<class F>void rejects(F&& f){bool rejected=false;try{f();}catch(const std::invalid_argument&){rejected=true;}check(rejected,"Calibration accepted invalid input");}
void contracts(){
 using namespace blah2::startup;
 Target target{3,0,1000,1,{{0,0},{1,0}}};
 check(TargetCalibration::coherentPairs(target,100,10,0)==970,"CAF processing gain lost valid sample pairs");
 check(TargetCalibration::coherentPairs(target,100,20,0)==1940,"CAF gain did not scale with sample count");
 rejects([&]{TargetCalibration::coherentPairs(target,3,10,0);});
 Map<std::complex<double>> map(1,9);map.doppler={0};for(int i=0;i<9;++i){map.delay.push_back(i);map.data[0][i]={1,0};}
 const auto fused=[](const auto& values){return std::norm(values[0]);};
 const auto unitPower=TargetCalibration::unitEchoPower(target,100,10,0,{0},{{1,0}},{1},fused);
 check(TargetCalibration::unitEchoPower(target,100,10,0,{0},{{2,0}},{1},fused)==4*unitPower,
  "Private unit-echo power lost reference amplitude scaling");
 Target rotating=target;rotating.dopplerHz=7;
 std::complex<double> sum{};for(unsigned sample=0;sample<97;++sample)
  sum+=std::polar(1.,2*std::acos(-1.)*rotating.dopplerHz*sample/rotating.sampleRate);
 check(std::abs(TargetCalibration::coherentPairs(rotating,100,10,0)-10*std::abs(sum))<1e-10,
  "Private gain omitted intra-block Doppler attenuation");
 const auto observe=[&](TargetCalibration& c,const auto& m){c.observeNegative(target,m,1,2,unitPower,1);};
 TargetCalibration calibration;observe(calibration,map);const double expected=calibration.amplitude(1,4);
 // CUT and both guard cells are deliberately enormous: none calibrates noise.
 for(unsigned i=2;i<=4;++i)map.data[0][i]={1e12,0};
 TargetCalibration guarded;observe(guarded,map);check(guarded.amplitude(1,4)==expected,"CUT/guard contaminated private calibration");
 rejects([&]{guarded.amplitude(2,4);});rejects([&]{guarded.amplitude(1,0);});
 rejects([&]{guarded.amplitude(1,1e300);});
 for(unsigned i=1;i<8;++i)observe(guarded,map);
 rejects([&]{observe(guarded,map);});
 TargetCalibration invalid;
 rejects([&]{invalid.observeNegative(target,map,1,2,0,1);});
 for(auto& row:map.data)for(auto& cell:row)cell={0,0};
 rejects([&]{observe(invalid,map);});
 guarded.clear();check(guarded.updates()==0,"Calibration reset retained measurements");
 IqData a(32),b(32),c(32),d(32);Target weak=target,strong=target;weak.amplitude=.001;strong.amplitude=100;
 fillTargetIq({&a,&b},32,0,weak,false);fillTargetIq({&c,&d},32,0,strong,false);
 check(a.view_data()==c.view_data()&&b.view_data()==d.view_data(),"Negative reference changed with echo amplitude");
 Target moving{245,-16,2400000,1,{{0,0},{1,0}}};TargetMotion motion;
 std::vector<double> axis,history;for(int i=-32;i<=32;++i)axis.push_back(2.*i);
 double delay=moving.delay,phase=0,previous=moving.dopplerHz;constexpr double drift=-.589/125*.5;
 for(unsigned frame=0;frame<264;++frame){
  if(frame){delay+=previous*drift;phase=std::remainder(phase+previous*.5,1.);}
  motion.advance(moving,frame,axis,.5,drift,12);previous=moving.dopplerHz;
  check(moving.delay==uint32_t(std::llround(delay))&&moving.phaseCycles==phase,
   "Private motion delay/phase do not follow Doppler");
  check(moving.dopplerHz>=-24&&moving.dopplerHz<=-16,"Private motion left valid Doppler cells");
  history.push_back(moving.dopplerHz);
  if(history.size()>=12){const auto bounds=std::minmax_element(history.end()-12,history.end());
   check(*bounds.second-*bounds.first>3,"Private moving target became stationary over12 CPIs");}
 }
}
void run(unsigned samples,unsigned mode,bool moving=true){
 using namespace blah2;
 const unsigned fs=2*samples,paths=mode?5:1,inputs=mode?5:2;
 std::vector<std::unique_ptr<IqData>> storage;
 std::vector<IqData*> capture,surveillance;
 std::vector<uint32_t> referenceChannels,surveillanceChannels;
 for(unsigned i=0;i<inputs;++i){storage.push_back(std::make_unique<IqData>(samples));capture.push_back(storage.back().get());}
 for(unsigned i=0;i<paths;++i){const unsigned channel=mode?i:1;surveillanceChannels.push_back(channel);surveillance.push_back(capture[channel]);}
 if(mode)for(unsigned i=0;i<5;++i)referenceChannels.push_back(i);else referenceChannels={0};
 IqData reference(samples);ArrayReferenceSynthesizer synthesizer;
 WienerHopf::PreparedReference prepared(0,3,samples);
 std::vector<std::unique_ptr<WienerHopf>> clutter;
 std::vector<std::unique_ptr<Ambiguity>> ambiguity;
 for(unsigned i=0;i<paths;++i){clutter.push_back(std::make_unique<WienerHopf>(0,3,samples,WienerHopf::Workspace::SharedReference));
  ambiguity.push_back(std::make_unique<Ambiguity>(0,64,-32,32,fs,samples,true));}
 auto& geometry=*ambiguity.front();const double cpi=geometry.get_cpi();
 startup::Target target{32,geometry.result()->doppler[8],double(fs),1,std::vector<std::complex<double>>(inputs)};
 const double pi=std::acos(-1.),frequency=509e6,radius=.15,direct=.75,azimuth=direct+pi/2;
 CoherentBeamBank::Config b;b.frequencyHz=frequency;b.beams=12;b.directPathAzimuthRad=direct;
 for(unsigned i=0;i<paths;++i){const double angle=2*pi*i/paths;
  b.positionsM.push_back({radius*std::cos(angle),radius*std::sin(angle),0});
  target.response[surveillanceChannels[i]]=mode<2?(mode?std::polar(1.,angle):std::complex<double>{1,0}):
   std::polar(1.,-2*pi*frequency*radius/299792458.*(std::cos(angle)*(std::cos(azimuth)-std::cos(direct))+
    std::sin(angle)*(std::sin(azimuth)-std::sin(direct))));}
 std::unique_ptr<CoherentBeamBank> coherent;if(mode>=2)coherent=std::make_unique<CoherentBeamBank>(b);
 Noncoherent noncoherent;startup::TargetCalibration calibration;startup::TargetMotion motion;
 pipeline::ProductConfig p;p.integrationCpis=8;p.integrationMotion=track_before_detect::encodeMotionBins(2,4,true);
 p.delayDriftPerHz=-double(fs)/frequency*cpi;p.pfa=1e-5/12;p.minimumDelay=5;p.minimumDoppler=15;
 p.cpiSeconds=cpi;p.trackerM=4;p.trackerN=25;p.trackerDelete=7;p.minimumSnr=5;
 p.rangeResolution=299792458./fs;p.wavelength=299792458./frequency;
 p.recovery.stationaryClutter=true;p.recovery.persistenceCpis=12;p.recovery.maximumDopplerHz=0;
 pipeline::ProductStages products{pipeline::RadarPlan(p)};
 resident::Qualification admission(resident::Qualification::startupCount(8,8,4,25,7),8);
 const double gate=std::max(detection::integrated_cfar::alpha(p.pfa/2,12,2),std::pow(10.,p.minimumSnr/5));
 unsigned finalDetections=0,confirmed=0,maximumDetections=0;
 for(unsigned frame=0;frame<40;++frame){
  if(frame==8)target.amplitude=calibration.amplitude(8,gate);
  if(moving&&frame>=8){
   if(frame==8)target.amplitude*=pi/2;
   motion.advance(target,frame-8,geometry.result()->doppler,cpi,p.delayDriftPerHz,12);
  }
  startup::fillTargetIq(capture,samples,frame,target,frame>=8);
  if(mode)synthesizer.process_into(capture,reference);else reference.replace(std::deque<std::complex<double>>(capture[0]->view_data()));
  check(prepared.prepare(reference),"Private IQ regression reference preparation failed");
  std::vector<Map<std::complex<double>>*> maps;
  for(unsigned i=0;i<paths;++i){check(clutter[i]->process(prepared,surveillance[i]),"Private IQ regression clutter failed");
   maps.push_back(ambiguity[i]->process(&reference,surveillance[i]));}
  const auto& metrics=synthesizer.get_metrics();
  if(coherent){const auto scale=resident_fusion::whiteningScales(metrics.covariance.data(),metrics.directPathWeights.data(),
    paths,.001,mode==3&&!metrics.covariance.empty());coherent->calibrate(metrics.directPathWeights,mode==3?scale.data():nullptr);}
  std::unique_ptr<Map<std::complex<double>>> map;
  pipeline::fuseMaps(maps,noncoherent,coherent.get(),map);
  double unitPower=0;
  if(frame==7){unitPower=startup::TargetCalibration::unitEchoPower(target,geometry.get_n_corr(),geometry.get_n_doppler_bins(),0,
    referenceChannels,mode?metrics.weights:std::vector<std::complex<double>>{{1,0}},surveillanceChannels,
    [&](const auto& values){if(coherent){const auto powers=coherent->powerBank(values);return *std::max_element(powers.begin(),powers.end());}
     double power=0;for(auto value:values)power+=std::norm(value);return power/values.size();});}
  const auto result=products.process(map,{1,uint64_t(frame)*samples,uint64_t(frame+1)*samples,
    1000+uint64_t(frame)*500,true});
  if(frame==7){check(result.integration&&result.integration->evidence&&result.integration->evidence->banks[0].ready,
    "Calibration did not receive a mature independent negative bank");
   calibration.observeNegative(target,*result.integration->evidence->banks[0].map,2,6,unitPower,8);}
  finalDetections=result.detections->get_nDetections();maximumDetections=std::max(maximumDetections,finalDetections);
  confirmed=0;const auto states=products.tracker.qualificationState();for(const auto& s:states)confirmed+=s.state==2||s.state==3;
  if(frame<8)check(!finalDetections&&states.empty(),"Negative IQ control admitted detection/track");
  if(frame>=15){
   unsigned birthEligible=0;const auto snrs=result.detections->get_snr();
   const auto delays=result.detections->get_delay(),dopplers=result.detections->get_doppler();
   for(size_t i=0;i<snrs.size();++i)if(snrs[i]>=p.minimumSnr){++birthEligible;
    check(std::abs(delays[i]-target.delay)<1&&std::abs(dopplers[i]-target.dopplerHz)<2/cpi,
     "Private IQ produced an unrelated birth-eligible detection");}
   check(birthEligible==1&&states.size()==1,"Private IQ produced a missing target or false track");
  }
  try{admission.control(frame>=8,frame>=8?frame>=15:frame>=7,finalDetections,states.size(),confirmed,moving,frame>=39);}
  catch(...){std::cerr<<"mode="<<mode<<" samples="<<samples<<" frame="<<frame+1<<" amplitude="<<target.amplitude
   <<" gate="<<gate<<" detections="<<finalDetections<<" confirmed="<<confirmed
   <<resident::controlDiagnostic(&products.tracker,{},target.delay,target.dopplerHz,1/cpi)<<'\n';throw;}
 }
 std::cout<<"mode="<<mode<<" samples="<<samples<<" amplitude="<<target.amplitude<<" detections="<<finalDetections
  <<" max_detections="<<maximumDetections<<" confirmed="<<confirmed<<" moving="<<moving
  <<" quarantined="<<products.tracker.frame_diagnostics().stationaryQuarantined<<std::endl;
 check(finalDetections==1&&confirmed==unsigned(moving),"Calibrated IQ did not produce expected target state");
 if(!moving)check(products.tracker.frame_diagnostics().stationaryQuarantined==1,
  "Fixed positive echo escaped stationary quarantine");
 if(!moving){bool rejected=false;
  try{admission.control(true,true,finalDetections,1,0,true,true);}catch(const std::logic_error&){rejected=true;}
  check(rejected,"Exact stationary frame40 startup failure was not reproduced");}
 admission.requireControls();calibration.clear();check(!calibration.updates(),"Private calibration retained reset state");
 rejects([&]{calibration.amplitude(8,gate);});
}
}
int main(){try{
 check(fftw_init_threads(),"FFTW initialization failed");blah2::set_fftw_planner_threads(1);
 contracts();
 for(unsigned samples:{8000u,16000u})for(unsigned mode=0;mode<4;++mode)run(samples,mode);
 run(8000,0,false);
 std::cout<<"PASS private IQ calibration M4/N25/delete7/depth8 negative_clean=1 moving_confirmed=1 stationary_quarantined=1 false_tracks=0 modes=dedicated,array,coherent,whitened geometries=2\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
