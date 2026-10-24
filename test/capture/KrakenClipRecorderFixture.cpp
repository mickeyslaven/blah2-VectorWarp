#include "capture/Recording.h"
#include "capture/kraken/HeimdallFrame.h"
#include "capture/kraken/KrakenClipRecorder.h"
#include "data/meta/EventTime.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <rapidjson/document.h>
#include <sstream>
#include <thread>
#include <unistd.h>

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

void ingest(blah2::KrakenClipRecorder& recorder, WireFrame value)
{
  assert(recorder.ingest(value.fixed, std::move(value.metadata),
    std::move(value.payload), value.header));
}

blah2::IqClipRequest request(uint64_t revision, int64_t start, int64_t end)
{
  return {revision, start + (end - start) / 2, start, end,
    {"eligibility:event-" + std::to_string(revision)},
    {"pass-" + std::to_string(revision)}};
}

blah2::IqClipStatus wait(blah2::KrakenClipRecorder& recorder)
{
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto status = recorder.status();
    if (status.terminal) return status;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  throw std::runtime_error("Clip recorder did not reach a terminal state");
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
    int64_t clock = 1000000000LL;
    blah2::KrakenClipRecorder::Options options;
    options.ringSeconds = 0.02;
    options.clockNanoseconds = [&] { return clock; };

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
        "{\"timestamp\":999}", delayedTime);
      rapidjson::Document document;
      document.Parse(annotated.c_str());
      assert(!document.HasParseError() && document["eventTime"]["exact"] == false &&
        document["eventTime"]["uncertaintyBounded"] == false &&
        document["eventTime"]["uncertaintyNs"].IsNull());
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
      const auto status = wait(recorder);
      assert(status.revision == 1 && status.error.empty());
      assert(status.samples == 5ULL * FRAME_SAMPLES);
      blah2::RecordingReader reader(status.file,
        {"auto", "Kraken", CHANNELS, SAMPLE_RATE, FREQUENCY, 0});
      assert(reader.metadata().format == "mchq" &&
        reader.metadata().samples == status.samples);
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

    // Millisecond clip bounds normally cut through Heimdall frames. Both end
    // frames are trimmed channel-by-channel while retaining valid MCHQ blocks.
    {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      ingest(recorder, frame(20, 3000));
      ingest(recorder, frame(21, 3000));
      recorder.request(request(10, 999, 1002));
      ingest(recorder, frame(22, 3000));
      const auto status = wait(recorder);
      assert(status.revision == 10 && status.error.empty() &&
        status.samples == 7200);
      blah2::RecordingReader reader(status.file,
        {"auto", "Kraken", CHANNELS, SAMPLE_RATE, FREQUENCY, 0});
      blah2::IqBlock block;
      for (uint32_t count : {2400U, 3000U, 1800U})
      {
        assert(reader.read(block) && block.size() == CHANNELS);
        for (const auto& channel : block) assert(channel.size() == count);
      }
      assert(!reader.read(block));
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
      const auto status = wait(recorder);
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
      const auto status = wait(recorder);
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
      blocked.ringSeconds = 0.002;
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
      const auto status = wait(recorder);
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
      const auto saturated = wait(recorder);
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
      const auto failed = wait(recorder);
      assert(failed.revision == 8 && failed.file.empty() &&
        failed.error.find("Cannot create clip") != std::string::npos);
    }

    // The native boundary independently rejects any API regression that asks
    // for more than the physical ten-second maximum.
    {
      blah2::KrakenClipRecorder recorder(directory.string(), CHANNELS,
        SAMPLE_RATE, FREQUENCY, options);
      recorder.request(request(9, 1000, 11001));
      const auto failed = wait(recorder);
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
