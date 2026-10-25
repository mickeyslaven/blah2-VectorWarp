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
  double nulledNorm=0;std::complex<double> alignedProjection{};
  for(uint32_t channel=0;channel<channels;++channel){
    nulledNorm+=std::norm(nulled[channel]);
    alignedProjection+=std::conj(direct[channel])*nulled[channel];
  }
  REQUIRE(nulledNorm==Catch::Approx(1).epsilon(1e-12));
  REQUIRE(std::abs(alignedProjection)==Catch::Approx(.1).epsilon(1e-12));
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

TEST_CASE("ideal and measured fusion share the exact normalized null contract")
{
  constexpr uint32_t beams=2,channels=5;
  const std::array<std::complex<double>,5> direct{{
    std::polar(1.0,.1),std::polar(.8,-.7),std::polar(1.2,.4),
    std::polar(.6,1.1),std::polar(1.4,-1.3)}};
  const double scale[5]={.5,.8,1.1,1.7,2.0};
  std::array<std::complex<double>,8> offAxis{};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  for(uint32_t channel=0;channel<channels;++channel){
    offAxis[channel]=direct[channel]*std::polar(1.0,.31*double(channel+1));
    manifold[channels+channel]=std::complex<float>(offAxis[channel]/direct[channel]);
  }
  const std::array<const std::complex<double>*,2> sources{{direct.data(),offAxis.data()}};
  for(const bool whiten:{false,true})for(const auto* source:sources){
    const auto baseline=blah2::resident_fusion::normalizedWeights(
      [&]{std::array<std::complex<double>,8> value{};for(uint32_t i=0;i<channels;++i)value[i]=source[i];return value;}(),
      direct.data(),scale,channels,0,whiten);
    std::complex<double> baselineProjection{};double directNorm=0;
    for(uint32_t i=0;i<channels;++i)directNorm+=std::norm(direct[i]);
    const double anchorScale=1/std::sqrt(directNorm);
    for(uint32_t i=0;i<channels;++i)
      baselineProjection+=std::conj(direct[i]*anchorScale)*baseline[i];
    for(const double db:{0.0,6.0,20.0}){
      const double fraction=1-std::pow(10.0,-db/20.0);
      const auto ideal=blah2::resident_fusion::normalizedWeights(
        [&]{std::array<std::complex<double>,8> value{};for(uint32_t i=0;i<channels;++i)value[i]=source[i];return value;}(),
        direct.data(),scale,channels,fraction,whiten);
      const uint32_t beam=source==direct.data()?0:1;
      const auto measured=blah2::resident_fusion::measuredWeights(manifold,beams,channels,
        beam,0,direct.data(),scale,fraction,whiten);
      for(const auto& value:{ideal,measured}){
        double norm=0;std::complex<double> projection{};
        for(uint32_t i=0;i<channels;++i){
          norm+=std::norm(value[i]);
          projection+=std::conj(direct[i]*anchorScale)*value[i];
        }
        REQUIRE(norm==Catch::Approx(1).epsilon(1e-10));
        REQUIRE(std::abs(projection)==Catch::Approx(
          std::pow(10.0,-db/20.0)*std::abs(baselineProjection)).epsilon(1e-6));
      }
    }
  }
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
