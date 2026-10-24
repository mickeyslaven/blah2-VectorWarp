#include "Usrp.h"
#include "UsrpSettings.h"
#include "UsrpStream.h"
#include "UsrpIngress.h"
#include "UsrpCoherence.h"

#include <string.h>
#include <iostream>
#include <vector>
#include <complex>
#include <uhd/usrp/multi_usrp.hpp>
#include <cmath>
#include <stdexcept>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// constructor
Usrp::Usrp(std::string _type, uint32_t _fc, uint32_t _fs, 
  std::string _path, bool *_saveIq, std::string _address, 
  std::string _subdev, std::vector<std::string> _antenna, 
  std::vector<double> _gain)
    : Source(_type, _fc, _fs, _path, _saveIq), deviceTime(_fs)
{
  address = _address;
  subdev = _subdev;
  antenna = _antenna;
  gain = _gain;
  if (antenna.size() != 2 || gain.size() != 2)
    throw std::invalid_argument("[USRP] Two antenna ports and two gain values are required.");
}

void Usrp::start()
{
  stopRequested = false;
}

void Usrp::stop()
{
  stopRequested = true;
}

Source::SampleTimePosition Usrp::sample_time_position() const
{
  std::lock_guard<std::mutex> lock(provenanceMutex);
  const auto timing = deviceTime.snapshot();
  if (!timing.available || !coherenceAvailable || timingState != "qualified") return {};
  return {true, timing.epoch, timing.samples, timing.epochTick, fs,
    coherence.clockSource, coherence.timeSource, coherence.motherboard,
    coherence.subdevices};
}

