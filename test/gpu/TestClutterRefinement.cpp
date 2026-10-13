// CPU-only independent oracle for the production FP64 baseline helper.
#include "process/ambiguity/GpuClutterRefinement.h"
#include "process/utility/FftwThreads.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
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
}
int main()try{
  boundaryAndLifetime();coefficientPrecision();geometryAndPlanner();
  std::cout<<"PASS CPU clutter refinement direct FP64, boundaries, coefficient precision, lifetime, geometry and planner restoration\n";
  return 0;
}catch(const std::exception& error){std::cerr<<"FAIL CPU clutter refinement: "<<error.what()<<'\n';return 1;}
