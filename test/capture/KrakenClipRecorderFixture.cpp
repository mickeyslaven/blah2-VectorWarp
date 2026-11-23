#include "capture/Recording.h"
#include "capture/RecordingAcquisitionJson.h"
#include "capture/kraken/HeimdallFrame.h"
#include "capture/kraken/KrakenClipRecorder.h"
#include "capture/kraken/HostArrivalClock.h"
#include "data/meta/EventTime.h"
#include "process/tracker/SignalProgressClock.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <rapidjson/document.h>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace blah2 {
struct KrakenClipRecorderTestAccess {
  static void discardUnpublishedAttempt(KrakenClipRecorder& recorder,
      uint32_t samples) {
    // Ordinary fixture frames have not been published to any receiver. A
    // failed synthetic attempt can be retried without representing a source
    // gap. Only the fixture's single ingress thread touches this counter.
    const auto skipped = recorder.skippedIngressSamples.exchange(0,
      std::memory_order_acq_rel);
    assert(skipped == samples);
  }
  static void hold(KrakenClipRecorder& recorder, std::mutex& gate,
      std::condition_variable& changed, bool& held, bool& release) {
    std::lock_guard<std::mutex> recorderLock(recorder.mutex);
    std::unique_lock<std::mutex> gateLock(gate);
    held = true; changed.notify_one();
    changed.wait(gateLock, [&] { return release; });
  }
};
}

