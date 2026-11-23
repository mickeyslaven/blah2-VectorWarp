#include "capture/Recording.h"
#include "capture/Replay.h"
#include "capture/Sha256.h"
#include "capture/RecordingAcquisitionJson.h"
#include "capture/Source.h"
#include "capture/rspduo/SampleSequence.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <atomic>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
// Interpose only in this test executable: exercise the real writer's stall,
// failure, batching, and queue bound without adding production I/O hooks.
namespace recording_write_fixture {
std::atomic<bool> armed{false}, blocked{false}, entered{false}, fail{false}, headers{false};
std::atomic<unsigned> calls{0};
dev_t device=0; ino_t inode=0;
}
extern "C" ssize_t write(int fd,const void* bytes,size_t size) {
  using namespace recording_write_fixture;
  struct stat status{};
  const bool header = headers.load() && size >= 8 && !std::memcmp(bytes,"BLAH2IQ\0",8);
  if(armed.load() && (header || (!headers.load() && !::fstat(fd,&status) &&
      status.st_dev==device && status.st_ino==inode))) {
    ++calls; entered=true;
    while(blocked.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(fail.load()) { errno=EIO; return -1; }
  }
  return ::syscall(SYS_write,fd,bytes,size);
}
struct RecordingWriteFixture {
  explicit RecordingWriteFixture(const std::filesystem::path& path,bool stall=false,
      bool nextHeader=false) {
    struct stat status{};
    if(::stat(path.c_str(),&status)) throw std::runtime_error("Cannot inspect injected recording");
    using namespace recording_write_fixture;
    device=status.st_dev; inode=status.st_ino; calls=0; entered=false; fail=false;
    blocked=stall; headers=nextHeader; armed=true;
  }
  ~RecordingWriteFixture() { recording_write_fixture::blocked=false; recording_write_fixture::armed=false; }
  void await_write() {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!recording_write_fixture::entered && std::chrono::steady_clock::now()<deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if(!recording_write_fixture::entered) throw std::runtime_error("Injected write did not start");
  }
};
#endif

using namespace blah2;
class MockSource final : public Source {
public:
  explicit MockSource(const std::string& path, bool* save)
    : Source("Mock", 100000000, 2000000, path, save) {}
  void process(IqData*, IqData*) override {}
  void start() override {}
  void stop() override {}
};
class BackendFixtureSource final : public Source {
  std::string details;
public:
  BackendFixtureSource(const std::string& receiver,const std::string& path,
      bool* save,std::string provenance)
    : Source(receiver,527000000,2400000,path,save),details(std::move(provenance)) {}
  void process(IqData*, IqData*) override {}
  void start() override {}
  void stop() override {}
  SampleTimePosition sample_time_position() const override {
    return {true,7,1000,40000000,2400000,"external","internal","B210","A:A A:B"};
  }
  std::string recording_provenance_json() const override { return details; }
  std::vector<double> recording_applied_gain_db() const override {
    return type == "Usrp" ? std::vector<double>{10.25, 20.5} : std::vector<double>{};
  }
};
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void rejects(const std::function<void()>& work,const char* message) {
  bool rejected=false; try { work(); } catch(const std::runtime_error&) { rejected=true; }
  require(rejected,message);
}
void write(const std::filesystem::path& path,const std::vector<uint8_t>& bytes) {
  std::ofstream stream(path,std::ios::binary); stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}

struct CacheFixtureIo {
  struct Call { char operation; uint64_t offset, bytes; bool wait; };
  static inline std::vector<Call> calls;
  static inline bool supported=true, failWriteback=false, failFlush=false;
  static void reset() { calls.clear(); supported=true; failWriteback=failFlush=false; }
  static void prepare(int) {}
  static bool writeback(int, uint64_t offset, uint64_t bytes, bool wait) {
    calls.push_back({'w',offset,bytes,wait});
    if(failWriteback) throw std::runtime_error("injected writeback failure");
    return supported;
  }
  static void flush(int) {
    calls.push_back({'f',0,0,false});
    if(failFlush) throw std::runtime_error("injected flush failure");
  }
  static void discard(int, uint64_t offset, uint64_t bytes) {
    calls.push_back({'d',offset,bytes,false});
  }
};

struct MutatingDigestIo : RecordingFileCacheIo {
  static inline int mutationFd = -1;
  static inline bool mutated = false;
  static void discard(int, uint64_t, uint64_t) {
    if (mutated) return;
    const uint8_t byte = 0;
    const timespec times[2]{{1234567890,0}, {1234567891,0}};
    // Rewrite an already consumed sample without changing the file size. Pin
    // mtime explicitly so the test also works on coarse timestamp filesystems.
    mutated = ::pwrite(mutationFd, &byte, 1, 83) == 1 && !::futimens(mutationFd, times);
  }
};

void recording_cache_tests(const std::filesystem::path& directory) {
  using Cache = RecordingFileCache<CacheFixtureIo>;
  constexpr auto window = Cache::windowBytes;
  CacheFixtureIo::reset(); Cache writer;
  writer.written(1,window-1);
  require(CacheFixtureIo::calls.empty(),"Partial cache window triggered writeback");
  writer.written(1,window+17);
  require(CacheFixtureIo::calls.size()==1 && CacheFixtureIo::calls[0].operation=='w' &&
    !CacheFixtureIo::calls[0].wait && CacheFixtureIo::calls[0].bytes==window,
    "First complete window was not submitted asynchronously");
  writer.written(1,2*window+31);
  const auto& calls=CacheFixtureIo::calls;
  require(calls.size()==4 && calls[1].operation=='w' && calls[1].wait &&
    calls[1].offset==0 && calls[1].bytes==window && calls[2].operation=='d' &&
    calls[2].offset==0 && calls[2].bytes==window && calls[3].operation=='w' &&
    !calls[3].wait && calls[3].offset==window && calls[3].bytes==window,
    "Writer discarded pending bytes before writeback completion or crossed its range");
  CacheFixtureIo::failWriteback=true;
  rejects([&]{writer.written(1,3*window);},"Writeback error was hidden");
  require(calls.back().operation=='w',"Failed writeback discarded uncompleted bytes");

  CacheFixtureIo::reset(); Cache fallback; CacheFixtureIo::supported=false;
  fallback.written(1,window);
  require(calls.size()==3 && calls[1].operation=='f' && calls[2].operation=='d',
    "Unsupported range writeback did not flush before discarding");
  CacheFixtureIo::failFlush=true;
  rejects([&]{fallback.written(1,2*window);},"Fallback flush failure was hidden");
  require(calls.back().operation=='f',"Failed fallback flush discarded bytes");

  CacheFixtureIo::reset(); Cache reader;
  reader.consumed(1,window-1); reader.consumed(1,window+23);
  reader.consumed(1,window+23,true);
  require(calls.size()==2 && calls[0].operation=='d' && calls[0].offset==0 &&
    calls[0].bytes==window && calls[1].operation=='d' && calls[1].offset==window &&
    calls[1].bytes==23,"Digest cache policy discarded unread bytes or retained its tail");
  Cache::durable(1,window+23);
  require(calls.back().offset==0 && calls.back().bytes==window+23,
    "Durable completion failed to discard the rewritten recording header");

  // Exercise multiple cache windows, exact sample replay, and completion.
  const auto filename=directory/"cache-window.blah2iq";
  const IqBlock block(2,std::vector<std::complex<float>>(262144,{0.125f,-0.5f}));
  {
    RecordingWriter recording(filename,{2,1000,100000000,0,"blah2"});
    for(unsigned i=0;i<9;++i) recording.append(block);
    recording.close();
  }
  require(std::filesystem::file_size(filename)==64+9*(16+262144ULL*16),
    "Cache management changed the recording byte count");
  // Independent Python struct.pack/hashlib oracle: v1 header, nine complete
  // channel-major binary32 blocks, and the final sample-count/complete fields.
  const std::string expected = "f20a82a830969590fef87d732bcc2ce85912e28d42e2a59fc52c5f558df2cf9e";
  uint64_t hashedBytes = 0;
  require(acquisition::fileDigest(filename, &hashedBytes)==expected &&
    hashedBytes==std::filesystem::file_size(filename),
    "Cache management changed bytes or the acquisition digest");
  const int digestFd = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC);
  require(digestFd >= 0 && ::lseek(digestFd, 117, SEEK_SET)==117, "Cannot position digest fixture");
  CacheFixtureIo::reset();
  const auto integrity = acquisition::fileDigest<CacheFixtureIo>(digestFd);
  const auto position = ::lseek(digestFd, 0, SEEK_CUR); ::close(digestFd);
  require(integrity.first==hashedBytes && integrity.second==expected && position==117,
    "Shared full-file verification changed the replay cursor or digest");
  require(calls.size()==3 && calls[0].operation=='d' && calls[0].offset==0 && calls[0].bytes==window &&
    calls[1].operation=='d' && calls[1].offset==window && calls[1].bytes==window &&
    calls[2].operation=='d' && calls[2].offset==2*window && calls[2].bytes==hashedBytes-2*window,
    "Shared full-file verification failed to evict consumed windows and tail");
  ReplayOptions options{"blah2","Usrp",2,1000,100000000,0};
  options.expectedRecordingSha256 = expected;
  RecordingReader replay(filename,options);
  IqBlock recovered; unsigned count=0;
  while(replay.read(recovered)) {
    require(recovered==block,"Cache writeback changed exact IQ samples"); ++count;
  }
  require(count==9 && replay.metadata().samples==9*262144ULL,
    "Cache writeback changed completion or block lineage");
  const int mutationFd = ::open(filename.c_str(), O_WRONLY | O_CLOEXEC);
  require(mutationFd >= 0, "Cannot open digest mutation fixture");
  const timespec before[2]{{1234567890,0}, {1234567890,0}};
  require(!::futimens(mutationFd, before), "Cannot initialize digest mutation timestamp");
  MutatingDigestIo::mutationFd = mutationFd;
  bool changed = false;
  try { (void)acquisition::fileDigest<MutatingDigestIo>(filename); }
  catch (const std::runtime_error&) { changed = true; }
  ::close(mutationFd);
  require(MutatingDigestIo::mutated && changed,
    "Acquisition digest accepted a same-size overwrite of already hashed bytes");
}

