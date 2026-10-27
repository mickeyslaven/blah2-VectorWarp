#include "AsyncReferenceExporter.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
std::pair<std::string,uint16_t> parse_endpoint(const std::string& value) {
  const auto separator=value.rfind(':');
  if(separator==std::string::npos||!separator||separator+1==value.size())
    throw std::invalid_argument("External reference endpoint must be IPv4:port");
  unsigned port=0;const char* first=value.data()+separator+1;
  const char* last=value.data()+value.size();const auto parsed=std::from_chars(first,last,port);
  if(parsed.ec!=std::errc{}||parsed.ptr!=last||!port||port>65535)
    throw std::invalid_argument("External reference endpoint has invalid port");
  const std::string host=value.substr(0,separator);
  (void)asio::ip::make_address_v4(host);
  return {host,static_cast<uint16_t>(port)};
}
void put32(std::array<uint8_t,64>& out,size_t at,uint32_t value){
  for(unsigned i=0;i<4;++i)out[at+i]=uint8_t(value>>(24-8*i));
}
void put64(std::array<uint8_t,64>& out,size_t at,uint64_t value){
  for(unsigned i=0;i<8;++i)out[at+i]=uint8_t(value>>(56-8*i));
}
int16_t sample(double value){
  return static_cast<int16_t>(std::clamp(std::llround(value),-32768LL,32767LL));
}
}

AsyncReferenceExporter::AsyncReferenceExporter(std::string endpoint,
  uint32_t sampleRate,uint32_t frequency,uint32_t samples)
  :host_(parse_endpoint(endpoint).first),port_(parse_endpoint(endpoint).second),
   // Eight seconds of bounded elasticity at a 500 ms CPI. This absorbs HP
   // segment-seal and filesystem latency without ever blocking native Owl
   // processing; sustained backpressure still drops only the export copy.
   sampleRate_(sampleRate),frequency_(frequency),samples_(samples),frames_(16){
  if(!sampleRate_||!frequency_||!samples_)
    throw std::invalid_argument("Invalid external reference geometry");
  for(auto& frame:frames_)frame.iq.resize(uint64_t(samples_)*2);
  worker_=std::thread([this]{run();});
}

AsyncReferenceExporter::~AsyncReferenceExporter(){
  stopping_.store(true,std::memory_order_release);ready_.notify_all();
  asio::error_code ignored;socket_.cancel(ignored);socket_.close(ignored);
  if(worker_.joinable())worker_.join();
}

bool AsyncReferenceExporter::try_submit(const IqData& reference,uint64_t epoch,
  uint64_t firstSample) noexcept{
  if(reference.get_length()!=samples_||stopping_.load(std::memory_order_acquire)){
    dropped_.fetch_add(1,std::memory_order_relaxed);return false;
  }
  size_t selected=frames_.size();
  {
    std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
    if(!lock.owns_lock()){dropped_.fetch_add(1,std::memory_order_relaxed);return false;}
    for(size_t i=0;i<frames_.size();++i)if(frames_[i].free){selected=i;frames_[i].free=false;break;}
  }
  if(selected==frames_.size()){dropped_.fetch_add(1,std::memory_order_relaxed);return false;}
  Frame& frame=frames_[selected];
  for(uint32_t i=0;i<samples_;++i){const auto& value=reference[i];
    frame.iq[2*i]=sample(value.real());frame.iq[2*i+1]=sample(value.imag());}
  {
    std::lock_guard<std::mutex> lock(mutex_);frame.sequence=nextSequence_++;
    frame.epoch=epoch;frame.firstSample=firstSample;queue_.push_back(selected);
  }
  submitted_.fetch_add(1,std::memory_order_relaxed);ready_.notify_one();return true;
}

AsyncReferenceExporter::Stats AsyncReferenceExporter::stats()const noexcept{
  return {submitted_.load(),sent_.load(),dropped_.load(),failures_.load()};
}
void AsyncReferenceExporter::connect(){
  if(socket_.is_open())return;
  socket_=asio::ip::tcp::socket(io_);
  socket_.connect({asio::ip::make_address_v4(host_),port_});
  socket_.set_option(asio::ip::tcp::no_delay(true));
}
void AsyncReferenceExporter::send_frame(const Frame& frame){
  std::array<uint8_t,64> h{};std::copy_n("VWREF001",8,h.begin());put32(h,8,1);put32(h,12,64);
  put64(h,16,frame.sequence);put64(h,24,frame.epoch);put64(h,32,frame.firstSample);
  put32(h,40,sampleRate_);put32(h,44,frequency_);put32(h,48,samples_);put32(h,52,0);
  put32(h,56,uint32_t(frame.iq.size()*sizeof(int16_t)));put32(h,60,1);
  std::array<asio::const_buffer,2> buffers{asio::buffer(h),asio::buffer(frame.iq)};
  asio::write(socket_,buffers);
}
void AsyncReferenceExporter::run()noexcept{
  while(!stopping_.load(std::memory_order_acquire)){
    size_t selected=frames_.size();{
      std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[&]{return stopping_.load()||!queue_.empty();});
      if(stopping_.load())break;
      selected=queue_.front();
      queue_.pop_front();
    }
    try{connect();send_frame(frames_[selected]);sent_.fetch_add(1);}catch(...){
      failures_.fetch_add(1);asio::error_code ignored;socket_.close(ignored);}
    {std::lock_guard<std::mutex> lock(mutex_);frames_[selected].free=true;}
  }
}
