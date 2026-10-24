#include "process/ambiguity/ResidentFusionWeights.h"
#include "process/analysis/ProductionReplayContract.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
template<size_t N> std::array<std::complex<double>,N> legacyIdealOrdering(
 std::array<std::complex<double>,N> steering,
 const std::array<std::complex<double>,N>& direct,const double* channelScale,
 double nullFraction,bool whiten){
 double directNorm=0,steeringNorm=0;for(size_t channel=0;channel<N;++channel){
  directNorm+=std::norm(direct[channel]);steeringNorm+=std::norm(steering[channel]);}
 const double steeringScale=std::sqrt(directNorm/steeringNorm);
 std::complex<double> projection{};for(size_t channel=0;channel<N;++channel){
  steering[channel]*=steeringScale;projection+=std::conj(direct[channel])*steering[channel];}
 projection*=nullFraction/directNorm;for(size_t channel=0;channel<N;++channel)
  steering[channel]-=projection*direct[channel];
 std::array<std::complex<double>,N> result{};
 if(whiten){double norm=0;for(size_t channel=0;channel<N;++channel){
   result[channel]=channelScale[channel]*steering[channel];norm+=std::norm(result[channel]);}
  for(auto& value:result)value/=std::sqrt(norm);
 }else for(size_t channel=0;channel<N;++channel)result[channel]=steering[channel]/std::sqrt(directNorm);
 return result;
}
}

TEST_CASE("resident measured fusion uses target to direct response ratios")
{
  constexpr uint32_t beams=4,channels=3,targetBeam=0;
  const std::array<std::complex<double>,3> direct{{
    std::polar(1.0/std::sqrt(3.0),.2),std::polar(1.0/std::sqrt(3.0),-.4),
    std::polar(1.0/std::sqrt(3.0),.7)}};
  const std::array<std::complex<double>,3> ratio{{
    std::polar(.7,-.3),std::polar(1.4,.8),std::polar(.9,-1.1)}};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  std::vector<std::complex<float>> directResponse{{.123456789f,-.987654321f},
    {.75f,.125f},{-.4f,.9f}};
  std::array<std::complex<double>,3> quantizedRatio{};
  for(uint32_t channel=0;channel<channels;++channel){
    manifold[targetBeam*channels+channel]=std::complex<float>(
      std::complex<double>(directResponse[channel])*ratio[channel]);
    quantizedRatio[channel]=std::complex<double>(manifold[targetBeam*channels+channel])/
      std::complex<double>(directResponse[channel]);
  }
  const double scale[3]={1,1,1};
  const auto weights=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,targetBeam,direct.data(),scale,0,false);
  double rawNorm=0;for(uint32_t channel=0;channel<channels;++channel)
    rawNorm+=std::norm(direct[channel]*quantizedRatio[channel]);
  for(uint32_t channel=0;channel<channels;++channel)
    REQUIRE(std::abs(weights[channel]-direct[channel]*quantizedRatio[channel]/std::sqrt(rawNorm))<1e-6);
}

TEST_CASE("resident measured fusion preserves legacy null-before-whitening ordering")
{
  constexpr uint32_t beams=4,channels=3;
  const std::array<std::complex<double>,3> direct{{
    {1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0}}};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  std::vector<std::complex<float>> directResponse(channels,{1,0});
  const double unity[3]={1,1,1};
  const auto nulled=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,0,direct.data(),unity,.9,false);
  for(uint32_t channel=0;channel<channels;++channel)
    REQUIRE(std::abs(nulled[channel])==Catch::Approx(.1/std::sqrt(3.0)).epsilon(1e-12));
  manifold[channels]={1,0};manifold[channels+1]={0,1};manifold[channels+2]={-1,0};
  const double inverseNoise[3]={.5,1,2};
  const auto whitened=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,1,direct.data(),inverseNoise,.9,true);
  std::array<std::complex<double>,3> steering{{direct[0],{0,1/std::sqrt(3.0)},
    {-1/std::sqrt(3.0),0}}};
  const auto expected=legacyIdealOrdering(steering,direct,inverseNoise,.9,true);
  for(uint32_t channel=0;channel<channels;++channel)
    REQUIRE(std::abs(whitened[channel]-expected[channel])<1e-12);
}

TEST_CASE("measured fusion does not change active ideal whitening semantics")
{
  constexpr uint32_t beams=4,channels=3;
  const std::array<std::complex<double>,3> direct{{
    {1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0}}};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  std::vector<std::complex<float>> directResponse(channels,{1,0});
  const double inverseNoise[3]={.5,1,2};
  const auto nulled=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,0,direct.data(),inverseNoise,.99,true);
  const auto expected=legacyIdealOrdering(direct,direct,inverseNoise,.99,true);
  for(uint32_t channel=0;channel<channels;++channel)
    REQUIRE(std::abs(nulled[channel]-expected[channel])<1e-12);
}

TEST_CASE("ideal and equivalent measured surveillance banks share whitening and null ordering")
{
  constexpr uint32_t beams=4,channels=3,targetBeam=1;
  const std::array<std::complex<double>,3> direct{{
    std::polar(.8,.2),std::polar(1.1,-.6),std::polar(.6,1.0)}};
  std::array<std::complex<double>,8> steering{};
  steering[0]=direct[0]*std::polar(1.0,.4);
  steering[1]=direct[1]*std::polar(1.0,-.3);
  steering[2]=direct[2]*std::polar(1.0,.8);
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  std::vector<std::complex<float>> directResponse(channels,{1,0});
  for(uint32_t channel=0;channel<channels;++channel)
    manifold[targetBeam*channels+channel]=std::complex<float>(steering[channel]/direct[channel]);
  const double inverseNoise[3]={.55,1.0,1.8};
  const auto ideal=blah2::resident_fusion::calibratedWeights(steering,channels,
    direct.data(),inverseNoise,.75,true);
  const auto measured=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,targetBeam,direct.data(),inverseNoise,.75,true);
  for(uint32_t channel=0;channel<channels;++channel)
    REQUIRE(std::abs(ideal[channel]-measured[channel])<1e-6);
}

TEST_CASE("production replay trial and temporal contract matches resident bounds")
{
  REQUIRE(blah2::production_replay::familywisePfa(.04, 4, 9) ==
    Catch::Approx(.04 / 36).epsilon(1e-15));
  REQUIRE_THROWS_AS(blah2::production_replay::familywisePfa(.04, 0, 1), std::invalid_argument);
  REQUIRE_NOTHROW(blah2::production_replay::temporal(1, 0, 0, true));
  REQUIRE_NOTHROW(blah2::production_replay::temporal(8, 2, 16, true));
  REQUIRE_THROWS_AS(blah2::production_replay::temporal(1, 1, 0, true), std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::production_replay::temporal(4, 0, 0, false), std::invalid_argument);
}

TEST_CASE("production replay covariance input is bounded and fail closed")
{
  REQUIRE_NOTHROW(blah2::production_replay::covariance({1,.2,.2,2},2));
  REQUIRE_THROWS_AS(blah2::production_replay::covariance({1,.2,.2,0},2), std::invalid_argument);
  REQUIRE_THROWS_AS(blah2::production_replay::covariance({1,.2,.2},2), std::invalid_argument);
}