void recording_batch_tests(const std::filesystem::path& directory) {
  const auto path=directory/"callback-batches.blah2iq";
  IqBlock callback(2,std::vector<std::complex<float>>(4086));
  for(unsigned channel=0;channel<2;++channel)
    for(unsigned i=0;i<4086;++i)
      callback[channel][i] = {float(16*channel+i%251)/256, -float(32*channel+i%127+1)/128};
  RecordingWriter writer(path,{2,24576000,548000000,0,"blah2"});
#if defined(__linux__)
  RecordingWriteFixture writes(path);
#endif
  for(unsigned i=0;i<41;++i) writer.append(callback);
  writer.append_slice(callback,11,17);
  writer.close();
  require(writer.samples()==41*4086+17,"Batching changed accepted sample accounting");
#if defined(__linux__)
  require(recording_write_fixture::calls==4,
    "Callback batches did not use two full writes, one tail, and one completion header");
#endif
  // Independent struct.pack/hashlib oracle, including all original callback
  // headers, sequence offsets, channel order, and the short final slice.
  require(acquisition::fileDigest(path)=="1dfbd75c2a6648ff790c642fc553c2b3b89f15ce494e306ad023d3f9a95c2128",
    "Callback batching changed exact on-disk bytes");
  RecordingReader reader(path,{"blah2","Usrp",2,24576000,548000000,0});
  IqBlock actual;
  for(unsigned i=0;i<41;++i)
    require(reader.read(actual) && actual==callback,"Batching changed callback boundaries");
  IqBlock tail(2);
  for(unsigned channel=0;channel<2;++channel)
    tail[channel].assign(callback[channel].begin()+11,callback[channel].begin()+28);
  require(reader.read(actual) && actual==tail &&
    !reader.read(actual),"Batching lost or extended its partial tail");

  const auto abortedPath=directory/"aborted-batch.blah2iq";
  RecordingWriter aborted(abortedPath,{2,24576000,548000000,0,"blah2"});
  aborted.append(callback); aborted.abort();
  require(aborted.abort_ready(),"Unpublished partial batch retained a disk operation");
  rejects([&]{aborted.append(callback);},"Aborted recording accepted more IQ");
  rejects([&]{aborted.close();},"Aborted recording was finalized");
  rejects([&]{RecordingReader invalid(abortedPath,{"blah2","Usrp",2,24576000,548000000,0});},
    "Aborted recording was admitted to replay");

#if defined(__linux__)
  {
    const auto queuedPath=directory/"bounded-stalled-writer.blah2iq";
    RecordingWriter queued(queuedPath,{2,24576000,548000000,0,"blah2"});
    RecordingWriteFixture stalled(queuedPath,true);
    unsigned accepted=0;
    for(;accepted<17;++accepted) queued.append(callback);
    stalled.await_write();
    bool full=false;
    try { for(;accepted<2048;++accepted) queued.append(callback); }
    catch(const std::runtime_error& error) { full=std::string(error.what()).find("bounded write queue is full")!=std::string::npos; }
    require(full && uint64_t(accepted)*(16+4086*16)<=(64ULL<<20) &&
      uint64_t(accepted+1)*(16+4086*16)>(63ULL<<20),
      "Stalled writer did not enforce its 64 MiB capacity including in-flight/partial batches");
    require(!queued.abort_ready(),"Abort concealed an in-flight disk operation");
    recording_write_fixture::blocked=false;
    rejects([&]{queued.close();},"Queue exhaustion produced a complete recording");
    rejects([&]{RecordingReader invalid(queuedPath,{"blah2","Usrp",2,24576000,548000000,0});},
      "Queue-exhausted recording was admitted to replay");
  }
  {
    const auto failurePath=directory/"failed-batch-write.blah2iq";
    RecordingWriter failed(failurePath,{2,24576000,548000000,0,"blah2"});
    RecordingWriteFixture disk(failurePath,true);
    for(unsigned i=0;i<17;++i) failed.append(callback);
    disk.await_write();
    recording_write_fixture::fail=true;
    recording_write_fixture::blocked=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!failed.abort_ready() && std::chrono::steady_clock::now()<deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(failed.abort_ready(),"Asynchronous writer failure did not retire its in-flight batch");
    rejects([&]{failed.append(callback);},"Asynchronous disk failure accepted later callback IQ");
    require(failed.samples()==17*4086,"Failed callback advanced accepted sample accounting");
    rejects([&]{failed.close();},"Injected disk failure was hidden");
    rejects([&]{RecordingReader invalid(failurePath,{"blah2","Usrp",2,24576000,548000000,0});},
      "Disk-failed recording was admitted to replay");
  }
  {
    bool save=false;
    MockSource source(directory.string(),&save);
    const auto oldPath=source.open_file();
    RecordingWriteFixture disk(oldPath,true);
    for(unsigned i=0;i<17;++i) source.record_block(callback);
    disk.await_write();
    auto stopped=std::async(std::launch::async,[&]{source.recording_epoch_discontinuity("injected stalled epoch");});
    const bool returned=stopped.wait_for(std::chrono::milliseconds(250))==std::future_status::ready;
    if(!returned) recording_write_fixture::blocked=false;
    stopped.get();
    require(returned,"Receiver failure waited for a stalled disk worker");
    source.record_block(callback);
    source.service_recording(); // Must also refuse to join the stalled writer.
    require(source.recording_status().resumePending && !source.recording_status().active,
      "Epoch rollover joined a disk-blocked writer in the receiver callback");
    recording_write_fixture::blocked=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!source.recording_status().active && std::chrono::steady_clock::now()<deadline) {
      source.service_recording();
      source.record_block(callback);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(source.recording_status().active && source.recording_status().samples==4086 &&
      source.recording_status().file!=oldPath,"Idle aborted epoch did not resume with fresh bytes");
    source.close_file();
    rejects([&]{RecordingReader invalid(oldPath,{"blah2","Mock",2,24576000,548000000,0});},
      "Aborted pre-boundary epoch was finalized during rollover");
  }
  for(bool cancel : {false,true}) {
    bool save=false;
    BackendFixtureSource source("Usrp",directory.string(),&save,"{}");
    const auto oldPath=source.open_file();
    source.record_block(callback,{true,7,100,0,2400000,{},{},{},{}});
    source.recording_epoch_discontinuity("prepare next epoch");
    RecordingWriteFixture disk(oldPath,true,true);
    auto prepared=std::async(std::launch::async,[&]{source.service_recording();});
    // Release the injected disk before future destruction on every assertion.
    struct ReleaseDisk { ~ReleaseDisk() { recording_write_fixture::blocked=false; } } release;
    disk.await_write();
    auto ingress=std::async(std::launch::async,[&]{
      source.record_block(callback,{true,8,200,0,2400000,{},{},{},{}});
      if(cancel) source.recording_discontinuity("terminal fault while preparing");
    });
    const bool returned=ingress.wait_for(std::chrono::milliseconds(250))==std::future_status::ready;
    if(!returned) recording_write_fixture::blocked=false;
    ingress.get();
    require(returned,"Resume header I/O held the acquisition state lock");
    require(!source.recording_status().active,"Pending resume fabricated active recording");
    recording_write_fixture::blocked=false;
    prepared.get();
    source.record_block(callback,{true,0,900,0,2400000,{},{},{},{}});
    const auto state=source.recording_status();
    require(cancel ? (!state.active && !state.resumePending) :
      (state.active && !state.resumePending && state.samples==4086),
      "Prepared resume ignored cancellation or lost its first accepted block");
    if(!cancel) {
      source.close_file();
      RecordingReader resumed(state.file,{"blah2","Usrp",2,2400000,527000000,0});
      require(resumed.provenance().epoch==0 && resumed.provenance().sourceFirstSample==900 &&
        resumed.read(actual) && actual==callback && !resumed.read(actual),
        "Control preparation substituted an earlier epoch/position or unaccepted IQ");
    }
  }
#endif
}

