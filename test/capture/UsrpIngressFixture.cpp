#include "capture/usrp/Usrp.h"
#include "capture/usrp/UsrpIngress.h"
#include <uhd/usrp/multi_usrp.hpp>
#include <cassert>
#include <filesystem>
#include <future>
#include <iostream>
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
        uhd::fake.error = error;
        uhd::fake.outOfSequence = error == uhd::rx_metadata_t::ERROR_CODE_NONE;
        IqData a(128), b(128);
        Usrp radio("Usrp", 527000000, rate, "/unused", &save,
          "type=b200", "A:A A:B", {"RX2", "RX2"}, {10, 20});
        const auto message = rejects([&] { radio.process(&a, &b); });
        assert(message.find("Receive discontinuity") != std::string::npos);
        assert(message.find("accepted_blocks=1") != std::string::npos);
        assert(message.find("accepted_samples_per_channel=7") != std::string::npos);
        assert(a.get_length() == 7 && b.get_length() == 7 && uhd::fake.calls == 2);
        for (unsigned i = 0; i < 7; ++i) {
          assert(a.view_data()[i] == std::complex<double>(i + .25, -double(i)));
          assert(b.view_data()[i] == std::complex<double>(i + 100.5, i));
        }
        assert(uhd::fake.started && uhd::fake.stopped);
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
    // Software queue-full must stop, not evict old IQ to hide a gap.
    {
      uhd::fake = {}; uhd::fake.failCall = 100;
      IqData a(10), b(10);
      Usrp radio("Usrp", 527000000, 2000000, "/unused", &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      const auto message = rejects([&] { radio.process(&a, &b); });
      assert(message.find("IQ processing queue is full") != std::string::npos);
      assert(a.get_length() == 7 && b.get_length() == 7 && uhd::fake.stopped);
      assert(a.view_data().front().real() == .25);
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
      assert(uhd::fake.stopped && a.get_length() == 7 && b.get_length() == 7);
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
    // Real recording writer receives only valid samples, then marks gap incomplete.
    {
      char pattern[] = "/tmp/vectorwarp-usrp-fixture-XXXXXX";
      const char* directory = mkdtemp(pattern);
      assert(directory);
      uhd::fake = {};
      IqData a(128), b(128);
      Usrp radio("Usrp", 527000000, 2000000, directory, &save,
        "", "A:A A:B", {"RX2", "RX2"}, {10, 20});
      const auto path = radio.open_file();
      rejects([&] { radio.process(&a, &b); });
      const auto status = radio.recording_status();
      assert(!status.active && status.samples == 7 && !status.error.empty());
      blah2::ReplayOptions options;
      options.receiver = "Usrp"; options.channels = 2;
      options.sampleRate = 2000000; options.frequency = 527000000;
      const auto message = rejects([&] { blah2::RecordingReader reader(path, options); });
      assert(message.find("not finished cleanly") != std::string::npos);
      std::filesystem::remove(path); std::filesystem::remove(directory);
    }
    std::cout << "USRP actual receive-loop fixtures passed (fake UHD; no hardware opened)\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
