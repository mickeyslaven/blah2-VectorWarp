// CPU-only independent oracle for the production FP64 baseline helper.
#include "process/ambiguity/GpuClutterRefinement.h"
#include "process/ambiguity/GpuClutterBatch.h"
#include "process/utility/FftwThreads.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Double=std::complex<double>;
using Float=std::complex<float>;
using blah2::clutter_refinement::BlockedBaseline;
void check(bool ok,const char* why){ if(!ok) throw std::runtime_error(why); }
template<class Exception,class Function> void rejects(Function fn,const char* why){
  bool caught=false;
  try { fn(); } catch(const Exception&) { caught=true; }
  check(caught,why);
}
std::vector<Double> direct(const std::vector<Float>& x,const std::vector<Double>& q,bool circular){
  std::vector<Double> y(x.size());
  for(size_t i=0;i<x.size();++i)
    for(size_t j=0;j<q.size();++j)
      if(circular || i>=j) y[i]+=q[j]*Double(x[(i+x.size()-j)%x.size()]);
  return y;
}
void near(const std::vector<Double>& actual,const std::vector<Double>& expected){
  check(actual.size()==expected.size(),"Oracle length differs");
  for(size_t i=0;i<actual.size();++i)
    check(std::isfinite(actual[i].real()) && std::isfinite(actual[i].imag()) &&
      std::abs(actual[i]-expected[i])<=2e-12*std::max(1.,std::abs(expected[i])),"FP64 FIR differs from direct convolution");
}
void covarianceOracle(const std::vector<Double>& actual,const std::vector<Float>& reference){
  double power=0;
  for(const auto value:reference)power+=std::norm(Double(value));
  for(size_t lag=0;lag<actual.size();++lag){
    Double expected{};
    for(size_t i=0;i<reference.size();++i)
      expected+=Double(reference[i])*std::conj(Double(reference[(i+reference.size()-lag)%reference.size()]));
    check(std::abs(actual[lag]-expected)<=2e-12*std::max(power,1e-30),
      "FP64 blocked covariance differs from direct circular sum");
  }
}
void covarianceBoundaries(){
  for(const auto& shape:{std::pair<uint32_t,uint32_t>{32,1},{1057,5},{4099,1261},{37,37}}){
    const auto n=shape.first,b=shape.second;
    std::vector<Float> original(n),reference(n);
    for(uint32_t i=0;i<n;++i)original[i]=Float(float(int(i%31)-15)/32,float(int(i%19)-9)/16);
    original[0]={7,-3};original[n-1]={-5,11}; // Strong asymmetric circular edge.
    BlockedBaseline fir(n,b);
    rejects<std::logic_error>([&]{fir.covariance();},"Unprepared covariance accepted");
    for(int delay:{-31,4}){
      for(uint32_t i=0;i<n;++i)reference[i]=original[size_t((int64_t(i)-delay+n)%n)];
      fir.prepare(reference.data(),n);
      covarianceOracle(fir.covariance(),reference);
      const auto cached=fir.covariance();
      std::vector<Double> q(b),output(n);q[0]={.125,-.25};
      fir.estimate(q.data(),output.data());
      near(output,direct(reference,q,false));
      check(fir.covariance()==cached,"FIR scratch mutated cached covariance");
      fir.invalidate();
      rejects<std::logic_error>([&]{fir.covariance();},"Invalidated covariance reused");
    }
    std::fill(reference.begin(),reference.end(),Float{});reference[n-1]={.125,.25};
    fir.prepare(reference.data(),n);covarianceOracle(fir.covariance(),reference);
    reference[0]={std::numeric_limits<float>::quiet_NaN(),0};
    rejects<std::domain_error>([&]{fir.prepare(reference.data(),n);},"Nonfinite covariance input accepted");
    rejects<std::logic_error>([&]{fir.covariance();},"Failed prepare retained covariance");
  }
}
void boundaryAndLifetime(){
  constexpr uint32_t N=1057,B=5; // Includes a complete overlap-save block and its short tail.
  std::vector<Float> original(N),reference(N),input(N),residual(N);
  for(uint32_t i=0;i<N;++i) original[i]=Float(float(int(i%9)-4)/8,float(int(i%7)-3)/16);
  std::vector<Double> q(B); q[0]={.5,.25};q[B-1]={-.125,.0625};
  BlockedBaseline fir(N,B); std::vector<Double> linear(N);
  for(int delay:{-3,2}){
    for(uint32_t i=0;i<N;++i)reference[i]=original[size_t((int64_t(i)-delay+N)%N)];
    fir.prepare(reference.data(),N); fir.estimate(q.data(),linear.data());
    const auto expected=direct(reference,q,false), circular=direct(reference,q,true);
    near(linear,expected);
    bool edge=false;
    for(uint32_t i=0;i<N;++i){
      input[i]=Float(circular[i]);
      check(Double(input[i])==circular[i],"Circular fixture not binary-exact in FP32");
      if(i<B-1)edge=edge || std::abs(circular[i]-expected[i])>1e-4;
      else check(circular[i]==expected[i],"Independent circular/linear tail mismatch");
    }
    check(edge,"Fixture did not exercise circular wrap");
    double power=0,inputPower=0;
    fir.residual(input.data(),q.data(),linear.data(),residual.data(),&power,&inputPower);
    for(auto value:residual)check(std::abs(value)<2e-12,"y=Cq correction residual not zero");
    check(power<1e-20 && inputPower>0,"Residual diagnostic powers differ");
    std::vector<Double> fusedLinear(N);
    std::vector<Float> fusedResidual(N);
    fir.estimateResidual(input.data(),q.data(),fusedLinear.data(),fusedResidual.data());
    check(fusedLinear==linear,"Fused baseline differs from separate FP64 estimate");
    check(fusedResidual==residual,"Fused residual differs from separate formation");
    // Published baseline is linear: final y-Lq must retain the first B-1 wrapped terms.
    bool retained=false;
    for(uint32_t i=0;i<B-1;++i)retained=retained || std::abs(Double(input[i])-linear[i])>1e-4;
    check(retained,"Linear FIR edge was incorrectly removed");
    std::vector<Double> zero(B);
    fir.estimate(zero.data(),linear.data()); near(linear,std::vector<Double>(N));
    fir.residual(input.data(),zero.data(),linear.data(),residual.data());
    check(residual==input,"q=0 changed authoritative rounded input");
    auto invalid=q; invalid[0]={std::numeric_limits<double>::quiet_NaN(),0};
    rejects<std::domain_error>([&]{fir.estimate(invalid.data(),linear.data());},"Nonfinite coefficients accepted");
    fir.invalidate();
    rejects<std::logic_error>([&]{fir.estimate(q.data(),linear.data());},"Invalidated spectra reused");
    rejects<std::logic_error>([&]{fir.residual(input.data(),q.data(),linear.data(),residual.data());},"Invalidated residual accepted");
  }
  // Different reference must replace every cached spectrum, including block history.
  std::fill(reference.begin(),reference.end(),Float{}); reference[0]={1,0};reference[N-1]={-.5,.25};
  fir.prepare(reference.data(),N);fir.estimate(q.data(),linear.data());near(linear,direct(reference,q,false));
  auto bad=reference;bad[N-1]={std::numeric_limits<float>::infinity(),0};
  rejects<std::domain_error>([&]{fir.prepare(bad.data(),N);},"Nonfinite reference accepted");
  rejects<std::logic_error>([&]{fir.estimate(q.data(),linear.data());},"Failed preparation retained old cache");
  fir.prepare(reference.data(),N);fir.estimate(q.data(),linear.data());near(linear,direct(reference,q,false));
  auto badInput=input;badInput[0]={std::numeric_limits<float>::quiet_NaN(),0};
  rejects<std::domain_error>([&]{fir.residual(badInput.data(),q.data(),linear.data(),residual.data());},"Nonfinite residual input accepted");
  rejects<std::invalid_argument>([&]{fir.prepare(reference.data(),N-1);},"Wrong reference length accepted");
  check(fir.fft()>=2*B && fir.storageBytes()>0,"Missing bounded workspace accounting");
}
void coefficientPrecision(){
  constexpr uint32_t N=32;std::vector<Float> x(N);x[0]={1,0};
  std::vector<Double> q{{1+std::ldexp(1.,-30),std::ldexp(1.,-31)}},out(N);
  check(Double(Float(q[0]))!=q[0],"Precision fixture did not detect FP32 coefficient cast");
  BlockedBaseline fir(N,1);fir.prepare(x.data(),N);fir.estimate(q.data(),out.data());
  near(out,direct(x,q,false));
  check(std::abs(out[0]-Double(Float(q[0])))>5e-10,"Coefficient was rounded before FP64 FIR");
}
// Compare the former scratch-plus-copy path with an offset mapped destination.
// Host-only storage models both direct and staged layouts; Vulkan fence/cache
// visibility still requires the physical backend qualification tests.
void directResidualUpload(){
  for(const auto& shape:{std::pair<uint32_t,uint32_t>{32,1},{1057,5},{4099,1261},{37,37}}){
    const uint32_t n=shape.first,b=shape.second,capacity=2*n;
    BlockedBaseline fir(n,b);
    std::vector<Float> reference(n),input(3*n),scratch(capacity),expected(capacity);
    std::vector<Double> q(3*b),oldLinear(capacity),newLinear(capacity);
    for(size_t i=0;i<input.size();++i)input[i]={float(int(i%23)-11)/16,float(int(i%17)-8)/32};
    for(size_t i=0;i<q.size();++i)q[i]={double(int(i%13)-6)/128,double(int(i%7)-3)/256};
    const auto originalInput=input;
    for(uint32_t prefix:{0u,n}){
      const Float guard{97,-101};
      std::vector<Float> mapped(size_t(prefix)+capacity+2,guard);
      auto* destination=mapped.data()+prefix+1;
      const auto checkGuards=[&]{
        check(std::all_of(mapped.begin(),mapped.begin()+prefix+1,
          [&](Float value){return value==guard;}) && mapped.back()==guard,
          "Direct residual overwrote reference prefix or allocation guard");
      };
      for(uint32_t frame=0;frame<2;++frame){
        for(uint32_t i=0;i<n;++i)reference[i]={float(int((i+frame)%19)-9)/16,float(int(i%11)-5)/32};
        reference.front()={7,-3};reference.back()={-5,float(11+frame)};
        fir.prepare(reference.data(),n);
        // Full, partial, then full: the same mapped address is reused without
        // stale residuals, stale channel padding, or an intervening allocation.
        for(uint32_t first:{0u,2u,0u}){
          const uint32_t active=std::min(2u,3-first),count=active*n;
          for(uint32_t channel=0;channel<active;++channel){
            const size_t source=size_t(first+channel)*n,weights=size_t(first+channel)*b;
            fir.estimateResidual(input.data()+source,q.data()+weights,
              oldLinear.data()+channel*n,scratch.data()+channel*n);
            fir.estimateResidual(input.data()+source,q.data()+weights,
              newLinear.data()+channel*n,destination+channel*n);
          }
          blah2::copyClutterBatch(expected.data(),capacity,scratch.data(),count);
          blah2::padClutterBatch(destination,capacity,count);
          check(std::memcmp(oldLinear.data(),newLinear.data(),size_t(count)*sizeof(Double))==0,
            "Direct upload changed FP64 baseline bits");
          check(std::memcmp(expected.data(),destination,size_t(capacity)*sizeof(Float))==0,
            "Direct residual differs bitwise from scratch-plus-copy");
          check(input==originalInput,"Direct upload changed authoritative surveillance");
          checkGuards();
        }
      }
      for(float bad:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}){
        // Fail in the last sample of the second active channel, after earlier
        // mapped writes. A partially written upload must never be committed.
        auto invalid=input;invalid[capacity-1]={bad,0};
        bool committed=false;
        rejects<std::domain_error>([&]{
          for(uint32_t channel=0;channel<2;++channel)
            fir.estimateResidual(invalid.data()+channel*n,q.data()+channel*b,
              newLinear.data()+channel*n,destination+channel*n);
          blah2::padClutterBatch(destination,capacity,capacity);
          committed=true;
        },"Nonfinite direct residual was accepted");
        check(!committed,"Rejected direct residual was committed");
        checkGuards();
        // Match the pipeline's frame rejection/reset and reuse its allocation.
        fir.invalidate();
        rejects<std::logic_error>([&]{fir.estimateResidual(input.data(),q.data(),newLinear.data(),destination);},
          "Rejected frame reused invalidated reference");
        fir.prepare(reference.data(),n);
        fir.estimateResidual(input.data(),q.data(),oldLinear.data(),scratch.data());
        fir.estimateResidual(input.data(),q.data(),newLinear.data(),destination);
        blah2::copyClutterBatch(expected.data(),capacity,scratch.data(),n);
        blah2::padClutterBatch(destination,capacity,n);
        check(std::memcmp(expected.data(),destination,size_t(capacity)*sizeof(Float))==0,
          "Rejected direct upload contaminated the next partial batch");
        checkGuards();
      }
    }
  }
}
void geometryAndPlanner(){
  rejects<std::invalid_argument>([]{BlockedBaseline x(0,1);},"Zero samples accepted");
  rejects<std::invalid_argument>([]{BlockedBaseline x(32,0);},"Zero taps accepted");
  rejects<std::invalid_argument>([]{BlockedBaseline x(32,33);},"Taps above samples accepted");
  rejects<std::invalid_argument>([]{BlockedBaseline x(UINT32_MAX,1);},"Unaddressable sample geometry accepted");
  // FFT-length overflow must reject before attempting large reference/cache allocations.
  rejects<std::invalid_argument>([]{BlockedBaseline x(INT32_MAX,INT32_MAX);},"FFT overflow geometry accepted");
  check(fftw_init_threads()!=0,"FFTW threads initialization failed");
  const int previous=blah2::fftw_planner_threads();
  struct Restore{int previous;~Restore(){blah2::set_fftw_planner_threads(previous);}} restore{previous};
  blah2::set_fftw_planner_threads(3);
  for(unsigned iteration=0;iteration<3;++iteration){
    {BlockedBaseline fir(32,5);check(blah2::fftw_planner_threads()==3,"Construction changed planner thread setting");}
    check(blah2::fftw_planner_threads()==3,"Destruction changed planner thread setting");
  }
}
void immutableCurrentReference(){
  constexpr uint32_t n=32,taps=3;BlockedBaseline fir(n,taps);
  std::vector<Float> reference(n),surveillance(n),residual(n);
  std::vector<Double> q={{.125,.25},{-.0625,.125},{.03125,-.0625}},first(n),next(n);
  for(uint32_t i=0;i<n;++i){reference[i]={float(i%7),float(i%5)};surveillance[i]={float(i),-float(i)};}
  fir.prepare(reference.data(),n);fir.estimateResidual(surveillance.data(),q.data(),first.data(),residual.data());
  for(auto& value:surveillance)value+=Float{31,-17};
  fir.estimateResidual(surveillance.data(),q.data(),next.data(),residual.data());
  check(next==first,"Frozen FIR estimate depends on current surveillance");
  for(auto& value:reference)value*=2.f;
  fir.prepare(reference.data(),n);fir.estimateResidual(surveillance.data(),q.data(),next.data(),residual.data());
  near(next,direct(reference,q,false));
  for(uint32_t i=0;i<n;++i)check(std::abs(next[i]-2.*first[i])<1e-12,"Frozen FIR reused stale reference spectra");
  std::fill(reference.begin(),reference.end(),Float{});
  fir.prepare(reference.data(),n);fir.estimateResidual(surveillance.data(),q.data(),next.data(),residual.data());
  near(next,std::vector<Double>(n));check(residual==surveillance,"Frozen zero-reference FIR unexpectedly solved or modified input");
}
}
int main()try{
  boundaryAndLifetime();coefficientPrecision();geometryAndPlanner();covarianceBoundaries();directResidualUpload();immutableCurrentReference();
  std::cout<<"PASS CPU clutter refinement direct FP64, circular covariance through 1261 taps, copied/direct residual parity and rejection, boundaries, coefficient precision, lifetime, geometry and planner restoration\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"FAIL CPU clutter refinement: "<<error.what()<<'\n';return 1;}