std::string Usrp::startup_receipt_json() const
{
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  document.AddMember("schema", 1, allocator);
  document.AddMember("receiver", "Usrp", allocator);
  std::lock_guard<std::mutex> lock(provenanceMutex);
  document.AddMember("status",
    rapidjson::Value(coherenceAvailable ? "accepted" : "pending", allocator), allocator);
  document.AddMember("hardwareVerified",appliedSettingsAvailable,allocator);
  document.AddMember("readbackAvailable",appliedSettingsAvailable,allocator);
  document.AddMember("physicalRfVerified",false,allocator);
  rapidjson::Value requested(rapidjson::kObjectType);
  requested.AddMember("address", rapidjson::Value(address.c_str(), allocator), allocator);
  requested.AddMember("subdevices", rapidjson::Value(subdev.c_str(), allocator), allocator);
  requested.AddMember("frequency", fc, allocator);
  requested.AddMember("sampleRate", fs, allocator);
  rapidjson::Value antennas(rapidjson::kArrayType), gains(rapidjson::kArrayType);
  for (unsigned channel = 0; channel < 2; ++channel) {
    antennas.PushBack(rapidjson::Value(antenna[channel].c_str(), allocator), allocator);
    gains.PushBack(gain[channel], allocator);
  }
  requested.AddMember("antennas", antennas, allocator);
  requested.AddMember("gains", gains, allocator);
  document.AddMember("requested", requested, allocator);
  if(appliedSettingsAvailable) {
    rapidjson::Value applied(rapidjson::kObjectType),frequencies(rapidjson::kArrayType),
      rates(rapidjson::kArrayType),gainsApplied(rapidjson::kArrayType),antennasApplied(rapidjson::kArrayType);
    applied.AddMember("subdevices",rapidjson::Value(appliedSettings.subdevices.c_str(),allocator),allocator);
    for(unsigned channel=0;channel<2;++channel) {
      frequencies.PushBack(appliedSettings.frequencyHz[channel],allocator);
      rates.PushBack(appliedSettings.sampleRateHz[channel],allocator);
      gainsApplied.PushBack(appliedSettings.gainDb[channel],allocator);
      antennasApplied.PushBack(rapidjson::Value(appliedSettings.antenna[channel].c_str(),allocator),allocator);
    }
    applied.AddMember("perChannelFrequencyHz",frequencies,allocator);
    applied.AddMember("perChannelSampleRateHz",rates,allocator);
    applied.AddMember("perChannelGainDb",gainsApplied,allocator);
    applied.AddMember("perChannelAntenna",antennasApplied,allocator);
    document.AddMember("appliedReadback",applied,allocator);
  } else document.AddMember("appliedReadback",rapidjson::Value(rapidjson::kNullType),allocator);
  if (coherenceAvailable) {
    rapidjson::Value applied(rapidjson::kObjectType);
    applied.AddMember("motherboards", static_cast<uint64_t>(coherence.motherboards), allocator);
    applied.AddMember("channels", static_cast<uint64_t>(coherence.channels), allocator);
    applied.AddMember("motherboard", rapidjson::Value(coherence.motherboard.c_str(), allocator), allocator);
    applied.AddMember("clockSource", rapidjson::Value(coherence.clockSource.c_str(), allocator), allocator);
    applied.AddMember("timeSource", rapidjson::Value(coherence.timeSource.c_str(), allocator), allocator);
    applied.AddMember("subdevices", rapidjson::Value(coherence.subdevices.c_str(), allocator), allocator);
    applied.AddMember("policy", rapidjson::Value(coherence.policy.c_str(), allocator), allocator);
    applied.AddMember("singleMotherboard", true, allocator);
    rapidjson::Value identity(rapidjson::kObjectType), rxIds(rapidjson::kArrayType),
      rxSerials(rapidjson::kArrayType), rxSubdevices(rapidjson::kArrayType);
    identity.AddMember("complete", coherence.identityComplete, allocator);
    identity.AddMember("motherboardId",
      rapidjson::Value(coherence.motherboardId.c_str(), allocator), allocator);
    identity.AddMember("motherboardSerial",
      rapidjson::Value(coherence.motherboardSerial.c_str(), allocator), allocator);
    for (unsigned channel = 0; channel < 2; ++channel) {
      rxIds.PushBack(rapidjson::Value(coherence.rxIds[channel].c_str(), allocator), allocator);
      rxSerials.PushBack(rapidjson::Value(coherence.rxSerials[channel].c_str(), allocator), allocator);
      rxSubdevices.PushBack(rapidjson::Value(
        coherence.rxSubdeviceNames[channel].c_str(), allocator), allocator);
    }
    identity.AddMember("rxIds", rxIds, allocator);
    identity.AddMember("rxSerials", rxSerials, allocator);
    identity.AddMember("rxSubdeviceNames", rxSubdevices, allocator);
    applied.AddMember("deviceIdentity", identity, allocator);
    document.AddMember("coherence", applied, allocator);
  } else document.AddMember("coherence", rapidjson::Value(rapidjson::kNullType), allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

std::string Usrp::health_json() const
{
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  document.AddMember("schema", 1, allocator);
  document.AddMember("receiver", "Usrp", allocator);
  document.AddMember("monitoring", monitoring.load(), allocator);
  std::lock_guard<std::mutex> lock(provenanceMutex);
  const auto timing = deviceTime.snapshot();
  const bool qualified = timing.available && timingState == "qualified";
  rapidjson::Value time(rapidjson::kObjectType);
  time.AddMember("source", "uhd-device-time", allocator);
  time.AddMember("utcMapped", false, allocator);
  time.AddMember("continuous", qualified, allocator);
  time.AddMember("sampleRateHz", fs, allocator);
  const auto number = [&](int64_t value) {
    const auto text = std::to_string(value);
    return rapidjson::Value(text.c_str(), allocator);
  };
  const auto unsignedNumber = [&](uint64_t value) {
    const auto text = std::to_string(value);
    return rapidjson::Value(text.c_str(), allocator);
  };
  time.AddMember("epoch", unsignedNumber(timing.epoch), allocator);
  if (qualified) {
    time.AddMember("epochTick", number(timing.epochTick), allocator);
    time.AddMember("nextTick", number(timing.nextTick), allocator);
  } else {
    time.AddMember("epochTick", rapidjson::Value(rapidjson::kNullType), allocator);
    time.AddMember("nextTick", rapidjson::Value(rapidjson::kNullType), allocator);
  }
  time.AddMember("samples", unsignedNumber(qualified ? timing.samples : 0), allocator);
  time.AddMember("blocks", unsignedNumber(qualified ? timing.blocks : 0), allocator);
  document.AddMember("deviceTime", time, allocator);
  rapidjson::Value recovery(rapidjson::kObjectType);
  recovery.AddMember("state", rapidjson::Value(timingState.c_str(), allocator), allocator);
  recovery.AddMember("qualificationRequiredBlocks", REQUALIFICATION_BLOCKS, allocator);
  recovery.AddMember("qualificationObservedBlocks", qualificationObserved, allocator);
  recovery.AddMember("discontinuities", unsignedNumber(discontinuities), allocator);
  recovery.AddMember("requalifications", unsignedNumber(requalifications), allocator);
  recovery.AddMember("consecutiveDiscontinuities", unsignedNumber(consecutiveDiscontinuities), allocator);
  recovery.AddMember("discardedBlocks", unsignedNumber(discardedBlocks), allocator);
  recovery.AddMember("discardedSamplesPerChannel", unsignedNumber(discardedSamples), allocator);
  recovery.AddMember("lastDiscontinuity", rapidjson::Value(lastDiscontinuity.c_str(), allocator), allocator);
  document.AddMember("recovery", recovery, allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

std::string Usrp::recording_provenance_json() const
{
  rapidjson::Document document(rapidjson::kObjectType);
  auto& allocator = document.GetAllocator();
  document.AddMember("schema", "vectorwarp-usrp-recording-provenance-v1", allocator);
  for (const auto& item : {std::pair<const char*, std::string>{"startup", startup_receipt_json()},
                           {"timing", health_json()}}) {
    rapidjson::Document parsed;
    parsed.Parse(item.second.c_str());
    if (parsed.HasParseError() || !parsed.IsObject())
      throw std::runtime_error("[USRP] Could not serialize recording provenance.");
    rapidjson::Value copied;
    copied.CopyFrom(parsed, allocator);
    document.AddMember(rapidjson::Value(item.first, allocator), copied, allocator);
  }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer);
  return {buffer.GetString(), buffer.GetSize()};
}

void Usrp::process(IqData *buffer1, IqData *buffer2)
{
    if (!buffer1 || !buffer2 || buffer1 == buffer2)
      throw std::invalid_argument("[USRP] Two distinct IQ queues are required.");
    {
      std::lock_guard<std::mutex> lock(provenanceMutex);
      coherenceAvailable = false;
      appliedSettingsAvailable = false;
      appliedSettings = {};
      timingState = "requalifying";
      lastDiscontinuity.clear();
      qualificationObserved = discontinuities = requalifications = 0;
      consecutiveDiscontinuities = discardedBlocks = discardedSamples = 0;
      deviceTime.restart();
    }
    // create a USRP object
    uhd::usrp::multi_usrp::sptr usrp = 
      uhd::usrp::multi_usrp::make(address);

    // Set and independently read back both channels before opening the stream.
    const auto applied = apply_usrp_settings(*usrp, uhd::usrp::subdev_spec_t(subdev), fc, fs, gain, antenna);
    const auto attestation = attest_usrp_coherence(*usrp, subdev);
    {
      std::lock_guard<std::mutex> lock(provenanceMutex);
      coherence = attestation;
      coherenceAvailable = true;
      appliedSettings = applied;
      appliedSettingsAvailable = true;
    }

    // create a receive streamer
    uhd::stream_args_t streamArgs("fc32", "sc16");
    streamArgs.channels = {0, 1};
    uhd::rx_streamer::sptr rxStreamer = usrp->get_rx_stream(streamArgs);

    // allocate buffers to receive with samples (one buffer per channel)
    const size_t samps_per_buff = rxStreamer->get_max_num_samps();
    verify_usrp_receive_capacity(samps_per_buff);
    std::vector<std::complex<float>> usrpBuffer1(samps_per_buff);
    std::vector<std::complex<float>> usrpBuffer2(samps_per_buff);

    // create a vector of pointers to point to each of the channel buffers
    std::vector<std::complex<float>*> buff_ptrs;
    buff_ptrs.push_back(&usrpBuffer1.front());
    buff_ptrs.push_back(&usrpBuffer2.front());

    // setup stream
    uhd::rx_metadata_t metadata;
    uhd::stream_cmd_t streamCmd = uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS;
    streamCmd.stream_now = false;
    streamCmd.time_spec  = usrp->get_time_now() + uhd::time_spec_t(0.05);
    rxStreamer->issue_stream_cmd(streamCmd);
    struct StopStream {
      uhd::rx_streamer::sptr stream;
      ~StopStream() {
        try { stream->issue_stream_cmd(uhd::stream_cmd_t(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS)); }
        catch (...) {} // Do not replace the original receive/processing error.
      }
    } stopStream{rxStreamer};
    struct MonitorStream {
      std::atomic<bool>& active;
      ~MonitorStream() { active = false; }
    } monitorStream{monitoring};
    monitoring = true;
    UsrpIngressTiming ingressTiming;
    struct StagedBlock {
      UsrpDeviceTime::Observation timing;
      blah2::IqBlock samples;
    };
    std::vector<StagedBlock> staged;
    staged.reserve(REQUALIFICATION_BLOCKS);

    const auto block_start_position = [&](const UsrpDeviceTime::Observation& observation) {
      std::lock_guard<std::mutex> lock(provenanceMutex);
      const auto timing = deviceTime.snapshot();
      return SampleTimePosition{true, timing.epoch, observation.priorSamples,
        observation.firstTick, fs, coherence.clockSource, coherence.timeSource,
        coherence.motherboard, coherence.subdevices};
    };
    const auto recover = [&](const std::string& reason, uint64_t rejectedSamples) {
      uint64_t queuedSamples = 0;
      {
        std::unique_lock<IqData> firstLock(*buffer1);
        std::unique_lock<IqData> secondLock(*buffer2);
        queuedSamples = std::max(buffer1->get_length(), buffer2->get_length());
        buffer1->clear();
        buffer2->clear();
      }
      uint64_t stagedSamples = 0;
      for (const auto& block : staged)
        stagedSamples += block.samples.empty() ? 0 : block.samples.front().size();
      uint64_t consecutive = 0;
      {
        std::lock_guard<std::mutex> lock(provenanceMutex);
        deviceTime.discontinuity();
        timingState = "requalifying";
        qualificationObserved = 0;
        ++discontinuities;
        consecutive = ++consecutiveDiscontinuities;
        discardedBlocks += 1 + staged.size();
        discardedSamples += rejectedSamples + queuedSamples + stagedSamples;
        lastDiscontinuity = reason;
      }
      staged.clear();
      recording_epoch_discontinuity(reason);
      if (consecutive > MAX_CONSECUTIVE_DISCONTINUITIES) {
        close_file();
        throw std::runtime_error(reason +
          " Automatic requalification exceeded the bounded discontinuity budget." +
          ingressTiming.summary());
      }
    };

    while(!stopRequested)
    {
      // receive samples
      ingressTiming.before_receive();
      size_t nReceived = rxStreamer->recv(buff_ptrs, samps_per_buff, metadata);
      ingressTiming.received();
      if (stopRequested) break;
      // Reject only the discontinuous block. Clear unpublished pre-fault IQ,
      // roll the recording epoch, then require two continuous device-timed
      // UHD blocks before publishing the candidate epoch atomically.
      try {
        verify_usrp_receive(metadata, nReceived, samps_per_buff);
      }
      catch (const std::exception& error) {
        const std::string message = std::string(error.what()) + ingressTiming.summary();
        if (metadata.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE ||
            metadata.out_of_sequence) {
          recover(message, nReceived);
          continue;
        }
        recording_discontinuity(message);
        throw;
      }

      bool requalifying = false;
      {
        std::lock_guard<std::mutex> lock(provenanceMutex);
        requalifying = timingState != "qualified";
      }
      UsrpDeviceTime::Observation observation;
      try {
        observation = requalifying && !staged.empty() ?
          deviceTime.validate_after(metadata, nReceived, staged.back().timing) :
          deviceTime.validate(metadata, nReceived);
      } catch (const std::exception& error) {
        recover(std::string(error.what()) + ingressTiming.summary(), nReceived);
        continue;
      }

      if (requalifying) {
        blah2::IqBlock block(2);
        for (unsigned ch=0; ch<2; ++ch)
          block[ch].assign(buff_ptrs[ch], buff_ptrs[ch]+nReceived);
        staged.push_back({observation, std::move(block)});
        {
          std::lock_guard<std::mutex> lock(provenanceMutex);
          qualificationObserved = staged.size();
        }
        if (staged.size() < REQUALIFICATION_BLOCKS) continue;

        blah2::IqBlock qualified(2);
        for (const auto& candidate : staged)
          for (unsigned channel=0; channel<2; ++channel)
            qualified[channel].insert(qualified[channel].end(),
              candidate.samples[channel].begin(), candidate.samples[channel].end());
        const auto startPosition = block_start_position(staged.front().timing);
        try {
          append_usrp_block_committed(*buffer1, *buffer2,
            qualified[0].data(), qualified[1].data(), qualified[0].size(),
            ingressTiming, [&] {
              std::lock_guard<std::mutex> lock(provenanceMutex);
              for (const auto& candidate : staged) deviceTime.commit(candidate.timing);
              timingState = "qualified";
              qualificationObserved = REQUALIFICATION_BLOCKS;
              if (discontinuities) ++requalifications;
              consecutiveDiscontinuities = 0;
            });
          ingressTiming.blocks += staged.size() - 1;
        } catch (const std::exception& error) {
          const std::string message = std::string(error.what()) + ingressTiming.summary();
          recording_discontinuity(message);
          throw std::runtime_error(message);
        }
        record_block(qualified, startPosition);
        staged.clear();
        continue;
      }

      try {
        append_usrp_block_committed(*buffer1, *buffer2, buff_ptrs[0], buff_ptrs[1],
          nReceived, ingressTiming, [&] { deviceTime.commit(observation); });
      } catch (const std::exception& error) {
        const std::string message = std::string(error.what()) + ingressTiming.summary();
        recording_discontinuity(message);
        throw std::runtime_error(message);
      }
      if (recording_wants_samples() && nReceived) {
        blah2::IqBlock block(2);
        for (unsigned ch=0; ch<2; ++ch)
          block[ch].assign(buff_ptrs[ch], buff_ptrs[ch]+nReceived);
        record_block(block, block_start_position(observation));
      }
    }
}
