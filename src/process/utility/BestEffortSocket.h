#pragma once
#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace blah2 {
// Diagnostics only: no thread, waits, DNS, unbounded queue or exception escape.
// One <=64 KiB newline frame, at most four ready completions per CPI. A stalled
// partial frame closes its connection before retry, so it cannot splice JSON.
class BestEffortSocket {
 using Clock=std::chrono::steady_clock;
 asio::io_context io_;asio::ip::tcp::endpoint endpoint_;asio::ip::tcp::socket socket_{io_};
 std::shared_ptr<std::string> pending_;size_t offset_=0;bool connecting_=false,writing_=false,connected_=false;
 Clock::time_point deadline_{},retry_{};uint64_t dropped_=0,generation_=0;
 void failed(){asio::error_code ignored;socket_.close(ignored);connected_=false;
  ++generation_;connecting_=writing_=false;if(pending_){++dropped_;pending_.reset();}
  retry_=Clock::now()+std::chrono::seconds(5);}
 void write(){writing_=true;auto buffer=pending_;const auto generation=generation_;
  socket_.async_write_some(asio::buffer(buffer->data()+offset_,buffer->size()-offset_),
  [this,buffer,generation](asio::error_code ec,size_t n){if(generation!=generation_)return;
   writing_=false;if(ec||!n){failed();return;}
   offset_+=n;if(offset_==pending_->size()){pending_.reset();offset_=0;}
   else write();});}
 void pump(){io_.restart();for(unsigned i=0;i<4&&io_.poll_one();++i){}
  if((connecting_||pending_)&&Clock::now()>deadline_)failed();}
public:
 BestEffortSocket(const std::string& ip,uint16_t port):endpoint_(asio::ip::make_address(ip),port){}
 uint64_t dropped()const noexcept{return dropped_;}
 bool sendData(const std::string& data)noexcept{try{
  pump();if(data.size()>=65535||pending_){++dropped_;return false;}
  if(!connected_){if(connecting_||Clock::now()<retry_){++dropped_;return false;}
   // Do not queue a frame while connecting: old event frames must not build up.
   connecting_=true;deadline_=Clock::now()+std::chrono::seconds(2);
   const auto generation=generation_;
   socket_.async_connect(endpoint_,[this,generation](asio::error_code ec){if(generation!=generation_)return;connecting_=false;
    if(ec){failed();return;}connected_=true;});++dropped_;return false;}
  pending_=std::make_shared<std::string>(data);*pending_+='\n';offset_=0;deadline_=Clock::now()+std::chrono::seconds(2);write();pump();return true;
 }catch(...){failed();return false;}}
};
}