void acquisition_run_tests() {
  // Sixteen seconds at the production UHD rate, without allocating 6 GiB IQ.
  constexpr uint64_t rate = 24576000, block = 4086, duration = 16;
  constexpr uint64_t blocks = (rate * duration + block - 1) / block;
  const uint64_t first = 9007199254740993ULL;
  std::vector<RecordingFrameLineage> frames;
  for (uint64_t index = 0; index < blocks; ++index)
    require(acquisition::appendFrame(frames, {index + 1, first + index * block, block, 0, block}),
      "UHD callback granularity exhausted exact acquisition before 32 CPIs");
  require(frames.size() == 1 && frames.front().repeat == blocks &&
    frames.front().sourceCount == block && frames.front().count == block,
    "Run compression lost original UHD callback boundaries");
  const auto json = acquisition::serialize(nullptr, "Usrp", 2, rate, 548000000,
    7, first, first + blocks * block, true, frames);
  require(json.size() < 1024, "Constant-rate lineage metadata grew with recording duration");
  auto bad = frames;
  require(!acquisition::appendFrame(bad, {blocks + 1, first + blocks * block + 1, block, 0, block}),
    "Run compression erased a source-sample gap");
  require(!acquisition::appendFrame(bad, {blocks + 2, first + blocks * block, block, 0, block}),
    "Run compression erased an ingress-sequence gap");
  require(acquisition::appendFrame(frames, {blocks + 1, first + blocks * block, 10, 0, 10}) && frames.size() == 2,
    "Changed callback size did not start a new exact run");
  std::vector<RecordingFrameLineage> alternating;
  uint64_t sample = 0;
  for (size_t index = 0; index < acquisition::maximumFrames; ++index) {
    const uint64_t count = 1 + index % 2;
    require(acquisition::appendFrame(alternating, {index + 1, sample, count, 0, count}), "Bounded run rejected early");
    sample += count;
  }
  require(!acquisition::appendFrame(alternating, {acquisition::maximumFrames + 1, sample, 1, 0, 1}) &&
    alternating.size() == acquisition::maximumFrames, "Variable callback lineage exceeded its memory bound");
  std::vector<RecordingFrameLineage> native;
  require(acquisition::appendFrame(native, {1, 0, 4, 0, 4, {1,2,3,4}}) &&
    acquisition::appendFrame(native, {2, 4, 4, 0, 4, {1,2,3,4}}) && native.size() == 2,
    "Native conditioning frame means were merged");
  std::vector<RecordingFrameLineage> edge;
  require(acquisition::appendFrame(edge, {1, 0, 4, 2, 2}) &&
    acquisition::appendFrame(edge, {2, 4, 4, 0, 4}) && edge.size() == 2,
    "Trimmed acquisition edge was merged");
}

