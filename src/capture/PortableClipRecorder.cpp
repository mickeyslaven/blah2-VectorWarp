#include "PortableClipRecorder.h"
#include "Sha256.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace blah2 {
namespace {
constexpr std::size_t MAX_IDS = 4096;
bool valid_id(const std::string& value) { return !value.empty() && value.size() <= 256; }
std::string system_error(const std::string& message) {
  return message + ": " + std::strerror(errno);
}
std::pair<uint64_t,std::string> digest_file(const std::string& path) {
  const int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
  if(fd<0) throw std::runtime_error(system_error("Cannot hash clip"));
  Sha256 digest; uint64_t size=0; std::array<uint8_t,65536> bytes{};
  for(;;) {
    const auto count=::read(fd,bytes.data(),bytes.size());
    if(count<0&&errno==EINTR) continue;
    if(count<0) {
      const auto error=system_error("Cannot hash clip");
      ::close(fd);
      throw std::runtime_error(error);
    }
    if(!count) break;
    digest.update(bytes.data(),static_cast<std::size_t>(count));
    size+=static_cast<uint64_t>(count);
  }
  ::close(fd); return {size,digest.finish()};
}
void write_synced(const std::string& path,const std::string& value) {
  const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0640);
  if(fd<0)throw std::runtime_error(system_error("Cannot create clip metadata"));
  const char* data=value.data(); std::size_t left=value.size();
  while(left){const auto count=::write(fd,data,left);if(count<0&&errno==EINTR)continue;
    if(count<=0){const auto error=system_error("Cannot write clip metadata");::close(fd);throw std::runtime_error(error);}
    data+=count;left-=static_cast<std::size_t>(count);}
  if(::fsync(fd)){const auto error=system_error("Cannot flush clip metadata");::close(fd);throw std::runtime_error(error);} ::close(fd);
}
}
struct PortableClipRecorder::Block {
  uint64_t epoch = 0, first = 0, count = 0;
  IqBlock samples;
};
struct PortableClipRecorder::Active {
  IqClipRequest request;
  uint64_t epoch = 0, first = 0, end = 0;
  std::vector<std::shared_ptr<Block>> blocks;
};
struct PortableClipRecorder::Job {
  IqClipRequest request;
  uint64_t epoch = 0, first = 0, end = 0, sequence = 0;
  std::vector<std::shared_ptr<Block>> blocks;
};

PortableClipRecorder::PortableClipRecorder(std::string directory,
    std::string receiver, uint32_t channels, uint32_t sampleRate,
    uint32_t frequency, Options options)
  : directory(std::move(directory)), receiver(std::move(receiver)),
    channels(channels), sampleRate(sampleRate),
    frequency(frequency), options(std::move(options)) {
  if (channels < 2 || channels > 8 || !sampleRate || !frequency ||
      !std::isfinite(this->options.ringSeconds) ||
      !std::isfinite(this->options.maximumClipSeconds) ||
      this->options.ringSeconds < 0 || this->options.maximumClipSeconds <= 0 ||
      this->options.ringSeconds > this->options.maximumClipSeconds ||
      this->options.maximumClipSeconds > 10.0 ||
      !this->options.maximumWriterJobs)
    throw std::invalid_argument("Invalid portable clip recorder bounds");
  const long double retained = std::ceil(
    static_cast<long double>(sampleRate) * this->options.ringSeconds);
  ringSamples = static_cast<uint64_t>(retained);
  writer = std::thread(&PortableClipRecorder::writer_loop, this);
}

PortableClipRecorder::~PortableClipRecorder() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
    if (active) fail_active_locked("Capture stopped before clip post-roll completed");
  }
  condition.notify_all();
  if (writer.joinable()) writer.join();
}