namespace
{
constexpr uint32_t CHANNELS = 5;
constexpr uint32_t SAMPLE_RATE = 2400000;
constexpr uint32_t FREQUENCY = 527000000;
constexpr uint32_t FRAME_SAMPLES = 2400; // exactly one millisecond

void be(std::array<uint8_t, 32>& out, unsigned offset, uint32_t value)
{
  for (unsigned index = 0; index < 4; ++index)
    out[offset + index] = static_cast<uint8_t>(value >> (24 - index * 8));
}

void le_float(std::vector<uint8_t>& out, float value)
{
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  for (unsigned shift : {0, 8, 16, 24}) out.push_back(bits >> shift);
}

struct WireFrame
{
  std::array<uint8_t, 32> fixed{};
  std::vector<uint8_t> metadata, payload;
  HeimdallFrame::Header header;
  blah2::IqBlock decoded;
};

WireFrame frame(unsigned sequence, uint32_t samplesPerFrame = FRAME_SAMPLES)
{
  WireFrame value;
  for (const auto& word : std::vector<std::pair<unsigned, uint32_t>>{
      {0, 0x4d434851}, {4, CHANNELS}, {8, samplesPerFrame}, {12, 4},
      {16, 0}, {20, 11}, {24, 2}, {28, 0}})
    be(value.fixed, word.first, word.second);
  for (unsigned channel = 0; channel < CHANNELS; ++channel)
  {
    le_float(value.metadata, static_cast<float>(FREQUENCY));
    le_float(value.metadata, 15.0F + channel);
  }
  for (unsigned channel = 0; channel < CHANNELS; ++channel)
    for (unsigned sample = 0; sample < samplesPerFrame; ++sample)
    {
      value.payload.push_back(static_cast<uint8_t>((sequence * 7 +
        sample * (channel + 1) + channel * channel * 3) & 255));
      value.payload.push_back(static_cast<uint8_t>((sequence * 13 +
        sample * (channel + 2) + channel * 17) & 255));
    }
  value.header = HeimdallFrame::decode_header(value.fixed);
  HeimdallFrame::decode_metadata(value.header, value.metadata);
  value.decoded = HeimdallFrame::decode_payload(value.header, value.payload);
  return value;
}

void ingest(blah2::KrakenClipRecorder& recorder, const WireFrame value)
{
  // These sample-exact recorder fixtures model paced delivery. The first
  // receipt comes from the injected clock; subsequent receipts advance by
  // the actual frame duration, including the non-millisecond trim case.
  const auto previous = recorder.position();
  const int64_t receipt = previous.available ? previous.epochEstimateNanoseconds +
    static_cast<int64_t>((static_cast<__int128>(previous.nextSample +
      value.header.samplesPerChannel) * 1000000000LL) / SAMPLE_RATE) : 0;
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::seconds(3);
  while (!recorder.ingest(value.fixed, value.metadata, value.payload,
      value.header, nullptr, receipt)) {
    // Writer wakeup/completion can briefly own the optional recorder mutex.
    // Keep the const synthetic frame until admitted, rather than assert that this
    // nonblocking API can never contend. The contention regression below
    // calls ingest directly and must still retain every skipped sample.
    blah2::KrakenClipRecorderTestAccess::discardUnpublishedAttempt(recorder,
      value.header.samplesPerChannel);
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("Clip recorder did not admit fixture frame");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WireFrame timed(WireFrame value, uint64_t first, uint64_t receipt)
{
  be(value.fixed, 0, HeimdallFrame::TIMED_MAGIC);
  auto word = [&](uint32_t v) {
    for (unsigned shift : {24, 16, 8, 0}) value.metadata.push_back(v >> shift);
  };
  for (uint32_t v : {1U, 96U, 1U, 0U}) word(v);
  for (uint64_t v : {uint64_t(1), first, first / value.header.samplesPerChannel,
      receipt, uint64_t(0), receipt, uint64_t(100), uint64_t(0),
      first / value.header.samplesPerChannel, first + value.header.samplesPerChannel}) {
    word(v >> 32); word(v);
  }
  value.header = HeimdallFrame::decode_header(value.fixed);
  HeimdallFrame::decode_metadata(value.header, value.metadata);
  return value;
}

blah2::IqClipRequest request(uint64_t revision, int64_t start, int64_t end)
{
  return {revision, start + (end - start) / 2, start, end,
    {"eligibility:event-" + std::to_string(revision)},
    {"pass-" + std::to_string(revision)}};
}

blah2::IqClipStatus wait(blah2::KrakenClipRecorder& recorder, uint64_t revision,
    std::chrono::seconds timeout = std::chrono::seconds(3))
{
  const auto deadline = std::chrono::steady_clock::now() +
    timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto status = recorder.status();
    // Status retains the latest completed revision while a newer request is
    // collecting or writing. An older terminal result is not our completion.
    if (status.terminal && status.revision == revision) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  throw std::runtime_error("Clip recorder did not complete revision " +
    std::to_string(revision));
}

rapidjson::Document json_file(const std::string& path)
{
  std::ifstream stream(path);
  std::ostringstream text;
  text << stream.rdbuf();
  rapidjson::Document document;
  document.Parse(text.str().c_str());
  assert(!document.HasParseError() && document.IsObject());
  return document;
}
}

int main()
{
  char temporary[] = "/tmp/vectorwarp-kraken-clips.XXXXXX";
  assert(mkdtemp(temporary));
  const std::filesystem::path directory(temporary);
  try
  {
    // Stable sample progression is fit against host receipts with a robust
    // estimator. A delivery step creates a fresh relative epoch; it does not
    // make the estimate hardware UTC.
    {
      blah2::kraken_timing::HostArrivalClock anchors(SAMPLE_RATE);
      for (uint64_t frame = 1; frame <= 5; ++frame) {
        const auto estimate = anchors.observe(frame * FRAME_SAMPLES,
          1000000000LL + static_cast<int64_t>(frame) * 1000000LL);
        assert(estimate.available && !estimate.discontinuity &&
          estimate.maximumResidualNanoseconds == 0 && estimate.driftPpm == 0);
      }
      const auto discontinuity = anchors.observe(6 * FRAME_SAMPLES, 1306000000LL);
      assert(discontinuity.discontinuity && discontinuity.anchors == 1 &&
        discontinuity.epochNanoseconds == 1300000000LL);
    }
    {
      blah2::kraken_timing::HostArrivalClock anchors(SAMPLE_RATE);
      for (uint64_t second = 10000; second < 10005; ++second) {
        const uint64_t sample = second * SAMPLE_RATE;
        const int64_t receipt = 1000000000LL + second * 1000100000LL;
        const auto estimate = anchors.observe(sample, receipt);
        if (second > 10000) {
          assert(estimate.driftPpm == 100 &&
            estimate.maximumResidualNanoseconds == 0);
          // The published nominal-rate epoch must recover the most recent
          // fitted receipt even after hours of sample progression.
          assert(estimate.epochNanoseconds +
            static_cast<int64_t>(second) * 1000000000LL == receipt);
        }
      }
    }
    int64_t clock = 1000000000LL;
    blah2::KrakenClipRecorder::Options options;
    options.ringSeconds = 0.02;
    options.lateClaimGuardSeconds = 0.001;
    options.clockNanoseconds = [&] { return clock; };

    // Production retention is finite and matches API late-claim policy.
    assert(blah2::KrakenClipRecorder::Options{}.ringSeconds == 10.0);
    assert(blah2::KrakenClipRecorder::Options{}.lateClaimGuardSeconds == 1.0);
    for (bool ringBound : {false, true}) {
      auto invalid = options;
      if (ringBound) invalid.ringSeconds = 10.001;
      else invalid.maximumClipSeconds = 10.001;
      bool rejected = false;
      try {
        blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
          SAMPLE_RATE, FREQUENCY, invalid);
      } catch (const std::invalid_argument&) { rejected = true; }
      assert(rejected && "native clip/ring cap must not be expandable");
    }

    // Saturate the actual recorder mutex from another thread. Missing copies
    // fail the affected clip but cannot reset acquisition position or freeze
    // the receiver-independent tracker progression when metadata returns.
    {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      blah2::tracker_clock::SignalProgressClock progress;
      for (unsigned sequence = 0; sequence < 3; ++sequence) {
        ingest(recorder, frame(sequence));
        const auto position = recorder.position();
        assert(progress.advance(FRAME_SAMPLES, SAMPLE_RATE, true,
          position.epoch, position.nextSample) == sequence + 1);
      }
      const auto before = recorder.position();
      recorder.request(request(200, 1000, 1008));
      std::mutex gate;
      std::condition_variable changed;
      bool held = false, release = false;
      std::thread holder([&] {
        blah2::KrakenClipRecorderTestAccess::hold(recorder, gate, changed,
          held, release);
      });
      {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return held; });
      }
      for (unsigned sequence = 3; sequence < 5; ++sequence) {
        auto value = frame(sequence);
        const bool retained = recorder.ingest(value.fixed,
          std::move(value.metadata), std::move(value.payload), value.header,
          nullptr, 1000000000LL + sequence * 1000000LL);
        assert(!retained);
        assert(progress.advance(FRAME_SAMPLES, SAMPLE_RATE) == sequence + 1);
      }
      {
        std::lock_guard<std::mutex> lock(gate);
        release = true; changed.notify_one();
      }
      holder.join();
      auto value = frame(5);
      blah2::KrakenClipRecorder::Position after;
      assert(recorder.ingest(value.fixed, std::move(value.metadata),
        std::move(value.payload), value.header, &after, 1005000000LL));
      assert(after.available && !after.discontinuity && after.epoch == before.epoch);
      assert(after.nextSample == 6ULL * FRAME_SAMPLES);
      assert(after.epochEstimateNanoseconds == before.epochEstimateNanoseconds);
      assert(progress.advance(FRAME_SAMPLES, SAMPLE_RATE, true,
        after.epoch, after.nextSample) == 6);
      assert(recorder.status().revision == 200 && recorder.status().terminal &&
        recorder.status().error == "Kraken clip ring was saturated; recorder copy dropped");
      // The discarded prefix cannot be represented as a continuous clip.
      recorder.request(request(201, 1000, 1007));
      assert(recorder.status().revision == 201 && recorder.status().terminal &&
        recorder.status().error == "Kraken clip pre-roll is unavailable");
      recorder.request(request(202, 1004, 1007));
      // Recovery is still collecting: the previous terminal result remains
      // visible, so waiting for just any terminal status would return too soon.
      assert(recorder.status().revision == 201 && recorder.status().terminal);
      ingest(recorder, frame(6)); ingest(recorder, frame(7));
      const auto recovered = wait(recorder, 202);
      assert(recovered.revision == 202 && recovered.error.empty() &&
        recovered.samples == 3ULL * FRAME_SAMPLES);
      recorder.discontinuity("real input boundary");
      ingest(recorder, frame(8));
      const auto reset = recorder.position();
      assert(reset.epoch == before.epoch + 1 && reset.nextSample == FRAME_SAMPLES);
    }

