#include "process/integration/DirectPathCoherentExperiment.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace integration = blah2::integration;

namespace {
std::vector<uint32_t> channels(const std::string& text) {
  std::vector<uint32_t> result; std::stringstream input(text); std::string item;
  while (std::getline(input,item,',')) {
    if (item.empty() || item.find_first_not_of("0123456789") != std::string::npos)
      throw std::invalid_argument("Invalid channel list");
    result.push_back(uint32_t(std::stoul(item)));
  }
  if (result.empty() || result.size()>8) throw std::invalid_argument("Invalid channel count");
  return result;
}
uint32_t number(const char* text,const char* label) {
  const std::string value=text;
  if(value.empty()||value.find_first_not_of("0123456789")!=std::string::npos)
    throw std::invalid_argument(std::string("Invalid ")+label);
  return uint32_t(std::stoul(value));
}
}

int main(int argc,char** argv)try {
  if(argc<12||std::string(argv[1])!="--channels"||std::string(argv[3])!="--anchor"||
    std::string(argv[5])!="--max-cpis"||std::string(argv[7])!="--delay-motion"||
    std::string(argv[9])!="--doppler-motion") {
    std::cerr<<"Usage: directPathCoherentReplayInspect --channels 0,1,... --anchor N "
      "--max-cpis 4|8 --delay-motion N --doppler-motion N FRAME.vwrps...\n";
    return 2;
  }
  integration::DirectPathCoherentExperiment::Config config;
  config.channelIds=channels(argv[2]); config.anchorChannelId=number(argv[4],"anchor");
  config.temporal.maximumCpis=number(argv[6],"maximum CPI count");
  config.temporal.delayMotionBins=number(argv[8],"delay motion");
  config.temporal.dopplerMotionBins=number(argv[10],"Doppler motion");
  config.temporal.minimumCoherence=.6; config.minimumPilotCoherence=.25;
  config.maximumCells=1u<<20;
  config.spatialCoefficients.assign(config.channelIds.size(),
    {1/std::sqrt(double(config.channelIds.size())),0});
  integration::coherent_replay::ExperimentReader reader(config);
  uint64_t ready=0,valid=0; double peak=0,coherence=0;
  for(int argument=11;argument<argc;++argument) {
    const auto result=reader.append(argv[argument]);
    std::cout<<"frame="<<(argument-10)<<" reason="<<result.reason;
    for(const auto& bank:result.banks) {
      uint64_t bankValid=0; double bankPeak=0,bankCoherence=0;
      if(bank.ready) {
        ++ready;
        bankValid=std::count(bank.valid.begin(),bank.valid.end(),uint8_t(1));
        for(size_t cell=0;cell<bank.valid.size();++cell)if(bank.valid[cell]) {
          const auto columns=bank.map->get_nCols();
          bankPeak=std::max(bankPeak,std::abs(bank.map->data[cell/columns][cell%columns]));
          bankCoherence=std::max(bankCoherence,bank.coherence[cell]);
        }
        valid+=bankValid;peak=std::max(peak,bankPeak);coherence=std::max(coherence,bankCoherence);
      }
      std::cout<<" depth"<<bank.depth<<"_ready="<<bank.ready
        <<" depth"<<bank.depth<<"_valid="<<bankValid
        <<" depth"<<bank.depth<<"_peak="<<bankPeak
        <<" depth"<<bank.depth<<"_max_coherence="<<bankCoherence;
    }
    std::cout<<" null_calibrated="<<result.nullCalibrated<<'\n';
  }
  std::cout<<"COMPLETE frames="<<(argc-11)<<" ready_banks="<<ready
    <<" valid_cells="<<valid<<" peak="<<peak<<" max_coherence="<<coherence
    <<" null_calibrated=0 detector_mutation=0 gpu_execution=0\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"FAIL coherent-replay-inspect: "<<error.what()<<'\n';return 1;}
