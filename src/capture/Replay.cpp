#include "Replay.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace blah2 {
namespace {
using Clock = std::chrono::steady_clock;
class Locks {
  const std::vector<IqData*>& buffers_;
public:
  explicit Locks(const std::vector<IqData*>& buffers):buffers_(buffers) { for(auto* b:buffers_) b->lock(); }
  ~Locks() { for(auto it=buffers_.rbegin();it!=buffers_.rend();++it) (*it)->unlock(); }
};
void rest() { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}

void ReplayPlayer::run(const std::string& file,const ReplayOptions& options,
    const std::vector<IqData*>& buffers,uint32_t frameSamples,bool loop,
    const std::atomic<bool>& stopped,const Progress& publish,
    const std::function<bool()>& consumerBusy,const Control& control) {
  if(!frameSamples || buffers.size()!=options.channels) throw std::invalid_argument("Invalid playback channel/frame configuration");
  for(auto* buffer:buffers) if(!buffer || buffer->get_n()<frameSamples)
    throw std::invalid_argument("Playback buffer must hold at least one radar frame");
  ReplayProgress progress; publish(progress);
  RecordingReader reader(file,options);
  progress.total=reader.metadata().samples; progress.sampleRate=reader.metadata().sampleRate;
  progress.provenance=reader.provenance(); publish(progress);
  if(progress.total<frameSamples) throw std::runtime_error("Recording is shorter than one processing frame; reduce the frame interval");
  do {
    progress.state="playing"; progress.samples=0; progress.trailingSamples=0; publish(progress);
    auto start=Clock::now(); IqBlock block; uint64_t controlRevision=0;
    while(!stopped.load() && reader.read(block)) {
      size_t offset=0;
      while(offset<block.front().size() && !stopped.load()) {
        auto command=control();
        if(command.revision!=controlRevision) {
          controlRevision=command.revision;
          reader.seek(std::min(command.seekSample,progress.total));
          { Locks locks(buffers); for(auto* buffer:buffers)buffer->clear(); }
          progress.samples=std::min(command.seekSample,progress.total);
          ++progress.generation; progress.state=command.paused?"paused":"playing";publish(progress);
          start=Clock::now()-std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(double(progress.samples)/progress.sampleRate));
          block.clear(); break;
        }
        if(command.paused) {
          if(progress.state!="paused"){progress.state="paused";publish(progress);}
          rest(); start=Clock::now()-std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(double(progress.samples)/progress.sampleRate));
          continue;
        } else if(progress.state=="paused") { progress.state="playing";publish(progress); }
        const double elapsed=std::chrono::duration<double>(Clock::now()-start).count();
        const uint64_t permitted=uint64_t(elapsed*progress.sampleRate)+1;
        if(progress.samples>=permitted) { rest(); continue; }
        // Do not turn a large on-disk block into an instantaneous burst.  A
        // small quantum also gives cancellation and the consumer timely turns.
        size_t count=std::min<uint64_t>(block.front().size()-offset,
          std::min<uint64_t>(4096, permitted-progress.samples));
        {
          Locks locks(buffers);
          for(auto* buffer:buffers) count=std::min<size_t>(count,buffer->get_n()-buffer->get_length());
          // The live IqData queue normally drops its oldest sample when full.
          // File playback must not: consume only the space available on EVERY channel.
          for(size_t ch=0;ch<buffers.size();++ch)
            for(size_t i=0;i<count;++i) buffers[ch]->push_back(block[ch][offset+i]);
        }
        if(!count) { rest(); continue; }
        offset+=count; progress.samples+=count; publish(progress);
      }
    }
    if(stopped.load()) break;
    progress.state="draining"; publish(progress);
    bool drained=false;
    while(!stopped.load() && !drained) {
      {
        Locks locks(buffers);
        drained=!consumerBusy();
        for(auto* buffer:buffers) drained=drained && buffer->get_length()<frameSamples;
        if(drained) {
          progress.trailingSamples=buffers.front()->get_length();
          for(auto* buffer:buffers) buffer->clear();
        }
      }
      if(!drained) rest();
    }
    if(stopped.load()) break;
    if(!loop) { progress.state="complete"; publish(progress); return; }
    ++progress.loops; ++progress.generation; reader.rewind();
  } while(!stopped.load());
  progress.state="stopped"; publish(progress);
}
}