    // A completed event-time window remains recordable after ring wrap if its
    // actual sample interval is retained. Wall-clock age is not sufficient:
    // the second request has aged out even though it is a valid short window.
    {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      for (unsigned sequence = 0; sequence < 30; ++sequence)
        ingest(recorder, frame(sequence));
      const auto before = recorder.position();
      assert(before.nextSample == 30ULL * FRAME_SAMPLES);
      recorder.request(request(100, 1010, 1015));
      const auto late = wait(recorder, 100);
      assert(late.revision == 100 && late.error.empty() &&
        late.samples == 5ULL * FRAME_SAMPLES && !late.file.empty());
      // Repeated claims cannot create a second artifact or alter its identity.
      recorder.request(request(100, 1010, 1015));
      assert(recorder.status().file == late.file);
      assert(recorder.position().nextSample == before.nextSample);
      std::ifstream sidecar(late.file + ".json");
      const std::string json(std::istreambuf_iterator<char>(sidecar), {});
      rapidjson::Document document; document.Parse(json.c_str());
      assert(!document.HasParseError());
      assert(document["windowStart"].GetInt64() == 1010 &&
        document["windowEnd"].GetInt64() == 1015);
      assert(document["firstEpochSample"].GetUint64() == 11ULL * FRAME_SAMPLES &&
        document["endEpochSample"].GetUint64() == 16ULL * FRAME_SAMPLES);
      // This one still intersects a retained frame, but crosses the sample-
      // indexed safety guard. It must fail without relying on wall-clock age.
      recorder.request(request(101, 1009, 1015));
      assert(recorder.status().revision == 101 && recorder.status().file.empty() &&
        recorder.status().error == "Kraken clip pre-roll is unavailable");
      recorder.request(request(102, 1000, 1005));
      const auto expired = recorder.status();
      assert(expired.revision == 102 && expired.file.empty() &&
        expired.error == "Kraken clip pre-roll is unavailable");
    }

