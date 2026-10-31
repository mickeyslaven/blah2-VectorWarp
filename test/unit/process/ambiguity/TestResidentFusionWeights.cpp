#include "process/ambiguity/ResidentFusionWeights.h"
#include "process/analysis/ProductionReplayContract.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("resident measured fusion preserves soft null after whitening without renormalizing it away")
{
  constexpr uint32_t beams=4,channels=3;
  const std::array<std::complex<double>,3> direct{{
    {1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0},{1/std::sqrt(3.0),0}}};
  std::vector<std::complex<float>> manifold(beams*channels,{1,0});
  std::vector<std::complex<float>> directResponse(channels,{1,0});
  const double unity[3]={1,1,1};
  const auto nulled=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,0,direct.data(),unity,.9,false);
  double nulledNorm=0;std::complex<double> alignedProjection{};
  for(uint32_t channel=0;channel<channels;++channel){
    nulledNorm+=std::norm(nulled[channel]);
    alignedProjection+=std::conj(direct[channel])*nulled[channel];
  }
  REQUIRE(nulledNorm==Catch::Approx(.01).epsilon(1e-12));
  REQUIRE(std::abs(alignedProjection)==Catch::Approx(.1).epsilon(1e-12));
  manifold[channels]={1,0};manifold[channels+1]={0,1};manifold[channels+2]={-1,0};
  const double inverseNoise[3]={.5,1,2};
  const auto whitenedBaseline=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,1,direct.data(),inverseNoise,0,true);
  const auto whitened=blah2::resident_fusion::measuredWeights(manifold,directResponse,
    beams,channels,1,direct.data(),inverseNoise,.9,true);
  double norm=0;std::complex<double> baselineProjection{},projection{};
  for(uint32_t channel=0;channel<channels;++channel){
    norm+=std::norm(whitened[channel]);
    baselineProjection+=std::conj(direct[channel])*whitenedBaseline[channel];
    projection+=std::conj(direct[channel])*whitened[channel];
  }
  REQUIRE(norm<=1.0);
  REQUIRE(std::abs(projection)==Catch::Approx(.1*std::abs(baselineProjection)).epsilon(1e-12));
}

TEST_CASE("ideal and measured fusion share the exact attenuation-only null contract")
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
    const auto baseline=blah2::resident_fusion::calibratedWeights(
      [&]{std::array<std::complex<double>,8> value{};for(uint32_t i=0;i<channels;++i)value[i]=source[i];return value;}(),
      channels,direct.data(),scale,0,whiten);
    std::complex<double> baselineProjection{};double directNorm=0;
    for(uint32_t i=0;i<channels;++i)directNorm+=std::norm(direct[i]);
    const double anchorScale=1/std::sqrt(directNorm);
    for(uint32_t i=0;i<channels;++i)
      baselineProjection+=std::conj(direct[i]*anchorScale)*baseline[i];
    for(const double db:{0.0,6.0,20.0}){
      const double fraction=1-std::pow(10.0,-db/20.0);
      const auto ideal=blah2::resident_fusion::calibratedWeights(
        [&]{std::array<std::complex<double>,8> value{};for(uint32_t i=0;i<channels;++i)value[i]=source[i];return value;}(),
        channels,direct.data(),scale,fraction,whiten);
      const uint32_t beam=source==direct.data()?0:1;
      const std::vector<std::complex<float>> directResponse(channels,{1,0});
      const auto measured=blah2::resident_fusion::measuredWeights(manifold,directResponse,
        beams,channels,beam,direct.data(),scale,fraction,whiten);
      for(const auto& value:{ideal,measured}){
        double norm=0;std::complex<double> projection{};
        for(uint32_t i=0;i<channels;++i){
          norm+=std::norm(value[i]);
          projection+=std::conj(direct[i]*anchorScale)*value[i];
        }
        REQUIRE(norm<=1.0+1e-10);
        REQUIRE(std::abs(projection)==Catch::Approx(
          std::pow(10.0,-db/20.0)*std::abs(baselineProjection)).epsilon(1e-6));
      }
    }
  }
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

TEST_CASE("resident null weights are equivariant to arbitrary common phase")
{
  constexpr uint32_t channels=5;
  std::array<std::complex<double>,8> steering{},rotatedSteering{};
  std::array<std::complex<double>,channels> direct{},rotatedDirect{};
  const auto phase=std::polar(1.0,1.23456789);
  const double scale[channels]={.7,.9,1.1,1.3,1.6};
  for(uint32_t channel=0;channel<channels;++channel){
    direct[channel]=std::polar(.5+.1*channel,-.2+.31*channel);
    steering[channel]=direct[channel]*std::polar(.8+.05*channel,.17*channel);
    rotatedDirect[channel]=phase*direct[channel];
    rotatedSteering[channel]=phase*steering[channel];
  }
  for(bool whiten:{false,true})for(double fraction:{0.0,.5,.9}){
    const auto baseline=blah2::resident_fusion::calibratedWeights(steering,channels,
      direct.data(),scale,fraction,whiten);
    const auto rotated=blah2::resident_fusion::calibratedWeights(rotatedSteering,channels,
      rotatedDirect.data(),scale,fraction,whiten);
    for(uint32_t channel=0;channel<channels;++channel)
      REQUIRE(std::abs(rotated[channel]-phase*baseline[channel])<1e-12);
  }
}

TEST_CASE("aligned resident null is phase equivariant without invented orthogonal power")
{
  constexpr uint32_t channels=5;
  std::array<std::complex<double>,8> steering{},rotatedSteering{};
  std::array<std::complex<double>,channels> direct{},rotatedDirect{};
  const auto phase=std::polar(1.0,2.17);
  const double unity[channels]={1,1,1,1,1};
  for(uint32_t channel=0;channel<channels;++channel){
    direct[channel]=std::polar(.4+.1*channel,.23*channel);
    steering[channel]=direct[channel];
    rotatedDirect[channel]=phase*direct[channel];
    rotatedSteering[channel]=phase*steering[channel];
  }
  const auto baseline=blah2::resident_fusion::calibratedWeights(steering,channels,
    direct.data(),unity,.9,false);
  const auto rotated=blah2::resident_fusion::calibratedWeights(rotatedSteering,channels,
    rotatedDirect.data(),unity,.9,false);
  double power=0;
  for(uint32_t channel=0;channel<channels;++channel){
    REQUIRE(std::abs(rotated[channel]-phase*baseline[channel])<1e-12);
    power+=std::norm(baseline[channel]);
  }
  REQUIRE(power==Catch::Approx(.01).epsilon(1e-12));
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
