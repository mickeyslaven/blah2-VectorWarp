#include "Capture.h"
#include "kraken/Kraken.h"
#include "capture/PairedCpiSource.h"
#include <iostream>
#include <thread>
#include <csignal>
#include <cmath>
#include <limits>
#include <httplib.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

extern volatile sig_atomic_t STOP_SIGNAL;

// constants
const std::string Capture::VALID_TYPE[4] = {"RspDuo", "Usrp", "HackRF", "Kraken"};

// constructor
Capture::Capture(std::string _type, uint32_t _fs, uint32_t _fc, std::string _path)
{
  type = _type;
  fs = _fs;
  fc = _fc;
  path = _path;
  replay = false;
  saveIq = false;
}

void Capture::configure_aircraft_clips(bool enabled, double preSeconds,
    double postSeconds) {
  if (replay || !enabled) return;
  if (!std::isfinite(preSeconds) || !std::isfinite(postSeconds) ||
      preSeconds < 0 || postSeconds < 0 || preSeconds + postSeconds <= 0 ||
      preSeconds + postSeconds > 10)
    throw std::invalid_argument("Invalid capture.aircraft_clips duration");
  automaticClips = true;
  // Queue coalescing may legally expand either side of the configured base
  // window up to the native ten-second cap.
  clipPreSeconds = 10;
  clipMaximumSeconds = 10;
}

Capture::SamplePosition Capture::live_sample_position() const
{
  if (replay) {
    std::lock_guard<std::mutex> lock(statusMutex);
    const auto& value=replayProgress.provenance;
    // A native MCHT clip can carry a continuous receiver epoch/sample range
    // even when it intentionally lacks exact bounded UTC or a portable v2
    // provenance contract. Do not discard that relative source clock.
    if (!value.contractValid && !value.sourceSampleAvailable) return {};
    const uint64_t base=value.sourceSampleAvailable ? value.sourceFirstSample : 0;
    if (replayProgress.samples>UINT64_MAX-base) return {};
    return {true,value.epoch,base+replayProgress.samples,0,0,0,0,0,
      value.deviceTimeEpochTick,replayProgress.sampleRate,value.clockSource,
      value.deviceTimeSource,value.motherboard,value.subdevices};
  }
  const auto* kraken = dynamic_cast<const Kraken*>(device.get());
  if (kraken) {
    const auto value = kraken->sample_position();
    SamplePosition result;
    result.available = value.available;
    result.epoch = value.epoch;
    result.nextSample = value.nextSample;
    result.epochEstimateNanoseconds = value.epochEstimateNanoseconds;
    result.hostReceiptResidualNanoseconds = value.hostReceiptResidualNanoseconds;
    result.hostReceiptMaximumResidualNanoseconds = value.hostReceiptMaximumResidualNanoseconds;
    result.hostReceiptDriftPpm = value.hostReceiptDriftPpm;
    result.hostReceiptAnchors = value.hostReceiptAnchors;
    return result;
  }
  if (!device) return {};
  const auto value = device->sample_time_position();
  return {value.available, value.epoch, value.nextSample, 0,0,0,0,0,
    value.deviceTimeEpochTick, value.sampleRate, value.clockSource,
    value.timeSource, value.motherboard, value.subdevices};
}

std::vector<double> Capture::input_gain_db() const
{
  if(replay){std::lock_guard<std::mutex> lock(statusMutex);
    return replayProgress.provenance.perChannelGainDb;}
  const auto* kraken=dynamic_cast<const Kraken*>(device.get());
  return kraken?kraken->actual_gain_db():std::vector<double>{};
}

uint64_t Capture::input_generation() const
{
  if (replay) return replayGeneration.load(std::memory_order_acquire);
  return device ? device->processing_epoch() : 0;
}

void Capture::process(IqData *buffer1, IqData *buffer2, c4::yml::NodeRef config, 
  std::string ip_capture, uint16_t port_capture)
{
  process(std::vector<IqData *>{buffer1, buffer2}, config, ip_capture,
    port_capture);
}

