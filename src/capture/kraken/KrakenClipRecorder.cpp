#include "KrakenClipRecorder.h"
#include "data/meta/EventTime.h"

#include <algorithm>
#include <cerrno>
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
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

namespace blah2
{
namespace
{
constexpr int64_t NS_PER_SECOND = 1000000000LL;
constexpr int64_t NS_PER_MILLISECOND = 1000000LL;
constexpr std::size_t MAX_IDS = 4096;
constexpr std::size_t MAX_ID_LENGTH = 256;

void put_be_u32(uint8_t* bytes, uint32_t value)
{
  bytes[0] = static_cast<uint8_t>(value >> 24);
  bytes[1] = static_cast<uint8_t>(value >> 16);
  bytes[2] = static_cast<uint8_t>(value >> 8);
  bytes[3] = static_cast<uint8_t>(value);
}

bool valid_id(const std::string& value)
{
  if (value.empty() || value.size() > MAX_ID_LENGTH) return false;
  return std::none_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte < 0x20 || byte == 0x7f;
  });
}

std::string system_error(const std::string& prefix)
{
  return prefix + ": " + std::strerror(errno);
}

class Sha256
{
public:
  void update(const void* source, std::size_t count)
  {
    const auto* bytes = static_cast<const uint8_t*>(source);
    total += count;
    while (count)
    {
      const std::size_t copied = std::min(count, block.size() - used);
      std::copy(bytes, bytes + copied, block.begin() + used);
      used += copied;
      bytes += copied;
      count -= copied;
      if (used == block.size()) { transform(block.data()); used = 0; }
    }
  }