int64_t PortableClipRecorder::now_nanoseconds() const {
  if (options.clockNanoseconds) return options.clockNanoseconds();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t PortableClipRecorder::sample_for_time(int64_t nanoseconds) const {
  const long double value = static_cast<long double>(nanoseconds - epochNanoseconds) *
    sampleRate / 1000000000.0L;
  if (value <= 0) return 0;
  if (value >= 9000000000000000000.0L)
    return std::numeric_limits<int64_t>::max();
  return static_cast<int64_t>(std::llround(value));
}

void PortableClipRecorder::publish_locked(IqClipStatus value) {
  if (value.revision >= terminal.revision) terminal = std::move(value);
}

void PortableClipRecorder::fail_active_locked(const std::string& error) {
  if (!active) return;
  publish_locked({active->request.revision, true, {}, error, 0});
  active.reset();
}

bool PortableClipRecorder::ingest(IqBlock samples, uint64_t epoch,
    uint64_t firstSample, bool positionAvailable, int64_t receiptNanoseconds) {
  if (samples.size() != channels || samples.empty()) return false;
  const std::size_t count = samples.front().size();
  if (!count || std::any_of(samples.begin(), samples.end(),
      [&](const auto& channel) { return channel.size() != count; })) return false;
  std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;
  const int64_t receipt = receiptNanoseconds ? receiptNanoseconds : now_nanoseconds();
  uint64_t first = positionAvailable ? firstSample : nextSample;
  if (!haveEpoch || (positionAvailable && (epoch != streamEpoch || first != nextSample))) {
    if (haveEpoch) fail_active_locked("Receiver sample discontinuity during clip");
    ring.clear(); retainedSamples = 0;
    haveEpoch = true; streamEpoch = positionAvailable ? epoch : streamEpoch + 1;
    first = positionAvailable ? firstSample : 0;
    epochNanoseconds = receipt - static_cast<int64_t>(
      std::llround(static_cast<long double>(count) * 1000000000.0L / sampleRate)) -
      static_cast<int64_t>(std::llround(static_cast<long double>(first) *
        1000000000.0L / sampleRate));
  }
  if (first > UINT64_MAX - count) {
    fail_active_locked("Receiver sample position exhausted");
    ring.clear(); retainedSamples = 0; haveEpoch = false;
    return false;
  }
  auto block = std::make_shared<Block>();
  block->epoch = streamEpoch; block->first = first; block->count = count;
  block->samples = std::move(samples);
  nextSample = first + count;
  ring.push_back(block); retainedSamples += count;
  if (active && active->epoch == streamEpoch && nextSample > active->first)
    active->blocks.push_back(block);
  while (ring.size() > 1 && retainedSamples > ringSamples) {
    retainedSamples -= ring.front()->count;
    ring.pop_front();
  }
  if (active && nextSample >= active->end) finish_active_locked();
  return true;
}

void PortableClipRecorder::request(const IqClipRequest& request) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!request.revision || request.windowStartMs < 0 ||
      request.windowEndMs <= request.windowStartMs ||
      request.windowEndMs > std::numeric_limits<int64_t>::max() / 1000000LL ||
      request.requestedEventTimeMs < request.windowStartMs ||
      request.requestedEventTimeMs > request.windowEndMs ||
      request.eventIds.size() > MAX_IDS || request.passIds.size() > MAX_IDS ||
      std::any_of(request.eventIds.begin(), request.eventIds.end(),
        [](const auto& id) { return !valid_id(id); }) ||
      std::any_of(request.passIds.begin(), request.passIds.end(),
        [](const auto& id) { return !valid_id(id); })) {
    publish_locked({request.revision, true, {}, "Invalid IQ clip request", 0});
    return;
  }
  if (request.revision <= terminal.revision ||
      writerRevisions.count(request.revision) ||
      (active && active->request.revision == request.revision)) return;
  const long double duration = static_cast<long double>(request.windowEndMs -
    request.windowStartMs) / 1000.0L;
  if (duration > options.maximumClipSeconds) {
    publish_locked({request.revision, true, {}, "Requested IQ clip exceeds configured duration", 0});
    return;
  }
  if (active) fail_active_locked("IQ clip was superseded by a newer request");
  if (!haveEpoch) {
    publish_locked({request.revision, true, {}, "Kraken clip pre-roll is unavailable", 0});
    return;
  }
  const int64_t firstSigned = sample_for_time(request.windowStartMs * 1000000LL);
  const int64_t endSigned = sample_for_time(request.windowEndMs * 1000000LL);
  if (endSigned <= firstSigned || firstSigned < 0) {
    publish_locked({request.revision, true, {}, "IQ clip window does not map to live samples", 0});
    return;
  }
  const uint64_t first = static_cast<uint64_t>(firstSigned);
  const uint64_t end = static_cast<uint64_t>(endSigned);
  if (ring.empty() || first < ring.front()->first || end <= ring.front()->first) {
    publish_locked({request.revision, true, {}, "Kraken clip pre-roll is unavailable", 0});
    return;
  }
  active = std::make_unique<Active>();
  active->request = request; active->epoch = streamEpoch;
  active->first = first; active->end = end;
  for (const auto& block : ring)
    if (block->first + block->count > first && block->first < end)
      active->blocks.push_back(block);
  if (nextSample >= end) finish_active_locked();
}

void PortableClipRecorder::finish_active_locked() {
  if (!active) return;
  if (active->blocks.empty()) {
    fail_active_locked("IQ clip contains no samples");
    return;
  }
  if (jobs.size() >= options.maximumWriterJobs) {
    fail_active_locked("IQ clip writer queue is full");
    return;
  }
  auto job = std::make_unique<Job>();
  job->request = std::move(active->request); job->epoch = active->epoch;
  job->first = active->first; job->end = active->end;
  job->sequence = ++fileSequence; job->blocks = std::move(active->blocks);
  writerRevisions.insert(job->request.revision);
  jobs.push_back(std::move(job)); active.reset(); condition.notify_one();
}

IqClipStatus PortableClipRecorder::status() const {
  std::lock_guard<std::mutex> lock(mutex);
  return terminal;
}

