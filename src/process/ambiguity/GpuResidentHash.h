#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace blah2::resident {
// SHA-256 for immutable setup/cohort identity only; never part of signal math.
class Sha256 {
 std::array<uint32_t,8> h_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
 std::array<uint8_t,64> block_{};
 size_t used_=0;
 uint64_t bytes_=0;
 static uint32_t rotr(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
 void transform(){
  static constexpr uint32_t k[]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  uint32_t w[64]{};
  for(unsigned i=0;i<16;++i)for(unsigned j=0;j<4;++j)w[i]=(w[i]<<8)|block_[4*i+j];
  for(unsigned i=16;i<64;++i){uint32_t x=w[i-15],y=w[i-2];
  w[i]=w[i-16]+(rotr(x,7)^rotr(x,18)^(x>>3))+w[i-7]+(rotr(y,17)^rotr(y,19)^(y>>10));}
  auto a=h_[0],b=h_[1],c=h_[2],d=h_[3],e=h_[4],f=h_[5],g=h_[6],h=h_[7];
  for(unsigned i=0;i<64;++i){const uint32_t t1=h+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
  const uint32_t t2=(rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c));
  h=g;
  g=f;
  f=e;
  e=d+t1;
  d=c;
  c=b;
  b=a;
  a=t1+t2;}
  h_[0]+=a;
  h_[1]+=b;
  h_[2]+=c;
  h_[3]+=d;
  h_[4]+=e;
  h_[5]+=f;
  h_[6]+=g;
  h_[7]+=h;
 }
public:
 void add(const void* data,size_t n){if(n>UINT64_MAX/8-bytes_)throw std::overflow_error("Resident identity length overflow");
 bytes_+=n;
 const auto* p=static_cast<const uint8_t*>(data);
 while(n){size_t take=std::min(n,64-used_);
 std::memcpy(block_.data()+used_,p,take);
 used_+=take;
 p+=take;
 n-=take;
 if(used_==64){transform();
 used_=0;}}}
 std::array<uint8_t,32> finish(){const uint64_t bits=bytes_*8;
 block_[used_++]=0x80;
 if(used_>56){std::memset(block_.data()+used_,0,64-used_);
 transform();
 used_=0;}std::memset(block_.data()+used_,0,56-used_);
 for(unsigned i=0;i<8;++i)block_[63-i]=uint8_t(bits>>(8*i));
 transform();
 std::array<uint8_t,32> out{};
 for(unsigned i=0;i<8;++i)for(unsigned j=0;j<4;++j)out[4*i+j]=uint8_t(h_[i]>>(24-8*j));
 return out;}
};
}