  std::string finish()
  {
    const uint64_t bits = total * 8;
    block[used++] = 0x80;
    if (used > 56)
    {
      std::fill(block.begin() + used, block.end(), 0);
      transform(block.data());
      used = 0;
    }
    std::fill(block.begin() + used, block.begin() + 56, 0);
    for (unsigned index = 0; index < 8; ++index)
      block[63 - index] = static_cast<uint8_t>(bits >> (index * 8));
    transform(block.data());
    const char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (uint32_t word : state)
      for (int shift : {28, 24, 20, 16, 12, 8, 4, 0})
        result.push_back(hex[(word >> shift) & 15]);
    return result;
  }

private:
  static uint32_t rotate(uint32_t value, unsigned count)
  {
    return (value >> count) | (value << (32 - count));
  }
  void transform(const uint8_t* input)
  {
    static constexpr std::array<uint32_t, 64> constants{{
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2}};
    uint32_t words[64]{};
    for (unsigned index = 0; index < 16; ++index)
      words[index] = uint32_t(input[index * 4]) << 24 |
        uint32_t(input[index * 4 + 1]) << 16 |
        uint32_t(input[index * 4 + 2]) << 8 | input[index * 4 + 3];
    for (unsigned index = 16; index < 64; ++index)
    {
      const uint32_t s0 = rotate(words[index - 15], 7) ^
        rotate(words[index - 15], 18) ^ (words[index - 15] >> 3);
      const uint32_t s1 = rotate(words[index - 2], 17) ^
        rotate(words[index - 2], 19) ^ (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    uint32_t a=state[0], b=state[1], c=state[2], d=state[3];
    uint32_t e=state[4], f=state[5], g=state[6], h=state[7];
    for (unsigned index = 0; index < 64; ++index)
    {
      const uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
      const uint32_t choose = (e & f) ^ (~e & g);
      const uint32_t first = h + s1 + choose + constants[index] + words[index];
      const uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
      const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t second = s0 + majority;
      h=g; g=f; f=e; e=d+first; d=c; c=b; b=a; a=first+second;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
  }

  std::array<uint32_t, 8> state{{0x6a09e667,0xbb67ae85,0x3c6ef372,
    0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
  std::array<uint8_t, 64> block{};
  std::size_t used = 0;
  uint64_t total = 0;
};

class OutputFile
{
public:
  explicit OutputFile(const std::string& path, Sha256* digest = nullptr)
    : digest(digest)
  {
    // The processor group owns published captures.  Read-only group access lets
    // the separately sandboxed API verify the native SHA before admission.
    descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
      0640);
    if (descriptor < 0) throw std::runtime_error(system_error("Cannot create clip"));
  }
  ~OutputFile() { if (descriptor >= 0) ::close(descriptor); }
  void write(const void* source, std::size_t count)
  {
    auto* bytes = static_cast<const uint8_t*>(source);
    while (count)
    {
      const auto written = ::write(descriptor, bytes, count);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) throw std::runtime_error(system_error("Clip write failed"));
      bytes += written;
      count -= static_cast<std::size_t>(written);
      if (digest) digest->update(bytes - written, static_cast<std::size_t>(written));
    }
  }
  void flush()
  {
    if (::fsync(descriptor) != 0)
      throw std::runtime_error(system_error("Cannot flush clip"));
  }
private:
  int descriptor = -1;
  Sha256* digest = nullptr;
};

void lower_writer_priority()
{
#if defined(__APPLE__)
  (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
  (void)setpriority(PRIO_PROCESS, static_cast<id_t>(::syscall(SYS_gettid)), 10);
#endif
}
}

struct KrakenClipRecorder::Frame
{
  std::array<uint8_t, HeimdallFrame::FIXED_HEADER_SIZE> fixed{};
  std::vector<uint8_t> metadata;
  std::vector<uint8_t> payload;
  uint64_t firstSample = 0;
  uint32_t samples = 0;
  uint64_t epoch = 0;
};

struct KrakenClipRecorder::Active
{
  IqClipRequest request;
  uint64_t epoch = 0;
  int64_t epochEstimateNanoseconds = 0;
  uint64_t firstSample = 0;
  uint64_t endSample = 0;
  std::vector<std::shared_ptr<Frame>> frames;
};

struct KrakenClipRecorder::Job
{
  IqClipRequest request;
  uint64_t epoch = 0;
  int64_t epochEstimateNanoseconds = 0;
  uint64_t firstSample = 0;
  uint64_t endSample = 0;
  uint64_t sequence = 0;
  std::vector<std::shared_ptr<Frame>> frames;
};

KrakenClipRecorder::KrakenClipRecorder(std::string directory,
  uint32_t channels, uint32_t sampleRate, uint32_t frequency)
  : KrakenClipRecorder(std::move(directory), channels, sampleRate, frequency,
      Options{})
{
}

KrakenClipRecorder::KrakenClipRecorder(std::string directory,
  uint32_t channels, uint32_t sampleRate, uint32_t frequency, Options options)
  : directory(std::move(directory)), channels(channels), sampleRate(sampleRate),
    frequency(frequency), options(std::move(options)), hostArrivalClock(sampleRate)
{
  if (channels == 0 || channels > 8 || sampleRate == 0 || frequency == 0)
    throw std::invalid_argument("Invalid Kraken clip recording metadata");
  if (!std::isfinite(this->options.ringSeconds) ||
      this->options.ringSeconds <= 0 ||
      !std::isfinite(this->options.maximumClipSeconds) ||
      this->options.maximumClipSeconds <= 0 ||
      this->options.maximumWriterJobs == 0)
    throw std::invalid_argument("Invalid Kraken clip recorder bounds");
  const long double samples = this->options.ringSeconds * sampleRate;
  if (this->options.ringSeconds > static_cast<long double>(
      std::numeric_limits<uint64_t>::max()) / sampleRate)
    throw std::invalid_argument("Kraken clip ring is too large");
  ringSamples = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(samples)));
  writer = std::thread(&KrakenClipRecorder::writer_loop, this);
}

KrakenClipRecorder::~KrakenClipRecorder()
{
  {
    std::lock_guard<std::mutex> lock(mutex);
    stopping = true;
  }
  condition.notify_one();
  if (writer.joinable()) writer.join();
}

int64_t KrakenClipRecorder::now_nanoseconds() const
{
  if (options.clockNanoseconds) return options.clockNanoseconds();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

int64_t KrakenClipRecorder::sample_for_time(int64_t timeNanoseconds) const
{
  const __int128 delta = static_cast<__int128>(timeNanoseconds) -
    epochNanoseconds;
  const __int128 numerator = delta * sampleRate;
  const __int128 value = numerator >= 0 ?
    (numerator + NS_PER_SECOND - 1) / NS_PER_SECOND :
    numerator / NS_PER_SECOND;
  if (value > std::numeric_limits<int64_t>::max())
    return std::numeric_limits<int64_t>::max();
  if (value < std::numeric_limits<int64_t>::min())
    return std::numeric_limits<int64_t>::min();
  return static_cast<int64_t>(value);
}

void KrakenClipRecorder::publish_locked(IqClipStatus value)
{
  if (value.revision >= terminal.revision) terminal = std::move(value);
}

void KrakenClipRecorder::fail_active_locked(const std::string& error)
{
  if (!active) return;
  publish_locked({active->request.revision, true, {}, error, 0});
  active.reset();
}

void KrakenClipRecorder::reset_epoch_locked(const std::string& reason)
{
  fail_active_locked(reason);
  ring.clear();
  haveEpoch = false;
  epochNanoseconds = 0;
  hostArrivalClock.reset(); hostArrivalEstimate = {};
  hostArrivalDiscontinuity = false;
  nextSample = 0;
  ++epoch;
}

void KrakenClipRecorder::discontinuity(const std::string& reason)
{
  if (!mutex.try_lock())
  {
    ingressContention.store(true, std::memory_order_release);
    return;
  }
  std::unique_lock<std::mutex> lock(mutex, std::adopt_lock);
  reset_epoch_locked(reason.empty() ? "Kraken clip input discontinuity" : reason);
}

bool KrakenClipRecorder::ingest(
  const std::array<uint8_t, HeimdallFrame::FIXED_HEADER_SIZE>& fixed,
  std::vector<uint8_t> metadata, std::vector<uint8_t> payload,
  const HeimdallFrame::Header& header, Position* position,
  int64_t receiptNanoseconds)
{
  if (!mutex.try_lock())
  {
    ingressContention.store(true, std::memory_order_release);
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex, std::adopt_lock);
  if (ingressContention.exchange(false, std::memory_order_acq_rel))
    reset_epoch_locked("Kraken clip ring was saturated; recorder copy dropped");
  if (header.numChannels != channels || header.samplesPerChannel == 0 ||
      metadata.size() != HeimdallFrame::metadata_size(header) ||
      payload.size() != HeimdallFrame::payload_size(header))
  {
    reset_epoch_locked("Kraken clip input dimensions changed");
    return false;
  }
  if (!ring.empty() && (ring.back()->metadata != metadata ||
      !std::equal(fixed.begin() + 20, fixed.begin() + 28,
        ring.back()->fixed.begin() + 20)))
    reset_epoch_locked("Kraken clip tuning or gain metadata changed");
  if (!receiptNanoseconds) receiptNanoseconds = now_nanoseconds();
  if (!haveEpoch) { nextSample = 0; haveEpoch = true; }
  hostArrivalDiscontinuity = false;
  auto timing = hostArrivalClock.observe(nextSample + header.samplesPerChannel,
    receiptNanoseconds);
  if (timing.discontinuity) {
    reset_epoch_locked("Kraken host-receipt/sample anchor discontinuity");
    nextSample = 0; haveEpoch = true;
    timing = hostArrivalClock.observe(header.samplesPerChannel, receiptNanoseconds);
    hostArrivalDiscontinuity = true;
  }
  epochNanoseconds = timing.epochNanoseconds;
  hostArrivalEstimate = timing;

  auto frame = std::make_shared<Frame>();
  frame->fixed = fixed;
  frame->metadata = std::move(metadata);
  frame->payload = std::move(payload);
  frame->firstSample = nextSample;
  frame->samples = header.samplesPerChannel;
  frame->epoch = epoch;
  nextSample += header.samplesPerChannel;
  ring.push_back(frame);
  while (!ring.empty() && ring.front()->firstSample + ring.front()->samples +
      ringSamples < nextSample)
    ring.pop_front();

  if (active && active->epoch == epoch)
  {
    const uint64_t frameEnd = frame->firstSample + frame->samples;
    if (frameEnd > active->firstSample && frame->firstSample < active->endSample)
      active->frames.push_back(frame);
    if (nextSample >= active->endSample) finish_active_locked();
  }
  if (position) *position = {haveEpoch, hostArrivalDiscontinuity, epoch, nextSample,
    haveEpoch ? epochNanoseconds : 0, hostArrivalEstimate.residualNanoseconds,
    hostArrivalEstimate.maximumResidualNanoseconds, hostArrivalEstimate.driftPpm,
    hostArrivalEstimate.anchors};
  return true;
}

KrakenClipRecorder::Position KrakenClipRecorder::position() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return {haveEpoch, hostArrivalDiscontinuity, epoch, nextSample, haveEpoch ? epochNanoseconds : 0,
    hostArrivalEstimate.residualNanoseconds,
    hostArrivalEstimate.maximumResidualNanoseconds, hostArrivalEstimate.driftPpm,
    hostArrivalEstimate.anchors};
}

void KrakenClipRecorder::request(const IqClipRequest& value)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (value.revision == 0 || value.windowStartMs < 0 ||
      value.windowEndMs <= value.windowStartMs ||
      value.windowEndMs > std::numeric_limits<int64_t>::max() /
        NS_PER_MILLISECOND ||
      value.requestedEventTimeMs < value.windowStartMs ||
      value.requestedEventTimeMs > value.windowEndMs ||
      value.eventIds.size() > MAX_IDS || value.passIds.size() > MAX_IDS ||
      std::any_of(value.eventIds.begin(), value.eventIds.end(),
        [](const std::string& id) { return !valid_id(id); }) ||
      std::any_of(value.passIds.begin(), value.passIds.end(),
        [](const std::string& id) { return !valid_id(id); }))
  {
    if (value.revision)
      publish_locked({value.revision, true, {}, "Invalid Kraken clip request", 0});
    return;
  }
  const long double duration = static_cast<long double>(value.windowEndMs -
    value.windowStartMs) / 1000.0L;
  if (duration > options.maximumClipSeconds)
  {
    publish_locked({value.revision, true, {},
      "Kraken clip exceeds the ten-second recording limit", 0});
    return;
  }
  if ((active && active->request.revision == value.revision) ||
      writerRevisions.find(value.revision) != writerRevisions.end() ||
      terminal.revision == value.revision) return;
  start_request_locked(value);
}

