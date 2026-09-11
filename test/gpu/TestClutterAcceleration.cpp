#include "process/ambiguity/Acceleration.h"
#include "process/ambiguity/GpuProcess.h"
#include "process/clutter/WienerHopf.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>

namespace {
using Complex = std::complex<double>;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

std::deque<Complex> signal(size_t samples) {
  std::mt19937 random(7301);
  std::normal_distribution<double> noise;
  std::deque<Complex> result;
  for (size_t i = 0; i < samples; ++i)
    result.emplace_back(std::polar(1.0, .071*i) + .37*std::polar(1.0, -.193*i) +
      Complex(.05*noise(random), .05*noise(random)));
  return result;
}
std::vector<std::deque<Complex>> surveillance(const std::deque<Complex>& reference,
    size_t channels, bool oneTap = false) {
  std::vector<std::deque<Complex>> result(channels);
  for (size_t channel = 0; channel < channels; ++channel)
    for (size_t i = 0; i < reference.size(); ++i) {
      const auto direct = oneTap ? (4.0+.5*channel)*reference[i] :
        (4.0+.5*channel)*reference[(i+reference.size()-2)%reference.size()] +
          (.6-.1*channel)*reference[(i+3)%reference.size()];
      const auto residual = (channel == 0 ? 2e-5 : .03*(channel+1)) *
        std::polar(1.0, (.31+.03*channel)*i);
      result[channel].push_back(direct + residual);
    }
  return result;
}
std::vector<std::deque<Complex>> cpuResult(const std::deque<Complex>& reference,
    const std::vector<std::deque<Complex>>& input, int minimum, int maximum) {
  IqData ref(reference.size()); ref.replace(std::deque<Complex>(reference));
  std::vector<std::deque<Complex>> result;
  for (const auto& values : input) {
    IqData channel(values.size()); channel.replace(std::deque<Complex>(values));
    WienerHopf filter(minimum, maximum, reference.size());
    require(filter.process(&ref, &channel), "CPU clutter oracle rejected a full-rank fixture");
    result.push_back(channel.get_data());
  }
  return result;
}
void verifyOneTapDirect(const std::deque<Complex>& reference,
    const std::vector<std::deque<Complex>>& input, int minimum,
    const std::vector<std::deque<Complex>>& actual) {
  double autoCorrelation=0;
  for(const auto value:reference) autoCorrelation+=std::norm(value);
  for(size_t channel=0;channel<input.size();++channel){
    Complex cross{};
    for(size_t i=0;i<reference.size();++i){
      const int64_t shifted=(int64_t(i)-minimum)%int64_t(reference.size());
      cross+=input[channel][i]*std::conj(reference[shifted<0?shifted+reference.size():shifted]);
    }
    const Complex weight=cross/autoCorrelation;
    for(size_t i=0;i<reference.size();++i){
      const int64_t shifted=(int64_t(i)-minimum)%int64_t(reference.size());
      const Complex expected=input[channel][i]-
        reference[shifted<0?shifted+reference.size():shifted]*weight;
      require(std::abs(actual[channel][i]-expected)<1e-9,
        "CPU one-tap clutter shift differs from signed direct equation");
    }
  }
}

struct Mock final : blah2::GpuBackend, blah2::GpuClutterFrameBackend {
  blah2::GpuGeometry geometry;
  std::vector<std::complex<float>> reference, surveillance, output;
  std::vector<std::deque<Complex>> expected;
  std::deque<Complex> source;
  std::string fault;
  unsigned calls = 0;
  Mock(blah2::GpuGeometry g, std::deque<Complex> x,
      std::vector<std::deque<Complex>> result)
    : geometry(g), reference(g.clutterSamples),
      surveillance(size_t(g.clutterSamples)*g.channels),
      output(size_t(g.clutterSamples)*g.channels), expected(std::move(result)), source(std::move(x)) {}
  blah2::GpuDevice device() const override { return {"mock-clutter", "Mock clutter", 1}; }
  void process(const std::vector<std::complex<float>>&,
      const std::vector<std::complex<float>>&,
      std::vector<std::complex<float>>&) override {
    throw std::runtime_error("Ambiguity path unexpectedly called");
  }
  bool clutterAvailable() const override { return true; }
  blah2::GpuClutterBuffers clutterBuffers() override {
    return {reference.data(), reference.size(), surveillance.data(), surveillance.size(),
      output.data(), output.size()};
  }
  bool processClutterFrame() override {
    ++calls;
    if (fault=="throw") throw std::runtime_error("Injected clutter failure");
    if (fault=="reject") return false;
    for (size_t i = 0; i < source.size(); ++i) {
      const int64_t shifted = (int64_t(i)-geometry.clutterDelayMin)%int64_t(source.size());
      require(std::abs(std::complex<double>(reference[i]) -
        source[shifted < 0 ? shifted+source.size() : shifted]) < 2e-6,
        "GPU clutter producer used the wrong signed reference shift");
    }
    for (size_t channel = 0; channel < expected.size(); ++channel)
      for (size_t i = 0; i < source.size(); ++i)
        output[channel*source.size()+i] = std::complex<double>(
          surveillance[channel*source.size()+i]) - expected[channel][i];
    if (fault=="wrong") output[0] *= 2.0f;
    if (fault=="nan") output[0] = {std::numeric_limits<float>::quiet_NaN(),0};
    return true;
  }
};

void compare(const std::vector<std::unique_ptr<IqData>>& actual,
    const std::vector<std::deque<Complex>>& expected,
    const std::vector<std::deque<Complex>>& input) {
  for (size_t channel=0; channel<actual.size(); ++channel) {
    double error=0, signalPower=0, inputPower=0, peakError=0, peakInput=0;
    const auto& values=actual[channel]->view_data();
    require(values.size()==expected[channel].size(), "Filtered clutter sample count differs");
    for (size_t i=0; i<values.size(); ++i) {
      error += std::norm(values[i]-expected[channel][i]);
      signalPower += std::norm(expected[channel][i]);
      inputPower += std::norm(input[channel][i]);
      peakError = std::max(peakError, std::norm(values[i]-expected[channel][i]));
      peakInput = std::max(peakInput, std::norm(input[channel][i]));
    }
    const double filteredFloor=std::max(signalPower,inputPower*1e-8);
    require(error <= std::max(inputPower*1e-8, 1e-20),
      "Clutter per-channel RMS differs from FP64 CPU oracle");
    require(error <= std::max(filteredFloor*1e-8, 1e-20),
      "Clutter weak-residual RMS differs from FP64 CPU oracle");
    require(peakError <= std::max(peakInput*1e-8, 1e-20),
      "Clutter per-channel peak differs from FP64 CPU oracle");
  }
}

void runCase(int minimum, int maximum, size_t samples, const std::string& device) {
  constexpr uint32_t channels=3;
  const auto x=signal(samples);
  const auto y=surveillance(x, channels, maximum-minimum==1);
  const auto expected=cpuResult(x, y, minimum, maximum);
  if(maximum-minimum==1) verifyOneTapDirect(x,y,minimum,expected);
  const blah2::GpuGeometry geometry{8,4,2,channels,0,uint32_t(samples),
    uint32_t(maximum-minimum),minimum};
  std::shared_ptr<Mock> mock;
  blah2::Acceleration::BackendFactory factory;
  if (device.empty()) factory=[&](const blah2::GpuGeometry& g, const std::string&) {
    mock=std::make_shared<Mock>(g,x,expected);
    struct Holder final : blah2::GpuBackend, blah2::GpuClutterFrameBackend {
      std::shared_ptr<Mock> value;
      explicit Holder(std::shared_ptr<Mock> item):value(std::move(item)){}
      blah2::GpuDevice device() const override{return value->device();}
      void process(const std::vector<std::complex<float>>& a,
        const std::vector<std::complex<float>>& b,std::vector<std::complex<float>>& c) override{value->process(a,b,c);}
      bool clutterAvailable() const override{return true;}
      blah2::GpuClutterBuffers clutterBuffers() override{return value->clutterBuffers();}
      bool processClutterFrame() override{return value->processClutterFrame();}
    };
    return std::unique_ptr<blah2::GpuBackend>(std::make_unique<Holder>(mock));
  };
  blah2::Acceleration acceleration("gpu",geometry,2,48000,0,
    device.empty()?"auto":device,factory);
  std::vector<std::unique_ptr<IqData>> data;
  std::vector<std::unique_ptr<WienerHopf>> filters;
  std::vector<IqData*> pointers;
  for (uint32_t channel=0; channel<channels; ++channel) {
    data.push_back(std::make_unique<IqData>(samples)); pointers.push_back(data.back().get());
    filters.push_back(std::make_unique<WienerHopf>(minimum,maximum,samples));
  }
  for (unsigned frame=0; frame<blah2::Acceleration::ClutterQualificationFrames; ++frame) {
    auto frameInput=y;
    const auto frameExpected=cpuResult(x,frameInput,minimum,maximum);
    if (mock) mock->expected=frameExpected;
    IqData reference(samples); reference.replace(std::deque<Complex>(x));
    for (size_t channel=0; channel<channels; ++channel)
      data[channel]->replace(std::deque<Complex>(frameInput[channel]));
    const bool success=acceleration.processClutter(reference,pointers,[&] {
      bool ok=true;
      for (size_t channel=0; channel<channels; ++channel)
        ok=filters[channel]->process(&reference,data[channel].get()) && ok;
      return ok;
    });
    require(success,"Clutter fixture was rejected");
    compare(data,frameExpected,frameInput);
  }
  if(acceleration.clutterStatus().state!="fallback") {
    require(acceleration.clutterStatus().state=="checking" &&
      acceleration.clutterTiming().cpuExecuted && acceleration.clutterTiming().gpuExecuted,
      "GPU clutter-only qualification state differs");
  }
  std::cout << "PASS clutter min=" << minimum << " max=" << maximum
    << " samples=" << samples << " device=" << (device.empty()?"mock":device)
    << " state=" << acceleration.clutterStatus().state
    << " active=" << acceleration.clutterStatus().active
    << " reason=" << acceleration.clutterStatus().reason << '\n';
}

void runFallback(const std::string& fault) {
  constexpr size_t samples=64;
  const auto x=signal(samples);
  const auto y=surveillance(x,2);
  const auto expected=cpuResult(x,y,-2,3);
  const blah2::GpuGeometry geometry{8,4,2,2,0,samples,5,-2};
  std::shared_ptr<Mock> mock;
  auto factory=[&](const blah2::GpuGeometry& g,const std::string&)
      ->std::unique_ptr<blah2::GpuBackend>{
    mock=std::make_shared<Mock>(g,x,expected); mock->fault=fault;
    struct Holder final:blah2::GpuBackend,blah2::GpuClutterFrameBackend{
      std::shared_ptr<Mock> value;
      explicit Holder(std::shared_ptr<Mock> item):value(std::move(item)){}
      blah2::GpuDevice device()const override{return value->device();}
      void process(const std::vector<std::complex<float>>& a,
        const std::vector<std::complex<float>>& b,std::vector<std::complex<float>>& c)override{value->process(a,b,c);}
      bool clutterAvailable()const override{return true;}
      blah2::GpuClutterBuffers clutterBuffers()override{
        auto frame=value->clutterBuffers();
        if(value->fault=="shape") --frame.outputCount;
        return frame;
      }
      bool processClutterFrame()override{return value->processClutterFrame();}
    };
    return std::make_unique<Holder>(mock);
  };
  blah2::Acceleration acceleration("gpu",geometry,2,48000,0,"auto",factory);
  IqData reference(samples); reference.replace(std::deque<Complex>(x));
  std::vector<std::unique_ptr<IqData>> data;
  std::vector<std::unique_ptr<WienerHopf>> filters;
  std::vector<IqData*> pointers;
  for(size_t channel=0;channel<y.size();++channel){
    data.push_back(std::make_unique<IqData>(samples));
    data.back()->replace(std::deque<Complex>(y[channel])); pointers.push_back(data.back().get());
    filters.push_back(std::make_unique<WienerHopf>(-2,3,samples));
  }
  unsigned cpuCalls=0;
  const bool success=acceleration.processClutter(reference,pointers,[&]{
    ++cpuCalls; bool ok=true;
    for(size_t channel=0;channel<data.size();++channel)
      ok=filters[channel]->process(&reference,data[channel].get())&&ok;
    return ok;
  });
  require(success&&cpuCalls==1,"Clutter fault did not run CPU exactly once");
  compare(data,expected,y);
  require(acceleration.clutterStatus().active=="cpu"&&
    acceleration.clutterStatus().state=="fallback","Clutter fault did not select CPU");
  std::cout<<"PASS clutter-fallback="<<fault<<'\n';
}
void runRankDeficient(const std::string& device) {
  constexpr size_t samples=64, channels=2;
  const std::deque<Complex> x(samples,{1,0});
  std::vector<std::deque<Complex>> y(channels);
  for(size_t channel=0;channel<channels;++channel)
    for(size_t i=0;i<samples;++i)
      y[channel].push_back(std::polar(1.0,(.13+.07*channel)*i));
  const blah2::GpuGeometry geometry{8,4,2,channels,0,samples,4,0};
  blah2::Acceleration::BackendFactory factory;
  if(device.empty()) factory=[&](const blah2::GpuGeometry& g,const std::string&)
      ->std::unique_ptr<blah2::GpuBackend>{
    auto mock=std::make_shared<Mock>(g,x,y); mock->fault="throw";
    struct Holder final:blah2::GpuBackend,blah2::GpuClutterFrameBackend{
      std::shared_ptr<Mock> value;
      explicit Holder(std::shared_ptr<Mock> item):value(std::move(item)){}
      blah2::GpuDevice device()const override{return value->device();}
      void process(const std::vector<std::complex<float>>& a,
        const std::vector<std::complex<float>>& b,std::vector<std::complex<float>>& c)override{value->process(a,b,c);}
      bool clutterAvailable()const override{return true;}
      blah2::GpuClutterBuffers clutterBuffers()override{return value->clutterBuffers();}
      bool processClutterFrame()override{return value->processClutterFrame();}
    };
    return std::make_unique<Holder>(mock);
  };
  blah2::Acceleration acceleration("gpu",geometry,2,48000,0,
    device.empty()?"auto":device,factory);
  IqData reference(samples); reference.replace(std::deque<Complex>(x));
  std::vector<std::unique_ptr<IqData>> data;
  std::vector<std::unique_ptr<WienerHopf>> filters;
  std::vector<IqData*> pointers;
  for(size_t channel=0;channel<channels;++channel){
    data.push_back(std::make_unique<IqData>(samples));
    data.back()->replace(std::deque<Complex>(y[channel])); pointers.push_back(data.back().get());
    filters.push_back(std::make_unique<WienerHopf>(0,4,samples));
  }
  const bool success=acceleration.processClutter(reference,pointers,[&]{
    bool ok=true;
    for(size_t channel=0;channel<channels;++channel)
      ok=filters[channel]->process(&reference,data[channel].get())&&ok;
    return ok;
  });
  require(!success,"Rank-deficient clutter fixture was accepted");
  for(size_t channel=0;channel<channels;++channel)
    require(data[channel]->view_data()==y[channel],"Rejected clutter frame was partially published");
  std::cout<<"PASS clutter-rank-deficient device="<<(device.empty()?"mock":device)<<'\n';
}
void runThrowingCpuOracle() {
  constexpr size_t samples=64;
  const auto x=signal(samples);
  const auto y=surveillance(x,2);
  const auto expected=cpuResult(x,y,-2,3);
  const blah2::GpuGeometry geometry{8,4,2,2,0,samples,5,-2};
  auto factory=[&](const blah2::GpuGeometry& g,const std::string&)
      ->std::unique_ptr<blah2::GpuBackend>{
    auto mock=std::make_shared<Mock>(g,x,expected);
    struct Holder final:blah2::GpuBackend,blah2::GpuClutterFrameBackend{
      std::shared_ptr<Mock> value;
      explicit Holder(std::shared_ptr<Mock> item):value(std::move(item)){}
      blah2::GpuDevice device()const override{return value->device();}
      void process(const std::vector<std::complex<float>>& a,
        const std::vector<std::complex<float>>& b,std::vector<std::complex<float>>& c)override{value->process(a,b,c);}
      bool clutterAvailable()const override{return true;}
      blah2::GpuClutterBuffers clutterBuffers()override{return value->clutterBuffers();}
      bool processClutterFrame()override{return value->processClutterFrame();}
    };
    return std::make_unique<Holder>(mock);
  };
  blah2::Acceleration acceleration("gpu",geometry,2,48000,0,"auto",factory);
  IqData reference(samples); reference.replace(std::deque<Complex>(x));
  std::vector<std::unique_ptr<IqData>> data;
  std::vector<IqData*> pointers;
  for(const auto& channel:y){
    data.push_back(std::make_unique<IqData>(samples));
    data.back()->replace(std::deque<Complex>(channel)); pointers.push_back(data.back().get());
  }
  unsigned calls=0;
  bool threw=false;
  try {
    acceleration.processClutter(reference,pointers,[&]{
      ++calls;
      auto partial=data[0]->get_data(); partial.front()+=Complex(1,0);
      data[0]->replace(std::move(partial));
      throw std::runtime_error("Injected partial CPU oracle failure");
      return false;
    });
  } catch(const std::runtime_error& error) {
    threw=std::string(error.what())=="Injected partial CPU oracle failure";
  }
  require(threw&&calls==1,"Throwing CPU clutter oracle was retried or swallowed");
  require(data[0]->view_data().front()==y[0].front()+Complex(1,0),
    "Throwing CPU clutter oracle mutated the frame more than once");
  require(data[1]->view_data()==y[1],"Throwing CPU clutter oracle published GPU partial output");
  std::cout<<"PASS clutter-throwing-cpu-oracle\n";
}
void runComposed(const std::string& device) {
  constexpr uint32_t channels=2, samples=4800, fs=48000;
  constexpr int clutterMinimum=-3, clutterMaximum=5;
  const auto x=signal(samples);
  const auto base=surveillance(x,channels);
  std::vector<std::unique_ptr<Ambiguity>> maps, controls;
  std::vector<std::unique_ptr<WienerHopf>> filters, controlFilters;
  std::vector<std::unique_ptr<IqData>> data;
  std::vector<Ambiguity*> mapPointers;
  std::vector<IqData*> dataPointers;
  for(uint32_t channel=0;channel<channels;++channel){
    maps.push_back(std::make_unique<Ambiguity>(-5,20,-100,100,fs,samples,true));
    controls.push_back(std::make_unique<Ambiguity>(-5,20,-100,100,fs,samples,true));
    filters.push_back(std::make_unique<WienerHopf>(clutterMinimum,clutterMaximum,samples));
    controlFilters.push_back(std::make_unique<WienerHopf>(clutterMinimum,clutterMaximum,samples));
    data.push_back(std::make_unique<IqData>(samples));
    mapPointers.push_back(maps.back().get()); dataPointers.push_back(data.back().get());
  }
  const blah2::GpuGeometry geometry{maps[0]->get_nfft(),maps[0]->get_n_doppler_bins(),
    maps[0]->get_n_delay_bins(),channels,-5,samples,clutterMaximum-clutterMinimum,
    clutterMinimum};
  blah2::Acceleration acceleration("gpu",geometry,maps[0]->get_n_corr(),fs,
    maps[0]->get_doppler_middle(),device);
  for(unsigned frame=0;frame<blah2::Acceleration::TotalQualificationFrames+2;++frame){
    auto input=base;
    if(frame>=blah2::Acceleration::TotalQualificationFrames)
      for(size_t channel=0;channel<input.size();++channel)
        for(size_t i=0;i<samples;++i)
          input[channel][i]+=.007*std::polar(1.0,(.117+.01*channel)*i);
    IqData reference(samples); reference.replace(std::deque<Complex>(x));
    std::vector<std::vector<std::vector<Complex>>> expected;
    for(uint32_t channel=0;channel<channels;++channel){
      IqData truth(samples); truth.replace(std::deque<Complex>(input[channel]));
      require(controlFilters[channel]->process(&reference,&truth),
        "Composed CPU clutter control rejected fixture");
      controls[channel]->process(reference.view_data(),&truth);
      expected.push_back(controls[channel]->result()->data);
      data[channel]->replace(std::deque<Complex>(input[channel]));
    }
    require(acceleration.processClutter(reference,dataPointers,[&]{
      bool ok=true;
      for(uint32_t channel=0;channel<channels;++channel)
        ok=filters[channel]->process(&reference,data[channel].get())&&ok;
      return ok;
    }),"Composed clutter frame rejected");
    acceleration.process(reference.view_data(),dataPointers,mapPointers,[&]{
      for(uint32_t channel=0;channel<channels;++channel)
        maps[channel]->process(reference.view_data(),data[channel].get());
    });
    for(uint32_t channel=0;channel<channels;++channel){
      const auto& actual=maps[channel]->result()->data;
      double signalPower=0,errorPower=0,peak=0,maxError=0;
      for(size_t row=0;row<actual.size();++row)
        for(size_t column=0;column<actual[row].size();++column){
          signalPower+=std::norm(expected[channel][row][column]);
          errorPower+=std::norm(actual[row][column]-expected[channel][row][column]);
          peak=std::max(peak,std::abs(expected[channel][row][column]));
          maxError=std::max(maxError,std::abs(actual[row][column]-expected[channel][row][column]));
        }
      require(std::sqrt(errorPower/std::max(signalPower,1e-30))<=1e-4 &&
        maxError/std::max(peak,1e-30)<=1e-4,
        "Composed physical map differs from FP64 CPU control");
    }
  }
  require(acceleration.status().active=="vulkan",
    "Composed fixture lost the independently qualified GPU ambiguity stage");
  if(acceleration.clutterStatus().active!="vulkan"){
    const auto& reason=acceleration.clutterStatus().reason;
    require(reason.find("accuracy")!=std::string::npos ||
      reason.find("precision")!=std::string::npos,
      "Composed fixture accepted a non-precision GPU clutter failure");
  }
  std::cout<<"PASS clutter-composed device="<<device
    <<" clutter_active="<<acceleration.clutterStatus().active
    <<" clutter_reason="<<acceleration.clutterStatus().reason<<'\n';
}
}

int main(int argc,char** argv) try {
  const std::string device=argc>1?argv[1]:"";
  runCase(-3,5,256,device);
  runCase(0,8,256,device);
  runCase(0,1,256,device); // One-tap cancellation has no FIR edge artifact.
  runCase(3,4,250,device); // Non-power-of-two catches unsigned positive-lag wrap.
  runCase(3,11,256,device);
  runCase(-31,33,64,device); // B=N and L=2N+1.
  if(device.empty()) for(const std::string fault:{"wrong","nan","reject","throw","shape"}) runFallback(fault);
  runRankDeficient(device);
  if(device.empty()) runThrowingCpuOracle();
  else runComposed(device);
  return 0;
} catch(const std::exception& error) {
  std::cerr << "FAIL: " << error.what() << '\n'; return 1;
}
