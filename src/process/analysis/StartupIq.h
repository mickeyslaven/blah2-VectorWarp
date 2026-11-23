#pragma once

#include "data/IqData.h"
#include "data/Map.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace blah2::startup {

// Keep reference conditioning independent of the calibrated echo strength.
inline constexpr double directAmplitude=64;
inline constexpr double noiseAmplitude=.25;
inline constexpr double commonVariance=2.0/3.0;

// Bounded private qualification stimulus, never a capture/replay adapter. The
// caller selects a valid CFAR cell and derives amplitude from its actual gates.
struct Target {
 uint32_t delay=0;
 double dopplerHz=0,sampleRate=0,amplitude=0;
 std::vector<std::complex<double>> response;
 double phaseCycles=0;
};
// Positive controls must exercise motion, not the stationary-clutter veto. A
// bounded four-bin Doppler excursion repeats without drifting out of the CAF;
// delay and carrier phase are integrated from that same velocity. Only private
// synthesized IQ uses this trajectory. Production motion gates are unchanged.
class TargetMotion {
 double initialDoppler_=0,delay_=0,phase_=0,step_=0;
public:
 template<class Axis>void advance(Target& target,unsigned frame,const Axis& axis,
   double cpi,double delayDriftPerHz,unsigned persistence){
  if(axis.size()<2||!(cpi>0)||!std::isfinite(delayDriftPerHz))
   throw std::invalid_argument("Private moving target geometry invalid");
  if(!frame){
   initialDoppler_=target.dopplerHz;delay_=target.delay;phase_=0;
   const double bin=std::abs(axis[1]-axis[0]);
   const auto bounds=std::minmax_element(axis.begin(),axis.end());
   step_=std::copysign(4*bin,initialDoppler_);
   if(!(bin>0)||initialDoppler_+step_<*bounds.first||initialDoppler_+step_>*bounds.second)
    throw std::invalid_argument("Private moving target needs four nonstationary Doppler bins");
  }else{
   delay_+=target.dopplerHz*delayDriftPerHz;
   phase_=std::remainder(phase_+target.dopplerHz*cpi,1.0);
  }
  const unsigned half=std::max(1u,persistence? persistence-1:11u);
  const unsigned position=frame%(2*half);
  target.dopplerHz=initialDoppler_+step_*double(position<=half?position:2*half-position)/half;
  if(!(delay_>=1)||delay_>double(UINT32_MAX))
   throw std::invalid_argument("Private moving target left delay geometry");
  target.delay=uint32_t(std::llround(delay_));target.phaseCycles=phase_;
 }
};
inline void fillTargetIq(const std::vector<IqData*>& channels,uint32_t samples,
 unsigned frame,const Target& target,bool positive){
 if(channels.empty()||target.response.size()!=channels.size()||!samples||
    !target.delay||target.delay>=samples/4||!std::isfinite(target.sampleRate)||
    target.sampleRate<=0||!std::isfinite(target.dopplerHz)||!std::isfinite(target.phaseCycles)||
    !std::isfinite(target.amplitude)||target.amplitude<=0||target.amplitude>4096)
  throw std::invalid_argument("Private qualification target is outside bounded IQ geometry");
 std::vector<std::deque<std::complex<double>>*> output;
 for(auto* channel:channels){
  if(!channel||channel->get_n()<samples)
   throw std::invalid_argument("Private qualification IQ storage is too small");
  output.push_back(&channel->resize_for_write(samples));
 }
 for(auto value:target.response)if(!std::isfinite(value.real())||
   !std::isfinite(value.imag())||std::norm(value)>1.000001)
  throw std::invalid_argument("Private qualification spatial response invalid");
 uint32_t random=0x6d2b79f5u+frame*0x9e3779b9u;
 const auto scalar=[&]{random=random*1664525u+1013904223u;
  return (double(random>>16)-32768.0)/32768.0;};
 std::vector<std::complex<double>> common(samples+target.delay);
 for(auto& value:common){const double real=scalar(),imaginary=scalar();value={real,imaginary};}
 const double phaseStep=2*std::acos(-1.0)*target.dopplerHz/target.sampleRate;
 for(uint32_t sample=0;sample<samples;++sample){
  const auto echo=positive?target.amplitude*common[sample]*
   std::polar(1.0,2*std::acos(-1.0)*target.phaseCycles+phaseStep*sample):std::complex<double>{};
  for(size_t channel=0;channel<channels.size();++channel){
   const double real=scalar(),imaginary=scalar();
   const auto value=directAmplitude*common[sample+target.delay]+
    noiseAmplitude*std::complex<double>{real,imaginary}+target.response[channel]*echo;
   // Both backends receive the exact same canonical FP32-representable values.
   (*output[channel])[sample]={double(float(value.real())),double(float(value.imag()))};
  }
 }
}

// Private deterministic broadband IQ for device/geometry qualification only.
// Coherent direct energy plus independent channel noise keeps clutter solves
// nondegenerate. Power-of-two scaling exercises three different input levels.
// No capture identity, timestamp, tracker, recording or publication is involved.
inline void fillIq(const std::vector<IqData*>& channels, uint32_t samples,
    unsigned frame) {
  if (channels.empty() || !samples)
    throw std::invalid_argument("Startup IQ geometry is empty");
  std::vector<std::deque<std::complex<double>>*> output;
  output.reserve(channels.size());
  for (auto* channel : channels) {
    if (!channel || channel->get_n() < samples)
      throw std::invalid_argument("Startup IQ storage is too small");
    output.push_back(&channel->resize_for_write(samples));
  }
  uint32_t random = 0x6d2b79f5u + frame * 0x9e3779b9u;
  const auto scalar = [&] {
    random = random * 1664525u + 1013904223u;
    return (double(random >> 16) - 32768.0) / 32768.0;
  };
  const double scale = std::ldexp(1.0, int(frame % 3) * 4 - 4);
  for (uint32_t sample = 0; sample < samples; ++sample) {
    const double real = scalar(), imaginary = scalar();
    const std::complex<double> common{real, imaginary};
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
      const double noiseReal = scalar(), noiseImaginary = scalar();
      (*output[channel])[sample] = scale *
        (common * std::complex<double>{1.0, double(channel) / 16.0} +
         std::complex<double>{noiseReal, noiseImaginary} * .25);
    }
  }
}