void KrakenClipRecorder::start_request_locked(const IqClipRequest& value)
{
  // A newer revision supersedes an in-progress overlapping request. The raw
  // ring lets the replacement be rebuilt from the original epoch.
  std::vector<std::shared_ptr<Frame>> retained;
  uint64_t retainedEpoch = epoch;
  if (active)
  {
    retained = std::move(active->frames);
    retainedEpoch = active->epoch;
  }
  active.reset();
  if (!haveEpoch || ring.empty())
  {
    publish_locked({value.revision, true, {},
      "Kraken clip pre-roll is unavailable", 0});
    return;
  }
  const int64_t first = sample_for_time(value.windowStartMs * NS_PER_MILLISECOND);
  const int64_t end = sample_for_time(value.windowEndMs * NS_PER_MILLISECOND);
  if (first < 0 || end <= first)
  {
    publish_locked({value.revision, true, {},
      "Kraken clip pre-roll is unavailable", 0});
    return;
  }
  auto next = std::make_unique<Active>();
  next->request = value;
  next->epoch = epoch;
  next->epochEstimateNanoseconds = epochNanoseconds;
  next->firstSample = static_cast<uint64_t>(first);
  next->endSample = static_cast<uint64_t>(end);
  if (retainedEpoch != epoch) retained.clear();
  next->frames = std::move(retained);
  for (const auto& frame : ring)
  {
    const uint64_t frameEnd = frame->firstSample + frame->samples;
    if (frameEnd > next->firstSample && frame->firstSample < next->endSample &&
        std::none_of(next->frames.begin(), next->frames.end(),
          [&](const auto& existing) {
            return existing->firstSample == frame->firstSample &&
              existing->epoch == frame->epoch;
          }))
      next->frames.push_back(frame);
  }
  std::sort(next->frames.begin(), next->frames.end(),
    [](const auto& left, const auto& right) {
      return left->firstSample < right->firstSample;
    });
  if (next->frames.empty() ||
      next->frames.front()->firstSample > next->firstSample)
  {
    publish_locked({value.revision, true, {},
      "Kraken clip pre-roll is unavailable", 0});
    return;
  }
  active = std::move(next);
  if (nextSample >= active->endSample) finish_active_locked();
}

