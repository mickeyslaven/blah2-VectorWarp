#pragma once
#include <cstdint>
#include <stdexcept>

namespace blah2::resident {
// Qualification belongs to the immutable worker/device/geometry cohort, not
// to a contiguous run of capture samples. A gap clears only per-frame checks.
class Qualification {
 const uint32_t required_;
 uint32_t completed_=0,verified_=0;
public:
 enum Check:uint32_t {reference=1,filtered=2,maps=4,products=8};
 explicit Qualification(uint32_t required):required_(required){
  if(!required)throw std::invalid_argument("Resident qualification count is zero");
 }
 bool pending()const{return completed_<required_;}
 uint32_t completed()const{return completed_;}
 void resetInput()noexcept{verified_=0;}
 void begin()noexcept{verified_=0;}
 void checked(Check check){
  if(!pending())throw std::logic_error("Resident CPU oracle after qualification");
  verified_|=uint32_t(check);
 }
 void requireNumeric()const{
  if(pending()&&(verified_&7u)!=7u)throw std::logic_error("Resident startup oracle not completed");
 }
 void requireComplete()const{
  if(pending()&&verified_!=15u)throw std::logic_error("Resident startup oracle missing before commit");
 }
 void published(){requireComplete();if(pending())++completed_;verified_=0;}
 // Private pre-capture transactions use exactly the live qualification gate.
 // settle acknowledges committed worker ownership only: it must not publish
 // synthetic products. Admission is allowed only after the final reset returns.
 template<class Prepare,class Settle,class Reset>
 void prepareStartup(Prepare&& prepare,Settle&& settle,Reset&& reset){
  if(completed_||verified_)
   throw std::logic_error("Resident startup qualification requires a fresh cohort");
  while(pending()){
   begin();prepare(completed_);requireComplete();settle();published();
  }
  reset();resetInput();
 }
};
}
