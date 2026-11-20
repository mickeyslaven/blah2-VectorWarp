#include "process/utility/BestEffortSocket.h"
#include <iostream>
#include <stdexcept>
int main()try{
 asio::io_context io;asio::ip::tcp::acceptor listener(io,{asio::ip::address_v4::loopback(),0});
 const auto port=listener.local_endpoint().port();listener.non_blocking(true);
 blah2::BestEffortSocket producer("127.0.0.1",port);asio::ip::tcp::socket reader(io);
 std::string received;bool connected=false;char buffer[4096];
 const auto start=std::chrono::steady_clock::now();
 for(unsigned i=0;i<1000;++i){producer.sendData("{\"frame\":1}");asio::error_code ec;
  if(!connected){listener.accept(reader,ec);if(!ec){connected=true;reader.non_blocking(true);}}
  if(connected){const auto n=reader.read_some(asio::buffer(buffer),ec);if(!ec)received.append(buffer,n);}
 }
 if(received.empty()||received.find("{\"frame\":1}\n")==std::string::npos)
  throw std::runtime_error("newline stream not delivered");
 const auto drops=producer.dropped();if(producer.sendData(std::string(65535,'x'))||producer.dropped()<=drops)
  throw std::runtime_error("oversize frame was not rejected");
 asio::error_code ec;reader.close(ec);listener.close(ec);
 for(unsigned i=0;i<1000;++i)producer.sendData("{}");
 if(std::chrono::steady_clock::now()-start>std::chrono::seconds(2))throw std::runtime_error("diagnostic socket blocked");
 std::cout<<"PASS best-effort socket newline=1 oversize=1 close_failure=1 bounded_poll=1\n";
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