void KrakenClipRecorder::finish_active_locked()
{
  if (!active) return;
  uint64_t covered = active->firstSample;
  for (const auto& frame : active->frames)
  {
    if (frame->epoch != active->epoch) continue;
    const uint64_t frameStart = std::max(frame->firstSample, active->firstSample);
    const uint64_t frameEnd = std::min(frame->firstSample + frame->samples,
      active->endSample);
    if (frameStart > covered) break;
    covered = std::max(covered, frameEnd);
  }
  if (covered < active->endSample)
  {
    fail_active_locked("Kraken clip input is discontinuous");
    return;
  }
  if (jobs.size() >= options.maximumWriterJobs)
  {
    fail_active_locked("Kraken clip writer queue is saturated");
    return;
  }
  Job job;
  job.request = std::move(active->request);
  job.epoch = active->epoch;
  job.epochEstimateNanoseconds = active->epochEstimateNanoseconds;
  job.firstSample = active->firstSample;
  job.endSample = active->endSample;
  job.frames = std::move(active->frames);
  job.sequence = ++fileSequence;
  active.reset();
  writerRevisions.insert(job.request.revision);
  jobs.push_back(std::make_unique<Job>(std::move(job)));
  condition.notify_one();
}

IqClipStatus KrakenClipRecorder::status() const
{
  std::lock_guard<std::mutex> lock(mutex);
  return terminal;
}