void acquisition_contract_tests(const std::filesystem::path& directory) {
  unsigned serial = 0;
  const auto save = [&](const std::string& json, unsigned channels) {
    const auto filename = directory / ("acquisition-" + std::to_string(++serial) + ".blah2iq");
    RecordingWriter writer(filename, {channels, 1000, 100000000, 0, "blah2", json});
    writer.append(IqBlock(channels, std::vector<std::complex<float>>(12, {0.125f, -0.5f})));
    writer.close(); return filename;
  };
  const auto hash = [](const std::filesystem::path& filename) {
    std::ifstream input(filename, std::ios::binary);
    return acquisition::digest(std::string(std::istreambuf_iterator<char>(input), {}));
  };
  for (const std::string receiver : {"Usrp", "RspDuo", "HackRF", "Kraken"}) {
    const unsigned channels = receiver == "Kraken" ? 5 : 2;
    RecordingBlockMetadata metadata;
    metadata.receiverDetailsJson = "{\"identity\":\"fixture-device\",\"requestedGain\":[12,13]}";
    metadata.perChannelGainDb.assign(channels, 14.199999809265137);
    metadata.context.requestedChannelGainDb.assign(channels, 14.2);
    const bool readback = receiver == "Usrp" || receiver == "Kraken";
    metadata.gainProvenance = readback ? "sample-aligned-readback" : "requested-settings-only";
    metadata.context.referenceMode = channels == 5 ? "array_eigenbeam" : "dedicated";
    metadata.context.coherent = true;
    if (channels == 5) {
      metadata.context.referenceChannels = {0,1,2,3,4};
      metadata.context.surveillanceChannels = {0,1,2,3,4};
      metadata.context.geometryJson = "{\"units\":\"m\",\"x_axis_bearing_deg_true\":0,\"elements\":[{\"daq_channel\":0,\"position\":[0,0,0]},{\"daq_channel\":1,\"position\":[1,0,0]},{\"daq_channel\":2,\"position\":[0,1,0]},{\"daq_channel\":3,\"position\":[-1,0,0]},{\"daq_channel\":4,\"position\":[0,-1,0]}]}";
      metadata.context.geometryConfirmed = true;
    } else { metadata.context.referenceChannels = {0}; metadata.context.surveillanceChannels = {1}; }
    metadata.context.manifoldSha256 = {{"bearing", std::string(64, 'c')}, {"detection", std::string(64, 'd')}};
    metadata.context.configurationSha256 = std::string(64, 'a'); metadata.context.codeSha256 = std::string(64, 'b');
    // The same float-frame contract serves every receiver; capability strength
    // still comes from its actual gain/geometry metadata, never compression.
    {
      std::vector<RecordingFrameLineage> frames;
      for (uint64_t index = 0; index < 3; ++index)
        require(acquisition::appendFrame(frames, {17 + index, 100 + 4 * index, 4, 0, 4}), "Cannot append exact run");
      const auto runJson = acquisition::serialize(&metadata, receiver, channels, 1000, 100000000,
        7, 100, 112, true, frames);
      const auto saveRun = [&](const std::string& value, unsigned blockSize = 4) {
        const auto name = directory / ("acquisition-run-" + std::to_string(++serial) + ".blah2iq");
        RecordingWriter writer(name, {channels,1000,100000000,0,"blah2",value});
        for (unsigned at = 0; at < 12; at += blockSize)
          writer.append(IqBlock(channels, std::vector<std::complex<float>>(blockSize, {0.125f,-0.5f})));
        writer.close(); return name;
      };
      const auto path = saveRun(runJson);
      ReplayOptions runOptions{"blah2",receiver,channels,1000,100000000,0};
      runOptions.expectedRecordingSha256 = hash(path);
      RecordingReader reader(path, runOptions);
      require(reader.provenance().frames.size() == 1 && reader.provenance().frames[0].repeat == 3 &&
        reader.provenance().frames[0].sourceCount == 4 && reader.provenance().exactAcquisition == readback,
        "Compressed acquisition changed exactness or expanded its bounded metadata");
      IqBlock data;
      for (unsigned index = 0; index < 3; ++index)
        require(reader.read(data) && data[0].size() == 4 && data[0][3] == std::complex<float>(0.125f,-0.5f),
          "Compressed metadata changed native replay blocks");
      require(!reader.read(data), "Compressed acquisition replay did not reach EOF");
      for (const char* repeat : {"0", "2", "4", "18446744073709551615"}) {
        rapidjson::Document altered; altered.Parse(runJson.c_str());
        altered["frames"][0]["repeat"].SetString(repeat, altered.GetAllocator());
        const auto invalid = saveRun(acquisition::encode(altered));
        rejects([&] { RecordingReader bad(invalid, {"blah2",receiver,channels,1000,100000000,0}); },
          "Invalid repeated lineage was accepted");
      }
      for (const char* field : {"schema", "sequence", "sliceOffset"}) {
        rapidjson::Document altered; altered.Parse(runJson.c_str());
        if (std::string(field) == "schema") altered["schema"].SetString("vectorwarp-recording-acquisition-v1");
        else altered["frames"][0][field].SetString(
          std::string(field) == "sequence" ? "18446744073709551615" : "1",
          altered.GetAllocator());
        const auto invalid = saveRun(acquisition::encode(altered));
        rejects([&] { RecordingReader bad(invalid, {"blah2",receiver,channels,1000,100000000,0}); },
          "Frame-run version, overflow or slice violation was accepted");
      }
      const auto mismatched = saveRun(runJson, 6);
      rejects([&] { RecordingReader bad(mismatched, {"blah2",receiver,channels,1000,100000000,0}); },
        "Frame run did not match stored callback boundaries");
      // A v2 contract with no repeated entries must still bind the actual
      // block boundaries; removing repeat must not downgrade validation.
      for (bool explicitRepeat : {false, true}) {
        rapidjson::Document singleton; singleton.Parse(runJson.c_str());
        auto& frame = singleton["frames"][0];
        frame["sourceCount"].SetString("12"); frame["count"].SetString("12");
        if (explicitRepeat) frame["repeat"].SetString("1");
        else frame.RemoveMember("repeat");
        const auto singleJson = acquisition::encode(singleton);
        const auto invalid = saveRun(singleJson);
        rejects([&] { RecordingReader bad(invalid, {"blah2",receiver,channels,1000,100000000,0}); },
          "Unrepeated v2 lineage bypassed stored callback boundaries");
        const auto valid = saveRun(singleJson, 12);
        RecordingReader single(valid, {"blah2",receiver,channels,1000,100000000,0});
        require(single.provenance().frames.size() == 1 && single.provenance().frames[0].repeat == 1,
          "Valid unrepeated v2 lineage was rejected");
        singleton["schema"].SetString("vectorwarp-recording-acquisition-v1");
        frame.RemoveMember("repeat");
        const auto legacy = saveRun(acquisition::encode(singleton));
        RecordingReader compatible(legacy, {"blah2",receiver,channels,1000,100000000,0});
        require(compatible.provenance().acquisitionIntegrityValid,
          "Existing v1 lineage semantics changed");
      }
    }
    metadata.context.geographicSitesJson = R"({"schema":"vectorwarp-recorded-geographic-sites-v1","provenance":"operator-configured-at-acquisition","rx":{"latitude":40.1,"longitude":-80.2,"altitude":300.0,"site_id":"recorded-rx"},"tx":{"latitude":40.5,"longitude":-80.0,"altitude":450.0},"illuminators":{"lane-a":{"frequencyHz":100000000,"transmitter":{"latitude":40.5,"longitude":-80.0,"altitude":450.0,"site_id":"tower-a"}},"lane-b":{"frequencyHz":101000000,"transmitter":{"latitude":40.6,"longitude":-79.5,"altitude":500.0}}}})";
    const uint64_t first = 9007199254740993ULL;
    const auto json = acquisition::serialize(&metadata, receiver, channels, 1000, 100000000,
      9007199254740995ULL, first, first + 12, true, {{17, first - 2, 16, 2, 12}});
    const auto filename = save(json, channels);
    ReplayOptions options{"blah2", "HackRF", channels, 1000, 100000000, 0};
    options.expectedRecordingSha256 = hash(filename);
    RecordingReader ordinary(filename, options);
    require(ordinary.provenance().requested_gains_for_manifold() == metadata.context.requestedChannelGainDb,
      "Requested acquisition gains did not round trip separately from applied readback");
    require(ordinary.provenance().geographicSitesAvailable &&
      ordinary.provenance().acquisitionContext.geographicSitesJson == metadata.context.geographicSitesJson,
      "Authenticated geographic sites did not round trip across receiver topology");
    auto untrusted = options; untrusted.expectedRecordingSha256.clear();
    rejects([&] { (void)RecordingReader(filename, untrusted).provenance().requested_gains_for_manifold(); },
      "Untrusted requested gains became measured-manifold replay identity");
    {
      rapidjson::Document altered; altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
      altered.RemoveMember("requestedChannelGainDb");
      const auto legacy = save(acquisition::encode(altered), channels);
      auto legacyOptions = options; legacyOptions.expectedRecordingSha256 = hash(legacy);
      RecordingReader old(legacy, legacyOptions);
      require(old.provenance().acquisitionContext.requestedChannelGainDb.empty(),
        "Old recording invented requested gains from readback");
      rejects([&] { (void)old.provenance().requested_gains_for_manifold(); },
        "Missing requested gains did not fail measured-manifold replay closed");
      altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
      altered["requestedChannelGainDb"].PopBack();
      const auto invalid = save(acquisition::encode(altered), channels);
      rejects([&] { RecordingReader bad(invalid, untrusted); }, "Partial requested gain vector was accepted");
      altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
      altered["requestedChannelGainDb"][0].SetString("14.2");
      const auto malformed = save(acquisition::encode(altered), channels);
      rejects([&] { RecordingReader bad(malformed, untrusted); }, "Nonnumeric requested gain was accepted");
    }
    require(!RecordingReader(filename, untrusted).provenance().geographicSitesAvailable,
      "Self-declared geographic metadata became trusted");
    {
      rapidjson::Document altered; altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
      altered.RemoveMember("geographicSites"); altered.RemoveMember("geographicSitesSha256");
      const auto old = save(acquisition::encode(altered), channels);
      auto oldOptions = options; oldOptions.expectedRecordingSha256 = hash(old);
      require(!RecordingReader(old, oldOptions).provenance().geographicSitesAvailable,
        "Old acquisition contract inherited geographic claims");
      altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
      altered["geographicSites"]["rx"]["latitude"].SetDouble(41.0);
      const auto tampered = save(acquisition::encode(altered), channels);
      auto tamperedOptions = options; tamperedOptions.expectedRecordingSha256 = hash(tampered);
      rejects([&] { RecordingReader bad(tampered, tamperedOptions); }, "Tampered geographic site hash was accepted");
      altered["geographicSitesSha256"].SetString(acquisition::digest(acquisition::encode(altered["geographicSites"])).c_str(), altered.GetAllocator());
      const auto rehashed = save(acquisition::encode(altered), channels);
      rejects([&] { RecordingReader bad(rehashed, options); }, "Rehashed site tamper bypassed trusted IQ identity");
      altered["geographicSites"]["rx"].RemoveMember("altitude");
      const auto missing = save(acquisition::encode(altered), channels);
      rejects([&] { RecordingReader bad(missing, untrusted); }, "Incomplete recorded site was accepted");
    }
    require(ordinary.provenance().sourceFirstSample == first, "Acquisition sample position lost integer precision");
    require(ordinary.provenance().epoch == 9007199254740995ULL, "Acquisition epoch lost integer precision");
    require(ordinary.provenance().frames.front().sourceCount == 16, "Acquisition lost original source frame size");
    require(ordinary.provenance().acquisitionContext.manifoldSha256 == metadata.context.manifoldSha256,
      "Acquisition lost manifold identities");
    require(ordinary.provenance().acquisitionContext.referenceMode == metadata.context.referenceMode &&
      ordinary.provenance().acquisitionContext.referenceChannels == metadata.context.referenceChannels &&
      ordinary.provenance().acquisitionContext.surveillanceChannels == metadata.context.surveillanceChannels &&
      ordinary.provenance().acquisitionContext.coherent == metadata.context.coherent,
      "Receiver-independent acquisition channel topology did not round trip");
    if (ordinary.provenance().exactAcquisition != readback)
      throw std::runtime_error("Acquisition gain/geometry strength mismatch for " + receiver);
    if (!readback) require(ordinary.provenance().acquisitionReason.find("sample-aligned gain") != std::string::npos,
      "Unknown applied gain capability did not expose an exact-admission reason");
    require(ordinary.provenance().perChannelGainDb == (readback ? metadata.perChannelGainDb : std::vector<double>{}),
      "Applied gain path consumed requested-only settings");
    options.requireExactAcquisition = true;
    if (readback) {
      options.validateAcquisitionContext = true; options.expectedAcquisitionContext = metadata.context;
      RecordingReader strict(filename, options);
      auto wrongRequested = options;
      wrongRequested.expectedAcquisitionContext.requestedChannelGainDb[0] += .01;
      rejects([&] { RecordingReader bad(filename, wrongRequested); },
        "Exact replay accepted a conflicting requested gain declaration");
      require(strict.provenance().acquisitionHashTrusted, "Missing trusted hash status");
      auto changedLiveSites = options;
      changedLiveSites.expectedAcquisitionContext.geographicSitesJson = "null";
      require(RecordingReader(filename, changedLiveSites).provenance().geographicSitesAvailable,
        "Current live sites overrode authenticated recorded geography");
      auto wrongRoute = options; wrongRoute.expectedAcquisitionContext.surveillanceChannels.clear();
      rejects([&] { RecordingReader invalid(filename, wrongRoute); }, "Exact replay accepted different configured channel roles");
      wrongRoute = options; wrongRoute.expectedAcquisitionContext.manifoldSha256.clear();
      rejects([&] { RecordingReader invalid(filename, wrongRoute); }, "Exact replay accepted different configured manifolds");
      wrongRoute = options; wrongRoute.expectedAcquisitionContext.geometryJson = "{}";
      rejects([&] { RecordingReader invalid(filename, wrongRoute); }, "Exact replay accepted different configured geometry");
    }
    else rejects([&] { RecordingReader invalid(filename, options); }, "Requested-only backend passed exact admission");
    // Rehashing malformed metadata cannot bypass semantic admission, even when
    // a fixture deliberately provides its new hash as the external trust root.
    for (const char* field : {"perChannelGainDb", "topology", "geometry", "manifoldSha256", "frames", "codeSha256"}) {
      rapidjson::Document altered; altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str()); altered.RemoveMember(field);
      const auto missing = save(acquisition::encode(altered), channels);
      rejects([&] { RecordingReader invalid(missing, ReplayOptions{"blah2",receiver,channels,1000,100000000,0}); },
        "Missing acquisition field was accepted");
    }
    {
      rapidjson::Document altered; altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
      altered["topology"]["referenceChannels"][0].SetUint(channels);
      const auto invalid = save(acquisition::encode(altered), channels);
      rejects([&] { RecordingReader bad(invalid, ReplayOptions{"blah2",receiver,channels,1000,100000000,0}); },
        "Out-of-range acquisition channel role was accepted");
    }
    {
      rapidjson::Document altered; altered.Parse<rapidjson::kParseFullPrecisionFlag>(json.c_str());
      altered["frames"][0]["count"].SetString("11");
      const auto invalid = save(acquisition::encode(altered), channels);
      rejects([&] { RecordingReader bad(invalid, ReplayOptions{"blah2",receiver,channels,1000,100000000,0}); },
        "Acquisition frame count mismatch was accepted");
    }
    // Embedded metadata corruption is rejected without any sidecar or trusted
    // hash. IQ corruption is rejected against the separately pinned full hash.
    {
      std::fstream tamper(filename, std::ios::binary | std::ios::in | std::ios::out);
      tamper.seekp(128 + json.find("fixture-device")); tamper.put('X'); tamper.close();
      rejects([&] { RecordingReader invalid(filename, options); }, "Tampered embedded metadata was accepted");
    }
    const auto changedIq = save(json, channels); options.expectedRecordingSha256 = hash(changedIq);
    { std::fstream tamper(changedIq, std::ios::binary | std::ios::in | std::ios::out);
      tamper.seekp(-1, std::ios::end); tamper.put('X'); }
    rejects([&] { RecordingReader invalid(changedIq, options); }, "Tampered IQ passed external provenance hash");
  }
  const auto legacy = directory / "legacy-acquisition.blah2iq";
  { RecordingWriter writer(legacy, {2,1000,100000000,0,"blah2"});
    writer.append(IqBlock(2, std::vector<std::complex<float>>(12))); writer.close(); }
  ReplayOptions options{"blah2","Usrp",2,1000,100000000,0};
  { RecordingReader ordinary(legacy, options); require(!ordinary.provenance().exactAcquisition, "Legacy file gained acquisition claims"); }
  options.requireExactAcquisition = true; options.expectedRecordingSha256 = hash(legacy);
  rejects([&] { RecordingReader invalid(legacy, options); }, "Legacy playback passed exact-acquisition admission");
}