void PortableClipRecorder::discontinuity(const std::string& reason) {
  std::lock_guard<std::mutex> lock(mutex);
  fail_active_locked(reason.empty() ? "Receiver sample discontinuity during clip" : reason);
  ring.clear(); retainedSamples = 0; haveEpoch = false;
}

void PortableClipRecorder::writer_loop() {
  for (;;) {
    std::unique_ptr<Job> job;
    {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return stopping || !jobs.empty(); });
      if (jobs.empty()) { if (stopping) return; else continue; }
      job = std::move(jobs.front()); jobs.pop_front();
    }
    IqClipStatus result;
    try { result = write_job(*job); }
    catch (const std::exception& error) {
      result = {job->request.revision, true, {}, error.what(), 0};
    }
    std::lock_guard<std::mutex> lock(mutex);
    writerRevisions.erase(job->request.revision); publish_locked(std::move(result));
  }
}

IqClipStatus PortableClipRecorder::write_job(const Job& job) {
  if (options.beforeWrite) options.beforeWrite();
  std::filesystem::create_directories(directory);
  const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  const std::string file = (std::filesystem::path(directory) /
    ("aircraft-" + std::to_string(stamp) + "-" +
      std::to_string(job.sequence) + ".blah2iq")).string();
  const std::string partial=file+".partial", metadataPartial=file+".json.partial";
  RecordingWriter output(partial, {channels, sampleRate, frequency, 0, "blah2"});
  uint64_t written = 0;
  for (const auto& block : job.blocks) {
    const uint64_t begin = std::max(job.first, block->first);
    const uint64_t finish = std::min(job.end, block->first + block->count);
    if (finish <= begin) continue;
    const std::size_t offset = static_cast<std::size_t>(begin - block->first);
    const std::size_t count = static_cast<std::size_t>(finish - begin);
    IqBlock slice(channels);
    for (uint32_t channel = 0; channel < channels; ++channel)
      slice[channel].assign(block->samples[channel].begin() + offset,
        block->samples[channel].begin() + offset + count);
    output.append(slice); written += count;
  }
  if (!written) throw std::runtime_error("IQ clip contains no writable samples");
  output.close();
  if(::chmod(partial.c_str(),0640))throw std::runtime_error(system_error("Cannot set clip permissions"));
  const auto integrity=digest_file(partial);
  rapidjson::Document document(rapidjson::kObjectType); auto& allocator=document.GetAllocator();
  document.AddMember("schema","vectorwarp-portable-iq-clip-v1",allocator);
  document.AddMember("clipRevision",job.request.revision,allocator);
  document.AddMember("requestedEventTime",job.request.requestedEventTimeMs,allocator);
  document.AddMember("windowStart",job.request.windowStartMs,allocator);
  document.AddMember("windowEnd",job.request.windowEndMs,allocator);
  document.AddMember("channels",channels,allocator); document.AddMember("sampleRate",sampleRate,allocator);
  document.AddMember("frequency",frequency,allocator); document.AddMember("samplesPerChannel",written,allocator);
  document.AddMember("dataBytes",integrity.first,allocator); document.AddMember("createdAt",stamp,allocator);
  document.AddMember("format","blah2iq",allocator); document.AddMember("receiver",rapidjson::Value(receiver.c_str(),allocator),allocator);
  document.AddMember("encoding","float32-complex-channel-major-blocks",allocator);
  document.AddMember("dataSha256",rapidjson::Value(integrity.second.c_str(),allocator),allocator);
  document.AddMember("epoch",job.epoch,allocator); document.AddMember("firstEpochSample",job.first,allocator);
  document.AddMember("endEpochSample",job.end,allocator);
  auto strings=[&](const char* name,const std::vector<std::string>& values){rapidjson::Value array(rapidjson::kArrayType);
    for(const auto& value:values)array.PushBack(rapidjson::Value(value.c_str(),allocator),allocator);
    document.AddMember(rapidjson::Value(name,allocator),array,allocator);};
  strings("eventIds",job.request.eventIds); strings("passIds",job.request.passIds);
  rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> json(buffer); document.Accept(json);
  write_synced(metadataPartial,std::string(buffer.GetString(),buffer.GetSize())+"\n");
  bool dataPublished=false;
  try {
    if(::rename(partial.c_str(),file.c_str()))throw std::runtime_error(system_error("Cannot publish clip"));
    dataPublished=true;
    if(::rename(metadataPartial.c_str(),(file+".json").c_str()))throw std::runtime_error(system_error("Cannot publish clip metadata"));
    const int directoryFd=::open(directory.c_str(),O_RDONLY|O_CLOEXEC);
    if(directoryFd>=0){(void)::fsync(directoryFd);::close(directoryFd);}
  } catch(...) {
    (void)::unlink(partial.c_str());
    (void)::unlink(metadataPartial.c_str());
    if(dataPublished) (void)::unlink(file.c_str());
    throw;
  }
  return {job.request.revision, true, file, {}, written};
}
}
