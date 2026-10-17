#pragma once
#include "GpuResidentBackend.h"
#include <cstring>
#include <limits>
#include <stdexcept>
namespace blah2::resident::wire {
inline void require(bool ok,const char* why){if(!ok)throw std::invalid_argument(why);}
inline uint64_t add(uint64_t a,uint64_t b){require(b<=UINT64_MAX-a,"Resident size addition overflow");
return a+b;}
inline uint64_t mul(uint64_t a,uint64_t b){require(!a||b<=UINT64_MAX/a,"Resident size multiplication overflow");
return a*b;}
inline uint64_t align(uint64_t a,uint64_t n){require(n,"Resident alignment zero");
return mul(add(a,n-1)/n,n);}
struct Reader {
 ConstBytes b;
 void range(size_t at,size_t count)const{require(at<=b.size&&count<=b.size-at&&(b.data||!count),"Resident wire read outside buffer");}
 uint32_t u32(size_t at)const{range(at,4);
 uint32_t v=0;
 for(unsigned i=0;i<4;++i)v|=uint32_t(b.data[at+i])<<(8*i);
 return v;}
 uint64_t u64(size_t at)const{range(at,8);
 uint64_t v=0;
 for(unsigned i=0;i<8;++i)v|=uint64_t(b.data[at+i])<<(8*i);
 return v;}
 int32_t i32(size_t at)const{uint32_t v=u32(at);
 int32_t out;
 std::memcpy(&out,&v,4);
 return out;}
 double f64(size_t at)const{uint64_t v=u64(at);
 double out;
 std::memcpy(&out,&v,8);
 return out;}
 float f32(size_t at)const{uint32_t v=u32(at);float out;std::memcpy(&out,&v,4);return out;}
 void zeros(size_t at,size_t count)const{range(at,count);
 for(size_t i=0;i<count;++i)require(b.data[at+i]==0,"Resident reserved byte is nonzero");}
};
struct Writer {
 Bytes b;
 void range(size_t at,size_t count)const{Reader{{b.data,b.size}}.range(at,count);}
 void u32(size_t at,uint32_t v)const{range(at,4);
 for(unsigned i=0;i<4;++i)b.data[at+i]=uint8_t(v>>(8*i));}
 void u64(size_t at,uint64_t v)const{range(at,8);
 for(unsigned i=0;i<8;++i)b.data[at+i]=uint8_t(v>>(8*i));}
 void f64(size_t at,double v)const{uint64_t bits;
 std::memcpy(&bits,&v,8);
 u64(at,bits);}
 void f32(size_t at,float v)const{uint32_t bits;std::memcpy(&bits,&v,4);u32(at,bits);}
 void copy(size_t at,const void* p,size_t n)const{range(at,n);
 require(p||!n,"Resident null copy");
 if(n)std::memcpy(b.data+at,p,n);}
 void zero()const{range(0,b.size);
 if(b.size)std::memset(b.data,0,b.size);}
};
inline uint32_t narrow(uint64_t v){require(v<=UINT32_MAX,"Resident count cannot narrow");
return uint32_t(v);}
inline bool same(Token a,Token b){return a.epoch==b.epoch&&a.transaction==b.transaction&&a.generation==b.generation&&a.consumedSequence==b.consumedSequence;}
Hash configHash(const Config&);
uint64_t mandatoryPresence(const Config&);
}