void sha256_contract_tests() {
  // Independent hashlib SHA-256 vectors cover both padding blocks, binary
  // input, recording-sized writes, empty updates and copy/stream equivalence.
  const std::pair<std::size_t, const char*> vectors[] = {
{0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {1, "e7cf46a078fed4fafd0b5e3aff144802b853f8ae459a4f0c14add3314b7cc3a6"},
    {55, "2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384"},
    {56, "31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674"},
    {63, "5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1"},
    {64, "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e"},
    {65, "fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954"},
    {119, "b0dc41b1a384e2f1203f0351b38fbeaafceef577ce1191d5bfc25da39f721eae"},
    {120, "5df24dd802ac26132ce608dcb5f09841eef039ee0f152acf98d26d17fe4e88e6"},
    {127, "0fe729ff19257bd6fec853acc2ea355f6b34b58e6c0f684c3e188fcdfcd9baae"},
    {128, "0aedd4856f8eba0963627336ad5144a9a7dbe12498e6066f0165fc97d8ddee4c"},
    {4097, "042a02b55ba342cdd30322331816b6e4bca073f33bdb80149dc74a6f949802f4"},
    {65536, "6fc179cfd193754e6109ad043f56d146c7e7d7c3623ffceae318266286f58388"},
  };
  for (const auto& vector : vectors) {
    std::vector<uint8_t> input(vector.first);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = uint8_t(i * 37 + 11);
    Sha256 whole;
    whole.update(nullptr, 0);
    whole.update(input.data(), input.size());
    auto binary = whole;
    require(whole.finish() == vector.second, "SHA-256 known-answer mismatch");
    std::string encoded;
    for (uint8_t byte : binary.finishBytes()) {
      encoded += "0123456789abcdef"[byte >> 4];
      encoded += "0123456789abcdef"[byte & 15];
    }
    require(encoded == vector.second, "GPU and recording identity encodings differ");
    for (std::size_t step : {std::size_t(1), std::size_t(7), std::size_t(64), std::size_t(4096)}) {
      Sha256 streamed;
      for (std::size_t at = 0; at < input.size(); at += step) {
        streamed.update(input.data() + at, std::min(step, input.size() - at));
        streamed.update(nullptr, 0);
      }
      require(streamed.finish() == vector.second, "SHA-256 stream partition changed identity");
    }
  }
  if (std::numeric_limits<std::size_t>::max() > UINT64_MAX / 8) {
    Sha256 bounded;
    bool overflow = false;
    try { bounded.update(nullptr, std::numeric_limits<std::size_t>::max()); }
    catch (const std::overflow_error&) { overflow = true; }
    require(overflow, "SHA-256 oversized input was not rejected before access");
    bounded.update("abc", 3);
    require(bounded.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
      "SHA-256 rejection mutated the prior identity state");
  }
}

int main() try {
  sha256_contract_tests();
  char temporary[]="/tmp/blah2-recording-tests.XXXXXX";
  require(mkdtemp(temporary),"Cannot create test directory");
  const auto directory=std::filesystem::path(temporary), path=directory/"fixture.iq";
  auto cleanup=[&] { std::filesystem::remove_all(directory); };
  try {
    acquisition_run_tests();
    recording_cache_tests(directory);
    recording_batch_tests(directory);
    acquisition_contract_tests(directory);
    const uint32_t maxSample=std::numeric_limits<uint32_t>::max();
    require(rspduo_sequence::continues(maxSample,maxSample) &&
      rspduo_sequence::next(maxSample,1)==0 &&
      rspduo_sequence::next(maxSample-2,3)==0,
      "RSPduo sample counter wrap");
    for(unsigned channels=2;channels<=8;++channels) {
      IqBlock original(channels,std::vector<std::complex<float>>(11));
      for(unsigned ch=0;ch<channels;++ch) for(unsigned i=0;i<11;++i) original[ch][i]={float(ch*11+i)-40,float(i)-9};
      {
        RecordingWriter writer(path,{channels,2400000,527000000,0,"blah2"});
        writer.append(original); writer.append(original);
        require(writer.samples()==22,"Writer sample count"); writer.close(); writer.close();
        rejects([&] { writer.append(original); },"Accepted append to closed recording");
      }
      rejects([&] { RecordingWriter writer(path,{channels,2400000,527000000,0,"blah2"}); },"Overwrote an existing recording");
      ReplayOptions options{"auto","Kraken",channels,2400000,527000000,0};
      RecordingReader reader(path,options); IqBlock block;
      require(reader.metadata().samples==22,"Reader sample count");
      require(reader.provenance().legacy && !reader.provenance().exactTime,
        "Missing sidecar was not exposed as legacy/unqualified");
      auto decisionGrade=options; decisionGrade.provenanceMode="decision-grade";
      rejects([&] { RecordingReader invalid(path,decisionGrade); },
        "Decision-grade replay accepted a recording without provenance");
      for(unsigned repeat=0;repeat<3;++repeat) {
        require(reader.read(block) && block==original,"First recording block differs");
        require(reader.read(block) && block==original,"Second recording block differs");
        require(!reader.read(block),"Reader did not reach EOF"); reader.rewind();
      }
      reader.seek(5);
      require(reader.read(block) && block.front().size()==6 &&
        block.front().front()==original.front()[5],"Indexed replay seek changed the selected sample");
      require(reader.read(block) && block==original,"Indexed replay seek lost the following block");
      reader.seek(22); require(!reader.read(block),"Replay seek to EOF returned samples");
      options.channels=channels==2 ? 3 : 2;
      rejects([&] { RecordingReader invalid(path,options); },"Accepted wrong channel count");
      options.channels=channels; options.frequency++;
      rejects([&] { RecordingReader invalid(path,options); },"Accepted wrong frequency");
      options.frequency--; options.sampleRate++;
      rejects([&] { RecordingReader invalid(path,options); },"Accepted wrong sample rate");
      options.sampleRate--;
      std::filesystem::resize_file(path,std::filesystem::file_size(path)-1);
      rejects([&] { RecordingReader invalid(path,options); },"Accepted truncated recording");
      std::filesystem::remove(path);
    }
    {
      const auto clipPath=directory/"portable.blah2iq";
      const IqBlock original{{{1,2},{3,4},{5,6}},{{7,8},{9,10},{11,12}}};
      RecordingWriter writer(clipPath,{2,2400000,527000000,0,"blah2"});
      writer.append(original);writer.close();
      const ReplayOptions options{"auto","Usrp",2,2400000,527000000,0};
      RecordingReader legacy(clipPath,options);
      require(!legacy.provenance().sourceSampleAvailable&&legacy.provenance().legacy&&
        !legacy.provenance().contractValid&&!legacy.provenance().decisionGrade&&
        !legacy.provenance().exactTime&&!legacy.provenance().uncertaintyBounded,
        "Missing portable sidecar invented qualified source provenance");
      std::ifstream input(clipPath,std::ios::binary);
      const std::vector<uint8_t> data(std::istreambuf_iterator<char>(input),{});
      Sha256 digest;digest.update(data.data(),data.size());const auto hash=digest.finish();
      const auto sidecar=clipPath.string()+".json";
      const auto metadata=[&](const std::string& sha,uint64_t end,uint64_t bytes,
          unsigned channels=2,const std::string& gains="") {
        std::ofstream output(sidecar);
        output<<"{\"schema\":\"vectorwarp-portable-iq-clip-v1\","
          "\"format\":\"blah2iq\",\"encoding\":\"float32-complex-channel-major-blocks\","
          "\"channels\":"<<channels<<",\"sampleRate\":2400000,\"frequency\":527000000,"
          "\"samplesPerChannel\":3,\"epoch\":7,\"firstEpochSample\":1000,"
          "\"endEpochSample\":"<<end<<",\"dataBytes\":"<<bytes<<","
          "\"dataSha256\":\""<<sha<<"\""<<gains<<"}";
      };
      metadata(hash,1003,data.size());
      RecordingReader qualified(clipPath,options);
      const auto& provenance=qualified.provenance();
      require(provenance.sourceSampleAvailable&&provenance.epoch==7&&
        provenance.sourceFirstSample==1000&&provenance.legacy&&!provenance.present&&
        !provenance.contractValid&&!provenance.decisionGrade&&!provenance.exactTime&&
        !provenance.uncertaintyBounded&&!provenance.deviceTimeAvailable,
        "Portable relative identity changed exact-time or decision-grade qualification");
      IqBlock actual;
      require(qualified.read(actual)&&actual==original&&!qualified.read(actual),
        "Portable sidecar verification changed replay position or samples");
      require(provenance.perChannelGainDb.empty(),
        "Portable clip without applied gains invented calibration");
      metadata(hash,1003,data.size(),2,",\"perChannelGainDb\":[12.25,-3.5]");
      RecordingReader calibrated(clipPath,options);
      require(calibrated.provenance().perChannelGainDb==std::vector<double>({12.25,-3.5}),
        "Portable clip did not preserve exact applied gains");
      metadata(hash,1003,data.size(),2,",\"perChannelGainDb\":[14.199999809265137,0.10000000000000002]");
      RecordingReader preciseGains(clipPath,options);
      require(preciseGains.provenance().perChannelGainDb==
        std::vector<double>({double(float(14.2)),0.10000000000000002}),
        "Portable clip rounded applied gains used for exact manifold identity");
      for(const auto& invalidGains:{"null","[]","[1]","[1,2,3]","[1,\"2\"]","[1,1e999]"}) {
        metadata(hash,1003,data.size(),2,std::string(",\"perChannelGainDb\":")+invalidGains);
        rejects([&] { RecordingReader invalid(clipPath,options); },
          "Portable clip accepted malformed applied gains");
      }
      metadata(hash,1003,data.size());
      auto decisionGrade=options;decisionGrade.provenanceMode="decision-grade";
      rejects([&] { RecordingReader invalid(clipPath,decisionGrade); },
        "Portable relative identity was accepted as decision-grade provenance");
      auto wrongHash=hash;wrongHash[0]=wrongHash[0]=='0'?'1':'0';
      metadata(wrongHash,1003,data.size());
      rejects([&] { RecordingReader invalid(clipPath,options); },
        "Portable sidecar accepted a mismatched SHA-256");
      metadata(hash,1004,data.size());
      rejects([&] { RecordingReader invalid(clipPath,options); },
        "Portable sidecar accepted an inconsistent source sample interval");
      metadata(hash,999,data.size());
      rejects([&] { RecordingReader invalid(clipPath,options); },
        "Portable sidecar accepted a reversed source sample interval");
      metadata(hash,1003,data.size()+1);
      rejects([&] { RecordingReader invalid(clipPath,options); },
        "Portable sidecar accepted an inconsistent file size");
      metadata(hash,1003,data.size(),3);
      rejects([&] { RecordingReader invalid(clipPath,options); },
        "Portable sidecar accepted mismatched recording metadata");
      metadata(hash,1003,data.size());
      auto changed=data;changed.back()^=1;write(clipPath,changed);
      rejects([&] { RecordingReader invalid(clipPath,options); },
        "Portable sidecar accepted modified IQ bytes with the original SHA-256");
    }
    {
      const auto slicePath=directory/"slice.iq";
      IqBlock original{{{0,10},{1,11},{2,12},{3,13},{4,14}},
                       {{5,15},{6,16},{7,17},{8,18},{9,19}}};
      RecordingWriter writer(slicePath,{2,2000000,100000000,0,"blah2"});
      rejects([&] { writer.append_slice(original,0,0); },
        "Accepted an empty recording slice");
      rejects([&] { writer.append_slice(original,5,1); },
        "Accepted an out-of-range recording slice");
      rejects([&] { writer.append_slice(original,
        std::numeric_limits<std::size_t>::max(),1); },
        "Accepted an overflowing recording slice offset");
      auto invalid=original;
      invalid[0][2]={std::numeric_limits<float>::infinity(),0};
      rejects([&] { writer.append_slice(invalid,1,3); },
        "Accepted non-finite IQ inside a recording slice");
      writer.append_slice(original,1,3);
      rejects([&] { writer.append({{{1,2},{3,4}},{{5,6},{7,8},{9,10}}}); },
        "Accepted unequal recording channels");
      writer.close();
      RecordingReader reader(slicePath,{"auto","Mock",2,2000000,100000000,0});
      IqBlock actual;
      require(reader.read(actual) &&
        actual==IqBlock{{{1,11},{2,12},{3,13}},{{6,16},{7,17},{8,18}}},
        "Zero-copy recording slice changed sample order or values");
      require(!reader.read(actual),"Recording slice added unexpected samples");
    }
    {
      RecordingWriter writer(path,{2,2000000,100000000,0,"blah2"});
      writer.append({{{1,2}},{{3,4}}});
      rejects([&] { writer.append({{{std::numeric_limits<float>::quiet_NaN(),0}},{{0,0}}}); },"Accepted NaN IQ");
    }
    rejects([&] { RecordingReader invalid(path,{"auto","RspDuo",2,2000000,100000000,0}); },"Accepted unfinished recording");
    std::filesystem::remove(path);

    rejects([&] { RecordingWriter invalid(path,{1,2000000,100000000,0,"blah2"}); },"Accepted invalid writer metadata");
    require(!std::filesystem::exists(path),"Invalid writer left a recording artifact");

    bool save=false;
    MockSource empty(directory.string(),&save);
    const auto emptyPath=empty.open_file();
    rejects([&] { empty.close_file(); },"Accepted a clean close with no samples");
    require(empty.recording_status().error=="No samples were recorded",
      "Empty recording did not retain its error");
    rejects([&] { RecordingReader invalid(emptyPath,{"auto","Mock",2,2000000,100000000,0}); },
      "Empty recording was marked complete");

    MockSource source(directory.string(),&save);
    const auto sourcePath=source.open_file();
    source.record_channel(0,0,{{1,2},{3,4},{5,6}});
    source.record_channel(1,0,{{7,8},{9,10},{11,12}});
    source.close_file();
    const auto sourceStatus=source.recording_status();
    require(!sourceStatus.active && sourceStatus.error.empty() && sourceStatus.samples==3,
      "Source lost completed recording status");
    RecordingReader sourceReader(sourcePath,{"auto","Mock",2,2000000,100000000,0});
    IqBlock sourceBlock;
    require(sourceReader.read(sourceBlock) && sourceBlock[0].size()==3 && sourceBlock[1][2]==std::complex<float>(11,12),
      "Source paired recording differs");
    require(sourceReader.provenance().contractValid && !sourceReader.provenance().decisionGrade &&
      sourceReader.provenance().schema=="vectorwarp-recording-provenance-v2",
      "Source recording provenance was not consumed as explicitly inexact v2");
    std::filesystem::remove(sourcePath);
    std::filesystem::remove(sourcePath+".provenance.json");

    // Deterministic backend-shaped live -> portable record -> replay contracts.
    // The backend details deliberately cover distinct identity/gain/topology
    // shapes while the common reader consumes the same fail-closed envelope.
    const std::vector<std::pair<std::string,std::string>> backendDetails{
      {"Kraken",R"({"device":{"identityAvailable":false,"reason":"Heimdall protocol"},"channels":[{"gainDb":19.7},{"gainDb":20.7}],"rf":{"frequencyHz":527000000},"topology":{"transport":"heimdall-v2"}})"},
      {"RspDuo",R"({"device":{"serial":"RSP-FIXTURE"},"channels":[{"tuner":"A","gainReductionDb":50,"lnaState":1},{"tuner":"B","gainReductionDb":45,"lnaState":1}],"rf":{"frequencyHz":527000000,"bandwidth":5},"topology":{"mode":"Dual_Tuner"}})"},
      {"Usrp",R"({"device":{"address":"type=b200"},"channels":[{"antenna":"RX2","gainDb":10},{"antenna":"RX2","gainDb":20}],"rf":{"frequencyHz":527000000},"topology":{"subdevices":"A:A A:B","clockSource":"external","timeSource":"internal"}})"},
      {"HackRF",R"({"device":{"serials":["a1","b2"]},"channels":[{"lnaGainDb":16,"vgaGainDb":20,"amp":false},{"lnaGainDb":32,"vgaGainDb":40,"amp":true}],"rf":{"frequencyHz":527000000},"topology":{"clock":"shared","trigger":"hardware"}})"}
    };
    for(const auto& fixture:backendDetails) {
      BackendFixtureSource live(fixture.first,directory.string(),&save,fixture.second);
      RecordingContext context; context.referenceMode = "dedicated";
      context.referenceChannels = {0}; context.surveillanceChannels = {1};
      context.configurationSha256 = std::string(64, 'a'); context.codeSha256 = std::string(64, 'b');
      live.set_recording_context(context);
      const auto recorded=live.open_file();
      const uint64_t callbacks = acquisition::maximumFrames + 1;
      for (uint64_t index = 0; index < callbacks; ++index) {
        auto position = live.sample_time_position(); position.nextSample += 2 * index;
        live.record_block({{{1,2},{3,4}},{{5,6},{7,8}}}, position);
      }
      live.close_file();
      ReplayOptions qualified{"auto",fixture.first,2,2400000,527000000,0};
      RecordingReader replay(recorded,qualified);
      require(replay.provenance().frames.size() == 1 && replay.provenance().frames[0].repeat == callbacks,
        "Manual source lost bounded exact lineage after 4096 callbacks");
      require(std::filesystem::exists(recorded + ".acquisition.json"), "Manual source omitted acquisition manifest");
      auto exact = qualified; exact.requireExactAcquisition = true;
      exact.expectedRecordingSha256 = replay.provenance().recordingSha256;
      exact.expectedAcquisitionSha256 = replay.provenance().acquisitionSha256;
      if (fixture.first == "Usrp") {
        RecordingReader bound(recorded, exact);
        require(bound.provenance().perChannelGainDb == std::vector<double>({10.25,20.5}), "Manual applied gains did not round trip");
        auto missingAnchor = exact; missingAnchor.expectedAcquisitionSha256.clear();
        rejects([&] { RecordingReader invalid(recorded, missingAnchor); }, "Manual manifest trusted its self-declared metadata digest");
        auto wrongAnchor = exact; wrongAnchor.expectedAcquisitionSha256 = std::string(64, 'f');
        rejects([&] { RecordingReader invalid(recorded, wrongAnchor); }, "Manual manifest accepted mismatched metadata identity");
      } else rejects([&] { RecordingReader invalid(recorded, exact); }, "Unknown applied gains passed manual exact admission");
      IqBlock replayed;
      require(replay.read(replayed) && replayed[0][1]==std::complex<float>(3,4),
        "Backend v2 round trip changed samples");
      require(replay.provenance().contractValid && replay.provenance().receiver==fixture.first &&
        !replay.provenance().exactTime && !replay.provenance().uncertaintyBounded &&
        replay.provenance().configurationFingerprintValid &&
        replay.provenance().codeFingerprintMatches && replay.provenance().sourceSampleAvailable &&
        replay.provenance().sourceFirstSample==1000,
        "Backend v2 round trip lost receiver/sample-time provenance");
      qualified.provenanceMode="decision-grade";
      rejects([&] { RecordingReader invalid(recorded,qualified); },
        "Decision-grade replay accepted inexact backend time");
      if(fixture.first=="Kraken") {
        qualified.provenanceMode="legacy-unqualified";
        qualified.receiver="Usrp";
        RecordingReader crossReceiver(recorded,qualified);
        require(crossReceiver.provenance().receiver=="Kraken" &&
          crossReceiver.provenance().contractValid,
          "Cross-receiver replay lost the recording receiver provenance");
        qualified.receiver="Kraken";
        const auto sidecar=recorded+".provenance.json";
        std::ifstream input(sidecar);
        std::string tampered(std::istreambuf_iterator<char>(input),{});
        const auto gain=tampered.find("\"gainDb\":19.7");
        require(gain!=std::string::npos,"Fixture sidecar omitted Kraken gain");
        tampered.replace(gain,std::strlen("\"gainDb\":19.7"),"\"gainDb\":19.8");
        std::ofstream output(sidecar,std::ios::trunc); output << tampered; output.close();
        rejects([&] { RecordingReader invalid(recorded,qualified); },
          "Replay accepted tampered receiver settings provenance");
      }
      std::filesystem::remove(recorded);
      std::filesystem::remove(recorded+".provenance.json");
    }

    for (bool epochChange : {false, true}) {
      BackendFixtureSource live("Usrp",directory.string(),&save,"{}");
      RecordingContext context; context.configurationSha256 = std::string(64, 'a');
      context.codeSha256 = std::string(64, 'b'); live.set_recording_context(context);
      const auto recorded = live.open_file();
      auto position = live.sample_time_position();
      live.record_block({{{1,2}},{{3,4}}},position);
      ++position.nextSample;
      if (epochChange) ++position.epoch;
      else { context.configurationSha256 = std::string(64, 'c'); live.set_recording_context(context); }
      live.record_block({{{5,6}},{{7,8}}},position);
      live.close_file();
      RecordingReader unqualified(recorded,{"blah2","Usrp",2,2400000,527000000,0});
      require(!unqualified.provenance().acquisitionIntegrityValid && unqualified.provenance().frames.empty(),
        "Frame-run compression crossed an epoch or settings boundary");
    }

    MockSource unpaired(directory.string(),&save);
    unpaired.open_file();
    unpaired.record_channel(0,0,{{1,1},{2,2}});
    unpaired.record_channel(1,0,{{3,3}});
    rejects([&] { unpaired.close_file(); },"Accepted unpaired Source close");
    require(!unpaired.recording_status().error.empty(),"Unpaired close did not retain an error");

    MockSource gap(directory.string(),&save);
    gap.open_file();
    gap.record_channel(0,0,{{1,1}});
    gap.record_channel(0,2,{{2,2}});
    require(!gap.recording_status().error.empty(),"Source accepted a callback sample gap");

    MockSource discontinuity(directory.string(),&save);
    require(discontinuity.processing_epoch()==0,"New source did not start in processing epoch zero");
    const auto discontinuityPath=discontinuity.open_file();
    discontinuity.record_block({{{1,2}},{{3,4}}});
    discontinuity.recording_discontinuity("mock capture gap");
    require(discontinuity.processing_epoch()==1,
      "Capture gap did not advance the adaptive-processing epoch");
    const auto discontinuityStatus=discontinuity.recording_status();
    require(!discontinuityStatus.active && discontinuityStatus.samples==1 &&
      discontinuityStatus.error=="mock capture gap","Discontinuity status was not retained");
    rejects([&] { RecordingReader invalid(discontinuityPath,{"auto","Mock",2,2000000,100000000,0}); },
      "Discontinuous recording was marked complete");

    MockSource segmented(directory.string(),&save);
    const auto ambiguousEpoch=segmented.open_file();
    segmented.record_block({{{1,2}},{{3,4}}});
    segmented.recording_epoch_discontinuity("mock setting epoch changed");
    require(segmented.processing_epoch()==1,
      "RF setting change did not advance the adaptive-processing epoch");
    const auto transition=segmented.recording_status();
    require(!transition.active && transition.resumePending && transition.samples==1,
      "Qualified epoch rollover did not retain a bounded resume request");
    segmented.service_recording();
    segmented.record_block({{{5,6}},{{7,8}}});
    const auto resumed=segmented.recording_status();
    require(resumed.active && !resumed.resumePending && resumed.samples==1 &&
      resumed.file!=ambiguousEpoch,
      "First qualified post-boundary block did not lazily resume recording");
    segmented.close_file();
    rejects([&] { RecordingReader invalid(ambiguousEpoch,
      {"auto","Mock",2,2000000,100000000,0}); },
      "Ambiguous pre-boundary epoch was marked complete");
    RecordingReader resumedReader(resumed.file,
      {"auto","Mock",2,2000000,100000000,0});
    IqBlock resumedBlock;
    require(resumedReader.read(resumedBlock) &&
      resumedBlock[0][0]==std::complex<float>(5,6),
      "Post-boundary recording retained pre-boundary samples");
    std::filesystem::remove(ambiguousEpoch);
    std::filesystem::remove(ambiguousEpoch+".provenance.json");
    std::filesystem::remove(resumed.file);
    std::filesystem::remove(resumed.file+".provenance.json");

    MockSource unrecordedEpoch(directory.string(),&save);
    unrecordedEpoch.recording_epoch_discontinuity("unrecorded setting epoch changed");
    require(unrecordedEpoch.processing_epoch()==1,
      "An unrecorded live discontinuity did not reset adaptive processing");

    MockSource coordinatedEpoch(directory.string(),&save);
    coordinatedEpoch.recording_epoch_discontinuity(
      "buffer clearing owns this boundary", false);
    require(coordinatedEpoch.processing_epoch()==0,
      "Deferred discontinuity published before coordinated buffer clearing");
    coordinatedEpoch.advance_processing_epoch();
    require(coordinatedEpoch.processing_epoch()==1,
      "Coordinated buffer boundary did not publish its processing epoch");

    {
      RecordingWriter writer(path,{2,1000000,100000000,0,"blah2"});
      IqBlock replayBlock(2,std::vector<std::complex<float>>(6));
      for (unsigned ch=0;ch<2;++ch) for (unsigned i=0;i<6;++i) replayBlock[ch][i]={float(ch),float(i)};
      writer.append(replayBlock); writer.close();
      IqData first(4), second(4); std::vector<IqData*> queues{&first,&second};
      std::atomic<bool> stopped{false}, consume{true};
      std::atomic<uint64_t> committedSamples{0};
      std::atomic<bool> consumedAheadOfCommit{false};
      std::vector<std::complex<double>> observed;
      std::thread consumer([&] {
        while (consume.load()) {
          first.lock(); second.lock();
          if (first.get_length()>=4 && second.get_length()>=4) {
            if (committedSamples.load(std::memory_order_acquire) < 4)
              consumedAheadOfCommit = true;
            auto frame=first.drain_front(4); second.drain_front(4);
            observed.insert(observed.end(),frame.begin(),frame.end());
          }
          second.unlock(); first.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
      ReplayPlayer player; ReplayProgress last; unsigned updates=0;
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,false,stopped,
        [&](const ReplayProgress& progress) { last=progress; ++updates; }, [] { return false; },
        [] { return ReplayCommand{}; }, false, [&](uint64_t samples) {
          committedSamples.store(samples, std::memory_order_release);
        });
      consume=false; consumer.join();
      require(last.state=="complete" && !last.controlsAvailable && last.samples==6 && last.trailingSamples==2 && updates>2,
        "Replay did not reach bounded EOF completion");
      require(observed.size()==4 && observed[0]==std::complex<double>(0,0) &&
        observed[3]==std::complex<double>(0,3),"Replay changed frame ordering under backpressure");
      require(!consumedAheadOfCommit.load(),
        "Replay exposed a full CPI before publishing its source position");

      stopped=false; consume=true;
      std::thread loopConsumer([&] {
        while (consume.load()) {
          first.lock(); second.lock();
          if (first.get_length()>=4 && second.get_length()>=4) { first.drain_front(4); second.drain_front(4); }
          second.unlock(); first.unlock(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,true,stopped,
        [&](const ReplayProgress& progress) { if (progress.loops==1) stopped=true; }, [] { return false; });
      consume=false; loopConsumer.join();
      require(stopped.load(),"Replay loop was not cancellable");

      // Cancellation must also win while every playback queue is full.
      stopped=false;
      std::thread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(5)); stopped=true; });
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,false,stopped,
        [](const ReplayProgress&) {}, [] { return false; });
      cancel.join();
      require(stopped.load(),"Replay did not cancel while backpressured");

      // A seek is an indexed processor-generation boundary, and pause must not
      // consume samples while the user is scrubbing.
      first.clear(); second.clear(); stopped=false; consume=true;
      ReplayControlMailbox commands; commands.publish({true,0,0});
      bool sawPaused=false; ReplayProgress controlled;
      std::thread seekConsumer([&] {
        while(consume.load()) {
          first.lock(); second.lock();
          if(first.get_length()>=4&&second.get_length()>=4){first.drain_front(4);second.drain_front(4);}
          second.unlock();first.unlock();std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      });
      std::thread controller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        commands.publish({false,2,1});
      });
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,false,stopped,
        [&](const ReplayProgress& progress){controlled=progress;sawPaused=sawPaused||progress.state=="paused";},
        []{return false;},[&]{return commands.snapshot();});
      controller.join();consume=false;seekConsumer.join();
      require(sawPaused&&controlled.state=="complete"&&controlled.samples==6&&controlled.generation==1,
        "Replay pause/seek did not publish a clean processor generation");

      // No timing race: a synchronous consumer in the progress callback drains
      // complete frames, and commands are published at exact lifecycle edges.
      const auto drain = [&] {
        first.lock(); second.lock();
        if(first.get_length()>=4&&second.get_length()>=4) {
          first.drain_front(4); second.drain_front(4);
        }
        second.unlock(); first.unlock();
      };
      first.clear(); second.clear(); stopped=false;
      commands.publish({false,2,5});
      uint64_t secondLoopGeneration=0;
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,true,stopped,
        [&](const ReplayProgress& progress) {
          drain();
          if(progress.loops==1&&progress.samples>0) {
            secondLoopGeneration=progress.generation; stopped=true;
          }
        }, []{return false;},[&]{return commands.snapshot();});
      require(secondLoopGeneration==2,"Consumed seek revision reapplied after automatic rewind");

      first.clear(); second.clear(); stopped=false; commands.publish({false,0,0});
      unsigned completions=0; uint64_t resumedGeneration=0;
      player.run(path,{"auto","Mock",2,1000000,100000000,0},queues,4,false,stopped,
        [&](const ReplayProgress& progress) {
          drain();
          if(progress.state=="complete") {
            require(progress.controlsAvailable,"Idle EOF player hid its controls");
            resumedGeneration=progress.generation;
            if(++completions==1) commands.publish({false,2,1});
            else stopped=true;
          }
          if(progress.state=="stopped")
            require(!progress.controlsAvailable,"Stopped player advertised controls");
        }, []{return false;},[&]{return commands.snapshot();},true);
      require(completions==2&&resumedGeneration==1,
        "EOF player failed to consume a new seek/restart command");

      // Force old/new command observations around each publication. Revision
      // must never label fields from the previous publication.
      commands.publish({false,0,0});
      std::atomic<unsigned> phase{0};
      std::thread publisher([&] {
        while(phase.load()!=1) std::this_thread::yield();
        commands.publish({true,123456789,9}); phase=2;
        while(phase.load()!=3) std::this_thread::yield();
        commands.publish({false,987654321,10}); phase=4;
      });
      const auto before=commands.snapshot(); phase=1;
      while(phase.load()!=2) std::this_thread::yield();
      const auto during=commands.snapshot(); phase=3;
      while(phase.load()!=4) std::this_thread::yield();
      const auto after=commands.snapshot(); publisher.join();
      require(before.revision==0&&!before.paused&&before.seekSample==0&&
        during.revision==9&&during.paused&&during.seekSample==123456789&&
        after.revision==10&&!after.paused&&after.seekSample==987654321,
        "Replay control snapshot mixed publication fields");
      std::filesystem::remove(path);
    }

    // The common player is not a two-channel adapter: all supported coherent
    // channel counts retain the same first frame under a full-queue cancel.
    for (unsigned channels=2; channels<=8; ++channels) {
      const auto multiPath=directory/("replay-"+std::to_string(channels)+".iq");
      RecordingWriter writer(multiPath,{channels,1000000,100000000,0,"blah2"});
      IqBlock multi(channels,std::vector<std::complex<float>>(4));
      for (unsigned ch=0;ch<channels;++ch) for (unsigned i=0;i<4;++i) multi[ch][i]={float(ch),float(i)};
      writer.append(multi); writer.close();
      std::vector<std::unique_ptr<IqData>> storage;
      std::vector<IqData*> multiQueues;
      for (unsigned ch=0;ch<channels;++ch) { storage.push_back(std::make_unique<IqData>(4)); multiQueues.push_back(storage.back().get()); }
      std::atomic<bool> stopMulti{false}, fullFrameQueued{false};
      ReplayPlayer player;
      // The cancellation must occur while the grouped queues are actually
      // full. A fixed delay races CI scheduling and can stop playback before
      // it has written any channel, which tests scheduling rather than
      // multi-channel alignment.
      std::thread cancel([&] {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        while (!fullFrameQueued.load() && std::chrono::steady_clock::now()<deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        stopMulti=true;
      });
      player.run(multiPath,{"auto","Mock",channels,1000000,100000000,0},multiQueues,4,false,stopMulti,
        [&](const ReplayProgress& progress) { if (progress.samples==4) fullFrameQueued=true; }, [] { return false; });
      cancel.join();
      require(fullFrameQueued.load(),"Replay did not fill the coherent queues before cancellation");
      for (unsigned ch=0;ch<channels;++ch) {
        storage[ch]->lock(); const auto values=storage[ch]->get_data(); storage[ch]->unlock();
        require(values.size()==4 && values[0]==std::complex<double>(ch,0) && values[3]==std::complex<double>(ch,3),
          "Replay lost multi-channel alignment under backpressure");
      }
      std::filesystem::remove(multiPath);
    }

    std::vector<uint8_t> raw;
    for(int value : {-32768,32767,-1,0,1,-2,300,-400}) { const auto bits=static_cast<uint16_t>(value); raw.push_back(bits); raw.push_back(bits>>8); }
    write(path,raw);
    RecordingReader rsp(path,{"auto","RspDuo",2,2000000,100000000,0}); IqBlock block;
    require(rsp.read(block) && block[0][0]==std::complex<float>(-32768,32767) && block[1][1]==std::complex<float>(300,-400),"Legacy RSPduo values");
    require(!rsp.read(block),"Legacy RSPduo EOF");
    raw.pop_back(); write(path,raw);
    rejects([&] { RecordingReader invalid(path,{"auto","RspDuo",2,2000000,100000000,0}); },"Accepted partial legacy sample");

    write(path,{0x80,0x7f,0xff,0,1,2,3,4});
    RecordingReader hackrf(path,{"s8-interleaved","HackRF",2,2000000,100000000,0});
    require(hackrf.read(block) && block[0][0]==std::complex<float>(-128,127) && block[1][0]==std::complex<float>(-1,0),"Legacy signed int8 values");

    raw.clear();
    for(float v:{1.f,2.f,3.f,4.f,5.f,6.f,7.f,8.f}) {
      uint32_t bits; std::memcpy(&bits,&v,4); for(unsigned b=0;b<4;++b) raw.push_back(bits>>(b*8));
    }
    write(path,raw);
    rejects([&] { RecordingReader invalid(path,{"auto","Usrp",2,2400000,527000000,0}); },"Guessed old USRP block size");
    RecordingReader usrp(path,{"usrp-blocks","Usrp",2,2400000,527000000,2});
    require(usrp.read(block) && block[0][1]==std::complex<float>(3,4) && block[1][0]==std::complex<float>(5,6),"Legacy USRP block layout");

    raw.clear();
    for(uint32_t v:{0x4d434851U,2U,4U,4U,0U,0U,0U,0U}) for(int b=3;b>=0;--b) raw.push_back(v>>(b*8));
    for(float v:{527000000.f,49.6f,527000000.f,49.6f}) {
      uint32_t bits; std::memcpy(&bits,&v,4); for(unsigned b=0;b<4;++b) raw.push_back(bits>>(b*8));
    }
    for(unsigned ch=0;ch<2;++ch) for(unsigned i=0;i<4;++i) { raw.push_back(125+i); raw.push_back(130-i); }
    write(path,raw);
    RecordingReader kraken(path,{"auto","Kraken",2,2400000,527000000,0});
    require(kraken.metadata().samples==4 && kraken.read(block),"MCHQ recording read");
    require(std::abs(block[0][0]-std::complex<float>(-1.5/127.5,1.5/127.5))<1e-7,"MCHQ normalization");
    ReplayOptions strictKraken{"auto","Kraken",2,2400000,527000000,0};
    strictKraken.requireExactAcquisition = true;
    strictKraken.expectedRecordingSha256 = acquisition::digest(std::string(raw.begin(), raw.end()));
    rejects([&] { RecordingReader invalid(path, strictKraken); },
      "MCHQ without configured geometry/manifold identities passed exact acquisition qualification");
    raw.back()=128; raw[15]=3; write(path,raw); // Phase no longer calibrated.
    rejects([&] { RecordingReader invalid(path,{"auto","Kraken",2,2400000,527000000,0}); },"Accepted calibration frame");
    cleanup();
    std::cout << "Recording: 2–8 channels, exact samples, EOF/rewind, metadata, malformed files, legacy RSPduo/USRP/HackRF and MCHQ passed\n";
  } catch(...) { cleanup(); throw; }
} catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
