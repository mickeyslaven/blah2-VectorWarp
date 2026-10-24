#include "process/ambiguity/ResidentFusionWeights.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("resident measured fusion uses target to direct response ratios")
{
  constexpr uint32_t beams=4,channels=3,targetBeam=2,directBeam=0;
  const std::array<std::complex<double>,3> direct{{
    std::polar(1.0/std::sqrt(3.0),.2),std::polar(1.0/std::sqrt(3.0),-.4),
    std::polar(1.0/std::sqrt(3.0),.7)}};
  const std::array<std::complex<double>,3> ratio{{
    std::polar(.7,-.3),std::polar(1.4,.8),std::polar(.9,-1.1)}};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  for(uint32_t channel=0;channel<channels;++channel)
    manifold[targetBeam*channels+channel]=std::complex<float>(ratio[channel]);
  const double scale[3]={1,1,1};
  const auto weights=blah2::resident_fusion::measuredWeights(manifold,beams,channels,
    targetBeam,directBeam,direct.data(),scale,0,false);
  double rawNorm=0;for(uint32_t channel=0;channel<channels;++channel)
    rawNorm+=std::norm(direct[channel]*ratio[channel]);
  for(uint32_t channel=0;channel<channels;++channel)
    REQUIRE(std::abs(weights[channel]-direct[channel]*ratio[channel]/std::sqrt(rawNorm))<1e-6);
}

TEST_CASE("resident measured fusion preserves soft null and whitening")
{
  constexpr uint32_t beams=4,channels=3;
  const std::array<std::complex<double>,3> direct{{
    {1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0}}};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  const double unity[3]={1,1,1};
  const auto nulled=blah2::resident_fusion::measuredWeights(manifold,beams,channels,
    0,0,direct.data(),unity,.9,false);
  for(uint32_t channel=0;channel<channels;++channel)
    REQUIRE(std::abs(nulled[channel])==Catch::Approx(.1/std::sqrt(3.0)).epsilon(1e-12));
  manifold[channels]={1,0};manifold[channels+1]={0,1};manifold[channels+2]={-1,0};
  const double inverseNoise[3]={.5,1,2};
  const auto whitenedBaseline=blah2::resident_fusion::measuredWeights(manifold,beams,channels,
    1,0,direct.data(),inverseNoise,0,true);
  const auto whitened=blah2::resident_fusion::measuredWeights(manifold,beams,channels,
    1,0,direct.data(),inverseNoise,.9,true);
  double norm=0;for(uint32_t channel=0;channel<channels;++channel)norm+=std::norm(whitened[channel]);
  REQUIRE(norm==Catch::Approx(1).epsilon(1e-12));
  std::complex<double> baselineDirect{},nulledDirect{};
  for(uint32_t channel=0;channel<channels;++channel){
   baselineDirect+=std::conj(direct[channel])*whitenedBaseline[channel];
   nulledDirect+=std::conj(direct[channel])*whitened[channel];
  }
  REQUIRE(std::abs(nulledDirect)==Catch::Approx(.1*std::abs(baselineDirect)).epsilon(1e-12));
}

TEST_CASE("resident whitening does not reintroduce a direct-only null")
{
  constexpr uint32_t beams=4,channels=3;
  const std::array<std::complex<double>,3> direct{{
    {1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0}}};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  const double inverseNoise[3]={.5,1,2};
  const auto baseline=blah2::resident_fusion::measuredWeights(manifold,beams,channels,
    0,0,direct.data(),inverseNoise,0,true);
  const auto nulled=blah2::resident_fusion::measuredWeights(manifold,beams,channels,
    0,0,direct.data(),inverseNoise,.99,true);
  std::complex<double> baselineDirect{},nulledDirect{};
  for(uint32_t channel=0;channel<channels;++channel){
   baselineDirect+=std::conj(direct[channel])*baseline[channel];
   nulledDirect+=std::conj(direct[channel])*nulled[channel];
  }
  REQUIRE(std::abs(nulledDirect)<=.0100000001*std::abs(baselineDirect));
}
