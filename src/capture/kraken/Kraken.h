/// @file Kraken.h
/// @brief Coherent KrakenSDR capture through KrakenSDR Suite V2 Heimdall.

#ifndef KRAKEN_H
#define KRAKEN_H

#include "capture/Source.h"
#include "data/IqData.h"
#include "HeimdallFrame.h"
#include "KrakenClipRecorder.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

class Kraken : public Source
{
private:
  static constexpr std::size_t MAX_CHANNELS = 8;
  static constexpr uint32_t SUITE_SAMPLE_RATE = 2400000;
  std::size_t channelCount;
  std::string heimdallHost;
  uint16_t heimdallPort;
  std::atomic<bool> running{false};
  int socketFd{-1};
  std::unique_ptr<blah2::KrakenClipRecorder> clipRecorder;
  std::atomic<bool> samplePositionAvailable{false};
  std::atomic<uint64_t> sampleEpoch{0};
  std::atomic<uint64_t> nextEpochSample{0};
  std::atomic<int64_t> sampleEpochEstimateNanoseconds{0};
  mutable std::mutex provenanceMutex;
  HeimdallFrame::Header lastHeader;
  bool lastHeaderAvailable = false;

  void process_heimdall(const std::vector<IqData *>& buffers);
  void stream_heimdall(const std::vector<IqData *>& buffers);
  void connect_heimdall();
  void receive_exact(void *data, std::size_t length);
  std::array<uint8_t, HeimdallFrame::FIXED_HEADER_SIZE> receive_header();
  static void clear_buffers(const std::vector<IqData *>& buffers);

public:
  struct SamplePosition
  {
    bool available = false;
    uint64_t epoch = 0;
    uint64_t nextSample = 0;
    int64_t epochEstimateNanoseconds = 0;
  };
  Kraken(std::string type, uint32_t fc, uint32_t fs, std::string path,
    bool *saveIq, std::size_t channelCount, std::string heimdallHost,
    uint16_t heimdallPort);
  void process(IqData *buffer1, IqData *buffer2) override;
  void process(const std::vector<IqData *>& buffers) override;
  void start() override;
  void stop() override;
  void request_clip(const blah2::IqClipRequest& request);
  blah2::IqClipStatus clip_status() const;
  SamplePosition sample_position() const;
  Source::SampleTimePosition sample_time_position() const override;
  std::string recording_provenance_json() const override;
};

#endif
