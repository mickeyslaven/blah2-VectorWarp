#pragma once
#include "GpuResidentWire.h"
#include "GpuResidentEvidenceLayout.h"
#include <array>
#include <cmath>
#include <ostream>
namespace blah2::resident {
// Bounded DATA-ONLY decoding of acknowledged sections. No stage recomputation,
// status repair, DSP or Vulkan dependency. All arrays retain exact word order.
struct ReferenceState {
 uint64_t epoch=0,logicalFrame=0,samples=0,channels=0,frames=0,updates=0;
 uint32_t weightCount=0;
 std::array<std::complex<double>,8> weights{};
 double coherentFraction=0,coherentGainDb=0,scale=0;
};
struct ReadyProof {
 uint64_t frame=0,consumed=0,referenceGeneration=0;
 uint32_t stages=0,batchCapacity=0;
 std::array<uint32_t,16> referenceStatus{},residentStatus{},shape{};
 std::array<double,4> gpuMs{};
 uint64_t gpuTimeMask=0;
};
struct BatchProof {uint64_t generation=0;
uint32_t first=0,active=0,capacity=0;
std::array<uint32_t,16> nativeStatus{},residentStatus{};
std::array<double,9> gpuNativeMs{};
double gpuPackMs=0,gpuSubtractPackMs=0,hostMs=0;};
struct PreparedProof {
 uint64_t frame=0,consumed=0,nativeGeneration=0,residentGeneration=0,gpuTimeMask=0;
 uint32_t stages=0,batchCount=0,batchCapacity=0;
 std::array<uint32_t,6> nativeShape{};
 std::array<uint32_t,16> nativeReferenceStatus{},residentStatus{};
 std::array<uint32_t,32> postprocessStatus{};
 std::array<uint32_t,64> trackerStatus{};
 std::array<double,3> gpuNativeReferenceMs{};
 std::array<double,14> gpuMs{};
 std::array<BatchProof,8>batches{};
};
struct AllocationProof {uint64_t limit=0,used=0,peak=0,live=0;
uint32_t heaps=0,types=0;};
inline AllocationProof allocationProof(ConstBytes b){wire::Reader r{b};
wire::require(b.size==832&&r.u32(0)==1&&r.u32(4)==832&&r.u32(8)==1,"Resident allocation proof shape");
AllocationProof a{r.u64(24),r.u64(32),r.u64(40),r.u64(48),r.u32(12),r.u32(16)};
wire::require(a.heaps&&a.heaps<=16&&a.types&&a.types<=32&&a.used<=a.peak&&a.peak<=a.limit,"Resident allocation proof bounds");
r.zeros(20,4);
r.zeros(56,8);
uint64_t sum=0;
for(uint32_t i=0;i<16;++i){size_t at=64+32*i;
if(i>=a.heaps){r.zeros(at,32);
continue;}const auto physical=r.u64(at),limit=r.u64(at+8),used=r.u64(at+16);
wire::require(limit==std::min<uint64_t>(2ULL<<30,physical/4)&&used<=limit&&r.u32(at+24)<=1,"Resident heap proof bounds");
r.zeros(at+28,4);
sum=wire::add(sum,used);}wire::require(sum==a.used,"Resident heap proof sum differs");
for(uint32_t i=0;i<32;++i){if(i>=a.types)r.zeros(576+8*i,8);
else wire::require(r.u32(580+8*i)<a.heaps,"Resident memory type proof invalid");}return a;}
namespace proof_detail {
inline ConstBytes sub(ConstBytes b,size_t at,size_t n){wire::require(at<=b.size&&n<=b.size-at,"Resident proof slice");
return {b.data+at,n};}
template<size_t N>inline void words(wire::Reader& r,size_t at,std::array<uint32_t,N>& a){for(size_t i=0;i<N;++i)a[i]=r.u32(at+4*i);}
template<size_t N>inline void times(wire::Reader& r,size_t at,std::array<double,N>& a){for(size_t i=0;i<N;++i){a[i]=r.f64(at+8*i);
wire::require(std::isfinite(a[i])&&a[i]>=0,"Resident GPU timing proof invalid");}}
inline void common(ConstBytes b,size_t expected,Phase phase){wire::Reader r{b};
wire::require(b.size==expected&&r.u32(0)==1&&r.u32(4)==expected&&r.u32(8)==uint32_t(phase)&&!r.u32(12),"Resident evidence schema/phase");
r.zeros(124,4);}
}
inline ReferenceState referenceState(ConstBytes b){wire::Reader r{b};
wire::require(b.size==256&&r.u32(0)==1&&r.u32(4)==256&&r.u64(56)<=8,"Resident reference checkpoint shape");
ReferenceState s;
s.epoch=r.u64(8);
s.logicalFrame=r.u64(16);
s.samples=r.u64(24);
s.channels=r.u64(32);
s.frames=r.u64(40);
s.updates=r.u64(48);
s.weightCount=uint32_t(r.u64(56));
for(size_t i=0;i<8;++i){s.weights[i]={r.f64(64+16*i),r.f64(72+16*i)};
wire::require(std::isfinite(s.weights[i].real())&&std::isfinite(s.weights[i].imag()),"Resident checkpoint nonfinite weight");}s.coherentFraction=r.f64(192);
s.coherentGainDb=r.f64(200);
s.scale=r.f64(208);
wire::require(std::isfinite(s.coherentFraction)&&std::isfinite(s.coherentGainDb)&&std::isfinite(s.scale)&&s.scale>=0,"Resident checkpoint metrics invalid");
r.zeros(216,40);
return s;}
inline ReadyProof readyProof(ConstBytes b){proof_detail::common(b,readyEvidenceBytes,Phase::referenceReady);
wire::Reader r{b};
ReadyProof p;
p.frame=r.u64(40);
p.consumed=r.u64(56);
p.referenceGeneration=r.u64(80);
p.stages=r.u32(92);
p.batchCapacity=r.u32(116);
proof_detail::words(r,evidence::ready::referenceStatus,p.referenceStatus);
proof_detail::words(r,evidence::ready::residentStatus,p.residentStatus);
proof_detail::words(r,evidence::ready::shape,p.shape);
p.gpuTimeMask=r.u64(evidence::ready::times);
proof_detail::times(r,evidence::ready::times+16,p.gpuMs);
allocationProof(proof_detail::sub(b,256,832));
return p;}
inline PreparedProof preparedProof(ConstBytes b){proof_detail::common(b,preparedEvidenceBytes,Phase::prepared);
wire::Reader r{b};
PreparedProof p;
p.frame=r.u64(40);
p.consumed=r.u64(56);
p.nativeGeneration=r.u64(72);
p.residentGeneration=r.u64(80);
p.stages=r.u32(92);
p.batchCount=r.u32(112);
p.batchCapacity=r.u32(116);
wire::require(p.batchCount&&p.batchCount<=8&&p.batchCapacity&&p.batchCapacity<=8,"Resident batch proof bounds");
const size_t a=evidence::prepared::nativeReference;
wire::require(r.u64(a)==p.nativeGeneration,"Native generation proof differs");
proof_detail::words(r,a+8,p.nativeShape);
proof_detail::words(r,a+32,p.nativeReferenceStatus);
proof_detail::times(r,a+96,p.gpuNativeReferenceMs);
proof_detail::words(r,evidence::prepared::residentFinalStatus,p.residentStatus);
proof_detail::words(r,evidence::prepared::postprocessStatus,p.postprocessStatus);
proof_detail::words(r,evidence::prepared::trackerPreparedStatus,p.trackerStatus);
p.gpuTimeMask=r.u64(evidence::prepared::times);
proof_detail::times(r,evidence::prepared::times+16,p.gpuMs);
 for(uint32_t i=0;i<8;++i){const size_t a=evidence::prepared::nativeBatches+256*i;
 if(i>=p.batchCount){r.zeros(a,256);
 continue;}auto& t=p.batches[i];
 t.generation=r.u64(a);
 t.first=r.u32(a+8);
 t.active=r.u32(a+12);
 t.capacity=r.u32(a+16);
 r.zeros(a+20,4);
 proof_detail::words(r,a+24,t.nativeStatus);
 proof_detail::words(r,a+88,t.residentStatus);
 proof_detail::times(r,a+152,t.gpuNativeMs);
 t.gpuPackMs=r.f64(a+224);
 t.gpuSubtractPackMs=r.f64(a+232);
 t.hostMs=r.f64(a+240);
 wire::require(std::isfinite(t.gpuPackMs)&&t.gpuPackMs>=0&&std::isfinite(t.gpuSubtractPackMs)&&t.gpuSubtractPackMs>=0&&std::isfinite(t.hostMs)&&t.hostMs>=0,"Resident batch timing invalid");
 r.zeros(a+248,8);}allocationProof(proof_detail::sub(b,256,832));
 return p;}
// Snapshot copies the fixed host ledger only. No file descriptors or dynamic
// vectors. Charge sizeof(HostTimingSnapshot) for each retained qualifier frame.
class HostTimingSnapshot {
 static constexpr size_t start=evidence::prepared::hostSummary;
 std::array<uint8_t,preparedEvidenceBytes-start> data_{};
public:
 void assign(ConstBytes prepared){proof_detail::common(prepared,preparedEvidenceBytes,Phase::prepared);
 std::memcpy(data_.data(),prepared.data+start,data_.size());
 validate();}
 void validate()const;
 void write(std::ostream&,uint32_t call)const;
};
void validateDevice(const Config&,ConstBytes device);
uint64_t validateControl(const Config&,const Packet& request,const Packet& response,ConstBytes control,ConstBytes device,uint64_t previousEvent);
void validateEvidence(const Config&,const Packet&,ConstBytes ready,ConstBytes prepared,ConstBytes device);
}