    // Heimdall has no RF timestamp. Identical samples arriving 100 ms later
    // retain identical sample positions but necessarily move the host-derived
    // UTC estimate by 100 ms. The provenance must expose that estimate as
    // inexact with unbounded uncertainty instead of inventing device precision.
    {
      blah2::KrakenClipRecorder first(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      ingest(first, frame(0));
      const auto firstPosition = first.position();
      assert(firstPosition.available && firstPosition.nextSample == FRAME_SAMPLES &&
        firstPosition.epochEstimateNanoseconds == 999000000LL);
      const auto firstTime = blah2::kraken_cpi_midpoint_time(
        firstPosition.epochEstimateNanoseconds, firstPosition.epoch, 0,
        FRAME_SAMPLES, SAMPLE_RATE);
      assert(firstTime.present && firstTime.estimateAvailable &&
        firstTime.estimateNanoseconds == 999500000LL);

      clock += 100000000LL;
      blah2::KrakenClipRecorder delayed(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      ingest(delayed, frame(0));
      const auto delayedPosition = delayed.position();
      const auto delayedTime = blah2::kraken_cpi_midpoint_time(
        delayedPosition.epochEstimateNanoseconds, delayedPosition.epoch, 0,
        FRAME_SAMPLES, SAMPLE_RATE);
      assert(delayedPosition.nextSample == firstPosition.nextSample &&
        delayedTime.estimateNanoseconds - firstTime.estimateNanoseconds ==
          100000000LL);
      const auto annotated = blah2::annotate_event_time(
        "{\"timestamp\":999,\"gain\":7.699999809265137,\"response\":[1e-20,0.12345678901234567]}", delayedTime);
      rapidjson::Document document;
      document.Parse<rapidjson::kParseFullPrecisionFlag>(annotated.c_str());
      assert(!document.HasParseError() && document["eventTime"]["exact"] == false &&
        document["eventTime"]["uncertaintyBounded"] == false &&
        document["eventTime"]["uncertaintyNs"].IsNull());
      assert(document["gain"].GetDouble() == double(7.7f) &&
        document["response"][0].GetDouble() == 1e-20 &&
        document["response"][1].GetDouble() == 0.12345678901234567);
      clock = 1000000000LL;
    }

    // The requested 2 ms pre/3 ms post window is sample exact. The completed
    // MCHQ artifact goes through the production RecordingReader used by replay.
    {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      std::vector<blah2::IqBlock> expected;
      for (unsigned sequence = 0; sequence < 4; ++sequence)
      {
        auto value = frame(sequence);
        if (sequence >= 1) expected.push_back(value.decoded);
        ingest(recorder, std::move(value));
      }
      const auto position = recorder.position();
      assert(position.available && position.nextSample == 4ULL * FRAME_SAMPLES);
      recorder.request(request(1, 1000, 1005));
      for (unsigned sequence = 4; sequence < 6; ++sequence)
      {
        auto value = frame(sequence);
        expected.push_back(value.decoded);
        ingest(recorder, std::move(value));
      }
      const auto status = wait(recorder, 1);
      assert(status.revision == 1 && status.error.empty());
      assert(status.samples == 5ULL * FRAME_SAMPLES);
      blah2::RecordingReader reader(status.file,
        {"auto", "Kraken", CHANNELS, SAMPLE_RATE, FREQUENCY, 0});
      assert(reader.metadata().format == "mchq" &&
        reader.metadata().samples == status.samples);
      assert(reader.provenance().sourceSampleAvailable &&
        reader.provenance().epoch == 0 &&
        reader.provenance().sourceFirstSample == FRAME_SAMPLES);
      assert(!reader.provenance().contractValid &&
        !reader.provenance().decisionGrade);
      blah2::IqBlock block;
      uint64_t observed = 0;
      for (const auto& wanted : expected)
      {
        assert(reader.read(block));
        assert(block == wanted); // all five channels retain wire order
        observed += block.front().size();
      }
      assert(!reader.read(block) && observed == status.samples);
      const auto metadata = json_file(status.file + ".json");
      assert(metadata["schema"] == "vectorwarp-kraken-iq-clip-v1");
      assert(metadata["format"] == "mchq" && metadata["channels"] == 5);
      assert(metadata["sampleRate"] == SAMPLE_RATE &&
        metadata["samplesPerChannel"] == status.samples);
      assert(metadata["dataSha256"].IsString() &&
        metadata["dataSha256"].GetStringLength() == 64);
      assert(std::string(metadata["dataSha256"].GetString()) ==
        "a4c672ed6b0a45847394a02c477378a8ae497e2451665db542178d8ff02b7915");
      assert(metadata["channelFrequencies"].Size() == CHANNELS &&
        metadata["channelGains"].Size() == CHANNELS);
      assert(metadata["eventIds"][0] == "eligibility:event-1" &&
        metadata["passIds"][0] == "pass-1");
      assert(metadata["eventTime"].IsObject() &&
        metadata["eventTime"]["source"] ==
          "kraken_heimdall_host_arrival_estimate" &&
        metadata["eventTime"]["exact"] == false &&
        metadata["eventTime"]["uncertaintyBounded"] == false &&
        metadata["eventTime"]["uncertaintyNs"].IsNull());
    }

    // Native raw writes cross two full cache windows while their incremental
    // hash remains bound to exact original payload and conditioning boundaries.
    {
      // Keep each synthetic Heimdall frame within the wire protocol's 65,536
      // sample limit while making the complete clip cross two cache windows.
      constexpr unsigned samples = 60000, frames = 60;
      auto largeOptions = options;
      largeOptions.ringSeconds = largeOptions.maximumClipSeconds = 2.0;
      largeOptions.lateClaimGuardSeconds = 0;
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, largeOptions);
      for (unsigned index = 0; index < frames; ++index)
        ingest(recorder, frame(index, samples));
      const auto largePosition = recorder.position();
      const int64_t largeStartMs = largePosition.epochEstimateNanoseconds /
        1000000LL;
      recorder.request(request(90, largeStartMs, largeStartMs + 1500));
      const auto status = wait(recorder, 90, std::chrono::seconds(25));
      if (!status.error.empty())
        std::cerr << "Large native clip failed: " << status.error << '\n';
      assert(status.error.empty() && status.samples == uint64_t(frames) * samples);
      const auto metadata = json_file(status.file + ".json");
      assert(metadata["dataBytes"].GetUint64() == std::filesystem::file_size(status.file));
      assert(metadata["dataBytes"].GetUint64() > 2 * blah2::RecordingFileCache<>::windowBytes);
      blah2::ReplayOptions read{"mchq", "Kraken", CHANNELS, SAMPLE_RATE, FREQUENCY, 0};
      read.expectedRecordingSha256 = metadata["dataSha256"].GetString();
      blah2::RecordingReader replay(status.file, read);
      blah2::IqBlock recovered;
      for (unsigned index = 0; index < frames; ++index) {
        assert(replay.read(recovered));
        assert(recovered == frame(index, samples).decoded);
      }
      assert(!replay.read(recovered) && replay.provenance().frames.size() == frames);
    }

    // Millisecond clip bounds normally cut through Heimdall frames. Both end
    // frames are trimmed channel-by-channel while retaining valid MCHQ blocks.
    for (bool withTiming : {false, true}) {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      blah2::RecordingContext context;
      context.referenceMode = "array_eigenbeam";
      context.referenceChannels = context.surveillanceChannels = {0,1,2,3,4};
      context.coherent = context.geometryConfirmed = true;
      context.geometryJson = R"({"units":"m","x_axis_bearing_deg_true":0,"elements":[{"daq_channel":0,"position":[0,0,0]},{"daq_channel":1,"position":[1,0,0]},{"daq_channel":2,"position":[0,1,0]},{"daq_channel":3,"position":[-1,0,0]},{"daq_channel":4,"position":[0,-1,0]}]})";
      context.configurationSha256 = std::string(64, 'a');
      context.codeSha256 = std::string(64, 'b');
      context.manifoldSha256 = {{"bearing", std::string(64, 'c')}};
      context.geographicSitesJson = R"({"schema":"vectorwarp-recorded-geographic-sites-v1","provenance":"operator-configured-at-acquisition","rx":{"latitude":40.1,"longitude":-80.2,"altitude":300.0},"tx":{"latitude":40.5,"longitude":-80.0,"altitude":450.0},"illuminators":{}})";
      recorder.set_recording_context(context);
      ingest(recorder, withTiming ? timed(frame(20, 3000), 60000, 1000000000) : frame(20, 3000));
      ingest(recorder, withTiming ? timed(frame(21, 3000), 63000, 1001250000) : frame(21, 3000));
      recorder.request(request(10, 999, 1002));
      ingest(recorder, withTiming ? timed(frame(22, 3000), 66000, 1002500000) : frame(22, 3000));
      const auto status = wait(recorder, 10);
      assert(status.revision == 10 && status.error.empty() &&
        status.samples == 7200);
      blah2::RecordingReader reader(status.file,
        {"auto", "Kraken", CHANNELS, SAMPLE_RATE, FREQUENCY, 0});
      blah2::IqBlock block;
      unsigned originalSequence = 20;
      for (uint32_t count : {2400U, 3000U, 1800U})
      {
        assert(reader.read(block) && block.size() == CHANNELS);
        for (const auto& channel : block) assert(channel.size() == count);
        const auto original = frame(originalSequence, 3000);
        const uint32_t offset = originalSequence++ == 20 ? 600 : 0;
        for (uint32_t channel = 0; channel < CHANNELS; ++channel)
          assert(std::equal(block[channel].begin(), block[channel].end(),
            original.decoded[channel].begin() + offset));
      }
      assert(!reader.read(block));
      reader.seek(100); assert(reader.read(block));
      const auto original = frame(20, 3000);
      for (uint32_t channel = 0; channel < CHANNELS; ++channel)
        assert(std::equal(block[channel].begin(), block[channel].end(),
          original.decoded[channel].begin() + 700));
      assert(reader.provenance().acquisitionIntegrityValid);
      assert(!reader.provenance().geographicSitesAvailable);
      assert(reader.provenance().acquisitionContext.referenceChannels == context.referenceChannels);
      assert(reader.provenance().acquisitionContext.surveillanceChannels == context.surveillanceChannels);
      assert(reader.provenance().acquisitionContext.geometryJson == context.geometryJson);
      assert(reader.provenance().acquisitionContext.manifoldSha256 == context.manifoldSha256);
      assert(reader.provenance().perChannelGainDb == std::vector<double>({15,16,17,18,19}));
      assert(reader.provenance().gainProvenance == "sample-aligned-receiver-header");
      assert(reader.provenance().frames.size() == 3 && reader.provenance().frames[0].sliceOffset == 600);
      blah2::ReplayOptions exact{"auto", "Usrp", CHANNELS, SAMPLE_RATE, FREQUENCY, 0};
      exact.requireExactAcquisition = true;
      exact.expectedRecordingSha256 = reader.provenance().recordingSha256;
      exact.expectedAcquisitionSha256 = reader.provenance().acquisitionSha256;
      exact.validateAcquisitionContext = true; exact.expectedAcquisitionContext = context;
      const auto rejected = [&](const blah2::ReplayOptions& candidate) {
        bool failed = false;
        try { blah2::RecordingReader invalid(status.file, candidate); }
        catch (const std::runtime_error&) { failed = true; }
        assert(failed);
      };
      if (withTiming) {
        blah2::RecordingReader qualified(status.file, exact);
        assert(qualified.provenance().exactAcquisition && qualified.provenance().acquisitionHashTrusted);
        assert(qualified.provenance().geographicSitesAvailable &&
          qualified.provenance().acquisitionContext.geographicSitesJson == context.geographicSitesJson);
        auto altered = exact; altered.expectedAcquisitionSha256.clear(); rejected(altered);
        altered = exact; altered.expectedRecordingSha256 = std::string(64, 'f'); rejected(altered);
        altered = exact; altered.expectedAcquisitionSha256 = std::string(64, 'f'); rejected(altered);
        altered = exact; altered.expectedAcquisitionContext.surveillanceChannels = {0,1}; rejected(altered);
        assert(reader.provenance().sourceSampleAvailable &&
          reader.provenance().epoch == 1 &&
          reader.provenance().sourceFirstSample == 60600);
        assert(!reader.provenance().contractValid &&
          !reader.provenance().decisionGrade);
        std::ifstream file(status.file, std::ios::binary);
        std::array<uint8_t, 32> fixed{}; file.read(reinterpret_cast<char*>(fixed.data()), fixed.size());
        auto header = HeimdallFrame::decode_header(fixed);
        std::vector<uint8_t> metadata(HeimdallFrame::metadata_size(header));
        file.read(reinterpret_cast<char*>(metadata.data()), metadata.size());
        HeimdallFrame::decode_metadata(header, metadata);
        assert(header.timing.firstSample == 60600 && header.timing.anchorSample == 63000);
        assert(header.timing.realtimeNanoseconds == 1000000000);
        const auto sidecar = json_file(status.file + ".json");
        assert(!sidecar["upstreamTiming"]["hardwareUtc"].GetBool());
        assert(!sidecar["eventTime"]["exact"].GetBool());
      } else {
        assert(!reader.provenance().exactAcquisition);
        rejected(exact);
        // Legacy MCHQ frames have no embedded timing, but the authenticated
        // native clip sidecar still supplies the recorder's coherent sample
        // interval for offline truth alignment.
        assert(reader.provenance().sourceSampleAvailable &&
          reader.provenance().epoch == 0 &&
          reader.provenance().sourceFirstSample == 600);
        assert(!reader.provenance().contractValid &&
          !reader.provenance().decisionGrade);
      }
      // Changing metadata and its self-declared digest cannot satisfy the
      // externally pinned acquisition identity, even though ordinary playback
      // can still consume a structurally valid manifest.
      auto manifest = json_file(status.file + ".acquisition.json");
      manifest["acquisition"]["codeSha256"].SetString(std::string(64, 'd').c_str(), manifest.GetAllocator());
      const auto changedHash = blah2::acquisition::digest(blah2::acquisition::encode(manifest["acquisition"]));
      manifest["acquisitionSha256"].SetString(changedHash.c_str(), manifest.GetAllocator());
      { std::ofstream output(status.file + ".acquisition.json"); output << blah2::acquisition::encode(manifest); }
      rejected(exact);
      // Payload binding is independently checked, not just the metadata hash.
      manifest["recordingSha256"].SetString(std::string(64, 'e').c_str(), manifest.GetAllocator());
      { std::ofstream output(status.file + ".acquisition.json"); output << blah2::acquisition::encode(manifest); }
      auto wrongData = exact; wrongData.expectedAcquisitionSha256 = changedHash;
      rejected(wrongData);
      manifest["recordingSha256"].SetString(exact.expectedRecordingSha256.c_str(), manifest.GetAllocator());
      manifest["acquisition"]["frames"][0].RemoveMember("originalMeanIq");
      const auto missingMeanHash = blah2::acquisition::digest(blah2::acquisition::encode(manifest["acquisition"]));
      manifest["acquisitionSha256"].SetString(missingMeanHash.c_str(), manifest.GetAllocator());
      { std::ofstream output(status.file + ".acquisition.json"); output << blah2::acquisition::encode(manifest); }
      auto missingMean = exact; missingMean.expectedAcquisitionSha256 = missingMeanHash;
      rejected(missingMean);
      std::filesystem::remove(status.file + ".acquisition.json");
      blah2::RecordingReader legacy(status.file, {"auto", "Kraken", CHANNELS, SAMPLE_RATE, FREQUENCY, 0});
      assert(legacy.metadata().samples == 7200 && !legacy.provenance().exactAcquisition);
      rejected(exact);
    }

    // A revised overlapping request replaces the active bounds and produces
    // one complete extended clip, never a mixed or truncated acknowledgement.
    {
      auto shortRing = options;
      shortRing.ringSeconds = 0.002;
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, shortRing);
      for (unsigned sequence = 0; sequence < 4; ++sequence)
        ingest(recorder, frame(sequence));
      recorder.request(request(2, 1000, 1005));
      ingest(recorder, frame(4)); // original active clip retains evicted pre-roll
      recorder.request(request(3, 1000, 1007));
      for (unsigned sequence = 5; sequence < 8; ++sequence)
        ingest(recorder, frame(sequence));
      const auto status = wait(recorder, 3);
      assert(status.revision == 3 && status.error.empty() &&
        status.samples == 7ULL * FRAME_SAMPLES);
    }

