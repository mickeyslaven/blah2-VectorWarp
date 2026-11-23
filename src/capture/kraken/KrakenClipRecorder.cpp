#include "KrakenClipRecorder.h"
#include "capture/Sha256.h"
#include "capture/RecordingAcquisitionJson.h"
#include "capture/RecordingFileCache.h"
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
    RecordingFileCacheIo::prepare(descriptor);
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
      writtenBytes += static_cast<uint64_t>(written);
      cache.written(descriptor, writtenBytes);
    }
  }
  void flush()
  {
    RecordingFileCacheIo::flush(descriptor);
    RecordingFileCache<>::durable(descriptor, writtenBytes);
  }
private:
  int descriptor = -1;
  Sha256* digest = nullptr;
  RecordingFileCache<> cache;
  uint64_t writtenBytes = 0;
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
  uint64_t ingressSequence = 0;
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

void KrakenClipRecorder::set_recording_context(RecordingContext context)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (!ring.empty() || active || !jobs.empty() || !writerRevisions.empty())
    throw std::logic_error("Kraken recording context must be set before acquisition");
  recordingContext = std::move(context);
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
      this->options.ringSeconds > 10.0 ||
      !std::isfinite(this->options.lateClaimGuardSeconds) ||
      this->options.lateClaimGuardSeconds < 0 ||
      this->options.lateClaimGuardSeconds >= this->options.ringSeconds ||
      !std::isfinite(this->options.maximumClipSeconds) ||
      this->options.maximumClipSeconds <= 0 ||
      this->options.maximumClipSeconds > 10.0 ||
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
  skippedIngressSamples.store(0, std::memory_order_release);
  if (!mutex.try_lock())
  {
    pendingDiscontinuity.store(true, std::memory_order_release);
    return;
  }
  std::unique_lock<std::mutex> lock(mutex, std::adopt_lock);
  pendingDiscontinuity.store(false, std::memory_order_release);
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
    // The recorder copy is optional; the receiver still publishes these IQ
    // samples. Account for them without changing the inference sample epoch.
    auto skipped = skippedIngressSamples.load(std::memory_order_relaxed);
    do {
      if (skipped > UINT64_MAX - header.samplesPerChannel)
        throw std::overflow_error("Kraken skipped recorder samples exhausted");
    } while (!skippedIngressSamples.compare_exchange_weak(skipped,
      skipped + header.samplesPerChannel, std::memory_order_release,
      std::memory_order_relaxed));
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex, std::adopt_lock);
  if (pendingDiscontinuity.exchange(false, std::memory_order_acq_rel))
    reset_epoch_locked("Kraken input discontinuity while recorder was busy");
  const uint64_t skipped = skippedIngressSamples.exchange(0, std::memory_order_acq_rel);
  if (skipped) {
    fail_active_locked("Kraken clip ring was saturated; recorder copy dropped");
    ring.clear();
  }
  if (header.numChannels != channels || header.samplesPerChannel == 0 ||
      metadata.size() != HeimdallFrame::metadata_size(header) ||
      payload.size() != HeimdallFrame::payload_size(header))
  {
    reset_epoch_locked("Kraken clip input dimensions changed");
    return false;
  }
  if (!ring.empty() && (ring.back()->metadata.size() != metadata.size() ||
      !std::equal(metadata.begin(), metadata.begin() + header.numChannels * 8,
        ring.back()->metadata.begin()) ||
      !std::equal(fixed.begin() + 20, fixed.begin() + 28,
        ring.back()->fixed.begin() + 20)))
    reset_epoch_locked("Kraken clip tuning or gain metadata changed");
  if (!receiptNanoseconds) receiptNanoseconds = now_nanoseconds();
  // The USB callback anchor excludes Heimdall DSP/socket queueing. Its unknown
  // USB/ADC delay still makes this a provisional host estimate, never ADC UTC.
  if (header.timing.available)
    receiptNanoseconds = static_cast<int64_t>(header.timing.realtimeNanoseconds);
  if (!haveEpoch) { nextSample = 0; haveEpoch = true; }
  if (nextSample > UINT64_MAX - skipped ||
      nextSample + skipped > UINT64_MAX - header.samplesPerChannel)
    throw std::overflow_error("Kraken recorder sample position exhausted");
  nextSample += skipped;
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
  frame->ingressSequence = ++ingressSequence;
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
  // Wall clocks only make the API's request budget; actual retained sample
  // positions decide admission. For a completed late window keep the declared
  // guard behind the newest sample, even if a large frame straddles ring wrap.
  const uint64_t firstSample = static_cast<uint64_t>(first);
  const uint64_t endSample = static_cast<uint64_t>(end);
  const uint64_t guardSamples = static_cast<uint64_t>(
    std::ceil(options.lateClaimGuardSeconds * sampleRate));
  if (endSample <= nextSample && nextSample - firstSample > ringSamples - guardSamples) {
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
  bool acquisitionPublished = false;
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
      auto metadata = frame->metadata;
      if (fixed[3] == 'T') {
        auto original = HeimdallFrame::decode_header(frame->fixed);
        HeimdallFrame::decode_metadata(original, frame->metadata);
        const auto firstSample = original.timing.firstSample + offset;
        auto* destination = metadata.data() + channels * 8 + 24;
        put_be_u32(destination, static_cast<uint32_t>(firstSample >> 32));
        put_be_u32(destination + 4, static_cast<uint32_t>(firstSample));
      }
      output.write(metadata.data(), metadata.size());
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

    RecordingBlockMetadata acquisitionMetadata;
    acquisitionMetadata.context = recordingContext;
    acquisitionMetadata.perChannelGainDb.assign(firstHeader.gains.begin(), firstHeader.gains.end());
    acquisitionMetadata.gainProvenance = "sample-aligned-receiver-header";
    acquisitionMetadata.receiverDetailsJson =
      "{\"source\":\"Heimdall-frame-header\",\"gainReadback\":\"receiver-reported\","
      "\"deviceIdentityAvailable\":false,\"deviceIdentityReason\":\"Heimdall V2 does not expose Kraken serial identity\"}";
    std::vector<RecordingFrameLineage> acquisitionFrames;
    bool acquisitionPositions = firstHeader.timing.available;
    uint64_t acquisitionFirst = job.firstSample, acquisitionEpoch = job.epoch;
    if (firstHeader.timing.available) {
      acquisitionFirst = firstHeader.timing.firstSample + job.firstSample - job.frames.front()->firstSample;
      acquisitionEpoch = firstHeader.timing.epoch;
    }
    uint64_t recordedOffset = 0;
    bool acquisitionOverflow = false;
    for (const auto& frame : job.frames) {
      const auto begin = std::max(job.firstSample, frame->firstSample);
      const auto end = std::min(job.endSample, frame->firstSample + frame->samples);
      if (end <= begin) continue;
      if (acquisitionFrames.size() >= acquisition::maximumFrames) { acquisitionOverflow = true; break; }
      auto header = HeimdallFrame::decode_header(frame->fixed); HeimdallFrame::decode_metadata(header, frame->metadata);
      acquisitionPositions = acquisitionPositions && header.timing.available && header.timing.epoch == acquisitionEpoch;
      const auto sourceFirst = header.timing.available ? header.timing.firstSample : frame->firstSample;
      acquisitionFrames.push_back({frame->ingressSequence, sourceFirst, frame->samples, begin - frame->firstSample, end - begin});
      auto& means = acquisitionFrames.back().originalMeanIq;
      means.assign(channels * 2, 0);
      for (uint32_t channel = 0; channel < channels; ++channel) {
        const auto start = static_cast<size_t>(channel) * frame->samples * 2;
        for (uint32_t sample = 0; sample < frame->samples; ++sample) {
          means[channel * 2] += frame->payload[start + sample * 2];
          means[channel * 2 + 1] += frame->payload[start + sample * 2 + 1];
        }
        means[channel * 2] /= frame->samples; means[channel * 2 + 1] /= frame->samples;
      }
      recordedOffset += end - begin;
    }
    std::string acquisitionJson;
    if (!acquisitionOverflow && acquisitionFirst <= UINT64_MAX - recordedOffset)
      acquisitionJson = acquisition::serialize(&acquisitionMetadata, "Kraken", channels,
        sampleRate, frequency, acquisitionEpoch, acquisitionFirst, acquisitionFirst + recordedOffset,
        acquisitionPositions, acquisitionFrames);

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
    if (firstHeader.timing.available) {
      const auto& t = firstHeader.timing;
      rapidjson::Value upstream(rapidjson::kObjectType);
      upstream.AddMember("protocol", "MCHT-v1", allocator);
      upstream.AddMember("basis", "usb-callback-host-receipt", allocator);
      upstream.AddMember("hardwareUtc", false, allocator);
      upstream.AddMember("decisionGrade", false, allocator);
      upstream.AddMember("usbAdcLatencyKnown", false, allocator);
      upstream.AddMember("hostClockDisciplined", bool(t.flags & 4), allocator);
      upstream.AddMember("acquisitionEpoch", t.epoch, allocator);
      upstream.AddMember("firstUsbBufferSample", t.firstSample, allocator);
      upstream.AddMember("firstSample", t.firstSample +
        (std::max(job.firstSample, job.frames.front()->firstSample) - job.frames.front()->firstSample), allocator);
      upstream.AddMember("anchorSample", t.anchorSample, allocator);
      upstream.AddMember("transferSequence", t.transferSequence, allocator);
      upstream.AddMember("bufferSequence", t.bufferSequence, allocator);
      upstream.AddMember("monotonicRawNanoseconds", t.monotonicNanoseconds, allocator);
      upstream.AddMember("taiNanoseconds", t.taiNanoseconds, allocator);
      upstream.AddMember("realtimeNanoseconds", t.realtimeNanoseconds, allocator);
      upstream.AddMember("clockReadSpanNanoseconds", t.clockReadSpanNanoseconds, allocator);
      upstream.AddMember("systemMaxErrorNanoseconds", t.systemMaxErrorNanoseconds, allocator);
      document.AddMember("upstreamTiming", upstream, allocator);
    }
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
    acquisition::writeManifest(base, acquisitionJson, dataSha256,
      acquisitionJson.empty() ? "Acquisition frame lineage exceeded bounded capacity" : "");
    acquisitionPublished = true;
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
    if (acquisitionPublished) (void)::unlink((base + ".acquisition.json").c_str());
    result.error = error.what();
    result.samples = 0;
  }
  return result;
}
}
