// Private GPU-native refinement proof. Included inside VulkanBackend's namespace,
// after Buffer/NativePlan/Kernel/barrier. No FFTW or host DSP implementation.
// The production FP32 correlation and residual correction remain unchanged.
namespace gpu_native {
constexpr uint32_t fft = 1024, tileBlocks = 64, statusWords = 16;
enum Status : uint32_t { error, phase, row, channel, referenceBlocks,
  baselineBlocks, residualSamples, mergeSamples, solves, weightFfts,
  inputSamples, activeChannels, factorizations };
// Compiling an accidental call to the former host paths must fail.
struct ForbiddenHostMath {
  static void factorReference() = delete;
  static void solveWeights() = delete;
  static void baseline() = delete;
  static void residual() = delete;
  static void merge() = delete;
};
const std::string common = R"glsl(#version 450
#extension GL_ARB_gpu_shader_fp64 : require
layout(local_size_x=128) in;
layout(binding=0) coherent buffer Status { uint status[]; };
layout(push_constant) uniform Parameters { uint count; uint a; uint b; uint c; int d; } p;
uint index1() { return gl_GlobalInvocationID.x + gl_GlobalInvocationID.y*gl_NumWorkGroups.x*128; }
dvec2 mul(dvec2 a, dvec2 b) { return dvec2(a.x*b.x-a.y*b.y,a.x*b.y+a.y*b.x); }
dvec2 conjugate(dvec2 a) { return dvec2(a.x,-a.y); }
bool finite64(dvec2 a) { return !any(isnan(a)) && !any(isinf(a)); }
bool finite32(vec2 a) { return !any(isnan(a)) && !any(isinf(a)); }
void reject(uint code,uint phase,uint row,uint channel) {
  if(atomicCompSwap(status[0],0u,code)==0u) {
    status[1]=phase; status[2]=row; status[3]=channel;
  }
}
)glsl";
const std::string referencePack = common + R"glsl(
layout(binding=1) readonly buffer Input { vec2 inputData[]; };
layout(binding=2) writeonly buffer Work { dvec2 work[]; };
layout(binding=3) writeonly buffer Suffix { vec2 suffix[]; };
// a=N, b=B, c=hop, d=first block; count=64*1024.
void main() {
  uint i=index1(); if(i>=p.count) return;
  uint block=uint(p.d)+i/1024u, j=i%1024u;
  int source=int(block*p.c+j)-int(p.b-1u);
  vec2 value=(source<0 || uint(source)>=p.a)?vec2(0):inputData[source];
  if(!finite32(value)) reject(1u,1u,uint(max(source,0)),0u);
  work[i]=dvec2(value);
  if(j==0u && block*p.c<p.a) atomicAdd(status[4],1u);
  if(p.d==0 && i<p.b-1u) suffix[i]=inputData[p.a-(p.b-1u)+i];
}
)glsl";
const std::string factor = common + R"glsl(
layout(binding=1) readonly buffer Correlations { vec2 corr[]; };
layout(binding=2) coherent buffer Lower { dvec2 lower[]; };
shared uint failed;
shared double diagonal, smallest, largest;
// a=N,b=B. Exactly one workgroup; every dependency is workgroup-contained.
void main() {
  uint lane=gl_LocalInvocationID.x, B=p.b;
  if(lane==0u) { failed=atomicAdd(status[0],0u); smallest=1.7976931348623157e308LF; largest=0.0LF; }
  barrier(); if(failed!=0u) return;
  double scale=max(length(dvec2(corr[0])/double(p.a)),1e-30LF);
  for(uint column=0u;column<B;++column) {
    if(lane==0u) {
      dvec2 sum=dvec2(corr[0])/double(p.a);
      for(uint k=0u;k<column;++k) {
        dvec2 value=lower[column*B+k]; sum-=mul(value,conjugate(value));
      }
      if(!finite64(sum) || sum.x<=scale*1e-12LF || abs(sum.y)>scale*1e-5LF) {
        reject(2u,2u,column,0u); failed=1u;
      } else {
        diagonal=sqrt(sum.x); smallest=min(smallest,diagonal); largest=max(largest,diagonal);
        lower[column*B+column]=dvec2(diagonal,0.0LF);
      }
    }
    memoryBarrierBuffer(); barrier(); if(failed!=0u) return;
    for(uint row=column+1u+lane;row<B;row+=128u) {
      dvec2 sum=dvec2(corr[row-column])/double(p.a);
      for(uint k=0u;k<column;++k) sum-=mul(lower[row*B+k],conjugate(lower[column*B+k]));
      lower[row*B+column]=sum/diagonal;
    }
    memoryBarrierBuffer(); barrier();
  }
  if(lane==0u) {
    if(smallest<largest*1e-4LF) reject(3u,2u,0u,0u);
    else atomicAdd(status[12],1u);
  }
}
)glsl";
const std::string solve = common + R"glsl(
layout(binding=1) readonly buffer Correlations { vec2 corr[]; };
layout(binding=2) readonly buffer Lower { dvec2 lower[]; };
layout(binding=3) coherent buffer Solution { dvec2 solution[]; };
shared uint failed;
// Per-channel dependencies never leave this one workgroup. B is at most1024.
shared dvec2 localSolution[1024];
shared double residualPower[128], rhsPower[128];
// a=N,b=B,c=phase (3 initial,4 correction). One workgroup per channel.
void main() {
  uint lane=gl_LocalInvocationID.x,ch=gl_WorkGroupID.x,B=p.b,base=ch*B;
  if(lane==0u) failed=atomicAdd(status[0],0u);
  barrier(); if(failed!=0u || ch>=status[11]) return;
  for(uint row=lane;row<B;row+=128u) localSolution[row]=dvec2(corr[B+base+row])/double(p.a);
  barrier();
  // In-place forward substitution, each row's terms subtracted in ascending k.
  for(uint k=0u;k<B;++k) {
    if(lane==0u) localSolution[k]/=lower[k*B+k].x;
    barrier();
    for(uint row=k+1u+lane;row<B;row+=128u) localSolution[row]-=mul(lower[row*B+k],localSolution[k]);
    barrier();
  }
  // Backward solve has an independent per-row sum in ascending k, as the CPU
  // oracle; only lane zero publishes each dependent solved value.
  for(int row=int(B)-1;row>=0;--row) {
    if(lane==0u) {
      dvec2 sum=localSolution[uint(row)];
      for(uint k=uint(row)+1u;k<B;++k) sum-=mul(conjugate(lower[k*B+uint(row)]),localSolution[k]);
      localSolution[uint(row)]=sum/lower[uint(row)*B+uint(row)].x;
    }
    barrier();
  }
  double residual=0.0LF, rhs=0.0LF;
  for(uint row=lane;row<B;row+=128u) {
    if(!finite64(localSolution[row])) reject(4u,p.c,row,ch);
    dvec2 value=dvec2(0.0LF);
    for(uint col=0u;col<B;++col) {
      dvec2 matrix=row>=col?dvec2(corr[row-col]):conjugate(dvec2(corr[col-row]));
      value+=mul(matrix/double(p.a),localSolution[col]);
    }
    dvec2 expected=dvec2(corr[B+base+row])/double(p.a), difference=value-expected;
    residual+=dot(difference,difference); rhs+=dot(expected,expected);
  }
  residualPower[lane]=residual; rhsPower[lane]=rhs; barrier();
  if(lane==0u) {
    residual=0.0LF; rhs=0.0LF;
    for(uint i=0u;i<128u;++i) { residual+=residualPower[i]; rhs+=rhsPower[i]; }
    if(isnan(residual) || isinf(residual) || isnan(rhs) || isinf(rhs) || residual>max(rhs*1e-10LF,1e-20LF))
      reject(5u,p.c,0u,ch);
    atomicAdd(status[8],1u);
  }
  // Finish the original residual/finite guards before one final SSBO publish.
  // Failed output remains a rejected candidate, exactly as in the original path.
  barrier();
  for(uint row=lane;row<B;row+=128u) solution[base+row]=localSolution[row];
}
)glsl";
// O(B^2) Hermitian Toeplitz alternative to the dense Cholesky factor/solve.
// It is opt-in while physical qualification compares it with the dense path.
const std::string toeplitzFactor = common + R"glsl(
layout(binding=1) readonly buffer Correlations { vec2 corr[]; };
layout(binding=2) coherent buffer Lower { dvec2 lower[]; };
shared dvec2 alpha[1024], partial[128];
shared double beta, scale;
shared dvec2 gamma;
void main() {
 uint lane=gl_LocalInvocationID.x,B=p.b;
 if(lane==0u) {
  scale=(dvec2(corr[0])/double(p.a)).x; beta=scale;
  if(!finite64(dvec2(beta,0)) || beta<=1e-30LF) reject(2u,2u,0u,0u);
  else { lower[0]=dvec2(beta,0); if(B>1u) alpha[0]=-(dvec2(corr[1])/double(p.a))/beta; }
 }
 barrier(); if(status[0]!=0u)return;
 for(uint k=1u;k<B;++k) {
  if(lane==0u) {
   beta*=1.0LF-dot(alpha[k-1u],alpha[k-1u]);
   if(!finite64(dvec2(beta,0)) || beta<=scale*1e-8LF) reject(3u,2u,k,0u);
  }
  barrier(); if(status[0]!=0u)return;
  for(uint j=lane;j<k;j+=128u)lower[k*B+j]=alpha[j];
  if(lane==0u)lower[k*B+k]=dvec2(beta,0); barrier();
  if(k+1u<B) {
   dvec2 sum=dvec2(0); for(uint j=lane;j<k;j+=128u)
    sum+=mul(dvec2(corr[k-j])/double(p.a),alpha[j]);
   partial[lane]=sum; barrier();
   if(lane==0u) {
    dvec2 q=dvec2(corr[k+1u])/double(p.a);
    for(uint j=0u;j<128u;++j)q+=partial[j]; gamma=-q/beta;
    if(!finite64(gamma) || dot(gamma,gamma)>=1.0LF) reject(3u,2u,k,0u);
   }
   barrier(); if(status[0]!=0u)return;
   for(uint j=lane;j<k/2u;j+=128u) {
    uint pair=k-1u-j; dvec2 left=alpha[j],right=alpha[pair];
    alpha[j]=left+mul(gamma,conjugate(right));
    alpha[pair]=right+mul(gamma,conjugate(left));
   }
   if((k&1u)!=0u && lane==0u) { uint middle=k/2u; dvec2 value=alpha[middle];
    alpha[middle]=value+mul(gamma,conjugate(value)); }
   if(lane==0u)alpha[k]=gamma; barrier();
  }
 }
 if(lane==0u)atomicAdd(status[12],1u);
}
)glsl";
const std::string toeplitzSolve = common + R"glsl(
layout(binding=1) readonly buffer Correlations { vec2 corr[]; };
layout(binding=2) readonly buffer Lower { dvec2 lower[]; };
layout(binding=3) coherent buffer Solution { dvec2 solution[]; };
shared dvec2 localSolution[1024], partial[128];
shared double residualPower[128], rhsPower[128];
shared double beta, scale;
shared dvec2 mu;
void main() {
 uint lane=gl_LocalInvocationID.x,ch=gl_WorkGroupID.x,B=p.b,base=ch*B;
 if(ch>=status[11])return;
 if(lane==0u) {
  scale=lower[0].x; beta=scale;
  if(!finite64(dvec2(beta,0)) || beta<=1e-30LF) reject(2u,p.c,0u,ch);
  else localSolution[0]=dvec2(corr[B+base])/double(p.a)/beta;
 }
 barrier(); if(status[0]!=0u)return;
 for(uint k=1u;k<B;++k) {
  if(lane==0u) { beta=lower[k*B+k].x;
   if(!finite64(dvec2(beta,0)) || beta<=scale*1e-8LF)reject(3u,p.c,k,ch); }
  barrier(); if(status[0]!=0u)return;
  dvec2 sum=dvec2(0); for(uint j=lane;j<k;j+=128u)
   sum+=mul(dvec2(corr[k-j])/double(p.a),localSolution[j]);
  partial[lane]=sum; barrier();
  if(lane==0u) { dvec2 rhs=dvec2(corr[B+base+k])/double(p.a);
   for(uint j=0u;j<128u;++j)rhs-=partial[j]; mu=rhs/beta;
   if(!finite64(mu))reject(4u,p.c,k,ch); }
  barrier(); if(status[0]!=0u)return;
  for(uint j=lane;j<k;j+=128u)
   localSolution[j]+=mul(mu,conjugate(lower[k*B+(k-1u-j)]));
  if(lane==0u)localSolution[k]=mu; barrier();
 }
 double residual=0.0LF,rhs=0.0LF;
 for(uint row=lane;row<B;row+=128u){ dvec2 value=dvec2(0);for(uint col=0u;col<B;++col){dvec2 matrix=row>=col?dvec2(corr[row-col]):conjugate(dvec2(corr[col-row]));value+=mul(matrix/double(p.a),localSolution[col]);}dvec2 expected=dvec2(corr[B+base+row])/double(p.a),difference=value-expected;residual+=dot(difference,difference);rhs+=dot(expected,expected);if(!finite64(localSolution[row]))reject(4u,p.c,row,ch); }
 residualPower[lane]=residual;rhsPower[lane]=rhs;barrier();if(lane==0u){residual=0.0LF;rhs=0.0LF;for(uint i=0u;i<128u;++i){residual+=residualPower[i];rhs+=rhsPower[i];}if(isnan(residual)||isinf(residual)||isnan(rhs)||isinf(rhs)||residual>max(rhs*1e-10LF,1e-20LF))reject(5u,p.c,0u,ch);atomicAdd(status[8],1u);}barrier();for(uint row=lane;row<B;row+=128u)solution[base+row]=localSolution[row];
}
)glsl";
const std::string weightPack = common + R"glsl(
layout(binding=1) readonly buffer Initial { dvec2 initial[]; };
layout(binding=2) writeonly buffer Weights { dvec2 weights[]; };
// a=B, count=1024*capacity channels.
void main() {
  uint i=index1(); if(i>=p.count) return;
  uint ch=i/1024u,j=i%1024u;
  weights[i]=(ch<status[11] && j<p.a)?initial[ch*p.a+j]:dvec2(0.0LF);
  if(ch<status[11] && j==0u) atomicAdd(status[9],1u);
}
)glsl";
const std::string multiply = common + R"glsl(
layout(binding=1) readonly buffer Cache { dvec2 cache[]; };
layout(binding=2) readonly buffer Weights { dvec2 weights[]; };
layout(binding=3) writeonly buffer Work { dvec2 work[]; };
// a=channel,b=number of valid blocks,c=first block.
void main() {
  uint i=index1(); if(i>=p.count) return;
  work[i]=(p.a<status[11] && i/1024u<p.b)?mul(cache[p.c*1024u+i],weights[p.a*1024u+i%1024u]):dvec2(0.0LF);
}
)glsl";
const std::string gather = common + R"glsl(
layout(binding=1) readonly buffer Work { dvec2 work[]; };
layout(binding=2) writeonly buffer Baseline { dvec2 baseline[]; };
// a=N,b=B,c=channel,d=first output sample,count=valid output count.
void main() {
  uint i=index1(); if(i>=p.count || p.c>=status[11]) return;
  uint hop=1024u-p.b+1u;
  dvec2 value=work[(i/hop)*1024u+(p.b-1u)+i%hop]/1024.0LF;
  if(!finite64(value)) reject(6u,5u,uint(p.d)+i,p.c);
  baseline[p.c*p.a+uint(p.d)+i]=value;
  if(i%hop==0u) atomicAdd(status[5],1u);
}
)glsl";
const std::string residual = common + R"glsl(
layout(binding=1) readonly buffer Original { vec2 original[]; };
layout(binding=2) readonly buffer Initial { dvec2 initial[]; };
layout(binding=3) readonly buffer Baseline { dvec2 baseline[]; };
layout(binding=4) readonly buffer Suffix { vec2 suffix[]; };
layout(binding=5) writeonly buffer Residual { vec2 residual[]; };
// a=N,b=B,count=N*capacity channels. Original is worker-rounded y, captured
// before its destructive FFT. Cq=Lq+wrapped edge; published output uses Lq only.
void main() {
  uint i=index1(); if(i>=p.count) return;
  uint ch=i/p.a,j=i%p.a;
  if(ch>=status[11]) { residual[i]=vec2(0); return; }
  dvec2 circular=baseline[i];
  for(uint k=j+1u;k<p.b;++k) circular+=mul(initial[ch*p.b+k],dvec2(suffix[p.b-1u+j-k]));
  vec2 value=vec2(dvec2(original[i])-circular);
  if(!finite32(original[i]) || !finite32(value)) reject(7u,6u,j,ch);
  residual[i]=value;
  if(i%128u==0u) { uint covered=min(128u,status[11]*p.a-i);
    atomicAdd(status[6],covered); atomicAdd(status[10],covered); }
}
)glsl";
const std::string castCorrection = common + R"glsl(
layout(binding=1) readonly buffer Solution { dvec2 solution[]; };
layout(binding=2) writeonly buffer Rounded { vec2 rounded[]; };
// a=B,count=B*capacity channels.
void main() {
  uint i=index1(); if(i>=p.count) return;
  vec2 value=(i/p.a<status[11])?vec2(solution[i]):vec2(0);
  if(!finite32(value)) reject(8u,7u,i%p.a,i/p.a);
  rounded[i]=value;
}
)glsl";
const std::string merge = common + R"glsl(
layout(binding=1) readonly buffer Baseline { dvec2 baseline[]; };
layout(binding=2) buffer Estimate { vec2 estimate[]; };
// a=N,count=N*capacity channels. One final FP32 cast, same accepted boundary.
void main() {
  uint i=index1(); if(i>=p.count || i/p.a>=status[11]) return;
  vec2 correction=estimate[i], value=vec2(baseline[i]+dvec2(correction));
  if(!finite32(correction) || !finite32(value)) reject(9u,8u,i%p.a,i/p.a);
  estimate[i]=value;
  if(i%128u==0u) atomicAdd(status[7],min(128u,status[11]*p.a-i));
}
)glsl";