    // Sync/retune/connection discontinuities fail an active clip and clear the
    // pre-roll epoch, so no artifact can join samples from both sides.
    {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      for (unsigned sequence = 0; sequence < 4; ++sequence)
        ingest(recorder, frame(sequence));
      recorder.request(request(4, 1000, 1007));
      recorder.discontinuity("test synchronization loss");
      const auto status = wait(recorder, 4);
      assert(status.revision == 4 && status.file.empty() &&
        status.error == "test synchronization loss");
    }

    // Repeated polling while the writer owns a revision is idempotent. It must
    // not start the expired window again and briefly publish a false failure.
    {
      std::mutex gateMutex;
      std::condition_variable gateCondition;
      bool writerEntered = false, releaseWriter = false;
      auto blocked = options;
      blocked.ringSeconds = 0.004;
      blocked.beforeWrite = [&] {
        std::unique_lock<std::mutex> lock(gateMutex);
        writerEntered = true;
        gateCondition.notify_all();
        gateCondition.wait(lock, [&] { return releaseWriter; });
      };
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, blocked);
      for (unsigned sequence = 0; sequence < 4; ++sequence)
        ingest(recorder, frame(sequence));
      recorder.request(request(8, 1000, 1001));
      {
        std::unique_lock<std::mutex> lock(gateMutex);
        assert(gateCondition.wait_for(lock, std::chrono::seconds(2),
          [&] { return writerEntered; }));
      }
      for (unsigned sequence = 4; sequence < 8; ++sequence)
        ingest(recorder, frame(sequence));
      recorder.request(request(8, 1000, 1001));
      assert((recorder.status().revision != 8) &&
        "Polling must not restart a revision being written");
      {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseWriter = true;
      }
      gateCondition.notify_all();
      const auto status = wait(recorder, 8);
      assert(status.revision == 8 && status.error.empty() &&
        !status.file.empty());
    }

    // Writer queue saturation is visible and cannot block the receive side.
    {
      std::mutex gateMutex;
      std::condition_variable gateCondition;
      bool writerEntered = false, releaseWriter = false;
      auto blocked = options;
      blocked.maximumWriterJobs = 1;
      blocked.beforeWrite = [&] {
        std::unique_lock<std::mutex> lock(gateMutex);
        writerEntered = true;
        gateCondition.notify_all();
        gateCondition.wait(lock, [&] { return releaseWriter; });
      };
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, blocked);
      for (unsigned sequence = 0; sequence < 4; ++sequence)
        ingest(recorder, frame(sequence));
      recorder.request(request(5, 1000, 1001));
      {
        std::unique_lock<std::mutex> lock(gateMutex);
        assert(gateCondition.wait_for(lock, std::chrono::seconds(2),
          [&] { return writerEntered; }));
      }
      recorder.request(request(6, 1000, 1001));
      const auto before = std::chrono::steady_clock::now();
      recorder.request(request(7, 1000, 1001));
      ingest(recorder, frame(4));
      assert(std::chrono::steady_clock::now() - before <
        std::chrono::milliseconds(100));
      const auto saturated = wait(recorder, 7);
      assert(saturated.revision == 7 && saturated.error.find("saturated") !=
        std::string::npos);
      {
        std::lock_guard<std::mutex> lock(gateMutex);
        releaseWriter = true;
      }
      gateCondition.notify_all();
    }

    // Directory/disk failures occur on the writer and surface through status;
    // request admission itself performs no filesystem operation.
    {
      blah2::KrakenClipRecorder recorder((directory / "missing" / "path").string(),
        CHANNELS, SAMPLE_RATE, FREQUENCY, options);
      for (unsigned sequence = 0; sequence < 4; ++sequence)
        ingest(recorder, frame(sequence));
      const auto before = std::chrono::steady_clock::now();
      recorder.request(request(8, 1000, 1001));
      assert(std::chrono::steady_clock::now() - before <
        std::chrono::milliseconds(100));
      const auto failed = wait(recorder, 8);
      assert(failed.revision == 8 && failed.file.empty() &&
        failed.error.find("Cannot create clip") != std::string::npos);
    }

    // The native boundary independently rejects any API regression that asks
    // for more than the physical ten-second maximum.
    {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      recorder.request(request(9, 1000, 11001));
      const auto failed = wait(recorder, 9);
      assert(failed.revision == 9 &&
        failed.error.find("ten-second") != std::string::npos);
    }

    std::filesystem::remove_all(directory);
    std::cout << "Kraken bounded clips: explicit arrival-time uncertainty, exact pre/post samples, five-channel replay/order, overlap revision, discontinuity, queue saturation and async disk failure passed.\n";
  }
  catch (...)
  {
    std::filesystem::remove_all(directory);
    throw;
  }
}