// Private stimulus calibration, not a detector policy. Only independent CPU
// negative-control maps may contribute; no IQ/maps or production state persist.
class TargetCalibration {
 double noiseToUnitEcho_=0;
 unsigned updates_=0,depth_=0;
 static void positive(double value){
  if(!std::isfinite(value)||!(value>0))
   throw std::invalid_argument("Private target calibration has invalid noise/gain");
 }
public:
 void clear()noexcept{noiseToUnitEcho_=0;updates_=0;depth_=0;}
 unsigned updates()const noexcept{return updates_;}
 // Unwindowed block CAF: each positive lag retains correlation-delay pairs.
 // The Doppler FFT is coherent across blocks; its within-block phase ramp is
 // the finite geometric sum below, not an assumed requested-sample count.
 static double coherentPairs(const Target& target,unsigned correlation,
   unsigned blocks,double middle){
  if(!blocks||correlation<=target.delay||!std::isfinite(target.sampleRate)||
     !(target.sampleRate>0)||!std::isfinite(target.dopplerHz)||!std::isfinite(middle))
   throw std::invalid_argument("Private target calibration CAF geometry invalid");
  const double count=correlation-target.delay;
  const double x=std::acos(-1.)*(target.dopplerHz-middle)/target.sampleRate;
  const auto sinc=[](double v){return v==0?1.:std::sin(v)/v;};
  const double pairs=blocks*count*std::abs(sinc(count*x)/sinc(x));
  positive(pairs);return pairs;
 }
 template<class FusionPower>
 static double unitEchoPower(const Target& target,unsigned correlation,unsigned blocks,double middle,
   const std::vector<uint32_t>& referenceChannels,
   const std::vector<std::complex<double>>& referenceWeights,
   const std::vector<uint32_t>& surveillanceChannels,FusionPower&& fusionPower){
  if(referenceChannels.empty()||
     referenceChannels.size()!=referenceWeights.size()||surveillanceChannels.empty())
   throw std::invalid_argument("Private target calibration exceeds bounded geometry");
  std::complex<double> reference{},echo{};
  for(size_t i=0;i<referenceChannels.size();++i){
   const auto weight=std::conj(referenceWeights[i]);
   reference+=weight;echo+=weight*target.response.at(referenceChannels[i]);
  }
  positive(std::norm(reference));
  // Clutter removes the target component shared with the synthesized reference.
  const auto commonEcho=echo/reference;
  const double gain=directAmplitude*std::abs(reference)*commonVariance*
   coherentPairs(target,correlation,blocks,middle);
  std::vector<std::complex<double>> unit;
  for(auto channel:surveillanceChannels)unit.push_back(gain*(target.response.at(channel)-commonEcho));
  const double power=fusionPower(unit);positive(power);return power;
 }
 void observeNegative(const Target& target,const Map<std::complex<double>>& map,
   int guard,int train,double unitPower,unsigned depth){
  if(updates_>=8||guard<0||train<1||!depth||depth>8||(updates_&&depth_!=depth))
   throw std::invalid_argument("Private target calibration exceeds bounded geometry");
  const auto column=std::find(map.delay.begin(),map.delay.end(),int(target.delay));
  const auto row=std::find(map.doppler.begin(),map.doppler.end(),target.dopplerHz);
  if(column==map.delay.end()||row==map.doppler.end())
   throw std::invalid_argument("Private target calibration cell absent");
  const int center=int(column-map.delay.begin()),columns=int(map.delay.size());
  double noise=0;unsigned cells=0;
  for(int at=std::max(0,center-guard-train);at<=std::min(columns-1,center+guard+train);++at){
   if(std::abs(at-center)<=guard)continue; // Never CUT or guard cells.
   const double power=std::norm(map.data[row-map.doppler.begin()][at]);
   if(!std::isfinite(power)||power<0)
    throw std::invalid_argument("Private target calibration noise is nonfinite");
   noise+=power;++cells;
  }
  if(!cells)throw std::invalid_argument("Private target calibration has no training cells");
  positive(noise);positive(unitPower);
  const double ratio=noise/(cells*unitPower);positive(ratio);
  noiseToUnitEcho_+=ratio;positive(noiseToUnitEcho_);++updates_;depth_=depth;
 }
 double amplitude(unsigned depth,double gate)const{
  if(!depth||depth>8||!updates_||depth_!=depth)
   throw std::invalid_argument("Private target calibration lacks its negative window");
  positive(gate);
  // A 3 dB stimulus margin over the actual CFAR/track birth gate; this never
  // changes either gate. The caller selects the same negative detector bank
  // whose CFAR gate is used; a fixed-cell TBD control uses baseline2, not a
  // moving bank whose training paths cross the control itself.
  const double value=std::sqrt(2*gate*noiseToUnitEcho_/updates_);
  positive(value);
  if(value>4096)throw std::invalid_argument("Private target calibrated amplitude exceeds bound");
  return value;
 }
};

} // namespace blah2::startup
