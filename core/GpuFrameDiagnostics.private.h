#pragma once
// Frame-local diagnostic formatting only. No GPU work, global stream redirection,
// background thread, hidden flush, allocation growth, or accepted truncated log.
#include <array>
#include <cstring>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <streambuf>
namespace gpu_diagnostics {
class FrameLog {
 class Storage final:public std::streambuf {
  std::array<char,65536> bytes_{};
  size_t size_=0;
  bool overflow_=false;
 protected:
  int sync()override{return 0;} // unitbuf/flush formatting remains memory-only.
  std::streamsize xsputn(const char* data,std::streamsize count)override{
   if(count<0||count>static_cast<std::streamsize>(bytes_.size()-size_)){overflow_=true;return 0;}
   std::memcpy(bytes_.data()+size_,data,static_cast<size_t>(count));
   size_+=static_cast<size_t>(count);return count;
  }
  int_type overflow(int_type value)override{
   if(traits_type::eq_int_type(value,traits_type::eof()))return traits_type::not_eof(value);
   const char c=traits_type::to_char_type(value);
   return xsputn(&c,1)==1?value:traits_type::eof();
  }
 public:
  void reset()noexcept{size_=0;overflow_=false;}
  size_t size()const noexcept{return size_;}
  const char* data()const noexcept{return bytes_.data();}
  bool overflowed()const noexcept{return overflow_;}
 } storage_;
 std::ostream stream_{&storage_};
 bool active_=false;
public:
 static constexpr size_t capacity=65536;
 FrameLog(){stream_.imbue(std::cerr.getloc());}
 ~FrameLog()noexcept{
  try{flush();}catch(...){
   // Destruction follows an already failed/abandoned transaction; never accept
   // incomplete diagnostic proof or throw from Context destruction.
   try{std::cerr<<"GPU_DIAGNOSTIC_FAILURE abandoned_frame_drain=1\n";}catch(...){}
  }
 }
 FrameLog(const FrameLog&)=delete;
 FrameLog& operator=(const FrameLog&)=delete;
 bool active()const noexcept{return active_;}
 std::ostream& stream()noexcept{return active_?stream_:std::cerr;}
 void begin(){
  if(active_)throw std::logic_error("GPU diagnostic frame already active");
  if(stream_.getloc()!=std::cerr.getloc())throw std::logic_error("GPU diagnostic locale changed after initialization");
  storage_.reset();stream_.clear();
  stream_.flags(std::cerr.flags());stream_.precision(std::cerr.precision());
  stream_.width(std::cerr.width());stream_.fill(std::cerr.fill());
  active_=true;
 }
 // Caller owns the timing boundary. Normal use drains exactly once before the
 // resident host frame finishes and before Prepared can be acknowledged.
 void flush(std::ostream& sink=std::cerr){
  if(!active_)return;
  active_=false;
  const bool invalid=storage_.overflowed()||!stream_;
  const size_t size=storage_.size();
  sink.flags(stream_.flags());sink.precision(stream_.precision());
  sink.width(stream_.width());sink.fill(stream_.fill());
  if(size)sink.write(storage_.data(),static_cast<std::streamsize>(size));
  storage_.reset();stream_.clear();
  if(!sink)throw std::runtime_error("GPU diagnostic output write failed");
  if(invalid)throw std::runtime_error("GPU diagnostic frame exceeded fixed storage; complete frame rejected");
 }
};
} // namespace gpu_diagnostics
