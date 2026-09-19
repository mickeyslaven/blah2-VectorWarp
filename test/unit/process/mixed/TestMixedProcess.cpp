#include "process/mixed/MixedProcess.h"
#include "data/IqData.h"
#include <cassert>
#include <chrono>
#include <cmath>
#include <complex>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>
namespace blah2 { std::string gpuSiblingPath(const char*) {return "unused";} }
int main(int argc,char** argv){
  if(argc!=2)return 2;
  using Complex=std::complex<double>;
  std::deque<Complex> reference(blah2::mixed::samples,{3,4});
  std::deque<Complex> surveillance(blah2::mixed::samples,{5,6});
  std::vector<int16_t> paired(uint64_t(blah2::mixed::samples)*4);
  for(uint32_t i=0;i<blah2::mixed::samples;++i){
    paired[4*i]=11;paired[4*i+1]=-12;paired[4*i+2]=21;paired[4*i+3]=-22;
  }
  paired[0]=101;paired[1]=-102;paired[2]=201;paired[3]=-202;
  const uint64_t last=uint64_t(blah2::mixed::samples-1)*4;
  paired[last]=103;paired[last+1]=-104;paired[last+2]=203;paired[last+3]=-204;
  for(const auto* mode:{"good","crash","hang","wrong-sequence","wrong-shape","nan",
      "nan-tail","startup-hang","startup-error","bad-protocol","short-message"}){
    blah2::mixed::ProcessOptions options;
    options.executable=argv[1];options.argument=mode;
    options.startupMs=100;options.frameMs=100;
    bool failed=false;
    const auto started=std::chrono::steady_clock::now();
    std::vector<Complex> map{{91,92}},tail{{93,94}};
    try{
      blah2::mixed::Process process(options);
      process.run(reference,surveillance,map,tail);
      assert(std::string(mode)=="good");
      assert(map.size()==blah2::mixed::mapSamples);
      assert(tail.size()==blah2::mixed::tailSamples);
      assert(map[10]==Complex(10,3));
    }catch(const std::exception&){failed=true;}
    assert(failed==(std::string(mode)!="good"));
    assert(std::chrono::steady_clock::now()-started<std::chrono::seconds(2));
    if(failed){
      assert(map.size()==1&&map.front()==Complex(91,92));
      assert(tail.size()==1&&tail.front()==Complex(93,94));
    }
    assert(reference.front()==Complex(3,4)&&reference.back()==Complex(3,4));
    assert(surveillance.front()==Complex(5,6)&&surveillance.back()==Complex(5,6));
  }
  auto preparedOptions=[&](const char* mode){
    blah2::mixed::ProcessOptions options;
    options.executable=argv[1];options.argument=mode;
    options.startupMs=100;options.frameMs=100;
    return options;
  };
  auto checkPreparedInputs=[](const IqData& reference,const IqData& surveillance,
      uint32_t referenceChannel){
    const Complex firstReference=referenceChannel?Complex(201,-202):Complex(101,-102);
    const Complex firstSurveillance=referenceChannel?Complex(101,-102):Complex(201,-202);
    const Complex lastReference=referenceChannel?Complex(203,-204):Complex(103,-104);
    const Complex lastSurveillance=referenceChannel?Complex(103,-104):Complex(203,-204);
    assert(reference.view_data().front()==firstReference&&
      reference.view_data().back()==lastReference&&
      surveillance.view_data().front()==firstSurveillance&&
      surveillance.view_data().back()==lastSurveillance);
  };
  for(uint32_t referenceChannel=0;referenceChannel<2;++referenceChannel){
    IqData preparedReference(blah2::mixed::samples),preparedSurveillance(blah2::mixed::samples);
    std::vector<Complex> map{{91,92}},tail{{93,94}};
    blah2::mixed::Process process(preparedOptions(referenceChannel?"prepared-channel-1":"prepared-channel-0"));
    bool missing=false;
    try{process.run_prepared(map,tail);}catch(const std::logic_error&){missing=true;}
    assert(missing&&map.size()==1&&map.front()==Complex(91,92)&&tail.size()==1&&tail.front()==Complex(93,94));
    process.prepare_paired_i16(paired.data(),blah2::mixed::samples,
      preparedReference,preparedSurveillance,referenceChannel);
    checkPreparedInputs(preparedReference,preparedSurveillance,referenceChannel);
    process.run_prepared(map,tail);
    assert(map.size()==blah2::mixed::mapSamples&&tail.size()==blah2::mixed::tailSamples&&map[10]==Complex(10,3));
    map={{91,92}};tail={{93,94}};
    bool reused=false;
    try{process.run_prepared(map,tail);}catch(const std::logic_error&){reused=true;}
    assert(reused&&map.size()==1&&map.front()==Complex(91,92)&&tail.size()==1&&tail.front()==Complex(93,94));
  }
  for(const auto* mode:{"good","crash","hang","wrong-sequence","wrong-shape","nan",
      "nan-tail","startup-hang","startup-error","bad-protocol","short-message"}){
    IqData preparedReference(blah2::mixed::samples),preparedSurveillance(blah2::mixed::samples);
    std::vector<Complex> map{{91,92}},tail{{93,94}};
    bool failed=false;
    const auto started=std::chrono::steady_clock::now();
    try{
      blah2::mixed::Process process(preparedOptions(mode));
      process.prepare_paired_i16(paired.data(),blah2::mixed::samples,
        preparedReference,preparedSurveillance,0);
      process.run_prepared(map,tail);
      assert(std::string(mode)=="good");
    }catch(const std::exception&){failed=true;}
    assert(failed==(std::string(mode)!="good"));
    assert(std::chrono::steady_clock::now()-started<std::chrono::seconds(2));
    if(std::string(mode)=="startup-hang"||std::string(mode)=="startup-error")
      assert(preparedReference.get_length()==0&&preparedSurveillance.get_length()==0);
    else
      checkPreparedInputs(preparedReference,preparedSurveillance,0);
    if(failed)
      assert(map.size()==1&&map.front()==Complex(91,92)&&tail.size()==1&&tail.front()==Complex(93,94));
  }
  {
    IqData preparedReference(blah2::mixed::samples),preparedSurveillance(blah2::mixed::samples);
    blah2::mixed::Process process(preparedOptions("good"));
    process.prepare_paired_i16(paired.data(),blah2::mixed::samples,
      preparedReference,preparedSurveillance,0);
    const auto originalReference=preparedReference.get_data();
    const auto originalSurveillance=preparedSurveillance.get_data();
    bool invalid=false;
    try{process.prepare_paired_i16(nullptr,blah2::mixed::samples,
      preparedReference,preparedSurveillance,0);}catch(const std::invalid_argument&){invalid=true;}
    assert(invalid&&preparedReference.view_data()==originalReference&&
      preparedSurveillance.view_data()==originalSurveillance);
    std::vector<Complex> map{{91,92}},tail{{93,94}};
    bool stale=false;
    try{process.run_prepared(map,tail);}catch(const std::logic_error&){stale=true;}
    assert(stale&&map.size()==1&&map.front()==Complex(91,92)&&tail.size()==1&&tail.front()==Complex(93,94));
  }
}