class Refinement {
  Context& context_;
  uint32_t samples_, bins_, channels_, hop_, blocks_;
  std::unique_ptr<Buffer> status_, cache_, work_, suffix_, baseline_, lower_, solution_, initial_, weights_;
  std::unique_ptr<NativePlan> workPlan_, weightPlan_;
  std::unique_ptr<Kernel> pack_, factor_, solve_, weightsPack_, multiply_, gather_, residual_, cast_, merge_;
  bool toeplitz_ = false;
public:
  static uint64_t requiredBytes(uint32_t samples,uint32_t bins,uint32_t channels) {
    if(!bins || bins>fft || !samples || bins>samples || !channels || channels>8) return UINT64_MAX;
    const uint64_t blocks=(uint64_t(samples)+(fft-bins))/(fft-bins+1);
    const uint64_t cache=blocks*fft;
    if(cache>UINT32_MAX) return UINT64_MAX;
    return 16*(cache+uint64_t(tileBlocks)*fft+uint64_t(samples)*channels+
      uint64_t(bins)*bins+2*uint64_t(bins)*channels+uint64_t(fft)*channels)+
      8*std::max<uint32_t>(1,bins-1)+statusWords*4;
  }
  Refinement(Context& context,uint32_t samples,uint32_t bins,uint32_t channels,
      Buffer& reference,Buffer& correlations,Buffer& originalAndEstimate,Buffer& surveillance,Buffer& rounded)
    : context_(context),samples_(samples),bins_(bins),channels_(channels),
      hop_(bins && bins<=fft?fft-bins+1:1),blocks_(uint32_t((uint64_t(samples)+hop_-1)/hop_)) {
    const char* solver=std::getenv("BLAH2_GPU_TOEPLITZ_SOLVER");
    if(solver && std::string(solver)=="levinson") toeplitz_=true;
    else if(solver && *solver) throw std::invalid_argument("BLAH2_GPU_TOEPLITZ_SOLVER must be levinson");
    const auto& limits=context.candidate.properties.limits;
    const uint64_t needed=requiredBytes(samples,bins,channels);
    const uint32_t sharedRequired=toeplitz_?22*1024:19*1024;
    if(needed==UINT64_MAX || limits.maxComputeSharedMemorySize<sharedRequired ||
        limits.maxPerStageDescriptorStorageBuffers<6 || limits.maxDescriptorSetStorageBuffers<6)
      throw std::runtime_error("GPU-native refinement capacity is unsupported");
    auto buffer=[&](uint64_t bytes) {
      if(bytes>limits.maxStorageBufferRange) throw std::runtime_error("GPU-native storage range unavailable");
      return std::make_unique<Buffer>(context,bytes,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    };
    status_=std::make_unique<Buffer>(context,statusWords*4,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    cache_=buffer(uint64_t(blocks_)*fft*16);
    work_=buffer(uint64_t(tileBlocks)*fft*16);
    suffix_=buffer(std::max<uint32_t>(1,bins-1)*8);
    baseline_=buffer(uint64_t(samples)*channels*16);
    lower_=buffer(uint64_t(bins)*bins*16);
    solution_=buffer(uint64_t(bins)*channels*16);
    initial_=buffer(uint64_t(bins)*channels*16);
    weights_=buffer(uint64_t(fft)*channels*16);
    workPlan_=std::make_unique<NativePlan>(context,*work_,fft,tileBlocks,true,false,true);
    weightPlan_=std::make_unique<NativePlan>(context,*weights_,fft,channels,true,false,true);
    auto kernel=[&](const std::string& source,std::vector<Buffer*> buffers) {
      buffers.insert(buffers.begin(),status_.get());
      return std::make_unique<Kernel>(context,source.c_str(),buffers);
    };
    pack_=kernel(referencePack,{&reference,work_.get(),suffix_.get()});
    factor_=kernel(toeplitz_?toeplitzFactor:factor,{&correlations,lower_.get()});
    solve_=kernel(toeplitz_?toeplitzSolve:solve,{&correlations,lower_.get(),solution_.get()});
    weightsPack_=kernel(weightPack,{initial_.get(),weights_.get()});
    multiply_=kernel(multiply,{cache_.get(),weights_.get(),work_.get()});
    gather_=kernel(gather,{work_.get(),baseline_.get()});
    residual_=kernel(residual,{&originalAndEstimate,initial_.get(),baseline_.get(),suffix_.get(),&surveillance});
    cast_=kernel(castCorrection,{solution_.get(),&rounded});
    merge_=kernel(merge,{baseline_.get(),&originalAndEstimate});
    std::cerr<<"GPU native storage requested_bytes="<<needed<<" fft="<<fft<<" tile_blocks="<<tileBlocks
      <<" shader_float64=1 host_dsp_workspace_bytes=0 cpu_baseline_calls=0 cpu_solve_calls=0\n";
  }
  Buffer& borrowedReference() { return *baseline_; }
  Buffer& borrowedWork() { return *work_; }
  void resetStatus(uint32_t active) {
    std::array<uint32_t,statusWords> values{};
    values[activeChannels]=active;
    std::memcpy(status_->mapped,values.data(),sizeof(values));
    status_->flush(0,status_->bytes);
  }
  std::array<uint32_t,statusWords> readStatus() {
    status_->invalidate(0,status_->bytes);
    std::array<uint32_t,statusWords> values{};
    std::memcpy(values.data(),status_->mapped,sizeof(values));
    if(values[error]) {
      std::ostringstream why;
      why<<"GPU native precision guard code="<<values[error]
        <<" device_phase="<<values[phase]<<" row="<<values[row]<<" channel="<<values[channel];
      throw ClutterRejected(why.str());
    }
    return values;
  }
  uint32_t blocks() const { return blocks_; }
  uint32_t hop() const { return hop_; }
  void appendReference(VkCommandBuffer command) {
    for(uint32_t first=0;first<blocks_;first+=tileBlocks) {
      const uint32_t valid=std::min(tileBlocks,blocks_-first);
      pack_->append(command,{tileBlocks*fft,samples_,bins_,hop_,int32_t(first)});
      barrier(command);
      workPlan_->append(command,-1);
      barrier(command);
      VkBufferCopy copy{0,uint64_t(first)*fft*16,uint64_t(valid)*fft*16};
      vkCmdCopyBuffer(command,work_->handle,cache_->handle,1,&copy);
      barrier(command);
    }
  }
  void appendFactor(VkCommandBuffer command) {
    factor_->append(command,{128,samples_,bins_,0,0});
    barrier(command);
  }
  void appendSolve(VkCommandBuffer command,bool initial) {
    solve_->append(command,{128*channels_,samples_,bins_,initial?3u:4u,0});
    barrier(command);
    if(initial) {
      VkBufferCopy copy{0,0,initial_->bytes};
      vkCmdCopyBuffer(command,solution_->handle,initial_->handle,1,&copy);
      barrier(command);
    } else { cast_->append(command,{bins_*channels_,bins_,0,0,0});
    barrier(command); }
  }
  void appendBaseline(VkCommandBuffer command) {
    weightsPack_->append(command,{fft*channels_,bins_,0,0,0});
    barrier(command);
    weightPlan_->append(command,-1);
    barrier(command);
    for(uint32_t channel=0;channel<channels_;++channel)
      for(uint32_t first=0;first<blocks_;first+=tileBlocks) {
        const uint32_t valid=std::min(tileBlocks,blocks_-first),start=first*hop_;
        multiply_->append(command,{tileBlocks*fft,channel,valid,first,0});
        barrier(command);
        workPlan_->append(command,1);
        barrier(command);
        gather_->append(command,{std::min(valid*hop_,samples_-start),samples_,bins_,channel,int32_t(start)});
        barrier(command);
      }
  }
  void appendResidual(VkCommandBuffer command) {
    residual_->append(command,{samples_*channels_,samples_,bins_,0,0});
    barrier(command);
  }
  void appendMerge(VkCommandBuffer command) {
    merge_->append(command,{samples_*channels_,samples_,0,0,0});
    barrier(command);
  }
};

// Timestamp query pairs delimit actual device commands, never host fence latency.
class Timestamps {
  Context& context_;
  VkQueryPool pool_=VK_NULL_HANDLE;
  std::array<double,12> observed_{};
  uint32_t observedMask_=0;
public:
  explicit Timestamps(Context& context):context_(context) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType=VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount=24;
    check(vkCreateQueryPool(context.device,&info,nullptr,&pool_),"GPU-native timestamp pool");
  }
  double observed(uint32_t stage)const{if(stage>=12||!(observedMask_&(1u<<stage)))throw std::logic_error("GPU timestamp was not observed");
  return observed_[stage];}
  uint32_t observedMask()const{return observedMask_;}
  ~Timestamps(){ if(pool_) vkDestroyQueryPool(context_.device,pool_,nullptr); }
  void begin(VkCommandBuffer command,uint32_t stage) {
    vkCmdResetQueryPool(command,pool_,2*stage,2);
    vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,pool_,2*stage);
  }
  void end(VkCommandBuffer command,uint32_t stage) {
    vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,pool_,2*stage+1);
  }
  double milliseconds(uint32_t stage) {
    gpu_host::Scope hostQuery(context_.hostTiming,"query.read",stage);
    uint64_t ticks[2]{};
    check(vkGetQueryPoolResults(context_.device,pool_,2*stage,2,sizeof(ticks),ticks,sizeof(uint64_t),
      VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"GPU-native timestamp read");
    const uint64_t mask=context_.timestampValidBits==64?UINT64_MAX:((uint64_t(1)<<context_.timestampValidBits)-1);
    if(stage>=observed_.size())throw std::logic_error("GPU timestamp evidence index overflow");
    const double value=double((ticks[1]-ticks[0])&mask)*double(context_.candidate.properties.limits.timestampPeriod)/1e6;
    observed_[stage]=value;
    observedMask_|=1u<<stage;
    return value;
  }
};
} // namespace gpu_native
