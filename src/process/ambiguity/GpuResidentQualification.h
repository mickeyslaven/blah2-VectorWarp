#pragma once
#include "process/tracker/ConfirmationContract.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace blah2::resident {
// Qualification belongs to the immutable worker/device/geometry cohort, not
// to a contiguous run of capture samples. A gap clears only per-frame checks.
class Qualification {
 const uint32_t required_,depth_;
 uint32_t completed_=0,verified_=0,observed_=0;
 bool matureProducts_=false;
 bool negative_=false,positive_=false;
public:
 enum Check:uint32_t {reference=1,filtered=2,maps=4,products=8};
 // N<=255 permits at most32 depth8 credits:8 negative +32*8 positive CPIs.
 static constexpr uint32_t maximumStartupFrames=264;
 static uint32_t confirmationFrame(uint32_t depth,uint32_t minimum){
  if(!tracker_confirmation::confirmationDepthValid(depth)||!minimum||minimum>255)
   throw std::invalid_argument("Private qualification confirmation depth/M invalid");
  const uint32_t frames=2*depth+
   tracker_confirmation::confirmationMinimumWindow(minimum,depth)-1;
  if(frames>maximumStartupFrames)
   throw std::invalid_argument("Private qualification exceeds its 264-frame confirmation bound");
  return frames;
 }
 static uint32_t startupCount(uint32_t baseline,uint32_t depth,uint32_t minimum=0,
  uint32_t window=0,uint32_t misses=0){
  if(minimum)tracker_confirmation::validate(minimum,window,misses,depth);
  return std::max(baseline,confirmationFrame(depth,minimum?minimum:1)-depth+1);
 }
 explicit Qualification(uint32_t required,uint32_t depth=1):required_(required),depth_(depth){
  if(!required||(depth!=1&&depth!=2&&depth!=4&&depth!=8)||
     uint64_t(required)+depth-1>maximumStartupFrames)
   throw std::invalid_argument("Resident qualification count/depth invalid");
 }
 bool pending()const{return completed_<required_;}
 uint32_t completed()const{return completed_;}
 void control(bool positive,bool mature,uint32_t detections,uint64_t tracks,
  uint64_t confirmedTracks,bool requireTrack,bool confirmationDue){
  if(!pending())throw std::logic_error("Private qualification control after admission");
  if(confirmedTracks>tracks)throw std::logic_error("Private qualification track counts invalid");
  if(!positive){
   if(detections||tracks)throw std::logic_error("Private qualification negative control accepted a detection/track");
   if(mature)negative_=true;
  }else if(mature){
   if(!detections||(requireTrack&&confirmationDue&&!confirmedTracks))
    throw std::logic_error("Private qualification positive control produced no matching detection/track");
   if(!requireTrack||confirmedTracks)positive_=true;
  }
 }
 void requireControls()const{
  if(!negative_||!positive_)
   throw std::logic_error("Private qualification requires mature positive and negative controls");
 }
 void resetInput()noexcept{verified_=0;matureProducts_=false;}
 void begin()noexcept{verified_=0;matureProducts_=false;}
 void checked(Check check){
  if(!pending())throw std::logic_error("Resident CPU oracle after qualification");
  if(check==products&&depth_>1)
   throw std::logic_error("Temporal products require acknowledged mature numerical qualification");
  verified_|=uint32_t(check);
  if(check==products)matureProducts_=true;
 }
 void checkedProducts(uint32_t available){
  if(!pending()||!available||available>depth_)
   throw std::logic_error("Resident numerical product history invalid");
  verified_|=uint32_t(products);matureProducts_=available==depth_;
 }
 void requireNumeric()const{
  if(pending()&&(verified_&7u)!=7u)throw std::logic_error("Resident startup oracle not completed");
 }
 void requireComplete()const{
  if(pending()&&verified_!=15u)throw std::logic_error("Resident startup oracle missing before commit");
 }
 void published(){requireComplete();if(pending()){
  ++observed_;if(matureProducts_)++completed_;
 }verified_=0;matureProducts_=false;}
 // Private pre-capture transactions use exactly the live qualification gate.
 // settle acknowledges committed worker ownership only: it must not publish
 // synthetic products. Admission is allowed only after the final reset returns.
 template<class Prepare,class Settle,class Reset>
 void prepareStartup(Prepare&& prepare,Settle&& settle,Reset&& reset){
  if(completed_||verified_)
   throw std::logic_error("Resident startup qualification requires a fresh cohort");
  while(pending()){
   if(uint64_t(observed_)>=uint64_t(required_)+depth_-1)
    throw std::logic_error("Resident temporal startup history did not mature within its configured warmup bound");
   begin();prepare(observed_);requireComplete();settle();published();
  }
  reset();resetInput();
 }
};
}
