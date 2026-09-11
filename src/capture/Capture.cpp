#include "Capture.h"
#include "kraken/Kraken.h"
#include <iostream>
#include <thread>
#include <csignal>
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
  // Keep status available at EOF or after an input error. The browser can show
  // what happened and restart with corrected settings instead of losing the API.
  control.join();
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
  const auto recording = device ? device->recording_status() : Source::RecordingStatus{};
  document.AddMember("recording",recording.active,a);
  text("recordingFile",recording.file); text("recordingError",recording.error);
  document.AddMember("recordedSamples",recording.samples,a);
  document.AddMember("recordingRequestId",recordingRequestId,a);
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
    uint32_t legacyBlockSamples)
{
  replay = true;
  loop = _loop;
  file = _file;
  replayOptions.format = std::move(format);
  replayOptions.legacyBlockSamples = legacyBlockSamples;
}
