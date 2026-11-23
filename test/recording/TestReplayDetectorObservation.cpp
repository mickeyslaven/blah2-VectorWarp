// Production observer lineage/refusal regression; never physical evidence.
#include "process/analysis/ReplayCoherentObserver.h"
#include <rapidjson/document.h>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <unistd.h>
using namespace blah2::qualification_observer;
namespace {
void check(bool ok){if(!ok)throw std::runtime_error("Detector observation regression");}
template<class F>void rejects(F f){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
}
int main(){
  char temporary[]="/tmp/vectorwarp-detector-observer-XXXXXX";
  auto made=mkdtemp(temporary);if(!made)return 1;
  const std::filesystem::path directory(made);
  try {
    Config config;config.directory=directory;config.maxFrames=2;
    Frame frame;frame.sequence=1;frame.captureEpoch=9;frame.captureFirst=100;frame.captureEnd=102;
    frame.sampleRateHz=1000;frame.frequencyHz=100000000;frame.samplePositionAvailable=true;
    frame.channelIds={1};frame.referenceChannelIds={0};
    const auto iq=[](size_t,size_t){return std::complex<double>(1,0);};
    Products p;p.rows=1;p.columns=2;p.channels=1;p.delayBins={0,1};p.dopplerHz={10};
    p.referenceSamples=2;p.reference=[](size_t){return std::complex<double>(1,0);};
    p.caf=iq;p.fused=p.reference;
    {
      Observer observer(config,true,true);
      rejects([&]{observer.detector(frame,true,true,"cpu",2,1);});
      observer.input(frame,2,2,iq);
      rejects([&]{observer.detector(frame,true,true,"cpu",2,1);});
      observer.products(frame,p);
      auto wrong=frame;++wrong.captureEpoch;
      rejects([&]{observer.detector(wrong,true,true,"cpu",2,1);});
      rejects([&]{observer.detector(frame,true,false,"cpu",2,1);});
      rejects([&]{observer.detector(frame,true,true,"cpu",1,2);});
      observer.detector(frame,true,true,"cpu",2,1);
      rejects([&]{observer.detector(frame,true,true,"cpu",2,1);});
      ++frame.sequence;frame.captureFirst=102;frame.captureEnd=104;
      observer.input(frame,2,2,iq);observer.products(frame,p);
      observer.detector(frame,true,false,"cpu",0,0);
    }
    std::ifstream input(directory/"coherent-observer.jsonl");std::string line;unsigned count=0;
    while(std::getline(input,line)){
      rapidjson::Document value;value.Parse(line.c_str());check(!value.HasParseError());
      if(std::string(value["stage"].GetString())!="detector")continue;
      check(value["captureEpoch"].GetUint64()==9);
      check(value["evaluated_cells"].GetUint64()==(count?0:2));
      check(value["accepted_cells"].GetUint64()==(count?0:1));
      check(value["nonfinite_cells"].GetUint64()==0);++count;
    }
    check(count==2);
    config.directory=directory/"summary";std::filesystem::create_directory(config.directory);
    config.summaryOnly=true;config.maxSamplesPerChannel=1000000;config.maxMapCells=1000000;
    config.maxFrames=256;frame.captureFirst=0;frame.captureEnd=1000000;
    frame.resolvedProductPlan="{\"schema\":\"unit-fixture-only\"}";
    p.reference={};p.caf={};p.fused={};p.referenceSamples=0;
    {
      Observer observer(config,true,true);check(!observer.observesSamples());
      observer.input(frame,2,1000000,[](size_t,size_t)->std::complex<double>{
        throw std::runtime_error("Summary copied IQ");});
      observer.products(frame,p);observer.detector(frame,true,true,"cpu",2,1);
    }
    std::ifstream summary(std::filesystem::path(config.directory)/"coherent-observer.jsonl");
    count=0;
    while(std::getline(summary,line)){
      rapidjson::Document value;value.Parse(line.c_str());check(!value.HasParseError());
      check(std::string(value["resolved_product_plan"].GetString())==frame.resolvedProductPlan);
      check(!value.HasMember("channelized_iq")&&!value.HasMember("caf"));++count;
    }
    check(count==3);std::filesystem::remove_all(directory);
    std::cout<<"Detector observation measured counts, warmup and lineage passed\n";
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