void KrakenClipRecorder::writer_loop()
{
  lower_writer_priority();
  while (true)
  {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex);
      condition.wait(lock, [&] { return stopping || !jobs.empty(); });
      if (stopping && jobs.empty()) return;
      job = std::move(*jobs.front());
      jobs.pop_front();
    }
    if (options.beforeWrite) options.beforeWrite();
    auto result = write_job(job);
    std::lock_guard<std::mutex> lock(mutex);
    writerRevisions.erase(job.request.revision);
    publish_locked(std::move(result));
  }
}

IqClipStatus KrakenClipRecorder::write_job(const Job& job)
{
  IqClipStatus result{job.request.revision, true, {}, {},
    job.endSample - job.firstSample};
  std::string base;
  std::string partial;
  std::string metadataPartial;
  bool dataPublished = false;
  try
  {
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    base = (std::filesystem::path(directory) /
      ("kraken-clip-r" + std::to_string(job.request.revision) + "-" +
       std::to_string(stamp) + "-" + std::to_string(job.sequence) + ".mchq")).string();
    partial = base + ".partial";
    metadataPartial = base + ".json.partial";
    Sha256 digest;
    OutputFile output(partial, &digest);
    uint64_t dataBytes = 0;
    for (const auto& frame : job.frames)
    {
      const uint64_t first = std::max(job.firstSample, frame->firstSample);
      const uint64_t end = std::min(job.endSample,
        frame->firstSample + frame->samples);
      if (end <= first) continue;
      const uint32_t offset = static_cast<uint32_t>(first - frame->firstSample);
      const uint32_t count = static_cast<uint32_t>(end - first);
      auto fixed = frame->fixed;
      put_be_u32(fixed.data() + 8, count);
      output.write(fixed.data(), fixed.size());
      output.write(frame->metadata.data(), frame->metadata.size());
      dataBytes += fixed.size() + frame->metadata.size();
      for (uint32_t channel = 0; channel < channels; ++channel)
      {
        const auto source = frame->payload.data() +
          (static_cast<std::size_t>(channel) * frame->samples + offset) * 2;
        output.write(source, static_cast<std::size_t>(count) * 2);
        dataBytes += static_cast<uint64_t>(count) * 2;
      }
    }
    output.flush();
    const std::string dataSha256 = digest.finish();

    if (job.frames.empty())
      throw std::runtime_error("Kraken clip contains no Heimdall frames");
    auto firstHeader = HeimdallFrame::decode_header(job.frames.front()->fixed);
    HeimdallFrame::decode_metadata(firstHeader, job.frames.front()->metadata);

    rapidjson::Document document(rapidjson::kObjectType);
    auto& allocator = document.GetAllocator();
    document.AddMember("schema", "vectorwarp-kraken-iq-clip-v1", allocator);
    document.AddMember("clipRevision", job.request.revision, allocator);
    document.AddMember("requestedEventTime", job.request.requestedEventTimeMs,
      allocator);
    document.AddMember("windowStart", job.request.windowStartMs, allocator);
    document.AddMember("windowEnd", job.request.windowEndMs, allocator);
    document.AddMember("channels", channels, allocator);
    document.AddMember("sampleRate", sampleRate, allocator);
    document.AddMember("frequency", frequency, allocator);
    document.AddMember("samplesPerChannel", result.samples, allocator);
    document.AddMember("dataBytes", dataBytes, allocator);
    document.AddMember("createdAt", stamp / NS_PER_MILLISECOND, allocator);
    document.AddMember("format", "mchq", allocator);
    document.AddMember("receiver", "Kraken", allocator);
    document.AddMember("encoding", "uint8-complex-channel-major", allocator);
    document.AddMember("dataSha256",
      rapidjson::Value(dataSha256.c_str(), allocator), allocator);
    document.AddMember("epoch", job.epoch, allocator);
    document.AddMember("firstEpochSample", job.firstSample, allocator);
    document.AddMember("endEpochSample", job.endSample, allocator);
    const auto eventTime = kraken_cpi_midpoint_time(
      job.epochEstimateNanoseconds, job.epoch, job.firstSample,
      job.endSample, sampleRate);
    document.AddMember("eventTime", event_time_json(eventTime, allocator), allocator);
    document.AddMember("frequencyChangeCounter",
      firstHeader.frequencyChangeCounter, allocator);
    document.AddMember("groupIndex", firstHeader.currentGroupIndex, allocator);
    document.AddMember("phaseState", firstHeader.phaseState, allocator);
    document.AddMember("noiseSource", firstHeader.noiseSource, allocator);
    document.AddMember("retuningInProgress", firstHeader.retuningInProgress,
      allocator);
    auto numbers = [&](const char* name, const std::vector<float>& values) {
      rapidjson::Value array(rapidjson::kArrayType);
      for (float value : values) array.PushBack(value, allocator);
      document.AddMember(rapidjson::Value(name, allocator), array, allocator);
    };
    numbers("channelFrequencies", firstHeader.frequencies);
    numbers("channelGains", firstHeader.gains);
    rapidjson::Value channelOrder(rapidjson::kArrayType);
    for (uint32_t channel = 0; channel < channels; ++channel)
      channelOrder.PushBack(channel, allocator);
    document.AddMember("channelOrder", channelOrder, allocator);
    auto strings = [&](const char* name, const std::vector<std::string>& values) {
      rapidjson::Value array(rapidjson::kArrayType);
      for (const auto& value : values)
        array.PushBack(rapidjson::Value(value.c_str(), allocator), allocator);
      document.AddMember(rapidjson::Value(name, allocator), array, allocator);
    };
    strings("eventIds", job.request.eventIds);
    strings("passIds", job.request.passIds);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> json(buffer);
    document.Accept(json);
    OutputFile metadata(metadataPartial);
    metadata.write(buffer.GetString(), buffer.GetSize());
    metadata.write("\n", 1);
    metadata.flush();
    if (::rename(partial.c_str(), base.c_str()) != 0)
      throw std::runtime_error(system_error("Cannot finalize clip"));
    dataPublished = true;
    if (::rename(metadataPartial.c_str(), (base + ".json").c_str()) != 0)
      throw std::runtime_error(system_error("Cannot finalize clip metadata"));
    result.file = base;
  }
  catch (const std::exception& error)
  {
    if (!partial.empty()) (void)::unlink(partial.c_str());
    if (!metadataPartial.empty()) (void)::unlink(metadataPartial.c_str());
    // The sidecar is the completion marker. If publishing it fails after the
    // IQ rename, remove the otherwise orphaned data artifact as well.
    if (dataPublished && !base.empty()) (void)::unlink(base.c_str());
    result.error = error.what();
    result.samples = 0;
  }
  return result;
}
}
