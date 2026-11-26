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
      "--max-cpis 2|4|8 --delay-motion N --doppler-motion N [--migration-rates 0,RATE,...] FRAME.vwrps...\n";
    return 2;
  }
  integration::DirectPathCoherentExperiment::Config config;
  config.channelIds=channels(argv[2]); config.anchorChannelId=number(argv[4],"anchor");
  config.temporal.maximumCpis=number(argv[6],"maximum CPI count");
  config.temporal.delayMotionBins=number(argv[8],"delay motion");
  config.temporal.dopplerMotionBins=number(argv[10],"Doppler motion");
  int firstFrame = 11;
  if (std::string(argv[firstFrame]) == "--migration-rates") {
    if (argc < 14) throw std::invalid_argument("Migration rates need a list and snapshot");
    std::stringstream input(argv[12]); std::string item;
    while (std::getline(input, item, ',')) {
      size_t used = 0; const double rate = std::stod(item, &used);
      if (used != item.size()) throw std::invalid_argument("Invalid migration rate");
      config.migrationRates.push_back(rate);
    }
    if (std::string(argv[12]).empty() || std::string(argv[12]).back() == ',')
      throw std::invalid_argument("Invalid migration rate list");
    firstFrame = 13;
  }
  if(argc-firstFrame>int(blah2::resident_replay::frameLimit))
    throw std::invalid_argument("Replay experiment exceeds the snapshot frame bound");
  config.temporal.minimumCoherence=.6; config.minimumPilotCoherence=.25;
  config.maximumCells=1u<<20;
  config.spatialCoefficients.assign(config.channelIds.size(),
    {1/std::sqrt(double(config.channelIds.size())),0});
  integration::coherent_replay::ExperimentReader reader(config);
  uint64_t ready=0,valid=0; double peak=0,coherence=0;
  for(int argument=firstFrame;argument<argc;++argument) {
    const auto rates=reader.appendAll(argv[argument]);
    for (const auto& rate : rates) {
    const auto& result=rate.result;
    std::cout<<"frame="<<(argument-firstFrame+1)<<" rate_index="<<rate.rateIndex
      <<" migration_rate_bins_per_second="<<rate.delayRateBinsPerSecond
      <<" rate_corrected_fallback="<<rate.rateCorrectedFallback
      <<" per_rate_trials="<<rate.declaredPerCellTrials
      <<" combined_trials="<<rate.combinedPerCellTrials
      <<" interpolation_white_noise_gain="<<rate.whiteNoiseGain<<" reason="<<result.reason;
    for(size_t index=0;index<result.banks.size();++index) {
      const auto& bank=result.banks[index];
      uint64_t bankValid=0,hybridCoherent=0,hybridFallback=0;
      double bankPeak=0,bankCoherence=0;
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
      for(size_t cell=0;cell<result.noncoherent.banks[index+1].motion.size();++cell) {
        const auto selected=result.hybridAt(index,cell);
        if(selected.valid) {
          if(selected.coherent) ++hybridCoherent; else ++hybridFallback;
        }
      }
      std::cout<<" depth"<<bank.depth<<"_ready="<<bank.ready
        <<" depth"<<bank.depth<<"_valid="<<bankValid
        <<" depth"<<bank.depth<<"_peak="<<bankPeak
        <<" depth"<<bank.depth<<"_max_coherence="<<bankCoherence
        <<" depth"<<(2u<<index)<<"_hybrid_coherent="<<hybridCoherent
        <<" depth"<<(2u<<index)<<"_hybrid_fallback="<<hybridFallback;
    }
    std::cout<<" null_calibrated="<<result.nullCalibrated<<'\n';
    }
  }
  std::cout<<"COMPLETE frames="<<(argc-firstFrame)<<" ready_banks="<<ready
    <<" valid_cells="<<valid<<" peak="<<peak<<" max_coherence="<<coherence
    <<" null_calibrated=0 detector_mutation=0 gpu_execution=0\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"FAIL coherent-replay-inspect: "<<error.what()<<'\n';return 1;}
