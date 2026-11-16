#include "process/analysis/ResidentReplayInspector.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace rr=blah2::resident_replay;
int main(int argc,char** argv){
  try{
    if(argc<10||std::string(argv[1])!="--source-sha256"||std::string(argv[3])!="--rf-sha256"||
      std::string(argv[5])!="--manifest"||std::string(argv[7])!="--warmup-frames"){
      std::cerr<<"Usage: residentReplayInspect --source-sha256 HEX --rf-sha256 HEX --manifest FILE --warmup-frames N FRAME.vwrps...\n"
        <<"Manifest: one whitespace-separated row per frame, ordered independently of input files:\n"
        <<"epoch logicalFrame generation consumedSequence signalTimestampMs trackerTransaction configHash moduleHash rawIqHash deviceHash\n"
        <<"Use independently retained expectations; deriving them from inspected snapshots defeats provenance checking.\n";
      return 2;
    }
    const std::string warmupText=argv[8];
    if(warmupText.empty()||warmupText.find_first_not_of("0123456789")!=std::string::npos)throw std::invalid_argument("Invalid warmup count");
    const auto warmup=std::stoull(warmupText);
    auto expected=rr::readInspectorManifest(argv[6]);
    if(size_t(argc-9)!=expected.size())throw std::invalid_argument("Input file count differs from expected manifest");
    if(warmup>=expected.size())throw std::invalid_argument("Warmup must leave at least one evaluation frame");
    rr::SequenceInspector inspector(rr::parseInspectorHash(argv[2]),rr::parseInspectorHash(argv[4]),expected,size_t(warmup));
    std::vector<rr::InspectedFrame> reports;
    for(int i=9;i<argc;++i){
      try{const auto data=rr::readInspectorSnapshot(argv[i]);reports.push_back(inspector.append(data.data(),data.size()));}
      catch(const std::exception& e){throw std::invalid_argument(std::string(argv[i])+": "+e.what());}
    }
    inspector.finish();
    uint64_t covarianceValid=0,detections=0,trackObservations=0;
    for(const auto& f:reports){
      covarianceValid+=f.covarianceValid;detections+=f.detections;trackObservations+=f.tracks;
      std::cout<<"frame="<<f.identity.logicalFrame<<" generation="<<f.identity.generation
        <<" consumed_sequence="<<f.identity.consumedSequence<<" signal_timestamp_ms="<<f.identity.signalTimestampMs
        <<" phase="<<(f.warmup?"warmup":"eval")<<" covariance_valid="<<f.covarianceValid
        <<" history_available="<<f.historyAvailable<<" history_ready="<<f.historyReady
        <<" detections="<<f.detections<<" tracks="<<f.tracks<<'\n';
    }
    std::cout<<"COMPLETE frames="<<reports.size()<<" warmup_frames="<<warmup<<" eval_frames="<<reports.size()-warmup
      <<" covariance_valid_frames="<<covarianceValid<<" covariance_invalid_frames="<<reports.size()-covarianceValid
      <<" detections="<<detections<<" track_observations="<<trackObservations
      <<" gpu_execution=0 device_state_restore=0\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL resident-replay-inspect: "<<e.what()<<'\n';return 1;}
}