void Capture::process(const std::vector<IqData *>& buffers,
  c4::yml::NodeRef config, std::string ip_capture, uint16_t port_capture,
  uint32_t frameSamples)
{
  std::cout << (replay ? "Opening recording" : "Setting up device " + type) << std::endl;
  // Playback never constructs or initializes a physical receiver.
  if (!replay) {
    try {
      device = factory_source(type, config, buffers.size());
      if (!device) throw std::runtime_error("Capture backend is unavailable in this build");
      if (automaticClips && type != "Kraken")
        device->configure_portable_clips(clipPreSeconds, clipMaximumSeconds);
      if (pairedCpiQueue) {
        if (auto* paired = dynamic_cast<PairedCpiSource*>(device.get())) {
          paired->set_paired_cpi_queue(pairedCpiQueue);
          pairedCpiQueueActive.store(true);
        } else {
          std::cerr << "RSPduo paired CPI queue unavailable in this receiver module; using FIFO capture\n";
        }
      }
    } catch (const std::exception& error) { processing_error(error.what()); }
  }
  std::thread control([&] {
    httplib::Client cli("http://" + ip_capture + ":" + std::to_string(port_capture));
    cli.set_connection_timeout(0, 250000);
    cli.set_read_timeout(0, 500000);
    cli.set_write_timeout(0, 500000);
    bool deviceStopped = false;
    bool haveRecordingRevision = false;
    uint64_t recordingRevision = 0;
    auto applyRecordingRequest = [&](bool requested, uint64_t revision,
                                     bool revisioned) {
      const bool changed = revisioned ?
        (!haveRecordingRevision || revision != recordingRevision) : requested != saveIq;
      if (!changed) return;
      try {
        // A distinct revision is a distinct capture request, even when its
        // boolean value is unchanged: acknowledge it only after re-opening.
        if (saveIq) device->close_file();
        saveIq = requested;
        if (saveIq) device->open_file();
      } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
      if (revisioned) {
        haveRecordingRevision = true;
        recordingRevision = revision;
        std::lock_guard<std::mutex> lock(statusMutex);
        recordingRequestId = revision;
      }
    };
    auto applyClipRequest = [&](const rapidjson::Value& clip) {
      if (!clip.IsObject() || !clip.HasMember("pending") ||
          !clip["pending"].IsBool() || !clip["pending"].GetBool() ||
          !clip.HasMember("clipRevision") || !clip["clipRevision"].IsUint64() ||
          clip["clipRevision"].GetUint64() == 0)
        return;
      blah2::IqClipRequest value;
      value.revision = clip["clipRevision"].GetUint64();
      const auto integer_time = [&](const char* name, int64_t& output) {
        if (!clip.HasMember(name) || !clip[name].IsNumber()) return false;
        const double number = clip[name].GetDouble();
        if (!std::isfinite(number) || number < 0 ||
            number >= static_cast<double>(std::numeric_limits<int64_t>::max()) ||
            std::floor(number) != number)
          return false;
        output = static_cast<int64_t>(number);
        return true;
      };
      const auto ids = [&](const char* name, std::vector<std::string>& output) {
        if (!clip.HasMember(name) || !clip[name].IsArray()) return false;
        for (const auto& item : clip[name].GetArray()) {
          if (!item.IsString()) return false;
          output.emplace_back(item.GetString(), item.GetStringLength());
        }
        return true;
      };
      if (!integer_time("requestedEventTime", value.requestedEventTimeMs) ||
          !integer_time("windowStart", value.windowStartMs) ||
          !integer_time("windowEnd", value.windowEndMs) ||
          !ids("eventIds", value.eventIds) || !ids("passIds", value.passIds)) {
        std::lock_guard<std::mutex> lock(statusMutex);
        unsupportedClipRevision = value.revision;
        unsupportedClipError = "Invalid clip request from capture API";
        return;
      }
      if (automaticClips)
        device->request_clip(value);
      else {
        std::lock_guard<std::mutex> lock(statusMutex);
        unsupportedClipRevision = value.revision;
        unsupportedClipError = "Automatic IQ clips are disabled";
      }
    };
    while (!stopping.load()) {
      if (STOP_SIGNAL) request_stop();
      if (device && inputStopped.load() && !deviceStopped) {
        try { device->stop(); }
        catch (const std::exception& error) { processing_error(error.what()); }
        try { device->close_file(); }
        catch (const std::exception& error) { processing_error(error.what()); }
        deviceStopped = true;
      }
      if (device && !inputStopped.load()) {
        const auto request = cli.Get("/capture/request");
        if (request && request->status == 200) {
          rapidjson::Document payload;
          payload.Parse(request->body.c_str());
          if (!payload.HasParseError() && payload.IsObject() &&
              payload.HasMember("recording") && payload["recording"].IsBool() &&
              payload.HasMember("revision") && payload["revision"].IsUint64()) {
            applyRecordingRequest(payload["recording"].GetBool(),
              payload["revision"].GetUint64(), true);
            if (payload.HasMember("clip")) applyClipRequest(payload["clip"]);
          }
        } else if (request && request->status == 404) {
          const auto legacy = cli.Get("/capture");
          if (legacy && legacy->status == 200 &&
              (legacy->body == "true" || legacy->body == "false"))
            applyRecordingRequest(legacy->body == "true", 0, false);
        }
      }
      cli.Post("/api/processor/status", status_json(), "application/json");
      for (unsigned i=0; i<25 && !stopping.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (device && !deviceStopped) {
      try { device->stop(); }
      catch (const std::exception& error) { processing_error(error.what()); }
      try { device->close_file(); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    }
  });
  try {
    if (replay) {
      replayOptions.receiver = type; replayOptions.channels = buffers.size();
      replayOptions.sampleRate = fs; replayOptions.frequency = fc;
      blah2::ReplayPlayer player;
      player.run(file, replayOptions, buffers, frameSamples, loop, inputStopped,
        [&](const blah2::ReplayProgress& value) {
          std::lock_guard<std::mutex> lock(statusMutex);
          replayProgress = value; replayGeneration.store(value.loops);
        }, [&] { return processingBusy.load(); });
    } else if (device) { device->start(); device->process(buffers); }
  } catch (const std::exception& error) {
    processing_error(error.what()); request_input_stop();
  }
  if (pairedCpiQueueActive.exchange(false) && pairedCpiQueue)
    pairedCpiQueue->close();
  // Keep status available at EOF or after an input error. The browser can show
  // what happened and restart with corrected settings instead of losing the API.
  control.join();

  // The receiver and its callbacks have stopped, and the receive-loop error
  // handler has completed. Publish the cumulative overflow counts once more
  // so drops or callback faults after the final CPI cannot disappear at exit.
  // The processing thread may still drain queues: lock in channel order, as
  // BufferLocks and the receiver do, and release before status/HTTP operations.
  try {
    std::vector<uint64_t> backlog, dropped;
    backlog.reserve(buffers.size());
    dropped.reserve(buffers.size());
    {
      std::vector<std::unique_lock<IqData>> locks;
      locks.reserve(buffers.size());
      for (auto* buffer : buffers) locks.emplace_back(*buffer);
      for (auto* buffer : buffers) {
        backlog.push_back(buffer->get_length());
        dropped.push_back(buffer->get_dropped_samples());
      }
    }
    if (pairedCpiQueue) {
      const auto queue = pairedCpiQueue->stats();
      if (queue.published || queue.discardedSamples || queue.trailingSamples || queue.discontinuities) {
        const uint64_t queued = uint64_t(pairedCpiQueue->backlog_samples());
        backlog.assign(buffers.size(), queued);
        dropped.assign(buffers.size(), queue.discardedSamples);
      }
    }
    rapidjson::Document finalStatus;
    finalStatus.Parse(status_json().c_str());
    if (finalStatus.HasParseError() || !finalStatus.IsObject())
      throw std::runtime_error("Could not serialize final capture status");
    auto& allocator = finalStatus.GetAllocator();
    rapidjson::Value backlogJson(rapidjson::kArrayType), droppedJson(rapidjson::kArrayType);
    for (uint64_t value : backlog) backlogJson.PushBack(value, allocator);
    for (uint64_t value : dropped) droppedJson.PushBack(value, allocator);
    finalStatus.AddMember("captureStopped", true, allocator);
    finalStatus.AddMember("captureBacklogSamples", backlogJson, allocator);
    finalStatus.AddMember("captureDroppedSamples", droppedJson, allocator);
    rapidjson::StringBuffer body;
    rapidjson::Writer<rapidjson::StringBuffer> writer(body);
    finalStatus.Accept(writer);
    httplib::Client finalClient("http://" + ip_capture + ":" + std::to_string(port_capture));
    finalClient.set_connection_timeout(0, 250000);
    finalClient.set_read_timeout(0, 500000);
    finalClient.set_write_timeout(0, 500000);
    finalClient.Post("/api/processor/status", body.GetString(), "application/json");
  } catch (const std::exception& error) {
    // A missing telemetry peer must not replace the receiver's actual error
    // or prevent shutdown. Qualification requires receipt of this final post.
    std::cerr << "Final capture status: " << error.what() << '\n';
  }
}

void Capture::processing_error(const std::string& error) {
  std::lock_guard<std::mutex> lock(statusMutex);
  captureError = error;
  std::cerr << "Radar input: " << error << '\n';
}

std::string Capture::status_json() const {
  std::lock_guard<std::mutex> lock(statusMutex);
  rapidjson::Document document(rapidjson::kObjectType);
  auto& a = document.GetAllocator();
  auto text = [&](const char* name,const std::string& value) {
    document.AddMember(rapidjson::Value(name,a),rapidjson::Value(value.c_str(),a),a);
  };
  text("input",replay ? "replay" : "live"); text("receiver",type);
  text("state",captureError.empty() ? (replay ? replayProgress.state : "live") : "error");
  text("error",captureError); text("file",replay ? file : "");
  document.AddMember("sampleRate",fs,a); document.AddMember("frequency",fc,a);
  document.AddMember("positionSamples",replayProgress.samples,a);
  document.AddMember("totalSamples",replayProgress.total,a);
  document.AddMember("loops",replayProgress.loops,a);
  document.AddMember("trailingSamples",replayProgress.trailingSamples,a);
  rapidjson::Value provenance(rapidjson::kObjectType);
  provenance.AddMember("present",replayProgress.provenance.present,a);
  provenance.AddMember("contractValid",replayProgress.provenance.contractValid,a);
  provenance.AddMember("legacy",replayProgress.provenance.legacy,a);
  provenance.AddMember("decisionGrade",replayProgress.provenance.decisionGrade,a);
  provenance.AddMember("exactTime",replayProgress.provenance.exactTime,a);
  provenance.AddMember("uncertaintyBounded",replayProgress.provenance.uncertaintyBounded,a);
  provenance.AddMember("configurationFingerprintValid",replayProgress.provenance.configurationFingerprintValid,a);
  provenance.AddMember("codeFingerprintMatches",replayProgress.provenance.codeFingerprintMatches,a);
  provenance.AddMember("schema",rapidjson::Value(replayProgress.provenance.schema.c_str(),a),a);
  provenance.AddMember("backend",rapidjson::Value(replayProgress.provenance.backend.c_str(),a),a);
  provenance.AddMember("timeSource",rapidjson::Value(replayProgress.provenance.timeSource.c_str(),a),a);
  provenance.AddMember("reason",rapidjson::Value(replayProgress.provenance.reason.c_str(),a),a);
  document.AddMember("replayProvenance",provenance,a);
  const auto recording = device ? device->recording_status() : Source::RecordingStatus{};
  document.AddMember("recording",recording.active,a);
  document.AddMember("recordingResumePending",recording.resumePending,a);
  text("recordingFile",recording.file); text("recordingError",recording.error);
  document.AddMember("recordedSamples",recording.samples,a);
  document.AddMember("recordingRequestId",recordingRequestId,a);
  blah2::IqClipStatus clip;
  if (device) clip = device->clip_status();
  if (unsupportedClipRevision > clip.revision)
    clip = {unsupportedClipRevision, true, {}, unsupportedClipError, 0};
  if (clip.terminal && clip.revision) {
    document.AddMember("clipRevision", clip.revision, a);
    text("clipError", clip.error);
    text("clipFile", clip.file);
    document.AddMember("clipSamples", clip.samples, a);
  }
  if (pairedCpiQueue) {
    const auto queue = pairedCpiQueue->stats();
    document.AddMember("pairedCpiQueue", pairedCpiQueueActive.load(), a);
    document.AddMember("pairedCpiBacklogSamples", uint64_t(pairedCpiQueue->backlog_samples()), a);
    document.AddMember("pairedCpiDropped", queue.discardedSamples, a);
    document.AddMember("pairedCpiTrailingSamples", queue.trailingSamples, a);
  }
  if ((type == "RspDuo" || type == "Usrp" || type == "HackRF") && device) {
    const auto receiptJson = blah2::receiver_startup_receipt(device);
    rapidjson::Document receipt;
    receipt.Parse(receiptJson.c_str());
    if (!receipt.HasParseError() && receipt.IsObject()) {
      rapidjson::Value copied;
      copied.CopyFrom(receipt, a);
      document.AddMember("receiverStartup", copied, a);
    }
    const auto healthJson = blah2::receiver_health(device);
    rapidjson::Document health;
    health.Parse(healthJson.c_str());
    if (!health.HasParseError() && health.IsObject()) {
      rapidjson::Value copied;
      copied.CopyFrom(health, a);
      document.AddMember("receiverHealth", copied, a);
    }
  }
  rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  document.Accept(writer); return buffer.GetString();
}

blah2::ReceiverSource Capture::factory_source(const std::string& type,
  c4::yml::NodeRef config, std::size_t channelCount)
{
    if (type == VALID_TYPE[3])
    {
      std::string heimdallHost = "127.0.0.1";
      uint16_t heimdallPort = 8091;
      if (config.has_child("heimdall"))
      {
        config["heimdall"]["host"] >> heimdallHost;
        config["heimdall"]["port"] >> heimdallPort;
      }
      return blah2::ReceiverSource(new Kraken(type, fc, fs, path, &saveIq,
        channelCount, heimdallHost, heimdallPort));
    }

    Blah2ReceiverConfig settings;
    settings.frequency = fc;
    settings.sampleRate = fs;
    settings.channels = channelCount;
    settings.recordingPath = path.c_str();
    settings.saveIq = &saveIq;
    // Adapter constructors copy these strings during the synchronous call.
    std::string address, subdev, antenna[2], serial[2], rspduoSerial;
    if (type == VALID_TYPE[0])
    {
        if (config.has_child("serial")) config["serial"] >> rspduoSerial;
        settings.rspduoSerial = rspduoSerial.c_str();
        bool dabNotch, rfNotch;
        config["agcSetPoint"] >> settings.agcSetPoint;
        config["bandwidthNumber"] >> settings.bandwidthNumber;
        config["gainReduction"][0] >> settings.gainReduction[0];
        config["gainReduction"][1] >> settings.gainReduction[1];
        config["lnaState"] >> settings.lnaState;
        config["dabNotch"] >> dabNotch;
        config["rfNotch"] >> rfNotch;
        settings.dabNotch = dabNotch;
        settings.rfNotch = rfNotch;
    }
    else if (type == VALID_TYPE[1])
    {
        config["address"] >> address;
        config["subdev"] >> subdev;
        settings.address = address.c_str();
        settings.subdev = subdev.c_str();
        for (unsigned i = 0; i < 2; ++i) {
          config["antenna"][i] >> antenna[i];
          settings.antenna[i] = antenna[i].c_str();
          config["gain"][i] >> settings.gain[i];
        }
    }
    else if (type == VALID_TYPE[2])
    {
      for (unsigned i = 0; i < 2; ++i) {
        bool ampEnable;
        config["serial"][i] >> serial[i];
        settings.serial[i] = serial[i].c_str();
        config["gain_lna"][i] >> settings.gainLna[i];
        config["gain_vga"][i] >> settings.gainVga[i];
        config["amp_enable"][i] >> ampEnable;
        settings.ampEnable[i] = ampEnable;
      }
    }
    return blah2::load_receiver(type, settings);
}

void Capture::set_replay(bool _loop, std::string _file, std::string format,
    uint32_t legacyBlockSamples, std::string provenanceMode)
{
  replay = true;
  loop = _loop;
  file = _file;
  replayOptions.format = std::move(format);
  replayOptions.legacyBlockSamples = legacyBlockSamples;
  replayOptions.provenanceMode = std::move(provenanceMode);
}
