#pragma once
// Diagnostics never write a sink during a transaction. Explicit offline
// workers retain at most 64 * 64 KiB and drain only during worker teardown.
#include "GpuWorkerLifetime.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <ostream>
#include <streambuf>
#include <vector>

namespace gpu_diagnostics {
// The module remains loaded until its worker loop has replied and returned.
// Object destruction (including exception unwinding) only transfers ownership;
// the module/process teardown is the sole production sink boundary.
class DeferredOutput {
 std::vector<std::vector<char>> chunks_;
 size_t bytes_=0;
 bool truncated_=false;
public:
 static constexpr size_t maximumBytes=64*65536;
 void append(std::vector<char> bytes,bool truncated)noexcept{
   try {
     const size_t available=maximumBytes-bytes_;
     truncated_|=truncated||bytes.size()>available;
     if(bytes.size()>available)bytes.resize(available);
     if(!bytes.empty()){chunks_.push_back(std::move(bytes));bytes_+=chunks_.back().size();}
   }catch(...){truncated_=true;}
 }
 ~DeferredOutput()noexcept{
   try {
     for(const auto& bytes:chunks_)std::cerr.write(bytes.data(),std::streamsize(bytes.size()));
     if(truncated_)std::cerr<<"GPU_DIAGNOSTIC_TRUNCATED bounded_offline_storage=1\n";
   }catch(...){}
 }
};
inline DeferredOutput& deferredOutput(){static DeferredOutput value;return value;}
class FrameLog {
 class Storage final:public std::streambuf {
  std::vector<char> bytes_;
  size_t size_=0;
  bool accepting_=false,overflow_=false;
 protected:
  int sync()override{return 0;}
  std::streamsize xsputn(const char* data,std::streamsize count)override{
   if(count<=0||!accepting_)return std::max<std::streamsize>(0,count);
   const size_t copied=std::min<size_t>(size_t(count),bytes_.size()-size_);
   if(copied)std::memcpy(bytes_.data()+size_,data,copied);
   size_+=copied;overflow_|=copied!=size_t(count);
   // Diagnostic overflow must never poison a numerical transaction.
   return count;
  }
  int_type overflow(int_type value)override{
   if(traits_type::eq_int_type(value,traits_type::eof()))return traits_type::not_eof(value);
   const char c=traits_type::to_char_type(value);xsputn(&c,1);return value;
  }
 public:
  explicit Storage(size_t bytes):bytes_(bytes){}
  void accept(bool value)noexcept{accepting_=value;}
  size_t size()const noexcept{return size_;}
  const char* data()const noexcept{return bytes_.data();}
  bool overflowed()const noexcept{return overflow_;}
  void clear()noexcept{size_=0;overflow_=false;}
  std::vector<char> take(){bytes_.resize(size_);size_=0;return std::move(bytes_);}
 } storage_;
 std::ostream stream_{&storage_};
 unsigned limit_=0,frames_=0;
 bool active_=false;
public:
 static constexpr size_t capacity=65536;
 explicit FrameLog(unsigned limit=blah2::gpu_offline_diagnostics::frameLimit())
   :storage_(capacity*std::min(limit,64u)),limit_(std::min(limit,64u)){
   storage_.accept(limit_!=0);
 }
 ~FrameLog()noexcept{
   if(storage_.size()||storage_.overflowed())
     deferredOutput().append(storage_.take(),storage_.overflowed());
 }
 FrameLog(const FrameLog&)=delete;
 FrameLog& operator=(const FrameLog&)=delete;
 bool active()const noexcept{return active_;}
 std::ostream& stream()noexcept{return stream_;}
 void begin()noexcept{
   if(active_)return;
   if(frames_<=limit_)++frames_;
   active_=frames_<=limit_;
   storage_.accept(active_);
 }
 // The existing frame boundary remains memory-only, including failure/reset.
 void flush()noexcept{active_=false;}
 // Only teardown or an explicit offline test may drain a sink. Live objects
 // have no stored bytes, so even a blocked inherited stderr remains untouched.
 void drain(std::ostream& sink=std::cerr)noexcept{
   try {
     if(storage_.size())sink.write(storage_.data(),std::streamsize(storage_.size()));
     if(storage_.overflowed())sink<<"GPU_DIAGNOSTIC_TRUNCATED bounded_offline_storage=1\n";
   }catch(...){} // Optional diagnostics never determine DSP acceptance.
   storage_.clear();
 }
 size_t retainedBytes()const noexcept{return storage_.size();}
};
}
