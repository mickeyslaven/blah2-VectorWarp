#include "capture/usrp/Usrp.h"
#include "capture/usrp/UsrpIngress.h"
#include <uhd/usrp/multi_usrp.hpp>
#include <cassert>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <thread>
#include <unistd.h>

uhd::rx_streamer::sptr uhd::usrp::multi_usrp::get_rx_stream(
    const uhd::stream_args_t& args) const {
  assert(args.channels == std::vector<size_t>({0, 1}));
  return std::make_shared<rx_streamer>();
}

namespace {
template<class Action> std::string rejects(Action action) {
  try { action(); } catch (const std::exception& error) { return error.what(); }
  throw std::runtime_error("Expected receive failure");
}
void locks_released(IqData& a, IqData& b) {
  // A stranded lock fails CTest's timeout instead of passing after an exception.
  auto check = std::async(std::launch::async, [&] {
    std::unique_lock<IqData> first(a), second(b);
  });
  assert(check.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  check.get();
}
}
int main() {
  try {
    bool save = false;
    for (double rate : {2000000., 6000000.}) {
      for (auto error : {uhd::rx_metadata_t::ERROR_CODE_OVERFLOW,
                        uhd::rx_metadata_t::ERROR_CODE_TIMEOUT,
                        uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET,
                        uhd::rx_metadata_t::ERROR_CODE_NONE}) {
        uhd::fake = {};
        uhd::fake.failCall = 3;
        uhd::fake.error = error;
        uhd::fake.outOfSequence = error == uhd::rx_metadata_t::ERROR_CODE_NONE;
        IqData a(128), b(128);
        Usrp radio("Usrp", 527000000, rate, "/unused", &save,
          "type=b200", "A:A A:B", {"RX2", "RX2"}, {10, 20});
        uhd::fake.beforeReceive = [&] { if (uhd::fake.calls == 7) radio.stop(); };
        radio.process(&a, &b);
        assert(a.get_length() == 21 && b.get_length() == 21 && uhd::fake.calls == 7);
        for (unsigned i = 0; i < 21; ++i) {
          const unsigned sample = i % 7;
          assert(a.view_data()[i] == std::complex<double>(sample + .25, -double(sample)));
          assert(b.view_data()[i] == std::complex<double>(sample + 100.5, sample));
        }
        assert(uhd::fake.started && uhd::fake.stopped);
        rapidjson::Document health;
        health.Parse(radio.health_json().c_str());
        assert(!health.HasParseError() && !health["monitoring"].GetBool());
        assert(std::string(health["recovery"]["state"].GetString()) == "qualified");
        assert(std::string(health["deviceTime"]["epoch"].GetString()) == "1");
        assert(std::string(health["recovery"]["discontinuities"].GetString()) == "1");
        assert(std::string(health["recovery"]["requalifications"].GetString()) == "1");
        assert(std::string(health["recovery"]["discardedBlocks"].GetString()) == "1");
        assert(std::string(health["recovery"]["discardedSamplesPerChannel"].GetString()) == "21");
        locks_released(a, b);
      }
    }
    // Capacity/readback preconditions and both distinct queue pointers.
    for (size_t capacity : {size_t(0), size_t((1u << 20) + 1)}) {
      uhd::fake = {}; uhd::fake.capacity = capacity;
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      rejects([&] { radio.process(&a, &b); });
      assert(!uhd::fake.started && !a.get_length() && !b.get_length());
      rejects([&] { radio.process(nullptr, &b); });
      rejects([&] { radio.process(&a, &a); });
    }
    // Oversized UHD response must not touch either queue.
    {
      uhd::fake = {}; uhd::fake.count = 65;
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      const auto message = rejects([&] { radio.process(&a, &b); });
      assert(message.find("more samples") != std::string::npos);
      assert(!a.get_length() && !b.get_length() && uhd::fake.stopped);
    }
    // Missing, regressing and jumping UHD device time reject only the bad
    // block, clear pre-fault unpublished IQ, then publish a qualified epoch.
    for (const auto fault : {std::pair<size_t, long long>{3, 0},
                             {0, -1}, {0, 1}}) {
      uhd::fake = {}; uhd::fake.failCall = 100;
      if (fault.first) uhd::fake.missingTimeCall = fault.first;
      else { uhd::fake.jumpTimeCall = 3; uhd::fake.jumpTicks = fault.second; }
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      uhd::fake.beforeReceive = [&] { if (uhd::fake.calls == 7) radio.stop(); };
      radio.process(&a, &b);
      assert(a.get_length() == 21 && b.get_length() == 21 && uhd::fake.calls == 7);
      const auto time = radio.sample_time_position();
      assert(time.available && time.epoch == 1 && time.deviceTimeEpochTick == 40000021);
      assert(time.nextSample == 21 && time.sampleRate == 2000000);
      rapidjson::Document health;
      health.Parse(radio.health_json().c_str());
      assert(std::string(health["recovery"]["lastDiscontinuity"].GetString()).find(
        fault.first ? "omitted device time" : "device-time discontinuity") != std::string::npos);
      locks_released(a, b);
    }
    // Health exposes an interrupted epoch as requalifying rather than healthy
    // when capture stops before the bounded two-block window completes.
    {
      uhd::fake = {}; uhd::fake.failCall = 3;
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      uhd::fake.beforeReceive = [&] { if (uhd::fake.calls == 5) radio.stop(); };
      radio.process(&a, &b);
      assert(!a.get_length() && !b.get_length());
      rapidjson::Document health;
      health.Parse(radio.health_json().c_str());
      assert(std::string(health["recovery"]["state"].GetString()) == "requalifying");
      assert(health["recovery"]["qualificationObservedBlocks"].GetUint64() == 1);
      assert(!health["deviceTime"]["continuous"].GetBool());
    }
    // A permanently unhealthy transport cannot spin forever under the
    // best-effort policy; the bounded fault budget remains terminal.
    {
      uhd::fake = {}; uhd::fake.failEveryCall = true;
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      const auto message = rejects([&] { radio.process(&a, &b); });
      assert(message.find("bounded discontinuity budget") != std::string::npos);
      assert(uhd::fake.calls == 9 && !a.get_length() && !b.get_length());
    }
    // Unknown or unsupported clock topology fails before starting the stream.
    for (int fault = 0; fault < 4; ++fault) {
      uhd::fake = {};
      if (fault == 0) uhd::fake.motherboards = 2;
      if (fault == 1) uhd::fake.motherboard.clear();
      if (fault == 2) uhd::fake.clockSource.clear();
      if (fault == 3) uhd::fake.timeSource.clear();
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      rejects([&] { radio.process(&a, &b); });
      assert(!uhd::fake.started && !a.get_length() && !b.get_length());
      const auto position = radio.sample_time_position();
      assert(!position.available);
    }
    // Software queue-full must stop, not evict old IQ to hide a gap.
    {
      uhd::fake = {}; uhd::fake.failCall = 100;
      IqData a(10), b(10);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      const auto message = rejects([&] { radio.process(&a, &b); });
      assert(message.find("IQ processing queue is full") != std::string::npos);
      assert(a.get_length() == 0 && b.get_length() == 0 && uhd::fake.stopped);
      locks_released(a, b);
    }
    // Stop requested during recv must accept no new samples and stop the stream.
    {
      uhd::fake = {};
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      uhd::fake.beforeReceive = [&] { if (uhd::fake.calls == 2) radio.stop(); };
      radio.process(&a, &b);
      assert(uhd::fake.stopped && a.get_length() == 0 && b.get_length() == 0);
    }
    // Paired append rejects invalid/misaligned/insufficient second queues before writes.
    {
      const std::complex<float> samples[] = {{1, 2}, {3, 4}};
      UsrpIngressTiming timing;
      IqData a(8), b(1), mismatch(8);
      rejects([&] { append_usrp_block(a, b, samples, samples, 2, timing); });
      assert(!a.get_length() && !b.get_length());
      mismatch.push_back({9, 10});
      rejects([&] { append_usrp_block(a, mismatch, samples, samples, 1, timing); });
      assert(!a.get_length() && mismatch.get_length() == 1);
      rejects([&] { append_usrp_block(a, b, nullptr, samples, 1, timing); });
      rejects([&] { append_usrp_block(a, a, samples, samples, 1, timing); });
      append_usrp_block(a, b, nullptr, nullptr, 0, timing);
      append_usrp_block(a, b, samples, samples, 1, timing);
      assert(a.get_length() == 1 && b.get_length() == 1);
      assert(timing.blocks == 1 && timing.samples == 1);
      locks_released(a, b);
      locks_released(a, mismatch);
    }
    // Diagnostic gap excludes initial FPGA setup and blocking recv duration.
    {
      UsrpIngressTiming timing;
      using Clock = UsrpIngressTiming::Clock;
      const auto start = Clock::time_point{} + std::chrono::seconds(30);
      timing.before_receive(start);
      assert(timing.maxServiceGapMs == 0);
      timing.received(start + std::chrono::seconds(1));
      timing.before_receive(start + std::chrono::seconds(1) + std::chrono::milliseconds(7));
      assert(timing.maxServiceGapMs == 7);
    }
    // A receive fault closes only the pre-fault recording epoch. Two continuous
    // timed blocks requalify capture and lazily start a replayable new epoch.
    {
      char pattern[] = "/tmp/vectorwarp-usrp-fixture-XXXXXX";
      const char* directory = mkdtemp(pattern);
      assert(directory);
      uhd::fake = {}; uhd::fake.failCall = 3;
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, directory, &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      const auto path = radio.open_file();
      uhd::fake.beforeReceive = [&] { if (uhd::fake.calls == 7) radio.stop(); };
      radio.process(&a, &b);
      const auto status = radio.recording_status();
      assert(status.active && !status.resumePending && status.samples == 21 &&
        status.file != path && status.error.empty());
      radio.close_file();
      std::ifstream provenance(path + ".provenance.json");
      assert(provenance);
      rapidjson::IStreamWrapper stream(provenance);
      rapidjson::Document document;
      document.ParseStream(stream);
      assert(!document.HasParseError());
      assert(std::string(document["schema"].GetString()) ==
        "vectorwarp-recording-provenance-v2");
      const auto& details = document["receiverDetails"];
      assert(std::string(details["schema"].GetString()) ==
        "vectorwarp-usrp-recording-provenance-v1");
      assert(std::string(details["startup"]["status"].GetString()) == "accepted");
      assert(details["startup"]["hardwareVerified"].GetBool());
      assert(!details["startup"]["physicalRfVerified"].GetBool());
      assert(details["startup"]["appliedReadback"]["perChannelGainDb"][0].GetDouble() == 10);
      assert(details["startup"]["appliedReadback"]["perChannelGainDb"][1].GetDouble() == 20);
      assert(details["startup"]["coherence"]["singleMotherboard"].GetBool());
      assert(details["startup"]["coherence"]["deviceIdentity"]["complete"].GetBool());
      assert(std::string(details["startup"]["coherence"]["deviceIdentity"]
        ["motherboardSerial"].GetString()) == "31ABC42");
      assert(std::string(details["startup"]["coherence"]["policy"].GetString()) ==
        "single-motherboard-b210-v1");
      assert(!details["timing"]["deviceTime"]["continuous"].GetBool());
      assert(std::string(details["timing"]["deviceTime"]["epoch"].GetString()) == "1");
      assert(std::string(details["timing"]["recovery"]["state"].GetString()) == "requalifying");
      assert(std::string(details["timing"]["recovery"]["discontinuities"].GetString()) == "1");
      assert(!document["recording"]["complete"].GetBool());
      assert(std::string(document["timing"]["epoch"].GetString()) == "0");
      assert(std::string(document["timing"]["sourceFirstSample"].GetString()) == "0");
      assert(std::string(document["timing"]["deviceTimeEpochTick"].GetString()) == "40000000");
      assert(!document["timing"]["discontinuities"].Empty());
      blah2::ReplayOptions options;
      options.receiver = "Usrp"; options.channels = 2;
      options.sampleRate = 2000000; options.frequency = 527000000;
      const auto message = rejects([&] { blah2::RecordingReader reader(path, options); });
      assert(message.find("not finished cleanly") != std::string::npos);

      std::ifstream resumedProvenance(status.file + ".provenance.json");
      assert(resumedProvenance);
      rapidjson::IStreamWrapper resumedStream(resumedProvenance);
      rapidjson::Document resumedDocument;
      resumedDocument.ParseStream(resumedStream);
      assert(!resumedDocument.HasParseError() &&
        resumedDocument["recording"]["complete"].GetBool());
      assert(std::string(resumedDocument["recording"]["samples"].GetString()) == "21");
      assert(std::string(resumedDocument["timing"]["epoch"].GetString()) == "1");
      assert(std::string(resumedDocument["timing"]["sourceFirstSample"].GetString()) == "0");
      assert(std::string(resumedDocument["timing"]["deviceTimeEpochTick"].GetString()) == "40000021");
      const auto& resumedDetails = resumedDocument["receiverDetails"]["timing"];
      assert(std::string(resumedDetails["recovery"]["state"].GetString()) == "qualified");
      assert(std::string(resumedDetails["recovery"]["requalifications"].GetString()) == "1");
      blah2::RecordingReader replay(status.file, options);
      blah2::IqBlock replayed;
      assert(replay.read(replayed) && replayed[0].size() == 14);
      assert(replay.read(replayed) && replayed[0].size() == 7);
      assert(!replay.read(replayed));
      assert(replay.provenance().contractValid && !replay.provenance().decisionGrade &&
        !replay.provenance().exactTime && replay.provenance().sourceFirstSample == 0);
      std::filesystem::remove(path);
      std::filesystem::remove(path + ".provenance.json");
      std::filesystem::remove(status.file);
      std::filesystem::remove(status.file + ".provenance.json");
      std::filesystem::remove(directory);
    }
    std::cout << "USRP actual receive-loop fixtures passed (fake UHD; no hardware opened)\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
