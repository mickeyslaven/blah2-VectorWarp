#include "process/ambiguity/GpuClutterSolver.h"
#include "process/ambiguity/GpuClutterRefinement.h"
#include "process/clutter/WienerHopf.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace blah2::clutter_refinement {
struct DenseSolverTestAccess {
  static void factorAfterBackendFailure(DenseSolver& solver, const std::complex<float>* input) {
    solver.factorWith(input, [](arma::cx_mat& matrix) {
      // Armadillo may destroy/reset the failed factor. Neither that output
      // nor a second optimized attempt may become the compatibility input.
      matrix.fill(std::complex<double>(std::numeric_limits<double>::quiet_NaN(), 0));
      matrix.reset();
      return false;
    });
  }
};
}

namespace {
using Double = std::complex<double>;
using Float = std::complex<float>;
using blah2::clutter_refinement::DenseSolver;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class Exception, class Function> void rejects(Function function, const char* why) {
  try { function(); } catch (const Exception&) { return; }
  throw std::runtime_error(why);
}
void factor(DenseSolver& solver, const std::vector<Float>& input, bool compatibility) {
  if (compatibility)
    blah2::clutter_refinement::DenseSolverTestAccess::factorAfterBackendFailure(solver,input.data());
  else solver.factor(input.data());
}

arma::cx_mat covariance(const std::vector<Float>& input, uint32_t n, uint32_t b) {
  arma::cx_vec first(b);
  for (uint32_t i = 0; i < b; ++i) first[i] = std::conj(Double(input[i])) / double(n);
  // Same independent orientation and upper-factor oracle as WienerHopf.
  arma::cx_mat result = arma::toeplitz(first);
  for (uint32_t row = 0; row < b; ++row)
    for (uint32_t col = 0; col < row; ++col) result(row,col) = std::conj(result(row,col));
  return result;
}

arma::cx_mat scalarOracle(const arma::cx_mat& matrix, const arma::cx_mat& rhs) {
  const auto b = matrix.n_rows;
  arma::cx_mat lower(b,b,arma::fill::zeros), result(rhs), temporary(rhs);
  for (arma::uword row = 0; row < b; ++row)
    for (arma::uword col = 0; col <= row; ++col) {
      Double value = matrix(row,col);
      for (arma::uword k = 0; k < col; ++k) value -= lower(row,k)*std::conj(lower(col,k));
      lower(row,col) = row == col ? Double(std::sqrt(value.real()),0) : value/lower(col,col);
    }
  for (arma::uword channel = 0; channel < rhs.n_cols; ++channel) {
    for (arma::uword row = 0; row < b; ++row) {
      Double value = rhs(row,channel);
      for (arma::uword k = 0; k < row; ++k) value -= lower(row,k)*temporary(k,channel);
      temporary(row,channel) = value/lower(row,row);
    }
    for (int row = int(b)-1; row >= 0; --row) {
      Double value = temporary(row,channel);
      for (arma::uword k = row+1; k < b; ++k) value -= std::conj(lower(k,row))*result(k,channel);
      result(row,channel) = value/lower(row,row);
    }
  }
  return result;
}

void matrixOracle(uint32_t bins, double rho, bool compatibility = false) {
  constexpr uint32_t samples = 3072000, channels = 3;
  std::vector<Float> input(size_t(bins)*(channels+1));
  for (uint32_t i = 0; i < bins; ++i)
    input[i] = Float(double(samples) * std::polar(std::pow(rho,i), .071*i));
  const auto matrix = covariance(input,samples,bins);
  arma::cx_mat desired(bins,channels,arma::fill::zeros);
  desired(0,0) = {1,.2}; desired(bins-1,0) = {-.125,.03125};
  desired(1,1) = {.0001,-.0002}; desired(bins/2,1) = {-.25,.125};
  const arma::cx_mat rhs = matrix*desired;
  for (arma::uword i = 0; i < rhs.n_elem; ++i) input[bins+i] = Float(double(samples)*rhs[i]);
  arma::cx_mat rounded(bins,channels);
  for (arma::uword i = 0; i < rhs.n_elem; ++i) rounded[i] = Double(input[bins+i])/double(samples);
  arma::cx_mat upper, expected;
  require(arma::chol(upper,matrix),"CPU covariance oracle rejected fixture");
  expected = arma::solve(arma::trimatu(upper), arma::solve(arma::trimatl(upper.t()), rounded));
  DenseSolver solver(samples,bins,channels);
  factor(solver,input,compatibility);
  require(solver.compatibilityAttempts() == uint64_t(compatibility) &&
          solver.compatibilityFactors() == uint64_t(compatibility),
    "Unexpected compatibility path/counters for positive-definite covariance");
  const auto& actual = solver.solve(input.data());
  require(arma::norm(actual-expected,"fro") <= 2e-9 * std::max(1.,arma::norm(expected,"fro")),
    "Optimized GPU host solver differs from CPU WienerHopf algebra");
  require(arma::norm(matrix*actual-rounded,"fro") <= 1e-9*std::max(1.,arma::norm(rounded,"fro")),
    "Full covariance residual is incorrect");
  require(arma::norm(actual.col(2)) == 0,"Inactive channel acquired nonzero weights");
  if (bins <= 32)
    require(arma::norm(actual-scalarOracle(matrix,rounded),"fro") < 1e-11,
      "Optimized solver differs from pre-change scalar solver");
  // A correction solve reuses the factor; a new reference must replace it.
  for (size_t i = bins; i < input.size(); ++i) input[i] = Float{};
  require(arma::norm(solver.solve(input.data()),"fro") == 0,"Correction retained initial weights");
  std::fill(input.begin(),input.end(),Float{}); input[0] = Float(samples,0);
  input[bins] = Float(samples*.125, samples*-.25);
  factor(solver,input,compatibility);
  require(std::abs(solver.solve(input.data())(0,0)-Double(.125,-.25)) < 1e-15,
    "New reference retained stale factorization");
}

void rejectionGuards(bool compatibility = false) {
  constexpr uint32_t n=128,b=8;
  DenseSolver solver(n,b,1);
  std::vector<Float> input(2*b); input[0]={n,0}; input[b]={n,0};
  const auto rejectedFactor = [&] {
    rejects<std::domain_error>([&]{factor(solver,input,compatibility);},"Invalid covariance accepted");
    rejects<std::logic_error>([&]{solver.solve(input.data());},"Rejected covariance retained a factor");
  };
  factor(solver,input,compatibility); solver.solve(input.data());
  input[0]={0,0}; rejectedFactor();
  input[0]={n,0}; input[1]={n*2,0}; rejectedFactor();
  std::fill(input.begin(),input.begin()+b,Float(n,0)); rejectedFactor(); // rank one
  std::fill(input.begin(),input.end(),Float{}); input[0]={n,0};
  input[3]={std::numeric_limits<float>::infinity(),0}; rejectedFactor();
  input[3]={0,std::numeric_limits<float>::quiet_NaN()}; rejectedFactor();
  input[3]={0,0}; input[0]={n,n*.00002f}; rejectedFactor();
  input[0]={n,0}; factor(solver,input,compatibility);
  input[b+2]={std::numeric_limits<float>::quiet_NaN(),0};
  rejects<std::domain_error>([&]{solver.solve(input.data());},"Nonfinite RHS accepted");
  rejects<std::logic_error>([&]{solver.solve(input.data());},"Rejected solve retained prepared state");
  input[b+2]={0,0}; factor(solver,input,compatibility); solver.solve(input.data());
  solver.invalidate();
  rejects<std::logic_error>([&]{solver.solve(input.data());},"Invalidated frame reused covariance");
  rejects<std::invalid_argument>([]{DenseSolver s(0,1,1);},"Zero samples accepted");
  rejects<std::invalid_argument>([]{DenseSolver s(8,9,1);},"Excess taps accepted");
  rejects<std::invalid_argument>([]{DenseSolver s(8,2,9);},"Excess channels accepted");
  rejects<std::invalid_argument>([]{DenseSolver s(UINT32_MAX,UINT32_MAX,8);},
    "Unaddressable solver storage accepted");
  // A positive-definite matrix can pass LAPACK and still fail either of our
  // stricter legacy guards. This Toeplitz determinant approaches zero at
  // r2=2*r1*r1-1; power-of-two N preserves its FP32 correlations exactly.
  const double r1 = double(float(std::sqrt(.5)));
  const double boundary = 2*r1*r1-1;
  for (const auto& item : {std::pair<double,const char*>{1e-13,"guard=cholesky pivot"},
                         std::pair<double,const char*>{1e-9,"guard=conditioning"}}) {
    DenseSolver guarded(16,3,1);
    std::vector<Float> nearSingular{{16,0},{float(16*r1),0},{float(16*(boundary+item.first)),0}};
    bool rejected = false;
    try { factor(guarded,nearSingular,compatibility); }
    catch (const std::domain_error& error) {
      rejected = std::string(error.what()).find(item.second) != std::string::npos;
    }
    require(rejected,"LAPACK success bypassed a legacy pivot/conditioning guard");
    require(guarded.compatibilityAttempts() == uint64_t(compatibility) &&
            guarded.compatibilityFactors() == 0,
      "Post-LAPACK guard failure retried, or rejected legacy factor counted as accepted");
    rejects<std::logic_error>([&]{guarded.solve(nearSingular.data());},
      "Pivot/conditioning rejection left prepared state");
  }
  // The admitted imaginary diagonal is small relative to the covariance,
  // but amplified weights can still violate the original-matrix residual.
  // Neither successful factor path is permitted to waive that final guard.
  for (uint32_t i=0;i<b;++i) input[i]=Float(n*std::pow(.99,i),0);
  input[0]={n,n*1e-6f};
  std::fill(input.begin()+b,input.end(),Float{}); input[b]={n,0};
  factor(solver,input,compatibility);
  bool residualRejected = false;
  try { solver.solve(input.data()); }
  catch (const std::domain_error& error) {
    residualRejected = std::string(error.what()).find("guard=solve_residual") != std::string::npos;
  }
  require(residualRejected,"Compatibility factor bypassed original-covariance residual guard");
  rejects<std::logic_error>([&]{solver.solve(input.data());},
    "Residual rejection retained prepared state");
  // Nearly singular but full-rank covariance must still agree with CPU.
  matrixOracle(31,.9995,compatibility);
}

void actualBackendFailure() {
  // This indefinite matrix really fails POTRF on conforming backends. The
  // compatibility retry must also reject it, without accepting stale state.
  DenseSolver solver(128,8,1);
  std::vector<Float> input(16); input[0]={128,0}; input[8]={128,0};
  solver.factor(input.data()); solver.solve(input.data());
  input[1]={256,0};
  rejects<std::domain_error>([&]{solver.factor(input.data());},
    "Actual LAPACK failure escaped both factor guards");
  require(solver.compatibilityAttempts() == 1 && solver.compatibilityFactors() == 0,
    "Actual LAPACK failure did not report exactly one rejected compatibility retry");
  rejects<std::logic_error>([&]{solver.solve(input.data());},
    "Rejected automatic compatibility retry retained an old factor");
}

std::vector<Double> fullFftCovariance(const std::vector<Float>& reference, uint32_t bins) {
  using namespace blah2::clutter_refinement;
  auto work=allocate(reference.size());
  auto forward=makePlan(uint32_t(reference.size()),work.get(),FFTW_FORWARD);
  auto inverse=makePlan(uint32_t(reference.size()),work.get(),FFTW_BACKWARD);
  std::copy(reference.begin(),reference.end(),work.get());
  fftw_execute(forward.get());
  for(size_t i=0;i<reference.size();++i)work.get()[i]=Double(std::norm(work.get()[i]),0);
  fftw_execute(inverse.get());
  std::vector<Double> result(bins);
  for(uint32_t i=0;i<bins;++i)result[i]=work.get()[i]/double(reference.size());
  return result;
}

void productionCovarianceOracle() {
  constexpr uint32_t n=3072000,b=1261;
  std::mt19937 random(9831);std::uniform_int_distribution<int> noise(-1024,1024);
  std::vector<Float> reference(n);
  for(auto& value:reference)
    value=Float(512.f+noise(random)/1024.f,-256.f+noise(random)/1024.f)*1e-4f;
  // Same production N/B and authoritative rounded float input, with a strong
  // coherent reference over a weak full-rank broadband floor. Full-CPI FFT
  // is an independent oracle; production computes only small blocked FFTs.
  blah2::clutter_refinement::BlockedBaseline baseline(n,b);
  baseline.prepare(reference.data(),reference.size());
  const auto& actual=baseline.covariance();
  const auto expected=fullFftCovariance(reference,b);
  for(uint32_t i=0;i<b;++i)
    require(std::abs(actual[i]-expected[i])<=2e-12*std::abs(expected[0]),
      "Production strong-reference covariance differs from full FP64 FFT");
  arma::cx_mat matrix(b,b);
  for(uint32_t col=0;col<b;++col)
    for(uint32_t row=0;row<b;++row)
      matrix(row,col)=row>=col?expected[row-col]:std::conj(expected[col-row]);
  for(uint32_t i=0;i<b;++i)matrix(i,i)=Double(expected[0].real(),0);
  arma::cx_mat upper;
  require(arma::chol(upper,matrix),"Production full-FFT CPU oracle is not positive definite");
  std::vector<Float> rhs(2*b);
  arma::cx_vec rounded(b);
  for(uint32_t i=0;i<b;++i){
    rhs[i]={std::numeric_limits<float>::quiet_NaN(),0}; // No FP32 covariance reads.
    rhs[b+i]=Float(double(n)*expected[i]);
  }
  // Widen the stored FP32 RHS in a separate pass. GCC 11 can otherwise carry
  // the unrounded FP64 intermediate into the oracle during this same loop.
  for(uint32_t i=0;i<b;++i)
    rounded[i]=Double(rhs[b+i])/double(n);
  const arma::cx_vec weights=arma::solve(arma::trimatu(upper),
    arma::solve(arma::trimatl(upper.t()),rounded));
  DenseSolver solver(n,b,1);solver.factorCovariance(actual.data(),actual.size());
  const auto& solved=solver.solve(rhs.data());
  require(arma::norm(solved.col(0)-weights)<=2e-5*std::max(1.,arma::norm(weights)),
    "Strong-reference host covariance solve differs from full-FFT CPU oracle");
  require(arma::norm(matrix*solved.col(0)-rounded)<=1e-9*arma::norm(rounded),
    "Production host solve fails independent full-FFT covariance residual");
}

void covariancePrecisionAndState() {
  constexpr uint32_t n=8192,b=1261;
  std::vector<Float> reference(n,Float(1,0));reference[0]+=Float(1.f/64,0);
  // Exact analytic covariance: r[0]=N+2a+a^2 and all other lags=N+2a.
  // a^2 is below an FP32 ULP of r, but safely above the unchanged legacy
  // conditioning threshold. Rounding covariance to FP32 erases full rank.
  blah2::clutter_refinement::BlockedBaseline baseline(n,b);
  baseline.prepare(reference.data(),n);
  const auto& covariance=baseline.covariance();
  std::vector<Float> rounded(2*b);
  for(uint32_t i=0;i<b;++i){
    const Double exact(n+1./32+(i==0?1./4096:0),0);
    require(std::abs(covariance[i]-exact)<2e-11*std::abs(exact),
      "Weak full-rank covariance component lost in FP64 preparation");
    rounded[i]=Float(double(n)*exact);
  }
  DenseSolver legacy(n,b,1);
  rejects<std::domain_error>([&]{legacy.factor(rounded.data());},
    "FP32 rank-loss fixture unexpectedly retained its covariance floor");
  DenseSolver repaired(n,b,1);
  repaired.factorCovariance(covariance.data(),covariance.size());
  require(arma::norm(repaired.solve(rounded.data()),"fro")==0,
    "FP64 covariance changed zero RHS");
  auto bad=covariance;bad[1]={std::numeric_limits<double>::infinity(),0};
  rejects<std::domain_error>([&]{repaired.factorCovariance(bad.data(),bad.size());},
    "Nonfinite FP64 covariance accepted");
  rejects<std::logic_error>([&]{repaired.solve(rounded.data());},
    "Rejected FP64 covariance retained state");
  rejects<std::invalid_argument>([&]{repaired.factorCovariance(covariance.data(),b-1);},
    "FP64 covariance shape mismatch accepted");
  repaired.factorCovariance(covariance.data(),covariance.size());
  repaired.invalidate();
  rejects<std::logic_error>([&]{repaired.solve(rounded.data());},
    "Invalidated FP64 covariance reused");
}

std::vector<Float> correlations(const std::vector<Float>& x, const std::vector<Float>& y,
    uint32_t bins) {
  const size_t n=x.size(); std::vector<Float> result(2*bins);
  for (uint32_t lag=0;lag<bins;++lag) {
    Double a{},b{};
    for (size_t i=0;i<n;++i) {
      const Double shifted=Double(x[(i+n-lag)%n]);
      a+=Double(x[i])*std::conj(shifted); b+=Double(y[i])*std::conj(shifted);
    }
    // Match unnormalized FFT/product/IFFT followed by the host division by N.
    result[lag]=Float(double(n)*a); result[bins+lag]=Float(double(n)*b);
  }
  return result;
}

void cancellationOracle(int delay, bool compatibility = false, bool hostCovariance = false) {
  constexpr uint32_t n=2057,b=17; // Both full and short overlap-save blocks.
  std::mt19937 random(187); std::uniform_int_distribution<int> noise(-1024,1024);
  std::vector<Float> original(n),x(n),y(n),residual(n);
  for (auto& value:original) value=Float(noise(random)/32.f,noise(random)/32.f);
  for (uint32_t i=0;i<n;++i) x[i]=original[size_t((int64_t(i)-delay+n)%n)];
  for (uint32_t i=0;i<n;++i) {
    // Strong direct path, delayed multipath, and a weak time-varying echo.
    y[i]=Float(Double(x[i])*Double(1,.125)+Double(x[(i+n-5)%n])*.25+
      Double(x[(i+n-29)%n])*std::polar(.001,.051*i));
  }
  auto corr=correlations(x,y,b);
  blah2::clutter_refinement::BlockedBaseline baseline(n,b);
  baseline.prepare(x.data(),n);
  DenseSolver solver(n,b,1);
  if(hostCovariance){
    const auto& covariance=baseline.covariance();
    solver.factorCovariance(covariance.data(),covariance.size());
  }else factor(solver,corr,compatibility);
  const arma::cx_mat initial=solver.solve(corr.data());
  std::vector<Double> estimate(n),correction(n);
  baseline.estimate(initial.memptr(),estimate.data());
  baseline.residual(y.data(),initial.memptr(),estimate.data(),residual.data());
  corr=correlations(x,residual,b);
  const auto& weights=solver.solve(corr.data());
  // Independent linear convolution for the rounded correction coefficients.
  for (uint32_t i=0;i<n;++i)
    for (uint32_t tap=0;tap<b && tap<=i;++tap)
      correction[i]+=Double(Float(weights[tap]))*Double(x[i-tap]);
  IqData reference(n),surveillance(n);
  reference.replace(std::deque<Double>(original.begin(),original.end()));
  surveillance.replace(std::deque<Double>(y.begin(),y.end()));
  WienerHopf oracle(delay,delay+int(b),n);
  require(oracle.process(&reference,&surveillance),"Existing CPU cancellation rejected fixture");
  double error=0,power=0,inputPower=0,peakError=0,peakPower=0,peakInput=0;
  for (uint32_t i=0;i<n;++i) {
    const Double result=Double(y[i])-Double(Float(estimate[i]+Double(Float(correction[i]))));
    const Double expected=surveillance.view_data()[i];
    error+=std::norm(result-expected); power+=std::norm(expected); inputPower+=std::norm(Double(y[i]));
    peakError=std::max(peakError,std::norm(result-expected));
    peakPower=std::max(peakPower,std::norm(expected)); peakInput=std::max(peakInput,std::norm(Double(y[i])));
  }
  require(error <= std::max(std::max(power,inputPower*1e-8)*1e-8,1e-20),
    "Signed-delay/tail or weak-echo cancellation differs from CPU oracle");
  require(peakError <= std::max(std::max(peakPower,peakInput*1e-8)*1e-8,1e-20),
    "Signed-delay/tail peak exceeds CPU oracle contract");
}
}

int main() try {
  matrixOracle(17,.85); matrixOracle(1261,.985); rejectionGuards(); actualBackendFailure();
  cancellationOracle(-7); cancellationOracle(4);
  // Deterministic backend-failure injection is portable across LAPACK builds;
  // it does not claim to reproduce the unretained live covariance buffers.
  matrixOracle(17,.85,true); matrixOracle(1261,.985,true); rejectionGuards(true);
  cancellationOracle(-7,true); cancellationOracle(4,true);
  productionCovarianceOracle();covariancePrecisionAndState();
  cancellationOracle(-7,false,true);cancellationOracle(4,false,true);
  std::cout << "PASS dense FP64 clutter solver: production 3072000/1261 host covariance and strong-reference full-FFT oracle, FP32 rank-loss regression, scalar/CPU oracles, optimized and compatibility paths, batched RHS, near-singular/residual guards, nonfinite rejection, signed delay, FIR tail and weak echoes\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL dense clutter solver: " << error.what() << '\n'; return 1;
}
